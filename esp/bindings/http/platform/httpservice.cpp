/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#pragma warning (disable : 4786)

#include "esphttp.hpp"

//Jlib
#include "jliball.hpp"

#include "espcontext.hpp"
#include "esphttp.hpp"

//ESP Bindings
#include "http/platform/httpsecurecontext.hpp"
#include "http/platform/httpservice.hpp"
#include "http/platform/httptransport.hpp"
#include "http/platform/httpprot.hpp"

#include "htmlpage.hpp"
#include "dasds.hpp"

#include <map>
#include <inttypes.h>

/***************************************************************************
 *              CEspHttpServer Implementation
 ***************************************************************************/

CEspHttpServer::CEspHttpServer(ISocket& sock, CEspApplicationPort& apport, bool viewConfig, int maxRequestEntityLength):m_socket(sock), m_MaxRequestEntityLength(maxRequestEntityLength)
{
    m_request.setown(new CHttpRequest(sock));
    IEspContext* ctx = createEspContext(createHttpSecureContext(m_request.get()));
    m_request->setMaxRequestEntityLength(maxRequestEntityLength);
    m_response.setown(new CHttpResponse(sock));
    m_request->setOwnContext(ctx);
    m_response->setOwnContext(LINK(ctx));
    m_apport = &apport;
    if (apport.getDefaultBinding())
        m_defaultBinding.set(apport.getDefaultBinding()->queryBinding());
    m_viewConfig=viewConfig;
}

CEspHttpServer::~CEspHttpServer()
{
    try
    {
        IEspContext* ctx = m_request->queryContext();
        if (ctx)
        {
            //Request is logged only when there is an exception or the request time is very long.
            //If the flag of 'EspLogRequests' is set or the log level > LogNormal, the Request should
            //has been logged and it should not be logged here.
            ctx->setProcessingTime();
            if ((ctx->queryHasException() || (ctx->queryProcessingTime() > getSlowProcessingTime())) &&
                (getEspLogRequests() == LogRequestsWithIssuesOnly))
            {
                StringBuffer logStr;
                logStr.appendf("%s %s", m_request->queryMethod(), m_request->queryPath());

                const char* paramStr = m_request->queryParamStr();
                if (paramStr && *paramStr)
                    logStr.appendf("?%s", paramStr);

                if (ctx->queryHasException())
                    DBGLOG("Request with exception. Request[%s]", logStr.str());
                else
                    DBGLOG("Request with long processing time: %u seconds. Request[%s]",
                        (unsigned) (ctx->queryProcessingTime() * 0.001), logStr.str());

                if (m_request->isSoapMessage())
                {
                    StringBuffer requestStr;
                    m_request->getContent(requestStr);
                    if (requestStr.length())
                        m_request->logSOAPMessage(requestStr.str(), NULL);
                }
            }
        }

        m_request.clear();
        m_response.clear();
    }
    catch (...)
    {
        IERRLOG("In CEspHttpServer::~CEspHttpServer() -- Unknown Exception.");
    }
}

const char* getSubServiceDesc(sub_service stype)
{
#define DEF_CASE(s) case s: return #s;
    switch(stype)
    {
    DEF_CASE(sub_serv_unknown)
    DEF_CASE(sub_serv_root)
    DEF_CASE(sub_serv_main)
    DEF_CASE(sub_serv_service)
    DEF_CASE(sub_serv_method)
    DEF_CASE(sub_serv_files)
    DEF_CASE(sub_serv_itext)
    DEF_CASE(sub_serv_iframe)
    DEF_CASE(sub_serv_content)
    DEF_CASE(sub_serv_result)
    DEF_CASE(sub_serv_index)
    DEF_CASE(sub_serv_index_redirect)
    DEF_CASE(sub_serv_form)
    DEF_CASE(sub_serv_xform)
    DEF_CASE(sub_serv_query)
    DEF_CASE(sub_serv_instant_query)
    DEF_CASE(sub_serv_soap_builder)
    DEF_CASE(sub_serv_wsdl)
    DEF_CASE(sub_serv_xsd)
    DEF_CASE(sub_serv_config)
    DEF_CASE(sub_serv_getversion)
    DEF_CASE(sub_serv_reqsamplexml)
    DEF_CASE(sub_serv_respsamplexml)
    DEF_CASE(sub_serv_respsamplejson)
    DEF_CASE(sub_serv_file_upload)

    default: return "invalid-type";
    }
} 

static bool authenticateOptionalFailed(IEspContext& ctx, IEspHttpBinding* binding)
{
#ifdef ENABLE_NEW_SECURITY
    if (ctx.queryRequestParameters()->hasProp("internal"))
    {
        ISecUser* user = ctx.queryUser();
        if(!user || user->getStatus()==SecUserStatus_Inhouse || user->getStatus()==SecUserStatus_Unknown)
            return false;

        OERRLOG("User %s trying to access unauthorized feature: internal", user->getName() ? user->getName() : ctx.queryUserId());
        return true;
    }
    // TODO: handle binding specific optionals
#elif !defined(DISABLE_NEW_SECURITY)
#error Please include esphttp.hpp in this file.
#endif

    return false;
}

static bool checkHttpPathStaysWithinBounds(const char *path)
{
    if (isEmptyString(path))
        return true;
    //The path that follows /esp/files should be relative, not absolute - reject immediately if it is.
    if (isAbsolutePath(path))
        return false;
    int depth = 0;
    StringArray nodes;
    nodes.appendList(path, "/");
    ForEachItemIn(i, nodes)
    {
        const char *node = nodes.item(i);
        if (!*node || streq(node, ".")) //empty or "." doesn't advance
            continue;
        if (!streq(node, ".."))
            depth++;
        else
        {
            depth--;
            if (depth<0)  //only really care that the relative http path doesn't position itself above its own root node
                return false;
        }
    }
    return true;
}

EspHttpBinding* CEspHttpServer::getBinding()
{
    EspHttpBinding* thebinding=NULL;
    int ordinality=m_apport->getBindingCount();
    if (ordinality==1)
    {
        CEspBindingEntry *entry = m_apport->queryBindingItem(0);
        thebinding = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : NULL;
    }
    else if (m_defaultBinding)
        thebinding=dynamic_cast<EspHttpBinding*>(m_defaultBinding.get());
    return thebinding;
}

void checkSetCORSAllowOrigin(EspHttpBinding *binding, CHttpRequest *req, CHttpResponse *resp)
{
    StringBuffer origin;
    req->getHeader("Origin", origin);
    if (origin.length())
    {
        const IEspCorsAllowedOrigin *corsAllowed = binding->findCorsAllowedOrigin(origin, req->queryMethod());
        if (corsAllowed)
        {
            resp->setHeader("Access-Control-Allow-Origin", origin);
            resp->setHeader("Access-Control-Allow-Credentials", "true");
        }
    }
}

void CEspHttpServer::traceRequest(IEspContext* ctx, const char* normalizeMethod)
{
    // General Notice: Span attributes added here are recorded as shown. Certain backend collectors
    // may transform the names seen here. For example, `client.address` may become
    // `labels.client_address` on the backend. Not all attributes are affected by this.
    if (!queryTraceManager().isTracingEnabled())
        return;
    ISpan* span = queryThreadedActiveSpan();
    if (!span->isRecording())
        return;
    StringBuffer peer;
    span->setSpanAttribute("client.address", m_request->getPeer(peer));
    span->setSpanAttribute("http.request.method", normalizeMethod);

    // Create a facsimile of the original URL, excluding potentially sensitive information. Certain
    // backend collectors expect `url.full` to be recorded, and only display other URL attributes
    // if extracted from `url.full`.
    const char* scheme = (isSSL ? "https" : "http");
    const char* domain = m_request->queryHost();
    int         port = m_request->getPort();
    const char* path = m_request->queryPath();
    StringBuffer query, full;
    if (m_request->getParameterCount())
    {
        // Parameters in this set will be ignored. Generally limited to ESP-created parameters
        // created before reaching this point.
        static const std::set<std::string> ignoredParams{"__querystring"};
        // Parameters in this set will be included in the full URL's query fragment, with values
        // intact. All other parameters will be redacted, meaning excluded from the query fragment
        // and their names will be listed in the conditional `url.query.redacted` attribute. When
        // redaction occurs, a `REDACTED` parameter will be appended to the query fragment with the
        // number of redacted parameters.
        static const std::set<std::string> visibleParams{
            "all_annot_", "config_", "form_", "json_builder_", "no_annot_", "no_ns_", "rawxml_",
            "reqjson_", "reqxml_", "respjson_", "respxml_", "soap_builder_", "roxie_builder_",
            "ver_", "form", "main", "wsdl", "wsdl_ext", "xsd", "internal"};
        StringBuffer redacted;
        unsigned redactedCount = 0;
        bool addRedacted = doTrace(TraceFlags::Always, traceDetailed);
        IProperties* parameters = m_request->queryParameters(); // will not be NULL
        Owned<IPropertyIterator> it = parameters->getIterator();
        ForEach(*it)
        {
            const char* name = it->getPropKey();
            if (isEmptyString(name) || ignoredParams.count(name))
                continue;
            if (!visibleParams.count(name))
            {
                redactedCount++;
                if (addRedacted)
                {
                    if (!redacted.isEmpty())
                        redacted.append("&");
                    redacted.append(name);
                }
            }
            else
            {
                if (!query.isEmpty())
                    query.append('&');
                query.append(name);
                const char* value = it->queryPropValue();
                if (isEmptyString(value))
                    continue; // a property originally given as `foo=` will be represented as `foo`
                query.append('=').append(value);
            }
        }
        if (redactedCount)
        {
            if (!query.isEmpty())
                query.append('&');
            query.append("REDACTED").append('=').append(redactedCount);
            if (addRedacted)
                span->setSpanAttribute("url.query.redacted", redacted);
        }
    }
    full.append(scheme).append("://").append(domain).append(':').append(port);
    if (!isEmptyString(path))
    {
        if (path[0] != '/')
            full.append('/');
        full.append(path);
    }
    if (!query.isEmpty())
        full.append('?').append(query);
    span->setSpanAttribute("url.full", full);
}

// Enumeration of "esp" service methods. These are generally requests for files or form markup, in
// other words, they are web service overhead not specific to any ESP instance. Add new values as
// additional requests become relevant.
enum class EspGetMethod
{
    None, // empty name
    Files,
    Xslt,
    Body,
    Frame,
    TitleBar,
    Nav,
    NavData,
    NavMenuEvent,
    SoapReq,
    // DO NOT MAP NAMES TO THE FOLLOWING VALUES:
    Unhandled, // catch-all for any method name not explicitly handled by processRequest
    NotApplicable, // request not associated with the "esp" service
};
struct EspGetMethodNameComparator
{
    bool operator()(const char* lhs, const char* rhs) const { return stricmp(lhs, rhs) < 0; }
};
using EspGetMethodMap = std::map<const char*, EspGetMethod, EspGetMethodNameComparator>;
// Association of method names to specific "esp" service requests. This mapping allows
// processRequest to decide if the request should be traced before processing the request, without
// repeating the method name string comparisons. Multiple names may map to the same request, and
// all redundant names (e.g., "files" and "files_") must be included in the map.
static const EspGetMethodMap getRequests{
    // esp
    {"", EspGetMethod::None},
    // esp/<key>
    {"files", EspGetMethod::Files},
    {"xslt", EspGetMethod::Xslt},
    {"body", EspGetMethod::Body},
    {"frame", EspGetMethod::Frame},
    {"titlebar", EspGetMethod::TitleBar},
    {"nav", EspGetMethod::Nav},
    {"navdata", EspGetMethod::NavData},
    {"navmenuevent", EspGetMethod::NavMenuEvent},
    {"soapreq", EspGetMethod::SoapReq},
    // same as above but with trailing underscore
    {"files_", EspGetMethod::Files},
    {"xslt_", EspGetMethod::Xslt},
    {"body_", EspGetMethod::Body},
    {"frame_", EspGetMethod::Frame},
    {"titlebar_", EspGetMethod::TitleBar},
    {"nav_", EspGetMethod::Nav},
    {"navdata_", EspGetMethod::NavData},
    {"navmenuevent_", EspGetMethod::NavMenuEvent},
    {"soapreq_", EspGetMethod::SoapReq},
};

int CEspHttpServer::processRequest()
{
    IEspContext* ctx = m_request->queryContext();
    StringBuffer errMessage;
    m_request->setPersistentEnabled(m_apport->queryProtocol()->persistentEnabled() && !shouldClose);
    m_response->setPersistentEnabled(m_apport->queryProtocol()->persistentEnabled() && !shouldClose);
    m_response->enableCompression();
    try
    {
        if (m_request->receive(NULL)==-1) // MORE - pass in IMultiException if we want to see exceptions (which are not fatal)
            return -1;
    }
    catch(IEspHttpException* e)
    {
        m_response->sendException(e);
        ctx->addTraceSummaryValue(LogMin, "msg", e->errorMessage(errMessage).str(), TXSUMMARY_GRP_ENTERPRISE);
        e->Release();
        return 0;
    }
    catch (IException *e)
    {
        DBGLOG(e);
        sendInternalError(true);
        ctx->addTraceSummaryValue(LogMin, "msg", e->errorMessage(errMessage).str(), TXSUMMARY_GRP_ENTERPRISE);
        e->Release();
        return 0;
    }
    catch (...)
    {
        IERRLOG("Unknown Exception - reading request [CEspHttpServer::processRequest()]");
        sendInternalError(false);
        ctx->addTraceSummaryValue(LogMin, "msg", "Unknown Exception - reading request [CEspHttpServer::processRequest()]", TXSUMMARY_GRP_ENTERPRISE);
        return 0;
    }

    m_response->setPersistentEligible(m_request->getPersistentEligible());

    try
    {
        EspHttpBinding* thebinding = nullptr;
        Owned<IInterface> theBindingHolder; //hold on to the binding in case it gets released in the middle of processing a request
        
        StringBuffer method;
        m_request->getMethod(method);

        EspAuthState authState=authUnknown;
        sub_service stype=sub_serv_unknown;
        
        StringBuffer pathEx;
        StringBuffer serviceName;
        StringBuffer methodName;
        m_request->getEspPathInfo(stype, &pathEx, &serviceName, &methodName, false);
        ESPLOG(LogNormal,"sub service type: %s. parm: %s", getSubServiceDesc(stype), m_request->queryParamStr());

        // getEspPathInfo provides all information needed to decide if the request should be
        // traced. Create a server span, if needed, before proceding with request processing
        // so maximize the amount of request processing that can be traced. Specifically, user
        // authentication and authorization may generate trace output.
        bool wantTracing = queryTraceManager().isTracingEnabled();
        EspGetMethod espGetMethod = EspGetMethod::NotApplicable;
        if (streq(method, GET_METHOD))
        {
            if (sub_serv_root == stype)
                wantTracing = false;
            else if (strieq(serviceName, "esp"))
            {
                // At this time, the presence of a method name in the get request map is sufficient
                // to suppress trace output. The mapped value will be used later.
                EspGetMethodMap::const_iterator it = getRequests.find(methodName);
                if (it != getRequests.end())
                {
                    espGetMethod = it->second;
                    wantTracing = false;
                }
                else
                    espGetMethod = EspGetMethod::Unhandled;
            }
        }
        Owned<ISpan> serverSpan;
        if (wantTracing)
        {
            // The context will be destroyed when this request is destroyed. So initialise a
            // SpanScope in the context to ensure the span is also terminated at the same time.
            serverSpan.setown(m_request->createServerSpan(serviceName, methodName));
            ctx->setRequestSpan(serverSpan);
        }
        else
            serverSpan.setown(getNullSpan());
        ActiveSpanScope serverSpanScope(serverSpan);

        m_request->updateContext();
        ctx->setServiceName(serviceName.str());
        ctx->setHTTPMethod(method.str());
        ctx->setServiceMethod(methodName.str());
        ctx->addTraceSummaryValue(LogMin, "app.protocol", method.str(), TXSUMMARY_GRP_ENTERPRISE);
        ctx->addTraceSummaryValue(LogMin, "app.service", serviceName.str(), TXSUMMARY_GRP_ENTERPRISE);
        StringBuffer contentType;
        m_request->getContentType(contentType);
        ctx->addTraceSummaryValue(LogMin, "custom_fields.conttype", contentType.str(), TXSUMMARY_GRP_ENTERPRISE);
        StringBuffer userAgent;
        m_request->getHeader("User-Agent", userAgent);
        ctx->addTraceSummaryValue(LogMin, "custom_fields.agent", userAgent.str(), TXSUMMARY_GRP_ENTERPRISE);
        StringBuffer url;
        if(m_request->queryPath())
        {
            url.append(m_request->queryPath());
            if(m_request->queryParamStr())
                url.appendf("?%s", m_request->queryParamStr());
        }
        ctx->addTraceSummaryValue(LogMin, "custom_fields.URL", url.str(), TXSUMMARY_GRP_ENTERPRISE);

        m_response->setHeader(kGlobalIdHttpHeaderName, ctx->getGlobalId());

        if(strieq(method.str(), OPTIONS_METHOD))
            return onOptions();

        StringBuffer peerStr, pathStr;
        const char *userid=ctx->queryUserId();
        if (doTrace(traceHttp))
        {
            if (isEmptyString(userid))
                ESPLOG(LogMin, "%s %s, from %s", method.str(), m_request->getPath(pathStr).str(), m_request->getPeer(peerStr).str());
            else //user ID is in HTTP header
                ESPLOG(LogMin, "%s %s, from %s@%s", method.str(), m_request->getPath(pathStr).str(), userid, m_request->getPeer(peerStr).str());
        }
        authState = checkUserAuth();
        if ((authState == authTaskDone) || (authState == authFailed))
            return 0;

        // Set Content-Security-Policy header here to prevent frame injection
        // before any of the handler functions are called that could return a
        // page with a parameterized frame, such as legacy ECLWatch pages
        // returned as files. Restrict src to the current host with a wildcard
        // port to allow ws_ecl to be embedded in a frame when it is bound to
        // a different port.
        StringBuffer host;
        m_request->getHost(host);
        VStringBuffer frameSrc("frame-src %s:*", host.str());
        m_response->setHeader("Content-Security-Policy", frameSrc.str());

        if (!stricmp(method.str(), GET_METHOD))
        {
            if (stype==sub_serv_root)
            {
                return onGetApplicationFrame(m_request.get(), m_response.get(), ctx);
            }

            // Use the previously identified method selector to dispatch the request.
            switch (espGetMethod)
            {
            case EspGetMethod::None:
                return 0;
            case EspGetMethod::Files:
                if (!getTxSummaryResourceReq())
                    ctx->cancelTxSummary();
                checkInitEclIdeResponse(m_request, m_response);
                return onGetFile(m_request.get(), m_response.get(), pathEx.str());
            case EspGetMethod::Xslt:
                if (!getTxSummaryResourceReq())
                    ctx->cancelTxSummary();
                return onGetXslt(m_request.get(), m_response.get(), pathEx.str());
            case EspGetMethod::Body:
                return onGetMainWindow(m_request.get(), m_response.get());
            case EspGetMethod::Frame:
                return onGetApplicationFrame(m_request.get(), m_response.get(), ctx);
            case EspGetMethod::TitleBar:
                return onGetTitleBar(m_request.get(), m_response.get());
            case EspGetMethod::Nav:
                return onGetNavWindow(m_request.get(), m_response.get());
            case EspGetMethod::NavData:
                return onGetDynNavData(m_request.get(), m_response.get());
            case EspGetMethod::NavMenuEvent:
                return onGetNavEvent(m_request.get(), m_response.get());
            case EspGetMethod::SoapReq:
                return onGetBuildSoapRequest(m_request.get(), m_response.get());
            case EspGetMethod::Unhandled:
            case EspGetMethod::NotApplicable:
                break;
            default:
                IERRLOG("unexpected EspGetMethod value: %d", (int)espGetMethod);
                break;
            }
        }

        int ordinality=m_apport->getBindingCount();
        bool isSubService = false;
        EspHttpBinding* exactBinding = nullptr;
        bool exactIsSubService = false;
        if (ordinality>0)
        {
            if (ordinality==1)
            {
                CEspBindingEntry *entry = m_apport->queryBindingItem(0);
                thebinding = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : NULL;

                bool isSoapPost=(strieq(method.str(), POST_METHOD) && m_request->isSoapMessage());
                if (thebinding && !isSoapPost && !thebinding->isValidServiceName(*ctx, serviceName.str()))
                    thebinding=NULL;
            }
            else
            {
                EspHttpBinding* lbind=NULL;
                for (int index=0; !exactBinding && index<ordinality; index++)
                {
                    CEspBindingEntry *entry = m_apport->queryBindingItem(index);
                    lbind = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : NULL;
                    if (lbind)
                    {
                        if (lbind->isValidServiceName(*ctx, serviceName.str()))
                        {
                            if (!thebinding)
                            {
                                thebinding=lbind;
                                StringBuffer bindSvcName;
                                if (!strieq(serviceName, lbind->getServiceName(bindSvcName)))
                                    isSubService = true;
                            }
                            if (methodName.length() != 0 && lbind->isMethodInService(*ctx, serviceName.str(), methodName.str()))
                            {
                                exactBinding = lbind;
                                StringBuffer bindSvcName;
                                if (!strieq(serviceName, lbind->getServiceName(bindSvcName)))
                                    exactIsSubService = true;
                            }
                        }
                    }
                }
            }
            if (exactBinding)
            {
                thebinding = exactBinding;
                isSubService = exactIsSubService;
            }
            if (!thebinding && m_defaultBinding)
                thebinding=dynamic_cast<EspHttpBinding*>(m_defaultBinding.get());
        }

        if (thebinding)
            theBindingHolder.set(dynamic_cast<IInterface*>(thebinding));

        checkSetCORSAllowOrigin(thebinding, m_request, m_response);

        if (thebinding && thebinding->isUnrestrictedSSType(stype))
        {
            //Avoid creating a span for unrestrictedSSType requests
            thebinding->onGetUnrestricted(m_request.get(), m_response.get(), serviceName.str(), methodName.str(), stype);
            ctx->addTraceSummaryTimeStamp(LogMin, "handleHttp");
            return 0;
        }

        if (thebinding!=NULL)
        {
            if(stricmp(method.str(), POST_METHOD)==0)
            {
                traceRequest(ctx, POST_METHOD);
                thebinding->handleHttpPost(m_request.get(), m_response.get());
            }
            else if(!stricmp(method.str(), GET_METHOD))
            {
                traceRequest(ctx, GET_METHOD);
                if (stype==sub_serv_index_redirect)
                {
                    StringBuffer url;
                    if (isSubService)
                    {
                        StringBuffer qSvcName;
                        thebinding->qualifySubServiceName(*ctx,serviceName,NULL, qSvcName, NULL);
                        url.append(qSvcName);
                    }
                    else
                        thebinding->getServiceName(url);
                    url.append('/');
                    const char* parms = m_request->queryParamStr();
                    if (parms && *parms)
                        url.append('?').append(parms);
                    m_response->redirect(*m_request.get(),url);
                }
                else
                {
                    if (strieq(methodName.str(), "files_") && !checkHttpPathStaysWithinBounds(pathEx))
                    {
                        AERRLOG("Get File %s: attempted access outside of %sfiles/", pathEx.str(), getCFD());
                        m_response->setStatus(HTTP_STATUS_NOT_FOUND);
                        m_response->send();
                        return 0;
                    }
                    thebinding->onGet(m_request.get(), m_response.get());
                }
            }
            else
                unsupported(method);
        }
        else
        {
            serverSpan->setSpanAttribute("http.unbound_target", 1);
            if(!stricmp(method.str(), POST_METHOD))
            {
                traceRequest(ctx, POST_METHOD);
                onPost();
            }
            else if(!stricmp(method.str(), GET_METHOD))
            {
                traceRequest(ctx, GET_METHOD);
                onGet();
            }
            else
                unsupported(method);
        }
        ctx->addTraceSummaryTimeStamp(LogMin, "handleHttp");
    }
    catch(IEspHttpException* e)
    {
        m_response->sendException(e);
        ctx->addTraceSummaryValue(LogMin, "msg", e->errorMessage(errMessage).str(), TXSUMMARY_GRP_ENTERPRISE);
        VStringBuffer fault("F%d", e->errorCode());
        ctx->addTraceSummaryValue(LogMin, "custom_fields.soapFaultCode", fault.str(), TXSUMMARY_GRP_ENTERPRISE);
        e->Release();
        return 0;
    }
    catch (IException *e)
    {
        DBGLOG(e);
        sendInternalError(true);
        ctx->addTraceSummaryValue(LogMin, "msg", e->errorMessage(errMessage).str(), TXSUMMARY_GRP_ENTERPRISE);
        VStringBuffer fault("F%d", e->errorCode());
        ctx->addTraceSummaryValue(LogMin, "custom_fields.soapFaultCode", fault.str(), TXSUMMARY_GRP_ENTERPRISE);
        e->Release();
        return 0;
    }
    catch (...)
    {
        StringBuffer content_type;
        __int64 len = m_request->getContentLength();
        UWARNLOG("Unknown Exception - processing request");
        UWARNLOG("METHOD: %s, PATH: %s, TYPE: %s, CONTENT-LENGTH: %" I64F "d", m_request->queryMethod(), m_request->queryPath(), m_request->getContentType(content_type).str(), len);
        if (len > 0)
            m_request->logMessage(LOGCONTENT, "HTTP request content received:\n");
        sendInternalError(false);
        ctx->addTraceSummaryValue(LogMin, "msg", "Unknown exception caught in CEspHttpServer::processRequest", TXSUMMARY_GRP_ENTERPRISE);
        return 0;
    }

    return 0;
}

int CEspHttpServer::onGetApplicationFrame(CHttpRequest* request, CHttpResponse* response, IEspContext* ctx)
{
    time_t modtime = 0;

    IProperties *params = request->queryParameters();
    const char *inner=(params)?params->queryProp("inner") : NULL;
    StringBuffer ifmodifiedsince;
    request->getHeader("If-Modified-Since", ifmodifiedsince);

    if (inner&&*inner&&ifmodifiedsince.length())
    {
        response->setStatus(HTTP_STATUS_NOT_MODIFIED);
        response->send();
    }
    else
    {
        CEspBindingEntry* entry = m_apport->getDefaultBinding();
        if(entry)
        {
            EspHttpBinding *httpbind = dynamic_cast<EspHttpBinding *>(entry->queryBinding());
            if(httpbind)
            {
                const char *page = httpbind->getRootPage(ctx);
                if(page && *page)
                    return onGetFile(request, response, page);
            }
        }
        StringBuffer html;
        m_apport->getAppFrameHtml(modtime, inner, html, ctx);
        response->setContent(html.length(), html.str());
        response->setContentType("text/html; charset=UTF-8");
        response->setStatus(HTTP_STATUS_OK);

        char timestr[128];
#ifdef _WIN32
        ctime_s(timestr, 128, &modtime);
#else
        ctime_r(&modtime, timestr);
#endif
        int timelen = strlen(timestr);
        if (timelen > 0 && timestr[timelen -1] == '\n')
            timestr[timelen - 1] = '\0';
        response->addHeader("Last-Modified", timestr);
        response->send();
    }

    return 0;
}

int CEspHttpServer::onGetTitleBar(CHttpRequest* request, CHttpResponse* response)
{
    bool rawXml = request->queryParameters()->hasProp("rawxml_");
    StringBuffer m_headerHtml(m_apport->getTitleBarHtml(*request->queryContext(), rawXml));
    response->setContent(m_headerHtml.length(), m_headerHtml.str());
    response->setContentType(rawXml ? HTTP_TYPE_APPLICATION_XML_UTF8 : "text/html; charset=UTF-8");
    response->setStatus(HTTP_STATUS_OK);
    response->send();
    return 0;
}

int CEspHttpServer::onGetNavWindow(CHttpRequest* request, CHttpResponse* response)
{
    StringBuffer navContent;
    StringBuffer navContentType;
    m_apport->getNavBarContent(*request->queryContext(), navContent, navContentType, request->queryParameters()->hasProp("rawxml_"));
    response->setContent(navContent.length(), navContent.str());
    response->setContentType(navContentType.str());
    response->setStatus(HTTP_STATUS_OK);
    response->send();
    return 0;
}

int CEspHttpServer::onGetDynNavData(CHttpRequest* request, CHttpResponse* response)
{
    StringBuffer navContent;
    StringBuffer navContentType;
    bool         bVolatile;
    m_apport->getDynNavData(*request->queryContext(), request->queryParameters(), navContent, navContentType, bVolatile);
    if (bVolatile)
        response->addHeader("Cache-control",  "max-age=0");
    response->setContent(navContent.length(), navContent.str());
    response->setContentType(navContentType.str());
    response->setStatus(HTTP_STATUS_OK);
    response->send();
    return 0;
}

int CEspHttpServer::onGetNavEvent(CHttpRequest* request, CHttpResponse* response)
{
    m_apport->onGetNavEvent(*request->queryContext(), request, response);
    return 0;
}

int CEspHttpServer::onGetBuildSoapRequest(CHttpRequest* request, CHttpResponse* response)
{
    m_apport->onBuildSoapRequest(*request->queryContext(), request, response);
    return 0;
}

#ifdef _USE_OPENLDAP
int CEspHttpServer::onUpdatePasswordInput(CHttpRequest* request, CHttpResponse* response)
{
    StringBuffer html;
    m_apport->onUpdatePasswordInput(*request->queryContext(), html);

    response->setContent(html.length(), html.str());
    response->setContentType("text/html; charset=UTF-8");
    response->setStatus(HTTP_STATUS_OK);

    response->send();

    return 0;
}

int CEspHttpServer::onUpdatePassword(CHttpRequest* request, CHttpResponse* response)
{
    StringBuffer html;
    IEspContext* context = request->queryContext();
    unsigned returnCode = m_apport->onUpdatePassword(*context, request, html);
    if (returnCode == 0)
    {
        EspHttpBinding* binding = getBinding();
        StringBuffer authorizationHeader;
        request->getHeader("Authorization", authorizationHeader);
        //If the request has the "Authorization" header, the request is from the AuthPerRequest type.
        if (binding && authorizationHeader.isEmpty())
        {
            AuthType domainAuthType = binding->getDomainAuthType();
            if ((domainAuthType == AuthPerSessionOnly) || (domainAuthType == AuthTypeMixed))
            {//A session can only be set for those 2 auth types.
                StringBuffer urlCookie;
                readCookie(SESSION_START_URL_COOKIE, urlCookie);
                unsigned sessionID = createHTTPSession(context, binding, request->getParameters()->queryProp("username"), urlCookie.isEmpty() ? "/" : urlCookie.str());
                m_request->queryContext()->setSessionToken(sessionID);
                VStringBuffer cookieStr("%u", sessionID);
                addCookie(binding->querySessionIDCookieName(), cookieStr.str(), 0, true);
                addCookie(SESSION_AUTH_OK_COOKIE, "true", 0, false); //client can access this cookie.
                cookieStr.setf("%u", binding->getClientSessionTimeoutSeconds());
                addCookie(SESSION_TIMEOUT_COOKIE, cookieStr.str(), 0, false);
                clearCookie(SESSION_START_URL_COOKIE);
            }
        }
    }
    response->setContent(html.length(), html.str());
    response->setContentType("text/html; charset=UTF-8");
    response->setStatus(HTTP_STATUS_OK);

    response->send();
    return 0;
}
#endif

int CEspHttpServer::onGetMainWindow(CHttpRequest* request, CHttpResponse* response)
{
    StringBuffer url("../?main");
    double ver = request->queryContext()->getClientVersion();
    if (ver>0)
        url.appendf("&ver_=%g", ver);
    response->redirect(*request, url);
    return 0;
}

inline void make_env_var(StringArray &env, StringBuffer &var, const char *name, const char *value)
{
    env.append(var.clear().append(name).append('=').append(value).str());
}

inline void make_env_var(StringArray &env, StringBuffer &var, const char *name, const StringBuffer &value)
{
    env.append(var.clear().append(name).append('=').append(value).str());
}

inline void make_env_var(StringArray &env, StringBuffer &var, const char *name, __int64 value)
{
    env.append(var.clear().append(name).append('=').append(value).str());
}

bool skipHeader(const char *name)
{
    if (!stricmp(name, "CONTENT_LENGTH"))
        return true;
    else if (!strcmp(name, "AUTHORIZATION"))
        return true;
    else if (!strcmp(name, "CONTENT_TYPE"))
        return true;
    return false;
}

static void httpGetFile(CHttpRequest* request, CHttpResponse* response, const char *urlpath, const char *filepath)
{
    StringBuffer mimetype, etag, lastModified;
    MemoryBuffer content;
    bool modified = true;
    request->getHeader("If-None-Match", etag);
    request->getHeader("If-Modified-Since", lastModified);

    if (httpContentFromFile(filepath, mimetype, content, modified, lastModified, etag))
    {
        response->CheckModifiedHTTPContent(modified, lastModified.str(), etag.str(), mimetype.str(), content);
    }
    else
    {
        DBGLOG("Get File %s: file not found", filepath);
        response->setStatus(HTTP_STATUS_NOT_FOUND);
    }
    response->send();
}

static void httpGetDirectory(CHttpRequest* request, CHttpResponse* response, const char *urlpath, const char *dirpath, bool top, const StringBuffer &tail)
{
    Owned<IPropertyTree> tree = createPTree("directory", ipt_none);
    tree->setProp("@path", urlpath);
    Owned<IDirectoryIterator> dir = createDirectoryIterator(dirpath, NULL);
    ForEach(*dir)
    {
        IPropertyTree *entry = tree->addPropTree(dir->isDir() ? "directory" : "file", createPTree(ipt_none));
        StringBuffer s;
        entry->setProp("name", dir->getName(s));
        if (!dir->isDir())
            entry->setPropInt64("size", dir->getFileSize());
        CDateTime cdt;
        dir->getModifiedTime(cdt);
        entry->setProp("modified", cdt.getString(s.clear(), false));
    }

    const char *fmt = request->queryParameters()->queryProp("format");
    StringBuffer out;
    StringBuffer contentType;
    if (!fmt || strieq(fmt,"html"))
    {
        contentType.set("text/html");
        out.append("<!DOCTYPE html><html><body>");
        if (!top)
            out.appendf("<a href='%s'>..</a><br/>", tail.length() ? "." : "..");

        Owned<IPropertyTreeIterator> it = tree->getElements("*");
        ForEach(*it)
        {
            IPropertyTree &e = it->query();
            const char *href=e.queryProp("name");
            if (tail.length())
                out.appendf("<a href='%s/%s'>%s</a><br/>", tail.str(), href, href);
            else
                out.appendf("<a href='%s'>%s</a><br/>", href, href);
        }
        out.append("</body></html>");
    }
    else if (strieq(fmt, "json"))
    {
        contentType.set("application/json");
        toJSON(tree, out);
    }
    else if (strieq(fmt, "xml"))
    {
        contentType.set("application/xml");
        toXML(tree, out);
    }
    response->setStatus(HTTP_STATUS_OK);
    response->setContentType(contentType);
    response->setContent(out);
    response->send();
}

int CEspHttpServer::onGetFile(CHttpRequest* request, CHttpResponse* response, const char *urlpath)
{
        if (!request || !response || !urlpath)
            return -1;

        StringBuffer basedir(getCFD());
        basedir.append("files/");

        if (!checkHttpPathStaysWithinBounds(urlpath))
        {
            AERRLOG("Get File %s: attempted access outside of %s", urlpath, basedir.str());
            response->setStatus(HTTP_STATUS_NOT_FOUND);
            response->send();
            return 0;
        }

        StringBuffer ext;
        StringBuffer tail;
        splitFilename(urlpath, NULL, NULL, &tail, &ext);

        bool top = !urlpath || !*urlpath;
        StringBuffer httpPath;
        request->getPath(httpPath).str();
        if (httpPath.charAt(httpPath.length()-1)=='/')
            tail.clear();
        else if (top)
            tail.set("./files");

        StringBuffer fullpath;
        makeAbsolutePath(urlpath, basedir.str(), fullpath);
        if (!checkFileExists(fullpath) && !checkFileExists(fullpath.toUpperCase()) && !checkFileExists(fullpath.toLowerCase()))
        {
            DBGLOG("Get File %s: file not found", urlpath);
            response->setStatus(HTTP_STATUS_NOT_FOUND);
            response->send();
            return 0;
        }

        if (isDirectory(fullpath))
            httpGetDirectory(request, response, urlpath, fullpath, top, tail);
        else
            httpGetFile(request, response, urlpath, fullpath);
        return 0;
}

int CEspHttpServer::onGetXslt(CHttpRequest* request, CHttpResponse* response, const char *path)
{
        if (!request || !response || !path)
            return -1;
        
        if (!checkHttpPathStaysWithinBounds(path))
        {
            AERRLOG("Get File %s: attempted access outside of %sxslt/", path, getCFD());
            response->setStatus(HTTP_STATUS_NOT_FOUND);
            response->send();
            return 0;
        }

        StringBuffer mimetype, etag, lastModified;
        MemoryBuffer content;
        bool modified = true;
        request->getHeader("If-None-Match", etag);
        request->getHeader("If-Modified-Since", lastModified);

        VStringBuffer filepath("%ssmc_xslt/%s", getCFD(), path);
        if (httpContentFromFile(filepath.str(), mimetype, content, modified, lastModified.clear(), etag) ||
            httpContentFromFile(filepath.clear().append(getCFD()).append("xslt/").append(path).str(), mimetype, content, modified, lastModified.clear(), etag))
        {
            response->CheckModifiedHTTPContent(modified, lastModified.str(), etag.str(), mimetype.str(), content);
        }
        else
        {
            DBGLOG("Get XSLT %s: file not found", filepath.str());
            response->setStatus(HTTP_STATUS_NOT_FOUND);
        }

        response->send();
        return 0;
}


int CEspHttpServer::unsupported(StringBuffer& httpRequestMethod)
{

    // Per https://opentelemetry.io/docs/specs/semconv/attributes-registry/http/, the value
    // of http.request.method must be "known to the instrumentation" and is case sensitive.
    // Accept any value, converted to upper case, for improved performance.
    queryThreadedActiveSpan()->setSpanAttribute("http.request.method", httpRequestMethod.toUpperCase().str());
    return unsupported();
}

int CEspHttpServer::unsupported()
{
    HtmlPage page("Enterprise Services Platform");
    StringBuffer espHeader;

    espHeader.append("<table border=\"0\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" bgcolor=\"#000000\" height=\"108\">");
    espHeader.append("<tr><td width=\"24%\" height=\"24\" bgcolor=\"#000000\"><img border=\"0\" src=\"esp/files_/logo.gif\" width=\"258\" height=\"108\" /></td></tr>");
    espHeader.append("<tr><td width=\"24%\" height=\"24\" bgcolor=\"#AA0000\"><p align=\"center\" /><b><font color=\"#FFFFFF\" size=\"5\">Enterprise Services Platform</font></b></td></tr>");
    espHeader.append("</table>");

    page.appendContent(new CHtmlText(espHeader.str()));

    page.appendContent(new CHtmlHeader(H1, "Unsupported http method"));

    StringBuffer content;
    page.getHtml(content);

    m_response->setVersion(HTTP_VERSION);
    m_response->setContent(content.length(), content.str());
    m_response->setContentType("text/html; charset=UTF-8");
    m_response->setStatus(HTTP_STATUS_OK);

    m_response->send();

    return 0;
}

int CEspHttpServer::onOptions()
{
    m_response->setVersion(HTTP_VERSION);
    m_response->setStatus(HTTP_STATUS_OK);

    StringBuffer origin;
    m_request->getHeader("Origin", origin);

    if (origin.length())
    {
        EspHttpBinding *binding = getBinding();
        if (binding)
        {
            StringBuffer allowMethod;
            m_request->getHeader("Access-Control-Request-Method", allowMethod);

            const IEspCorsAllowedOrigin *corsAllowed = binding->findCorsAllowedOrigin(origin, allowMethod);
            if (corsAllowed)
            {
                m_response->setHeader("Access-Control-Allow-Origin", origin);
                m_response->setHeader("Access-Control-Allow-Credentials", "true");
                m_response->setHeader("Access-Control-Allow-Methods", corsAllowed->queryAllowedMethodsCSV());
                m_response->setHeader("Access-Control-Max-Age", corsAllowed->queryMaxAge());

                StringBuffer requestedAllowHeaders;
                m_request->getHeader("Access-Control-Request-Headers", requestedAllowHeaders);
                if (requestedAllowHeaders.length())
                {
                    StringBuffer allowedHeaders;
                    corsAllowed->getAllowedHeadersCSV(requestedAllowHeaders, allowedHeaders);
                    if (allowedHeaders.length())
                        m_response->setHeader("Access-Control-Allow-Headers", allowedHeaders);
                }
            }
        }
    }

    m_response->setContentType("text/plain");
    m_response->setContent("");

    m_response->send();

    return 0;
}

int CEspHttpServer::onPost()
{
    HtmlPage page("Enterprise Services Platform");
    StringBuffer espHeader;

    espHeader.append("<table border=\"0\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" bgcolor=\"#000000\" height=\"108\">");
    espHeader.append("<tr><td width=\"24%\" height=\"24\" bgcolor=\"#000000\"><img border=\"0\" src=\"esp/files_/logo.gif\" width=\"258\" height=\"108\" /></td></tr>");
    espHeader.append("<tr><td width=\"24%\" height=\"24\" bgcolor=\"#AA0000\"><p align=\"center\" /><b><font color=\"#FFFFFF\" size=\"5\">Enterprise Services Platform</font></b></td></tr>");
    espHeader.append("</table>");

    page.appendContent(new CHtmlText(espHeader.str()));

    page.appendContent(new CHtmlHeader(H1, "Invalid POST"));

    StringBuffer content;
    page.getHtml(content);

    m_response->setVersion(HTTP_VERSION);
    m_response->setContent(content.length(), content.str());
    m_response->setContentType("text/html; charset=UTF-8");
    m_response->setStatus(HTTP_STATUS_OK);

    m_response->send();

    return 0;
}

int CEspHttpServer::onGet()
{   
    if (m_request && m_request->queryParameters()->hasProp("config_") && m_viewConfig)
    {
        if (getESPContainer() && getESPContainer()->queryApplicationConfig())
        {
            StringBuffer content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
            if (m_request->queryParameters() && m_request->queryParameters()->hasProp("display"))
                content.append("<?xml-stylesheet type=\"text/xsl\" href=\"/esp/xslt/xmlformatter.xsl\"?>");
            toXML(getESPContainer()->queryApplicationConfig(), content);
            m_response->setContentType("application/xml");
            m_response->setContent(content);
            m_response->setVersion(HTTP_VERSION);
            m_response->send();
        }
        else
        {
            StringBuffer mimetype, etag, lastModified;
            MemoryBuffer content;
            bool modified = true;
            m_request->getHeader("If-None-Match", etag);
            m_request->getHeader("If-Modified-Since", lastModified);
            httpContentFromFile("esp.xml", mimetype, content, modified, lastModified, etag);
            m_response->CheckModifiedHTTPContent(modified, lastModified.str(), etag.str(), HTTP_TYPE_APPLICATION_XML_UTF8, content);
            m_response->setVersion(HTTP_VERSION);
            m_response->send();
        }
    }
    else
    {
        HtmlPage page("Enterprise Services Platform");
        page.appendContent(new CHtmlHeader(H1, "Available Services:"));

        CHtmlList * list = (CHtmlList *)page.appendContent(new CHtmlList);
        EspHttpBinding* lbind=NULL;
        int ordinality=m_apport->getBindingCount();

        double ver = m_request->queryContext()->getClientVersion();
        for(int index=0; index<ordinality; index++)
        {
            CEspBindingEntry *entry = m_apport->queryBindingItem(index);
            lbind = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : NULL;
            if (lbind)
            {
                StringBuffer srv, srvLink;
                lbind->getServiceName(srv);
                srvLink.appendf("/%s", srv.str());
                if (ver)
                    srvLink.appendf("?ver_=%g", ver);
                
                list->appendContent(new CHtmlLink(srv.str(), srvLink.str()));
            }
        }

        StringBuffer content;
        page.getHtml(content);

        m_response->setVersion(HTTP_VERSION);
        m_response->setContent(content.length(), content.str());
        m_response->setContentType("text/html; charset=UTF-8");
        m_response->setStatus(HTTP_STATUS_OK);

        m_response->send();
    }

    return 0;
}

EspAuthState CEspHttpServer::checkUserAuth()
{
    EspAuthRequest authReq;
    readAuthRequest(authReq);
    if (authReq.httpPath.isEmpty())
        throw MakeStringException(-1, "URL query string cannot be empty.");

    if (!authReq.authBinding)
        throw MakeStringException(-1, "Cannot find ESP HTTP Binding");

    ESPLOG(LogMax, "checkUserAuth: %s %s", m_request->isSoapMessage() ? "SOAP" : "HTTP", authReq.httpMethod.isEmpty() ? "??" : authReq.httpMethod.str());

    //The preCheckAuth() does not return authUnknown when:
    //No authentication is required for the ESP binding;
    //Or no authentication is required for certain situations of not rootAuthRequired();
    //Or this is a user request for updating password.
    EspAuthState authState = preCheckAuth(authReq);
    if (authState != authUnknown)
        return authState;

    StringBuffer authorizationHeader, originHeader;
    m_request->getHeader("Authorization", authorizationHeader);
    m_request->getHeader("Origin", originHeader);

    StringBuffer servName(authReq.ctx->queryServiceName(nullptr));
    if (servName.isEmpty())
    {
        authReq.authBinding->getServiceName(servName);
        authReq.ctx->setServiceName(servName.str());
    }

    AuthType domainAuthType = authReq.authBinding->getDomainAuthType();
    authReq.ctx->setDomainAuthType(domainAuthType);
    if (domainAuthType != AuthPerRequestOnly)
    {//Try session based authentication now.
        EspAuthState authState = checkUserAuthPerSession(authReq, authorizationHeader);
        if (authState != authUnknown)
            return authState;
    }
    if (domainAuthType != AuthPerSessionOnly)
    {// BasicAuthentication or SOAP calls
        EspAuthState authState = checkUserAuthPerRequest(authReq);
        if (authState != authUnknown)
            return authState;
    }

    //HTTP authentication failed. Send out a login page or 401.
    //The following 3 rules are used to detect  a REST type call.
    //   Any HTTP POST request;
    //   Any request which has a BasicAuthentication header;
    //   Any CORS calls.
    authReq.ctx->setAuthStatus(AUTH_STATUS_FAIL);
    bool authSession = (domainAuthType == AuthPerSessionOnly) || ((domainAuthType == AuthTypeMixed) &&
        authorizationHeader.isEmpty() && !authReq.authBinding->isCORSRequest(originHeader.str()) &&
        !strieq(authReq.httpMethod.str(), POST_METHOD));
    return handleAuthFailed(authSession, authReq, false, nullptr);
}

//Read authentication related information into EspAuthRequest.
void CEspHttpServer::readAuthRequest(EspAuthRequest& req)
{
    StringBuffer pathEx;
    m_request->getEspPathInfo(req.stype, &pathEx, &req.serviceName, &req.methodName, false);
    m_request->getMethod(req.httpMethod);
    m_request->getPath(req.httpPath);//m_httpPath

    req.isSoapPost = (strieq(req.httpMethod.str(), POST_METHOD) && m_request->isSoapMessage());
    req.ctx = m_request->queryContext();
    req.authBinding = getEspHttpBinding(req);
    req.requestParams = m_request->queryParameters();
}

EspHttpBinding* CEspHttpServer::getEspHttpBinding(EspAuthRequest& authReq)
{
    if (strieq(authReq.httpMethod.str(), GET_METHOD) && ((authReq.stype == sub_serv_root)
            || (!authReq.serviceName.isEmpty() && strieq(authReq.serviceName.str(), "esp"))))
        return getBinding();

    if(!m_apport)
        return nullptr;

    int ordinality=m_apport->getBindingCount();
    if (ordinality < 1)
        return nullptr;

    EspHttpBinding* espHttpBinding = nullptr;
    if (ordinality==1)
    {
        CEspBindingEntry *entry = m_apport->queryBindingItem(0);
        espHttpBinding = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : NULL;
        //If there is only one binding on the port, we allow SOAP calls to work if they go
        //to http://IP:Port without any service name on the path. Even without specifying
        //the service, if the request matches a method, the method will run. So, the espHttpBinding
        //is set to nullptr only if !authReq.isSoapPost.
        if (!authReq.isSoapPost && espHttpBinding && !espHttpBinding->isValidServiceName(*authReq.ctx, authReq.serviceName.str()))
            espHttpBinding=nullptr;
    }
    else
    {
        for (unsigned index=0; index<(unsigned)ordinality; index++)
        {
            CEspBindingEntry *entry = m_apport->queryBindingItem(index);
            EspHttpBinding* lbind = (entry) ? dynamic_cast<EspHttpBinding*>(entry->queryBinding()) : nullptr;
            if (lbind && lbind->isValidServiceName(*authReq.ctx, authReq.serviceName.str()))
            {
                espHttpBinding=lbind;
                break;
            }
        }
    }

    if (!espHttpBinding && m_defaultBinding)
        espHttpBinding=dynamic_cast<EspHttpBinding*>(m_defaultBinding.get());

    return espHttpBinding;
}

void CEspHttpServer::readDomainAuthDataFromSecureContext(IEspContext* ctx, IPropertyTree* tree)
{
    StringBuffer authData;
    ctx->querySecureContext()->getDomainAuthData(authData);
    if (!authData.isEmpty())
        tree->setProp(PropSessionDomainAuthData, authData);
}

void CEspHttpServer::setDomainAuthDataInSecureContext(IEspContext* ctx, IPropertyTree* tree)
{
    StringAttr authData = tree->queryProp(PropSessionDomainAuthData);
    if (!authData.isEmpty())
        ctx->querySecureContext()->setDomainAuthData(authData.get());
}

//Store the userid and DomainAuthData to temporary cookies before a session is created.
void CEspHttpServer::addTempCookie(IEspContext* ctx)
{
    addCookie(SESSION_ID_TEMP_COOKIE, ctx->queryUserId(), 0, true);

    StringBuffer authData;
    ctx->querySecureContext()->getDomainAuthData(authData);
    if (!authData.isEmpty())
    {
        StringBuffer encodedAuthData;
        JBASE64_Encode(authData.str(), authData.length(), encodedAuthData, false);
        addCookie(SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE, encodedAuthData, 0, true);
    }
}

void CEspHttpServer::readTempCookieToContext(IEspContext* ctx, bool setUserIDAsName)
{
    StringBuffer userID, authData;
    readCookie(SESSION_ID_TEMP_COOKIE, userID);
    readCookie(SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE, authData);
    if (!userID.isEmpty())
    {
        ctx->setUserID(userID.str());
        if (setUserIDAsName)
        {
            ISecUser* user = ctx->queryUser();
            if (user)
                user->setName(userID.str());
        }
    }
    if (!authData.isEmpty())
    {
        StringBuffer decodedData;
        ctx->querySecureContext()->setDomainAuthData(JBASE64_Decode(authData, decodedData));
    }
}

EspAuthState CEspHttpServer::preCheckAuth(EspAuthRequest& authReq)
{
    if (!isAuthRequiredForBinding(authReq))
    {
        if (!authReq.httpMethod.isEmpty() && !authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty()
            && strieq(authReq.httpMethod.str(), GET_METHOD) && strieq(authReq.serviceName.str(), "esp") && strieq(authReq.methodName.str(), "getauthtype"))
        {
            if (authReq.authBinding->getDomainAuthType() == AuthUserNameOnly)
                sendGetAuthTypeResponse(authReq, AUTH_TYPE_USERNAMEONLY);
            else
                sendGetAuthTypeResponse(authReq, AUTH_TYPE_NONE);
            return authTaskDone;
        }

        unsigned sessionID = readCookie(authReq.authBinding->querySessionIDCookieName());
        if (sessionID > 0)
        {
            if (authReq.authBinding->getDomainAuthType() == AuthUserNameOnly)
            {
                clearCookie(authReq.authBinding->querySessionIDCookieName());
                clearCookie(SESSION_ID_TEMP_COOKIE);
                clearCookie(SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE);
                clearCookie(SESSION_TIMEOUT_COOKIE);
                clearCookie(USER_ACCT_ERROR_COOKIE);
            }
            else
                clearSessionCookies(authReq);

            if (!authReq.serviceName.isEmpty() && strieq(authReq.serviceName.str(), "esp"))
            {
                const char* method = authReq.methodName.str();
                if (!isEmptyString(method))
                {
                    if (strieq(method, "lock") || strieq(method, "unlock"))
                    {
                        VStringBuffer errMsg("Action not supported: %s", method);
                        sendLockResponse(strieq(method, "lock"), true, errMsg.str());
                        return authTaskDone;
                    }
                    else if (strieq(method, "login") || strieq(method, "logout") || (strnicmp(method, "updatepassword", 14) == 0))
                    {
                        VStringBuffer errMsg("Action not supported: %s", method);
                        sendMessage(errMsg.str(), "text/html; charset=UTF-8");
                        return authTaskDone;
                    }
                    else if (strieq(method, "verify") || strieq(method, "get_session_timeout") || strieq(method, "reset_session_timeout"))
                    {
                        VStringBuffer errMsg("Action not supported: %s", method);
                        ESPSerializationFormat respFormat = m_request->queryContext()->getResponseFormat();
                        sendMessage(errMsg.str(), (respFormat == ESPSerializationJSON) ? "application/json" : "text/xml");
                        return authTaskDone;
                    }
                }
            }
        }

        if (authReq.authBinding->getDomainAuthType() == AuthUserNameOnly)
            return handleUserNameOnlyMode(authReq);
        return authSucceeded;
    }

    if (!m_apport->rootAuthRequired() && strieq(authReq.httpMethod.str(), GET_METHOD) &&
        ((authReq.stype == sub_serv_root) || (!authReq.serviceName.isEmpty() && strieq(authReq.serviceName.str(), "esp"))))
        return authSucceeded;

    if (!authReq.httpMethod.isEmpty() && !authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty()
        && strieq(authReq.httpMethod.str(), GET_METHOD) && strieq(authReq.serviceName.str(), "esp"))
    {
        if (strieq(authReq.methodName.str(), "getauthtype"))
        {
            sendGetAuthTypeResponse(authReq, nullptr);
            return authTaskDone;
        }
        if (strieq(authReq.methodName.str(), "verify"))
            return verifyCookies(authReq);
    }
#ifdef _USE_OPENLDAP
    if (!authReq.httpMethod.isEmpty() && !authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty() && strieq(authReq.serviceName.str(), "esp"))
    {
        if (strieq(authReq.httpMethod.str(), POST_METHOD) && strieq(authReq.methodName.str(), "updatepassword"))
        {
            EspHttpBinding* thebinding = getBinding();
            if (thebinding)
                thebinding->populateRequest(m_request.get());
            readTempCookieToContext(authReq.ctx, true);
            onUpdatePassword(m_request.get(), m_response.get());
            return authTaskDone;
        }
        if (strieq(authReq.httpMethod.str(), GET_METHOD) && strieq(authReq.methodName.str(), "updatepasswordinput"))//process before authentication check
        {
            readTempCookieToContext(authReq.ctx, false);
            onUpdatePasswordInput(m_request.get(), m_response.get());
            return authTaskDone;
        }
    }
#endif

    return authUnknown;
}

EspAuthState CEspHttpServer::verifyCookies(EspAuthRequest& authReq)
{
    CIArrayOf<CESPCookieVerification> cookies;
    //Check whether the ESP request has specified which cookie to be verified or not.
    IProperties* parms = m_request->queryParameters();
    if (parms)
    {
        Owned<IPropertyIterator> iter = parms->getIterator();
        ForEach(*iter)
        {
            const char* name = iter->getPropKey();
            if (strieq(name, "__querystring"))
                continue; //skip the property created by ESP.

            cookies.append(*(new CESPCookieVerification(name, "")));
        }
    }

    //If the ESP request has not specified which cookie to be verified,
    //we will verify the cookies in HTTP Cookie header.
    if (cookies.ordinality() == 0)
    {
        IArrayOf<CEspCookie>& cookiesUsed = m_request->queryCookies();
        ForEachItemIn(i, cookiesUsed)
        {
            CEspCookie& r = cookiesUsed.item(i);
            cookies.append(*(new CESPCookieVerification(r.getName(), r.getValue())));
        }
    }

    ForEachItemIn(ii, cookies)
        verifyCookie(authReq, cookies.item(ii));

    sendVerifyCookieResponse(authReq, cookies);
    return authTaskDone;
}

void CEspHttpServer::verifyCookie(EspAuthRequest& authReq, CESPCookieVerification& cookie)
{
    const char* name = cookie.cookieName.get();
    if (strieq(name, "ESPSessionID") || strieq(name, authReq.authBinding->querySessionIDCookieName()))
    {
        if (verifyESPSessionIDCookie(authReq))
            cookie.valid.set("true");
    }
    else if (strieq(name, SESSION_AUTH_OK_COOKIE))
        verifyESPAuthenticatedCookie(authReq, cookie);
    else if (strieq(name, USER_NAME_COOKIE))
        verifyESPUserNameCookie(authReq, cookie);
    else if (strieq(name, SESSION_TIMEOUT_COOKIE) || strieq(name, SESSION_AUTH_MSG_COOKIE) || strieq(name, USER_ACCT_ERROR_COOKIE))
    {
        //SESSION_TIMEOUT_COOKIE: used to pass timeout settings to a client.
        //SESSION_AUTH_MSG_COOKIE: used to pass authentication message to a client.
        //USER_ACCT_ERROR_COOKIE: used to pass user account status to a client.
        //A client should clean it as soon as received. ESP always returns invalid if it is asked.
        cookie.verificationDetails.set("ESP cannot verify this cookie. It is one-time use only.");
    }
    else if (strieq(name, SESSION_START_URL_COOKIE))
    {   
        //SESSION_START_URL_COOKIE: created as soon as a login process is started and used to redirect at the end of the login process.
        //After the login process, ESP should remove this cookie if possible. This verify function is designed to verify cookies before 
        //a login process is started. At that time, any SESSION_START_URL_COOKIE should be invalid.
        cookie.verificationDetails.set("ESP cannot verify this cookie. This cookie is only valid within a login process.");
    }
    else if (strieq(name, SESSION_ID_TEMP_COOKIE) || strieq(name, SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE))
    {   
        //SESSION_ID_TEMP_COOKIE: used for remembering UserID when a user is in AS_PASSWORD_VALID_BUT_EXPIRED state and updating password.
        //It is created before the updatepasswordinput is started. After the update process, ESP should remove this cookie if possible.
        //This verify function is designed to verify cookies before a login process is started. At that time, any SESSION_ID_TEMP_COOKIE
        //should be invalid.
        //SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE: similar to SESSION_ID_TEMP_COOKIE.
        cookie.verificationDetails.set("ESP cannot verify this cookie. This cookie is only valid when updating an expired password.");
    }
    else
        cookie.verificationDetails.set("ESP does not know this cookie");
}

void CEspHttpServer::verifyESPUserNameCookie(EspAuthRequest& authReq, CESPCookieVerification& cookie)
{
    //USER_NAME_COOKIE: used in UserNameOnlyMode for remembering the user name. Right now it is
    //always valid if ESP is in the UserNameOnlyMode.
    if (authReq.authBinding->getDomainAuthType() != AuthUserNameOnly)
        return;

    if (!cookie.cookieValue.get())
        cookie.cookieValue.set(m_request->queryCookie(USER_NAME_COOKIE)->getValue());
    if (cookie.cookieValue.get())
        cookie.valid.set("true");
}

bool CEspHttpServer::verifyESPSessionIDCookie(EspAuthRequest& authReq)
{
    unsigned sessionID = readCookie(authReq.authBinding->querySessionIDCookieName());
    if (sessionID == 0) //No valid SessionIDCookie found
        return false;

    //Timeout old sessions.
    Owned<IRemoteConnection> conn = getSDSConnection(authReq.authBinding->queryESPSessionSDSPath(), RTM_LOCK_WRITE, SESSION_SDS_LOCK_TIMEOUT);
    IPropertyTree* espSessions = conn->queryRoot();
    if (authReq.authBinding->getServerSessionTimeoutSeconds() >= 0)
    {
        CDateTime now;
        now.setNow();
        time_t timeNow = now.getSimple();
        if ((timeNow - lastSessionCleanUpTime) >= authReq.authBinding->getCheckSessionTimeoutSeconds())
        {
            lastSessionCleanUpTime = timeNow;
            timeoutESPSessions(authReq.authBinding, espSessions);
        }
    }

    //Now, check whether the session ID is valid or not.
    VStringBuffer xpath("%s[@port=\"%d\"]/%s%u", PathSessionApplication, authReq.authBinding->getPort(),
        PathSessionSession, sessionID);
    IPropertyTree* sessionTree = espSessions->queryBranch(xpath.str());
    if (!sessionTree)
        return false;
#ifdef _CONTAINERIZED
    return true;
#else
    StringBuffer peer;
    return streq(m_request->getPeer(peer).str(), sessionTree->queryProp(PropSessionNetworkAddress));
#endif
}

void CEspHttpServer::verifyESPAuthenticatedCookie(EspAuthRequest& authReq, CESPCookieVerification& cookie)
{
    if (!cookie.cookieValue.get())
        cookie.cookieValue.set(m_request->queryCookie(SESSION_AUTH_OK_COOKIE)->getValue());
    if (!cookie.cookieValue.get())
    {
        cookie.verificationDetails.set("Cookie value not found.");
        return;
    }

    bool checkValid = strieq(cookie.cookieValue.get(), "true") ? true : false;
    if (authReq.authBinding->getDomainAuthType() == AuthUserNameOnly)
    {//For AuthUserNameOnly, a user is 'authenticated' if there is a USER_NAME_COOKIE (which contains a user name).
        if (checkValid && m_request->queryCookie(USER_NAME_COOKIE))
            cookie.valid.set("true");
        if (!checkValid && !m_request->queryCookie(USER_NAME_COOKIE))
            cookie.valid.set("true");
        return;
    }

    bool isSessionIDCookieValid = verifyESPSessionIDCookie(authReq);
    if (checkValid && isSessionIDCookieValid)
        cookie.valid.set("true");
    else if (!checkValid && !isSessionIDCookieValid)
        cookie.valid.set("true");
}

void CEspHttpServer::sendVerifyCookieResponse(EspAuthRequest& authReq, CIArrayOf<CESPCookieVerification>& cookies)
{
    StringBuffer resp;
    ESPSerializationFormat format = m_request->queryContext()->getResponseFormat();
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.append(" \"VerifyResponse\": { ");
        ForEachItemIn(i, cookies)
        {
            CESPCookieVerification& cookie = cookies.item(i);
            const char* name = cookie.cookieName.get();
            if (i > 0)
                resp.append(",");
            resp.appendf(" \"%s\": { ", name);
            if (strieq(name, "ESPSessionID") || strieq(name, authReq.authBinding->querySessionIDCookieName()))
                resp.append(" \"Value\": \"(hidden)\"");
            else if (cookie.cookieValue.get())
                resp.appendf(" \"Value\": \"%s\"", cookie.cookieValue.get());
            resp.appendf(", \"Valid\": %s", cookie.valid.get());

            if (cookie.verificationDetails.get())
                resp.appendf(", \"Details\": \"%s\"", cookie.verificationDetails.get());
            resp.append(" }");
        }
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.set("<VerifyResponse>");
        ForEachItemIn(i, cookies)
        {
            CESPCookieVerification& cookie = cookies.item(i);
            const char* name = cookie.cookieName.get();
            resp.append("<").append(name).append(">");
            if (strieq(name, "ESPSessionID") || strieq(name, authReq.authBinding->querySessionIDCookieName()))
                resp.append("<Value>(hidden)</Value>");
            else if (cookie.cookieValue.get())
                resp.append("<Value>").append(cookie.cookieValue.get()).append("</Value>");
            resp.append("<Valid>").append(cookie.valid.get()).append("</Valid>");
            if (cookie.verificationDetails.get())
                resp.append("<Details>").append(cookie.verificationDetails.get()).append("</Details>");
            resp.append("</").append(name).append(">");
        }
        resp.append("</VerifyResponse>");
    }
    sendMessage(resp.str(), (format == ESPSerializationJSON) ? "application/json" : "text/xml");
}

bool CEspHttpServer::isMalformedUserName(const char *userName)
{
    StringBuffer s(userName);
    s.trim();
    if (s.isEmpty())
        return true;

    //Only check newline for now. May add more if needed.
    return strchr(s.str(), '\n');
}

EspAuthState CEspHttpServer::handleUserNameOnlyMode(EspAuthRequest& authReq)
{
    if (authReq.authBinding->isDomainAuthResources(authReq.httpPath.str()))
        return authSucceeded;//Give the permission to send out some pages used for getUserName page.

    StringBuffer userName;
    readCookie(USER_NAME_COOKIE, userName);
    if (!userName.isEmpty())
    {
        authReq.ctx->setUserID(userName.str());
        return authSucceeded;
    }

    //The userName may be in the Authorization header.
    authReq.ctx->getUserID(userName);
    if (!userName.isEmpty())
        return authSucceeded;

    bool malformedUserName = false;
    const char* userNameIn = (authReq.requestParams) ? authReq.requestParams->queryProp("username") : NULL;
    if (!isEmptyString(userNameIn))
        malformedUserName = isMalformedUserName(userNameIn);
    if (isEmptyString(userNameIn) || malformedUserName)
    {
        StringBuffer logMsg("Authentication failed");
        if (malformedUserName)
        {
            logMsg.append(" (malformed username)");
            //If the username comes from the GetUserName page (sent with POST), the page should have set the urlCookie and askUserLogin() should be called to get the username.
            StringBuffer urlCookie;
            readCookie(SESSION_START_URL_COOKIE, urlCookie);
            if (!urlCookie.isEmpty())
            {
                logMsg.append(": call askUserLogin.");
                ESPLOG(LogMin, "%s", logMsg.str());
                //Display a GetUserName (similar to login) page to get a user name.
                askUserLogin(authReq, "Malformed username.");
                return authFailed;
            }
        }

        //We should send BasicAuthenticationChallenge for CORS Request, HTPP Post Request, etc.
        StringBuffer authorizationHeader, originHeader;
        m_request->getHeader("Authorization", authorizationHeader);
        m_request->getHeader("Origin", originHeader);
        bool basicAuthentication = !authorizationHeader.isEmpty() || authReq.authBinding->isCORSRequest(originHeader.str()) ||
            strieq(authReq.httpMethod.str(), POST_METHOD);
        if (basicAuthentication)
        {
            logMsg.append(": send BasicAuthentication.");
            ESPLOG(LogMin, "%s", logMsg.str());
            m_response->sendBasicChallenge(authReq.authBinding->getChallengeRealm(), true);
        }
        else
        {
            logMsg.append(": call askUserLogin.");
            ESPLOG(LogMin, "%s", logMsg.str());
            //Display a GetUserName (similar to login) page to get a user name.
            askUserLogin(authReq, malformedUserName ? "Malformed username." : "Empty username.");
        }
        return authFailed;
    }

    //We just got the user name. Let's add it into cookie for future use.
    addCookie(USER_NAME_COOKIE, userNameIn, 0, false);
    addCookie(SESSION_AUTH_OK_COOKIE, "true", 0, false); //client can access this cookie.

    StringBuffer urlCookie;
    readCookie(SESSION_START_URL_COOKIE, urlCookie);
    clearCookie(SESSION_START_URL_COOKIE);
    bool canRedirect = authReq.authBinding->canRedirectAfterAuth(urlCookie.str());
    m_response->redirect(*m_request, canRedirect ? urlCookie.str() : "/");
    return authSucceeded;
}

bool CEspHttpServer::isAuthRequiredForBinding(EspAuthRequest& authReq)
{
    IAuthMap* authmap = authReq.authBinding->queryAuthMAP();
    if (!authmap) //No auth requirement
        return false;

    const char* authMethod = authReq.authBinding->queryAuthMethod();
    if (isEmptyString(authMethod) || strieq(authMethod, "none"))
        return false;

    ISecResourceList* rlist = authmap->getResourceList(authReq.httpPath.str());
    if(!rlist) //No auth requirement for the httpPath.
        return false;

    authReq.ctx->setAuthenticationMethod(authMethod);
    authReq.ctx->setResources(rlist);

    return true;
}

EspAuthState CEspHttpServer::checkUserAuthPerSession(EspAuthRequest& authReq, StringBuffer& authorizationHeader)
{
    ESPLOG(LogMax, "checkUserAuthPerSession");

    unsigned sessionID = readCookie(authReq.authBinding->querySessionIDCookieName());
    if (sessionID > 0)
        return authExistingSession(authReq, sessionID);//Check session based authentication using this session ID.

    if (authReq.authBinding->isDomainAuthResources(authReq.httpPath.str()))
        return authSucceeded;//Give the permission to send out some pages used for login or logout.

    if (authReq.authBinding->isUnrestrictedSSType(authReq.stype))
        return authSucceeded;//Give the permission to send out some pages which do not need user authentication.

    if (!authorizationHeader.isEmpty() && !isServiceMethodReq(authReq, "esp", "login")
        && !isServiceMethodReq(authReq, "esp", "unlock"))
        return authUnknown;

    StringBuffer urlCookie;
    readCookie(SESSION_START_URL_COOKIE, urlCookie);
    if (strieq(authReq.httpPath.str(), authReq.authBinding->queryLoginURL()))
    {//This is a request to ask for a login page.
        if (urlCookie.isEmpty())
            addCookie(SESSION_START_URL_COOKIE, "/", 0, true); //Will be redirected to / after authenticated.
        return authSucceeded;
    }

    if (authReq.serviceName.isEmpty() || authReq.methodName.isEmpty() || !strieq(authReq.serviceName.str(), "esp"))
        return authUnknown;
    const char* method = authReq.methodName.str();
    bool unlock = strieq(method, "unlock");
    if (!unlock && !strieq(authReq.methodName.str(), "login"))
        return authUnknown;

    const char* userName = (authReq.requestParams) ? authReq.requestParams->queryProp("username") : NULL;
    const char* password = (authReq.requestParams) ? authReq.requestParams->queryProp("password") : NULL;
    if (!isEmptyString(userName) && !isEmptyString(password))
    {
        const char *url = "/";
        //if the cookie came from us it would be encoded, but check for newline just in case the cookie was injected somehow
        //  unescaped newlines can be made to look like nefarious http headers
        if (!urlCookie.isEmpty() && !strchr(urlCookie, '\n'))
            url = urlCookie.str();
        return authNewSession(authReq, userName, password, url, unlock);
    }

    authReq.ctx->setAuthStatus(AUTH_STATUS_FAIL);
    if (unlock)
    {
        sendLockResponse(false, true, "Empty user name or password");
        return authTaskDone;
    }

    if (authReq.isSoapPost) //from SOAP Test page
        sendMessage("Authentication failed: empty user name or password.", "text/html; charset=UTF-8");
    else //from other page
        askUserLogin(authReq, "Empty username or password.");
    return authFailed;
}

EspAuthState CEspHttpServer::checkUserAuthPerRequest(EspAuthRequest& authReq)
{
    ESPLOG(LogMax, "checkUserAuthPerRequest");

    authReq.authBinding->populateRequest(m_request.get());
    authReq.ctx->querySecureContextEx()->setDomainAuthType(AuthPerRequestOnly);
    if (authReq.authBinding->doAuth(authReq.ctx))
    {//We do pass the authentication per the request
        // authenticate optional groups. Do we still need?
        authOptionalGroups(authReq);
        authReq.ctx->setAuthStatus(AUTH_STATUS_OK); //May be changed to AUTH_STATUS_NOACCESS if failed in feature level authorization.

        StringBuffer userName, peer;
        ESPLOG(LogNormal, "Authenticated for %s@%s", authReq.ctx->getUserID(userName).str(), m_request->getPeer(peer).str());
        return authSucceeded;
    }
    if (!authReq.isSoapPost)
        return authUnknown;

    //If SoapPost, username/password may be in soap:Header which is not in HTTP header.
    //The doAuth() may check them inside CSoapService::processHeader() later.
    authReq.ctx->setToBeAuthenticated(true);
    return authPending;
}

void CEspHttpServer::sendMessage(const char* msg, const char* msgType)
{
    if (!isEmptyString(msg))
        m_response->setContent(msg);
    m_response->setContentType(msgType);
    m_response->setStatus(HTTP_STATUS_OK);
    m_response->send();
}

EspAuthState CEspHttpServer::authNewSession(EspAuthRequest& authReq, const char* _userName, const char* _password, const char* sessionStartURL, bool unlock)
{
    StringBuffer peer;
    m_request->getPeer(peer);

    ESPLOG(LogMax, "authNewSession for %s@%s", _userName, peer.str());

    authReq.ctx->setUserID(_userName);
    authReq.ctx->setPassword(_password);
    authReq.authBinding->populateRequest(m_request.get());
    authReq.ctx->querySecureContextEx()->setDomainAuthType(AuthPerSessionOnly);
    if (!authReq.authBinding->doAuth(authReq.ctx))
    {
        authReq.ctx->setAuthStatus(AUTH_STATUS_FAIL);
        ESPLOG(LogMin, "Authentication failed for %s@%s", _userName, peer.str());
        return handleAuthFailed(true, authReq, unlock, "User authentication failed.");
    }
    // authenticate optional groups
    authOptionalGroups(authReq);

    unsigned sessionID = createHTTPSession(authReq.ctx, authReq.authBinding, _userName, sessionStartURL);
    authReq.ctx->setSessionToken(sessionID);

    ESPLOG(LogMax, "Authenticated for %s@%s", _userName, peer.str());

    VStringBuffer cookieStr("%u", sessionID);
    addCookie(authReq.authBinding->querySessionIDCookieName(), cookieStr.str(), 0, true);
    cookieStr.setf("%u", authReq.authBinding->getClientSessionTimeoutSeconds());
    addCookie(SESSION_AUTH_OK_COOKIE, "true", 0, false); //client can access this cookie.
    addCookie(SESSION_TIMEOUT_COOKIE, cookieStr.str(), 0, false);
    clearCookie(SESSION_AUTH_MSG_COOKIE);
    clearCookie(SESSION_START_URL_COOKIE);

    authReq.ctx->setAuthStatus(AUTH_STATUS_OK); //May be changed to AUTH_STATUS_NOACCESS if failed in feature level authorization.
    if (unlock)
    {
        sendLockResponse(false, false, "Unlocked");
        return authTaskDone;
    }

    bool canRedirect = authReq.authBinding->canRedirectAfterAuth(sessionStartURL);
    m_response->redirect(*m_request, canRedirect ? sessionStartURL : "/");
    return authSucceeded;
}

void CEspHttpServer::sendException(EspAuthRequest& authReq, unsigned code, const char* msg)
{
    if (authReq.isSoapPost) //from SOAP Test page
    {
        sendMessage(msg, "text/html; charset=UTF-8");
        return;
    }

    StringBuffer resp;
    ESPSerializationFormat format = m_request->queryContext()->getResponseFormat();
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.append(" \"Exceptions\": { ");
        resp.append(" \"Exception\": { ");
        resp.appendf("\"Code\": \"%u\"", code);
        if (!isEmptyString(msg))
            resp.appendf(", \"Message\": \"%s\"", msg);
        resp.append(" }");
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.set("<Exceptions><Exception>");
        resp.append("<Code>").append(code).append("</Code>");
        if (!isEmptyString(msg))
            resp.append("<Message>").append(msg).append("</Message>");
        resp.append("</Exception></Exceptions>");
    }
    sendMessage(resp.str(), (format == ESPSerializationJSON) ? "application/json" : "text/xml");
}

/**
 * @brief Return a generic internal server error to the client.
 *
 * Exceptions caught by processRequest fall into three categories.
 *
 * 1. HTTP exceptions. These are assumed to contain messages suitable for client consumption and
 *    are returned to the caller.
 * 2. Other known exceptions, derived from IException. These are assumed to contain messages that
 *    include implementation details not appropriate for client consumption.
 * 3. Unexpected exceptions that cannot be described.
 *
 * This method handles categories 2 and 3. A generic message indicating that an exception occurred
 * is returned to the client with status code 500. The parameter indicates whether trace output is
 * likely to include exception details, and the returned message will refer the client to examine
 * the logs for more information when appropriate.
 *
 * @param loggedDetails 
 */
void CEspHttpServer::sendInternalError(bool loggedDetails)
{
    IEspContext* ctx = m_request->queryContext();
    const char*  globalId = (ctx ? ctx->getGlobalId() : nullptr);
    StringBuffer content;
    if (!isEmptyString(globalId))
        content.appendf("Request failed for global transaction '%s'.", globalId);
    else
        content.append("Request failed.");
    if (loggedDetails)
        content.append(" Check log files for more information.");
    m_response->setStatus(HTTP_STATUS_INTERNAL_SERVER_ERROR);
    m_response->setContentType(HTTP_TYPE_TEXT_PLAIN);
    m_response->setContent(content);
    m_response->send();
}

void CEspHttpServer::sendAuthorizationMsg(EspAuthRequest& authReq)
{
    StringBuffer resp;
    const char* errMsg = authReq.ctx->getRespMsg();
    ESPSerializationFormat format = m_request->queryContext()->getResponseFormat();
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.append(" \"LoginResponse\": { ");
        if (isEmptyString(errMsg))
            resp.append("\"Error\": \"Access Denied.\"");
        else
            resp.appendf("\"Error\": \"%s\"", errMsg);
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.set("<LoginResponse><Error>");
        resp.append("Access Denied.");
        if (!isEmptyString(errMsg))
            resp.append(" ").append(errMsg);
        resp.append("</Error></LoginResponse>");
    }
    sendMessage(resp.str(), (format == ESPSerializationJSON) ? "application/json" : "text/xml");
}

void CEspHttpServer::sendLockResponse(bool lock, bool error, const char* msg)
{
    StringBuffer resp;
    ESPSerializationFormat format = m_request->queryContext()->getResponseFormat();
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.appendf(" \"%sResponse\": { ", lock ? "Lock" : "Unlock");
        resp.appendf("\"Error\": %d", error);
        if (!isEmptyString(msg))
            resp.appendf(", \"Message\": \"%s\"", msg);
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.setf("<%sResponse>", lock ? "Lock" : "Unlock");
        resp.appendf("<Error>%d</Error>", error);
        if (!isEmptyString(msg))
            resp.appendf("<Message>%s</Message>", msg);
        resp.appendf("</%sResponse>", lock ? "Lock" : "Unlock");
    }
    sendMessage(resp.str(), (format == ESPSerializationJSON) ? "application/json" : "text/xml");
}

void CEspHttpServer::sendGetAuthTypeResponse(EspAuthRequest& authReq, const char* authType)
{
    StringBuffer authTypeStr(authType);
    if (authTypeStr.isEmpty())
    {
        switch (authReq.authBinding->getDomainAuthType())
        {
            case AuthUserNameOnly:
                authTypeStr.set(AUTH_TYPE_USERNAMEONLY);
                break;
            case AuthPerRequestOnly:
                authTypeStr.set(AUTH_TYPE_PERREQUESTONLY);
                break;
            case AuthPerSessionOnly:
                authTypeStr.set(AUTH_TYPE_PERSESSIONONLY);
                break;
            default:
                authTypeStr.set(AUTH_TYPE_MIXED);
                break;
        }
    }

    StringBuffer resp;
    ESPSerializationFormat format = m_request->queryContext()->getResponseFormat();
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.append("\"GetAuthTypeResponse\": { ");
        resp.appendf("\"AuthType\": \"%s\"", authTypeStr.str());
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.setf("<GetAuthTypeResponse><AuthType>%s</AuthType></GetAuthTypeResponse>", authTypeStr.str());
    }
    sendMessage(resp.str(), (format == ESPSerializationJSON) ? "application/json" : "text/xml");
}

void CEspHttpServer::createGetSessionTimeoutResponse(StringBuffer& resp, ESPSerializationFormat format, IPropertyTree* sessionTree)
{
    //The timeoutAt is a time stamp for when the session should be timed out on ESP server.
    //The 0 is used to indicate that the session is already timed out.
    __int64 timeoutAt = sessionTree ? sessionTree->getPropInt64(PropSessionTimeoutAt, 0) : 0;
    bool timeoutByAdmin = sessionTree ? sessionTree->getPropBool(PropSessionTimeoutByAdmin, false) : false;
    if (format == ESPSerializationJSON)
    {
        resp.set("{ ");
        resp.append(" \"GetSessionTimeoutResponse\": { ");
        resp.appendf("\"TimeoutAt\": %lld", timeoutAt);
        if (timeoutByAdmin)
            resp.appendf(", \"TimeoutByAdmin\": true");
        resp.append(" }");
        resp.append(" }");
    }
    else
    {
        resp.set("<GetSessionTimeoutResponse>");
        resp.appendf("<TimeoutAt>%lld</TimeoutAt>", timeoutAt);
        if (timeoutByAdmin)
            resp.append("<TimeoutByAdmin>true</TimeoutByAdmin>");
        resp.append("</GetSessionTimeoutResponse>");
    }
}

void CEspHttpServer::resetSessionTimeout(EspAuthRequest& authReq, unsigned sessionID, StringBuffer& resp, ESPSerializationFormat format, IPropertyTree* sessionTree)
{
    if (format == ESPSerializationJSON)
    {
        resp.set("{ \"ResetSessionTimeoutResponse\": { ");
    }
    else
    {
        resp.set("<ResetSessionTimeoutResponse>");
    }
    if (!sessionTree)
    {
        if (format == ESPSerializationJSON)
        {
            resp.append("\"The session has already expired\": true");
        }
        else
        {
            resp.append("The session has already expired.");
        }
    }
    else
    {
        unsigned timeoutSeconds = 60 * authReq.requestParams->getPropInt("_timeout");
        if (timeoutSeconds == 0)
            timeoutSeconds = authReq.authBinding->getServerSessionTimeoutSeconds();

        CDateTime now;
        now.setNow();
        time_t createTime = now.getSimple();
        time_t timeoutAt = createTime + timeoutSeconds;
        sessionTree->setPropInt64(PropSessionLastAccessed, createTime);
        sessionTree->setPropInt64(PropSessionTimeoutAt, timeoutAt);

        VStringBuffer sessionIDStr("%u", sessionID);
        addCookie(authReq.authBinding->querySessionIDCookieName(), sessionIDStr.str(), 0, true);
        addCookie(SESSION_AUTH_OK_COOKIE, "true", 0, false); //client can access this cookie.

        if (getEspLogLevel()>=LogMax)
        {
            CDateTime timeoutAtCDT;
            StringBuffer timeoutAtString, nowString;
            timetToIDateTime(&timeoutAtCDT, timeoutAt);
            PROGLOG("Reset %s for (/%s/%s) at <%s><%" PRId64 "> : expires at <%s><%" PRId64 ">", PropSessionTimeoutAt,
                authReq.serviceName.isEmpty() ? "" : authReq.serviceName.str(), authReq.methodName.isEmpty() ? "" : authReq.methodName.str(),
                now.getString(nowString).str(), (int64_t)createTime, timeoutAtCDT.getString(timeoutAtString).str(), (int64_t)timeoutAt);
        }
        else
            ESPLOG(LogMin, "Reset %s for (/%s/%s) : %" PRId64 "", PropSessionTimeoutAt, authReq.serviceName.isEmpty() ? "" : authReq.serviceName.str(),
                authReq.methodName.isEmpty() ? "" : authReq.methodName.str(), (int64_t)timeoutAt);

        if (format == ESPSerializationJSON)
        {
            resp.append("\"Session timer reset\": true");
        }
        else
        {
            resp.append("Session timer reset");
        }
    }
    if (format == ESPSerializationJSON)
    {
        resp.append(" } }");
    }
    else
    {
        resp.append("</ResetSessionTimeoutResponse>");
    }
}

void CEspHttpServer::sendSessionReloadHTMLPage(IEspContext* ctx, EspAuthRequest& authReq, const char* errMsg)
{
    StringBuffer espURL;
    if (isSSL)
        espURL.set("https://");
    else
        espURL.set("http://");
    m_request->getHost(espURL);
    if (m_request->getHasPortInHost())
        espURL.append(":").append(m_request->getPort());

    StringBuffer content(
        "<!DOCTYPE html>"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head>"
            "<meta charset=utf-8\"/>"
            "<title class=\"loginStr\"></title>"
            "<style type=\"text/css\">"
                "body {"
                    "font-family: Lucida Sans, Lucida Grande, Arial !important;"
                    "font-size: 15px !important;"
                    "background-color: #1A9BD7;"
                "}"

                ".container {"
                    "width: 99%;"
                    "position: absolute;"
                    "top: 50%;"
                    "transform: translateY(-50%);"
                "}"

                ".container a {"
                    "color: #1A9BD7;"
                "}"

                ".container a:visited, .container a:link {"
                    "color: #1A9BD7;"
                "}"

                ".formContainer {"
                    "width: 500px;"
                    "padding: 20px 0 20px 0;"
                    "border-radius: 5px;"
                    "background-color: #fff;"
                    "margin: auto;"
                "}"

                ".login {"
                    "width: 400px;"
                    "margin: auto;"
                "}"

                ".login input {"
                    "margin-bottom: 20px;"
                    "width: 300px;"
                    "padding: 8px;"
                    "border: 1px solid #bfbfbf;"
                "}"

                ".login form {"
                    "margin: auto;"
                    "width: 300px;"
                "}"

                "img {"
                    "display: block;"
                    "margin: auto;"
                "}"

                "p {"
                    "text-align: center"
                "}"
            "</style>"
            "</head>"
            "<body>"
                "<div id=\"container\" class=\"container\">"
                    "<div class=\"formContainer\">");
        content.appendf("<img id=\"logo\" src=\"%s%s\" />", espURL.str(), authReq.authBinding->queryLoginLogoURL());
         content.append("<div class=\"login\">");
            content.appendf("<p class=\"loginStr\">%s <a href=\"%s\" class=\"loginStr\">Please click here to log into ECL Watch.</a></p>", errMsg, espURL.str());
         content.append("</div>"
                    "</div>"
                 "</div>"
            "</body>"
       "</html>");
    m_response->setContent(content.length(), content.str());
    m_response->setContentType("text/html");
    m_response->setStatus(HTTP_STATUS_OK);
    m_response->send();
}

EspAuthState CEspHttpServer::authExistingSession(EspAuthRequest& authReq, unsigned sessionID)
{
    ESPLOG(LogMax, "authExistingSession: %s<%u>", PropSessionID, sessionID);

    bool getLoginPage = false;
    if (authReq.authBinding->isDomainAuthResources(authReq.httpPath.str()))
    {
        if (!strieq(authReq.httpPath.str(), authReq.authBinding->queryLoginURL()))
            return authSucceeded;//Give the permission to send out some unrestricted resource pages.
        getLoginPage = true;
    }

    Owned<IRemoteConnection> conn = getSDSConnection(authReq.authBinding->queryESPSessionSDSPath(), RTM_LOCK_WRITE, SESSION_SDS_LOCK_TIMEOUT);
    IPropertyTree* espSessions = conn->queryRoot();
    if (authReq.authBinding->getServerSessionTimeoutSeconds() >= 0)
    {
        CDateTime now;
        now.setNow();
        time_t timeNow = now.getSimple();
        if (timeNow - lastSessionCleanUpTime >= authReq.authBinding->getCheckSessionTimeoutSeconds())
        {
            lastSessionCleanUpTime = timeNow;
            timeoutESPSessions(authReq.authBinding, espSessions);
        }
    }

    VStringBuffer xpath("%s[@port=\"%d\"]/%s%u", PathSessionApplication, authReq.authBinding->getPort(), PathSessionSession, sessionID);
    IPropertyTree* sessionTree = espSessions->queryBranch(xpath.str());
    if (!authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty() && strieq(authReq.serviceName.str(), "esp"))
    {
        StringBuffer content;
        ESPSerializationFormat respFormat = m_request->queryContext()->getResponseFormat();
        if (strieq(authReq.methodName.str(), "get_session_timeout"))
            createGetSessionTimeoutResponse(content, respFormat, sessionTree);
        else if (strieq(authReq.methodName.str(), "reset_session_timeout"))
            resetSessionTimeout(authReq, sessionID, content, respFormat, sessionTree);
        if (!content.isEmpty())
        {
            sendMessage(content.str(), (respFormat == ESPSerializationJSON) ? "application/json" : "text/xml");
            return authTaskDone;
        }
    }

    if (!sessionTree)
    {
        authReq.ctx->setAuthStatus(AUTH_STATUS_FAIL);
        clearSessionCookies(authReq);
        sendSessionReloadHTMLPage(m_request->queryContext(), authReq, "Authentication failed: invalid session.");
        ESPLOG(LogMin, "Authentication failed: invalid session ID '%u'. clearSessionCookies() called for the session.", sessionID);
        return authFailed;
    }

    StringBuffer peer;
    const char* sessionStartIP = sessionTree->queryProp(PropSessionNetworkAddress);
    if (!streq(m_request->getPeer(peer).str(), sessionStartIP))
    {
#ifdef _CONTAINERIZED
        ESPLOG(LogMax, "####Peer changed");
#else
        authReq.ctx->setAuthStatus(AUTH_STATUS_FAIL);
        clearSessionCookies(authReq);
        sendSessionReloadHTMLPage(m_request->queryContext(), authReq, "Authentication failed: Network address for ESP session has been changed.");
        ESPLOG(LogMin, "Authentication failed: session ID %u from IP %s. ", sessionID, peer.str());
        return authFailed;
#endif
    }

    authOptionalGroups(authReq);

    //The UserID has to be set before the populateRequest() because the UserID is used to create the user object.
    //After the user object is created, we may call addSessionToken().
    StringAttr userID = sessionTree->queryProp(PropSessionUserID);
    authReq.ctx->setUserID(userID.str());
    authReq.authBinding->populateRequest(m_request.get());
    authReq.ctx->setSessionToken(sessionID);
    authReq.ctx->queryUser()->setAuthenticateStatus(AS_AUTHENTICATED);
    authReq.ctx->setAuthStatus(AUTH_STATUS_OK); //May be changed to AUTH_STATUS_NOACCESS if failed in feature level authorization.
    setDomainAuthDataInSecureContext(authReq.ctx, sessionTree);

    ESPLOG(LogMax, "Authenticated for %s<%u> %s@%s", PropSessionID, sessionID, userID.str(), sessionTree->queryProp(PropSessionNetworkAddress));
    if (!authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty() && strieq(authReq.serviceName.str(), "esp") && strieq(authReq.methodName.str(), "login"))
    {
        VStringBuffer msg("User %s has logged into this session. If you want to login as a different user, please logout and login again.", userID.str());
        sendMessage(msg.str(), "text/html; charset=UTF-8");
        return authTaskDone;
    }
    if (!authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty() && strieq(authReq.serviceName.str(), "esp") && strieq(authReq.methodName.str(), "lock"))
    {
        logoutSession(authReq, sessionID, espSessions, true);
        return authTaskDone;
    }

    if (!authReq.serviceName.isEmpty() && !authReq.methodName.isEmpty() && strieq(authReq.serviceName.str(), "esp") && strieq(authReq.methodName.str(), "logout"))
    {
        logoutSession(authReq, sessionID, espSessions, false);
        return authTaskDone;
    }

    //The "ECLWatchAutoRefresh" returns a flag: '1' means that the request is generated by a UI auto refresh action and '0' means not.
    StringBuffer autoRefresh;
    m_request->getParameter("ECLWatchAutoRefresh", autoRefresh);

    CDateTime now;
    now.setNow();
    time_t createTime = now.getSimple();
    sessionTree->setPropInt64(PropSessionLastAccessed, createTime);
    if (!sessionTree->getPropBool(PropSessionTimeoutByAdmin, false) && (autoRefresh.isEmpty() || strieq(autoRefresh.str(), "0")))
    {
        time_t timeoutAt = createTime + authReq.authBinding->getServerSessionTimeoutSeconds();
        sessionTree->setPropInt64(PropSessionTimeoutAt, timeoutAt);
        ESPLOG(LogMax, "Updated %s for (/%s/%s) : %" PRId64 "", PropSessionTimeoutAt, authReq.serviceName.isEmpty() ? "" : authReq.serviceName.str(),
            authReq.methodName.isEmpty() ? "" : authReq.methodName.str(), (int64_t)timeoutAt);
    }
    ///authReq.ctx->setAuthorized(true);
    VStringBuffer sessionIDStr("%u", sessionID);
    addCookie(authReq.authBinding->querySessionIDCookieName(), sessionIDStr.str(), 0, true);
    addCookie(SESSION_AUTH_OK_COOKIE, "true", 0, false); //client can access this cookie.
    if (getLoginPage)
        m_response->redirect(*m_request, "/");
    if (!authReq.authBinding->canRedirectAfterAuth(authReq.httpPath.str()))
        m_response->redirect(*m_request, "/");

    return authSucceeded;
}

void CEspHttpServer::logoutSession(EspAuthRequest& authReq, unsigned sessionID, IPropertyTree* espSessions, bool lock)
{
    //delete this session before logout
    VStringBuffer path("%s[@port=\"%d\"]", PathSessionApplication, authReq.authBinding->getPort());
    IPropertyTree* sessionTree = espSessions->queryBranch(path.str());
    if (sessionTree)
    {
        ICopyArrayOf<IPropertyTree> toRemove;
        path.setf("%s%u", PathSessionSession, sessionID);
        Owned<IPropertyTreeIterator> it = sessionTree->getElements(path.str());
        ForEach(*it)
            toRemove.append(it->query());

        IEspContext* ctx = m_request->queryContext();
        ISecManager* secmgr = nullptr;
        if (ctx)
            secmgr = ctx->querySecManager();

        ForEachItemIn(i, toRemove)
        {
            if (secmgr)
            {
                const char * user = toRemove.item(i).queryProp("@userid");
                if (user)
                {
                    //inform security manager that user is logged out
                    Owned<ISecUser> secUser = secmgr->createUser(user, ctx->querySecureContext());
                    secmgr->logoutUser(*secUser, ctx->querySecureContext());
                }
            }

            sessionTree->removeTree(&toRemove.item(i));
        }
    }
    else
        ESPLOG(LogMin, "Can't find session tree: %s[@port=\"%d\"]", PathSessionApplication, authReq.authBinding->getPort());

    ///authReq.ctx->setAuthorized(true);

    clearCookie(authReq.authBinding->querySessionIDCookieName());
    clearCookie(SESSION_AUTH_OK_COOKIE);
    clearCookie(SESSION_TIMEOUT_COOKIE);
    if (lock)
    {
        sendLockResponse(true, false, "Locked");
        return;
    }
    const char* logoutURL = authReq.authBinding->queryLogoutURL();
    if (!isEmptyString(logoutURL))
        m_response->redirect(*m_request, logoutURL);
    else
        sendMessage("Successfully logged out.", "text/html; charset=UTF-8");
}

EspAuthState CEspHttpServer::handleAuthFailed(bool sessionAuth, EspAuthRequest& authReq, bool unlock, const char* msg)
{
    ISecUser *user = authReq.ctx->queryUser();
    if (user)
    {
        switch (user->getAuthenticateStatus())
        {
        case AS_PASSWORD_VALID_BUT_EXPIRED :
            ESPLOG(LogMin, "ESP password expired for %s. Asking update ...", authReq.ctx->queryUserId());
            if (sessionAuth) //For session auth, store the userid to cookie for the updatepasswordinput form.
                addTempCookie(authReq.ctx);
            m_response->redirect(*m_request.get(), "/esp/updatepasswordinput");
            return authSucceeded;
        case AS_PASSWORD_EXPIRED :
            ESPLOG(LogMin, "ESP password expired for %s", authReq.ctx->queryUserId());
            break;
        case AS_ACCOUNT_DISABLED :
            ESPLOG(LogMin, "Account disabled for %s", authReq.ctx->queryUserId());
            addCookie(USER_ACCT_ERROR_COOKIE, "Account Disabled", 0, false);
            break;
        case AS_ACCOUNT_EXPIRED :
            ESPLOG(LogMin, "Account expired for %s", authReq.ctx->queryUserId());
            addCookie(USER_ACCT_ERROR_COOKIE, "Account Expired", 0, false);
            break;
        case AS_ACCOUNT_LOCKED :
            ESPLOG(LogMin, "Account locked for %s", authReq.ctx->queryUserId());
            addCookie(USER_ACCT_ERROR_COOKIE, "Account Locked", 0, false);
            break;
        case AS_ACCOUNT_ROOT_ACCESS_DENIED :
            addCookie(USER_ACCT_ERROR_COOKIE, authReq.ctx->getRespMsg(), 0, false);
            break;
        }
    }

    if (unlock)
    {
        ESPLOG(LogMin, "Unlock failed: invalid user name or password.");
        sendLockResponse(false, true, "Invalid user name or password");
        return authTaskDone;
    }

    if (!sessionAuth)
    {
        ESPLOG(LogMin, "Authentication failed: send BasicAuthentication.");
        m_response->sendBasicChallenge(authReq.authBinding->getChallengeRealm(), true);
    }
    else
    {
        ESPLOG(LogMin, "Authentication failed: call askUserLogin.");
        askUserLogin(authReq, msg);
    }
    return authFailed;
}

void CEspHttpServer::askUserLogin(EspAuthRequest& authReq, const char* msg)
{
    StringBuffer encoded;
    StringBuffer urlCookie;
    readCookie(SESSION_START_URL_COOKIE, urlCookie);
    if (urlCookie.isEmpty())
    {
        StringBuffer sessionStartURL(authReq.httpPath);
        if (authReq.requestParams && authReq.requestParams->hasProp("__querystring"))
            sessionStartURL.append("?").append(authReq.requestParams->queryProp("__querystring"));
        if (sessionStartURL.isEmpty() || streq(sessionStartURL.str(), "/WsSMC/") || strieq(sessionStartURL, authReq.authBinding->queryLoginURL()))
            sessionStartURL.set("/");

        addCookie(SESSION_START_URL_COOKIE, encodeURL(encoded.clear(), sessionStartURL.str()), 0, true); //time out when browser is closed
    }
    else if (changeRedirectURL(authReq))
    {
        StringBuffer sessionStartURL(authReq.httpPath);
        if (authReq.requestParams && authReq.requestParams->hasProp("__querystring"))
            sessionStartURL.append("?").append(authReq.requestParams->queryProp("__querystring"));
        addCookie(SESSION_START_URL_COOKIE, encodeURL(encoded.clear(), sessionStartURL.str()), 0, true); //time out when browser is closed
    }
    if (!isEmptyString(msg))
        addCookie(SESSION_AUTH_MSG_COOKIE, msg, 0, false); //time out when browser is closed
    m_response->redirect(*m_request, encodeURL(encoded.clear(), authReq.authBinding->queryLoginURL()));
}

bool CEspHttpServer::changeRedirectURL(EspAuthRequest& authReq)
{
    if (authReq.httpPath.isEmpty())
        return false;
    if (strieq(authReq.httpPath.str(), ECLWATCH_STUB_REQ))
        return true;
    return false;
}

unsigned CEspHttpServer::createHTTPSession(IEspContext* ctx, EspHttpBinding* authBinding, const char* userID, const char* sessionStartURL)
{
    CDateTime now;
    now.setNow();
    time_t createTime = now.getSimple();

    StringBuffer peer, sessionIDStr, sessionTag;
    VStringBuffer idStr("%s_%" PRId64 "", m_request->getPeer(peer).str(), (int64_t)createTime);
    unsigned sessionID = hashc((unsigned char *)idStr.str(), idStr.length(), 0);
    sessionIDStr.append(sessionID);

    sessionTag.appendf("%s%u", PathSessionSession, sessionID);
    ESPLOG(LogMax, "New sessionID <%u> at <%" PRId64 "> in createHTTPSession()", sessionID, (int64_t)createTime);
    Owned<IRemoteConnection> conn = getSDSConnection(authBinding->querySessionSDSPath(), RTM_LOCK_WRITE, SESSION_SDS_LOCK_TIMEOUT);
    IPropertyTree* domainSessions = conn->queryRoot();
    IPropertyTree* sessionTree = domainSessions->queryBranch(sessionTag.str());
    if (sessionTree)
    {
        sessionTree->setPropInt64(PropSessionLastAccessed, createTime);
        if (!sessionTree->getPropBool(PropSessionTimeoutByAdmin, false))
            sessionTree->setPropInt64(PropSessionTimeoutAt, createTime + authBinding->getServerSessionTimeoutSeconds());
        return sessionID;
    }
    ESPLOG(LogMax, "New sessionID <%d> at <%" PRId64 "> in createHTTPSession()", sessionID, (int64_t)createTime);

    IPropertyTree* ptree = domainSessions->addPropTree(sessionTag.str());
    ptree->setProp(PropSessionNetworkAddress, peer.str());
    ptree->setPropInt64(PropSessionID, sessionID);
    ptree->setPropInt64(PropSessionExternalID, hashc((unsigned char *)sessionIDStr.str(), sessionIDStr.length(), 0));
    ptree->setProp(PropSessionUserID, userID);
    ptree->setPropInt64(PropSessionCreateTime, createTime);
    ptree->setPropInt64(PropSessionLastAccessed, createTime);
    ptree->setPropInt64(PropSessionTimeoutAt, createTime + authBinding->getServerSessionTimeoutSeconds());
    ptree->setProp(PropSessionLoginURL, sessionStartURL);
    readDomainAuthDataFromSecureContext(ctx, ptree);
    return sessionID;
}

void CEspHttpServer::timeoutESPSessions(EspHttpBinding* authBinding, IPropertyTree* espSessions)
{
    //Removing HTTPSessions if timed out
    CDateTime now;
    now.setNow();
    time_t timeNow = now.getSimple();

    VStringBuffer xpath("%s*", PathSessionSession);
    Owned<IPropertyTreeIterator> iter1 = espSessions->getElements(PathSessionApplication);
    ForEach(*iter1)
    {
        ICopyArrayOf<IPropertyTree> toRemove;
        Owned<IPropertyTreeIterator> iter2 = iter1->query().getElements(xpath.str());
        ForEach(*iter2)
        {
            IPropertyTree& item = iter2->query();
            if (timeNow >= item.getPropInt64(PropSessionTimeoutAt, 0))
                toRemove.append(item);
        }
        ForEachItemIn(i, toRemove)
            iter1->query().removeTree(&toRemove.item(i));
    }
}

void CEspHttpServer::authOptionalGroups(EspAuthRequest& authReq)
{
    if (strieq(authReq.httpMethod.str(), GET_METHOD) && (authReq.stype==sub_serv_root) && authenticateOptionalFailed(*authReq.ctx, nullptr))
        throw createEspHttpException(HTTP_STATUS_FORBIDDEN_CODE, "Unauthorized Access to service root", HTTP_STATUS_FORBIDDEN);
    if ((!strieq(authReq.httpMethod.str(), GET_METHOD) || !strieq(authReq.serviceName.str(), "esp")) && authenticateOptionalFailed(*authReq.ctx, authReq.authBinding))
        throw createEspHttpException(HTTP_STATUS_FORBIDDEN_CODE, VStringBuffer("Unauthorized Access: %s %s", authReq.httpMethod.str(), authReq.serviceName.str()), HTTP_STATUS_FORBIDDEN);
}

IRemoteConnection* CEspHttpServer::getSDSConnection(const char* xpath, unsigned mode, unsigned timeout)
{
    Owned<IRemoteConnection> globalLock = querySDS().connect(xpath, myProcessSession(), RTM_LOCK_READ, SESSION_SDS_LOCK_TIMEOUT);
    if (!globalLock)
        throw MakeStringException(-1, "Unable to connect to ESP Session information in dali %s", xpath);
    return globalLock.getClear();
}

void CEspHttpServer::addCookie(const char* cookieName, const char *cookieValue, int maxAgeSec, bool httpOnly)
{
    CEspCookie* cookie = new CEspCookie(cookieName, cookieValue);
    if (maxAgeSec > 0)
    {
        char expiresTime[64];
        time_t tExpires;
        time(&tExpires);
        tExpires += maxAgeSec;
#ifdef _WIN32
        struct tm *gmtExpires;
        gmtExpires = gmtime(&tExpires);
        strftime(expiresTime, 64, "%a, %d %b %Y %H:%M:%S GMT", gmtExpires);
#else
        struct tm gmtExpires;
        gmtime_r(&tExpires, &gmtExpires);
        strftime(expiresTime, 64, "%a, %d %b %Y %H:%M:%S GMT", &gmtExpires);
#endif //_WIN32

        cookie->setExpires(expiresTime);
    }
    if (httpOnly)
        cookie->setHTTPOnly(true);
    cookie->setSameSite("Lax");
    m_response->addCookie(cookie);
}

void CEspHttpServer::clearSessionCookies(EspAuthRequest& authReq)
{
    clearCookie(authReq.authBinding->querySessionIDCookieName());
    clearCookie(SESSION_ID_TEMP_COOKIE);
    clearCookie(SESSION_DOMAIN_AUTH_DATA_TEMP_COOKIE);
    clearCookie(SESSION_START_URL_COOKIE);
    clearCookie(SESSION_AUTH_OK_COOKIE);
    clearCookie(SESSION_AUTH_MSG_COOKIE);
    clearCookie(SESSION_TIMEOUT_COOKIE);
    clearCookie(USER_ACCT_ERROR_COOKIE);
}

void CEspHttpServer::clearCookie(const char* cookieName)
{
    CEspCookie* cookie = new CEspCookie(cookieName, "");
    cookie->setExpires("Thu, 01 Jan 1970 00:00:01 GMT");
    m_response->addCookie(cookie);
    m_response->addHeader(cookieName,  "max-age=0");
}

unsigned CEspHttpServer::readCookie(const char* cookieName)
{
    CEspCookie* sessionIDCookie = m_request->queryCookie(cookieName);
    if (sessionIDCookie)
    {
        StringBuffer sessionIDStr(sessionIDCookie->getValue());
        if (sessionIDStr.length())
            return atoi(sessionIDStr.str());
    }
    return 0;
}

const char* CEspHttpServer::readCookie(const char* cookieName, StringBuffer& cookieValue)
{
    CEspCookie* sessionIDCookie = m_request->queryCookie(cookieName);
    if (sessionIDCookie)
        cookieValue.append(sessionIDCookie->getValue());
    return cookieValue.str();
}

bool CEspHttpServer::isServiceMethodReq(EspAuthRequest& authReq, const char* serviceName, const char* methodName)
{
    if (authReq.serviceName.isEmpty() || !strieq(authReq.serviceName.str(), serviceName))
        return false;
    if (authReq.methodName.isEmpty() || !strieq(authReq.methodName.str(), methodName))
        return false;
    return true;
}

bool CEspHttpServer::persistentEligible()
{
    if(m_request.get() != nullptr && m_response.get() != nullptr)
        return m_request->getPersistentEligible() && m_response->getPersistentEligible();
    else
        return false;
}
