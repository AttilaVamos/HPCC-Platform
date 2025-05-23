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

#include "ws_workunitsService.hpp"
#include "ws_fs.hpp"
#include "jlib.hpp"
#include "jconfig.hpp"
#include "jcontainerized.hpp"
#include "jflz.hpp"
#include "daclient.hpp"
#include "dalienv.hpp"
#include "dadfs.hpp"
#include "dfuwu.hpp"
#include "eclhelper.hpp"
#include "roxiecontrol.hpp"
#include "dfuutil.hpp"
#include "dautils.hpp"
#include "httpclient.hpp"
#include "portlist.h" //ROXIE_SERVER_PORT
#include "TpWrapper.hpp"

#define DALI_FILE_LOOKUP_TIMEOUT (1000*15*1)  // 15 seconds

//The CQuerySetQueryActionTypes[] has to match with the ESPenum QuerySetQueryActionTypes in the ecm file.
static unsigned NumOfQuerySetQueryActionTypes = 7;
static const char *QuerySetQueryActionTypes[] = { "Suspend", "Unsuspend", "ToggleSuspend", "Activate",
    "Delete", "RemoveAllAliases", "ResetQueryStats", NULL };

//The CQuerySetAliasActionTypes[] has to match with the ESPenum QuerySetAliasActionTypes in the ecm file.
static unsigned NumOfQuerySetAliasActionTypes = 1;
static const char *QuerySetAliasActionTypes[] = { "Deactivate", NULL };

void checkUseEspOrDaliIP(SocketEndpoint &ep, const char *ip, const char *esp)
{
    if (!ip || !*ip)
        return;
    ep.set(ip, 7070);
    if (ep.isLoopBack() || *ip=='.' || (ip[0]=='0' && ip[1]=='.'))
        ep.ipset(esp);
}

void ensureInputString(const char* input, bool lowerCase, StringBuffer& inputStr, int code, const char* msg)
{
    inputStr.set(input).trim();
    if (inputStr.isEmpty())
        throw MakeStringException(code, "%s", msg);
    if (lowerCase)
        inputStr.toLowerCase();
}

static IClientWsWorkunits *ensureWsWorkunitsClient(IClientWsWorkunits *ws, IEspContext *ctx, const char *netAddress, bool useSSL)
{
    if (ws)
        return LINK(ws);
    StringBuffer url;
    if (netAddress && *netAddress)
        url.appendf("%s://%s%s/WsWorkunits", useSSL ? "https" : "http", netAddress, (!strchr(netAddress, ':')) ? ":8010" : "");
    else
    {
        if (!ctx)
            throw MakeStringException(ECLWATCH_INVALID_IP, "Missing WsWorkunits service address");
        StringBuffer ip;
        short port = 0;
        ctx->getServAddress(ip, port);
        url.appendf("%s://%s:%d/WsWorkunits", useSSL ? "https" : "http", ip.str(), port);
    }
    Owned<IClientWsWorkunits> cws = createWsWorkunitsClient();
    cws->addServiceUrl(url);
    if (ctx && ctx->queryUserId() && *ctx->queryUserId())
        cws->setUsernameToken(ctx->queryUserId(), ctx->queryPassword(), NULL);
    return cws.getClear();
}

IClientWUQuerySetDetailsResponse *fetchQueryDetails(IClientWsWorkunits *_ws, IEspContext *ctx, const char *netAddress, const char *target, const char *queryid, bool useSSL)
{
    Owned<IClientWsWorkunits> ws = ensureWsWorkunitsClient(_ws, ctx, netAddress, useSSL);

    //using existing WUQuerysetDetails rather than extending WUQueryDetails, to support copying query meta data from prior releases
    Owned<IClientWUQuerySetDetailsRequest> reqQueryInfo = ws->createWUQuerysetDetailsRequest();
    reqQueryInfo->setClusterName(target);
    reqQueryInfo->setQuerySetName(target);
    reqQueryInfo->setFilter(queryid);
    reqQueryInfo->setFilterType("Id");
    return ws->WUQuerysetDetails(reqQueryInfo);
}

void fetchRemoteWorkunit(IClientWsWorkunits *_ws, IEspContext *ctx, const char *netAddress, const char *queryset, const char *query, const char *wuid, StringBuffer &name, StringBuffer &xml, StringBuffer &dllname, MemoryBuffer &dll, StringBuffer &daliServer, bool useSSL)
{
    Owned<IClientWsWorkunits> ws = ensureWsWorkunitsClient(_ws, ctx, netAddress, useSSL);
    Owned<IClientWULogFileRequest> req = ws->createWUFileRequest();
    if (queryset && *queryset)
        req->setQuerySet(queryset);
    if (query && *query)
        req->setQuery(query);
    if (wuid && *wuid)
        req->setWuid(wuid);
    req->setErrorMessageFormat(CErrorMessageFormat_XML);
    req->setType("xml");
    req->updateFileOptions().setFileType(CWUFileType::CWUFileType_XML);
    Owned<IClientWULogFileResponse> resp = ws->WUFile(req);
    if (!resp || resp->getExceptions().ordinality() || !resp->getThefile().length())
        throw MakeStringException(ECLWATCH_CANNOT_GET_WORKUNIT, "Cannot retrieve remote workunit");
    xml.append(resp->getThefile().length(), resp->getThefile().toByteArray());

    req->setType("dll");
    req->updateFileOptions().setFileType(CWUFileType::CWUFileType_DLL);
    resp.setown(ws->WUFile(req));
    if (!resp || resp->getExceptions().ordinality() || !resp->getThefile().length())
        throw MakeStringException(ECLWATCH_CANNOT_GET_WORKUNIT, "Cannot retrieve remote workunit shared object");
    dll.append(resp->getThefile());
    dllname.append(resp->getFileName());
    name.append(resp->getQueryName());
    SocketEndpoint ep;
    checkUseEspOrDaliIP(ep, resp->getDaliServer(), netAddress);
    if (!ep.isNull())
        ep.getEndpointHostText(daliServer);
}

void fetchRemoteWorkunitAndQueryDetails(IClientWsWorkunits *_ws, IEspContext *ctx, const char *netAddress, const char *queryset, const char *query, const char *wuid, StringBuffer &name, StringBuffer &xml, StringBuffer &dllname, MemoryBuffer &dll, StringBuffer &daliServer, Owned<IClientWUQuerySetDetailsResponse> &respQueryInfo, bool useSSL)
{
    Owned<IClientWsWorkunits> ws = ensureWsWorkunitsClient(_ws, ctx, netAddress, useSSL);
    fetchRemoteWorkunit(ws, ctx, netAddress, queryset, query, wuid, name, xml, dllname, dll, daliServer, useSSL);
    respQueryInfo.setown(fetchQueryDetails(ws, ctx, netAddress, queryset, query, useSSL));
}

void doWuFileCopy(IClientFileSpray &fs, IEspWULogicalFileCopyInfo &info, const char *logicalname, const char *cluster, bool isRoxie, bool supercopy)
{
    try
    {
        Owned<IClientCopy> req = fs.createCopyRequest();
        req->setSourceLogicalName(logicalname);
        req->setDestLogicalName(logicalname);
        req->setDestGroup(cluster);
        req->setSuperCopy(supercopy);
        if (isRoxie)
            req->setDestGroupRoxie("Yes");

        Owned<IClientCopyResponse> resp = fs.Copy(req);
        info.setDfuCopyWuid(resp->getResult());
    }
    catch (IException *e)
    {
        StringBuffer msg;
        info.setDfuCopyError(e->errorMessage(msg).str());
        e->Release();
    }
}

bool copyWULogicalFiles(IEspContext &context, IConstWorkUnit &cw, const char *cluster, bool isRoxie, bool copyLocal, IEspWUCopyLogicalClusterFileSections &lfinfo)
{
    if (isEmpty(cluster))
        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "copyWULogicalFiles Cluster parameter not set.");

    Owned<IUserDescriptor> udesc = createUserDescriptor();
    udesc->set(context.queryUserId(), context.queryPassword(), context.querySignature());

    IArrayOf<IEspWULogicalFileCopyInfo> foreign;
    IArrayOf<IEspWULogicalFileCopyInfo> onCluster;
    IArrayOf<IEspWULogicalFileCopyInfo> notOnCluster;
    IArrayOf<IEspWULogicalFileCopyInfo> notFound;

    Owned<IClientFileSpray> fs;
    if (copyLocal)
    {
        fs.setown(createFileSprayClient());
        VStringBuffer url("http://.:%d/FileSpray", 8010);
        fs->addServiceUrl(url.str());
    }

    Owned<IConstWUGraphIterator> graphs = &cw.getGraphs(GraphTypeActivities);
    ForEach(*graphs)
    {
        Owned <IPropertyTree> xgmml = graphs->query().getXGMMLTree(false, false);
        Owned<IPropertyTreeIterator> iter = xgmml->getElements(".//node");
        ForEach(*iter)
        {
            try
            {
                IPropertyTree &node = iter->query();
                ThorActivityKind kind = (ThorActivityKind) node.getPropInt("att[@name='_kind']/@value", TAKnone);

                if(kind==TAKdiskwrite || kind==TAKspillwrite || kind==TAKindexwrite || kind==TAKcsvwrite || kind==TAKxmlwrite || kind==TAKjsonwrite)
                    continue;
                if (node.getPropBool("att[@name='_isSpill']/@value") || node.getPropBool("att[@name='_isTransformSpill']/@value"))
                    continue;

                Owned<IEspWULogicalFileCopyInfo> info = createWULogicalFileCopyInfo();
                const char *logicalname = node.queryProp("att[@name='_indexFileName']/@value");
                if (logicalname)
                    info->setIsIndex(true);
                else
                    logicalname = node.queryProp("att[@name='_fileName']/@value");
                info->setLogicalName(logicalname);
                if (logicalname)
                {
                    if (!strnicmp("~foreign::", logicalname, 10))
                        foreign.append(*info.getClear());
                    else
                    {
                        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(logicalname, udesc, AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
                        if(!df)
                            notFound.append(*info.getClear());
                        else if (df->findCluster(cluster)!=NotFound)
                        {
                            onCluster.append(*info.getClear());
                        }
                        else
                        {
                            StringArray clusters;
                            df->getClusterNames(clusters);
                            info->setClusters(clusters);
                            if (copyLocal)
                            {
                                StringBuffer wuid;
                                bool supercopy = queryDistributedFileDirectory().isSuperFile(logicalname, udesc, NULL);
                                doWuFileCopy(*fs, *info, logicalname, cluster, isRoxie, supercopy);
                            }
                            notOnCluster.append(*info.getClear());
                        }
                    }
                }
            }
            catch(IException *e)
            {
                e->Release();
            }
        }
        lfinfo.setClusterName(cluster);
        lfinfo.setNotOnCluster(notOnCluster);
        lfinfo.setOnCluster(onCluster);
        lfinfo.setForeign(foreign);
        lfinfo.setNotFound(notFound);
    }

    return true;
}

void copyWULogicalFilesToTarget(IEspContext &context, IConstWUClusterInfo &clusterInfo, IConstWorkUnit &cw, IArrayOf<IConstWUCopyLogicalClusterFileSections> &clusterfiles, bool doLocalCopy)
{
    const StringArray &thors = clusterInfo.getThorProcesses();
    ForEachItemIn(i, thors)
    {
        Owned<IEspWUCopyLogicalClusterFileSections> files = createWUCopyLogicalClusterFileSections();
        copyWULogicalFiles(context, cw, thors.item(i), false, doLocalCopy, *files);
        clusterfiles.append(*files.getClear());
    }
    SCMStringBuffer roxie;
    clusterInfo.getRoxieProcess(roxie);
    if (roxie.length())
    {
        Owned<IEspWUCopyLogicalClusterFileSections> files = createWUCopyLogicalClusterFileSections();
        copyWULogicalFiles(context, cw, roxie.str(), true, doLocalCopy, *files);
        clusterfiles.append(*files.getClear());
    }
}

void QueryFilesInUse::loadTarget(IPropertyTree *t, const char *target, unsigned flags)
{
    if (!target || !*target)
        return;

    Owned<IConstWUClusterInfo> clusterInfo = getWUClusterInfoByName(target);
    if (!clusterInfo || !(clusterInfo->getPlatform() == RoxieCluster))
        return;

    Owned<IPropertyTree> queryRegistry = getQueryRegistry(target, true);
    if (!queryRegistry)
        return;

    SCMStringBuffer process;
    clusterInfo->getRoxieProcess(process);
    if (!process.length())
        return;

    Owned<IHpccPackageSet> ps = createPackageSet(process.str());
    const IHpccPackageMap *pm = (ps) ? ps->queryActiveMap(target) : NULL;
    const char *pmid = (pm) ? pm->queryPackageId() : NULL;

    VStringBuffer xpath("%s/@pmid", target);
    const char *pmidPrev = t->queryProp(xpath);
    if ((flags & UFO_RELOAD_TARGETS_CHANGED_PMID) && (pmid || pmidPrev))
    {
        if (!(pmid && pmidPrev) || !streq(pmid, pmidPrev))
            t->removeProp(target);
    }
    IPropertyTree *targetTree = ensurePTree(t, target);
    if (pm)
        targetTree->setProp("@pmid", pmid);

    if (flags & UFO_REMOVE_QUERIES_NOT_IN_QUERYSET)
    {
        Owned<IPropertyTreeIterator> cachedQueries = targetTree->getElements("Query");
        ForEach(*cachedQueries)
        {
            IPropertyTree &cachedQuery = cachedQueries->query();
            VStringBuffer xpath("Query[@id='%s']", cachedQuery.queryProp("@id"));
            if (!queryRegistry->hasProp(xpath))
                targetTree->removeTree(&cachedQuery);
        }
    }

    Owned<IPropertyTreeIterator> queries = queryRegistry->getElements("Query");
    ForEach(*queries)
    {
        if (aborting)
            return;
        IPropertyTree &query = queries->query();
        const char *queryid = query.queryProp("@id");
        if (!queryid || !*queryid)
            continue;
        const char *wuid = query.queryProp("@wuid");
        if (!wuid || !*wuid)
            continue;

        const char *pkgid=NULL;
        if (pm)
         {
             const IHpccPackage *pkg = pm->matchPackage(queryid);
             if (pkg)
                 pkgid = pkg->queryId();
         }
        VStringBuffer xpath("Query[@id='%s']", queryid);
        IPropertyTree *queryTree = targetTree->queryPropTree(xpath);
        if (queryTree)
        {
            const char *cachedPkgid = queryTree->queryProp("@pkgid");
            if (pkgid && *pkgid)
            {
                if (!(flags & UFO_RELOAD_MAPPED_QUERIES) && (cachedPkgid && streq(pkgid, cachedPkgid)))
                    continue;
            }
            else if (!cachedPkgid || !*cachedPkgid)
                continue;
            targetTree->removeTree(queryTree);
            queryTree = NULL;
        }

        Owned<IReferencedFileList> wufiles;
        //Only lock the workunit while the information is being gathered - not when files are resolved
        {
            Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
            Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid);
            if (!cw)
                continue;

            queryTree = targetTree->addPropTree("Query", createPTree("Query"));
            queryTree->setProp("@target", target); //for reference when searching across targets
            queryTree->setProp("@id", queryid);
            if (pkgid && *pkgid)
                queryTree->setProp("@pkgid", pkgid);

            IUserDescriptor **roxieUser = roxieUserMap.getValue(target);
            wufiles.setown(createReferencedFileList(roxieUser ? *roxieUser : NULL, true, true));
            wufiles->addFilesFromQuery(cw, pm, queryid);
        }
        if (aborting)
            return;
        StringArray locations;
        locations.append(process.str());
        wufiles->resolveFiles(locations, NULL, NULL, NULL, true, true, true, false, false);

        Owned<IReferencedFileIterator> files = wufiles->getFiles();
        ForEach(*files)
        {
            if (aborting)
                return;
            IReferencedFile &rf = files->query();
            //if (!(rf.getFlags() & RefSubFile))
            //    continue;
            const char *lfn = rf.getLogicalName();
            if (!lfn || !*lfn)
                continue;

            if (!queryTree->hasProp(xpath.setf("File[@lfn='%s']", lfn)))
            {
                IPropertyTree *fileTree = queryTree->addPropTree("File", createPTree("File"));
                fileTree->setProp("@lfn", lfn);
                if (rf.getFlags() & RefFileSuper)
                {
                    fileTree->setPropBool("@super", true);
                    ForEachItemIn(i, rf.getSubFileNames())
                    {
                        IPropertyTree *subfileTree = fileTree->addPropTree("SubFile");
                        subfileTree->setProp("@lfn", rf.getSubFileNames().item(i));
                    }
                }
                if (rf.getFlags() & RefFileNotFound)
                    fileTree->setPropBool("@notFound", true);
                const char *fpkgid = rf.queryPackageId();
                if (fpkgid && *fpkgid)
                    fileTree->setProp("@pkgid", fpkgid);
                if (rf.getFileSize())
                    fileTree->setPropInt64("@size", rf.getFileSize());
                if (rf.getNumParts())
                    fileTree->setPropInt("@numparts", rf.getNumParts());
            }
        }
    }
}

void QueryFilesInUse::loadTargets(IPropertyTree *t, unsigned flags)
{
#ifdef _CONTAINERIZED
    Owned<IStringIterator> targets = config::getContainerTargets("roxie", nullptr);
#else
    Owned<IStringIterator> targets = getTargetClusters("RoxieCluster", nullptr);
#endif
    SCMStringBuffer s;
    ForEach(*targets)
    {
        if (aborting)
            return;
        loadTarget(t, targets->str(s).str(), flags);
    }
}

void QueryFilesInUse::updateUsers()
{
    SCMStringBuffer target;

#ifdef _CONTAINERIZED
    Owned<IStringIterator> clusters = config::getContainerTargets("roxie", nullptr);
#else
    Owned<IStringIterator> clusters = getTargetClusters("RoxieCluster", nullptr);
#endif
    ForEach(*clusters)
    {
        Owned<IConstWUClusterInfo> info = getWUClusterInfoByName(clusters->str(target).str());
        Owned<IUserDescriptor> user = createUserDescriptor();
        const char *ldapUser = info->getLdapUser();
        if (!isEmptyString(ldapUser))
            user->set(ldapUser, info->getLdapPassword());
        else
            user->set("roxie", "");
        roxieUserMap.setValue(target.str(), user.getClear());
    }
}

IPropertyTreeIterator *QueryFilesInUse::findAllQueriesUsingFile(const char *lfn)
{
    if (!lfn || !*lfn)
        return NULL;

    Owned<IPropertyTree> t = getTree();
    VStringBuffer xpath("*/Query[File/@lfn='%s']", lfn);
    return t->getElements(xpath);
}

IPropertyTreeIterator *QueryFilesInUse::findQueriesUsingFile(const char *target, const char *lfn, StringAttr &pmid)
{
    if (!lfn || !*lfn)
        return NULL;
    if (!target || !*target)
        return findAllQueriesUsingFile(lfn);
    Owned<IPropertyTree> t = getTree();
    IPropertyTree *targetTree = t->queryPropTree(target);
    if (!targetTree)
        return NULL;
    pmid.set(targetTree->queryProp("@pmid"));

    VStringBuffer xpath("Query[File/@lfn='%s']", lfn);
    return targetTree->getElements(xpath);
}

bool CWsWorkunitsEx::onWUCopyLogicalFiles(IEspContext &context, IEspWUCopyLogicalFilesRequest &req, IEspWUCopyLogicalFilesResponse &resp)
{
    StringBuffer wuid(req.getWuid());
    WsWuHelpers::checkAndTrimWorkunit("WUCopyLogicalFiles", wuid);

    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid.str());
    if (!cw)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot open workunit %s", wuid.str());
    ensureWsWorkunitAccess(context, *cw, SecAccess_Full);

    resp.setWuid(wuid.str());

    StringAttr cluster;
    if (notEmpty(req.getCluster()))
        cluster.set(req.getCluster());
    else
        cluster.set(cw->queryClusterName());
    validateTargetName(cluster);

    Owned<IConstWUClusterInfo> clusterInfo = getWUClusterInfoByName(cluster);

    IArrayOf<IConstWUCopyLogicalClusterFileSections> clusterfiles;
    PROGLOG("WUCopyLogicalFiles: %s", wuid.str());
    copyWULogicalFilesToTarget(context, *clusterInfo, *cw, clusterfiles, req.getCopyLocal());
    resp.setClusterFiles(clusterfiles);

    return true;
}

static inline unsigned remainingMsWait(unsigned wait, unsigned start)
{
    if (wait==0 || wait==(unsigned)-1)
        return wait;
    unsigned waited = msTick()-start;
    return (wait>waited) ? wait-waited : 0;
}

#ifndef _CONTAINERIZED
bool reloadCluster(IConstWUClusterInfo *clusterInfo, unsigned wait)
#else
bool reloadCluster(MapStringToMyClass<ISmartSocketFactory> &roxieConnMap, const char *target, unsigned wait)
#endif
{
#ifndef _CONTAINERIZED
    if (0==wait || !clusterInfo || clusterInfo->getPlatform()!=RoxieCluster)
        return true;

    const SocketEndpointArray &addrs = clusterInfo->getRoxieServers();
    if (addrs.length() == 0)
        return true;
#else
    if (0==wait)
        return true;
    ISmartSocketFactory *conn = roxieConnMap.getValue(target);
    if (!conn)
        return true;
    if (!k8s::isActiveService(target))
        return true;
#endif
    try
    {
#ifndef _CONTAINERIZED
        Owned<IPropertyTree> result = sendRoxieControlAllNodes(addrs.item(0), "<control:reload/>", false, wait);
#else
        Owned<IPropertyTree> result = sendRoxieControlAllNodes(conn, "<control:reload/>", false, wait, ROXIECONNECTIONTIMEOUT);
#endif
        const char *status = result->queryProp("Endpoint[1]/Status");
        if (!status || !strieq(status, "ok"))
            return false;
    }
    catch(IMultiException *me)
    {
        StringBuffer err;
        IERRLOG("ERROR control:reloading roxie query info %s", me->errorMessage(err.append(me->errorCode()).append(' ')).str());
        me->Release();
        return false;
    }
    catch(IException *e)
    {
        StringBuffer err;
        IERRLOG("ERROR control:reloading roxie query info %s", e->errorMessage(err.append(e->errorCode()).append(' ')).str());
        e->Release();
        return false;
    }
    return true;
}

#ifndef _CONTAINERIZED
bool reloadCluster(const char *cluster, unsigned wait)
{
    Owned <IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(cluster);
    return (clusterInfo) ? reloadCluster(clusterInfo, wait) : true;
}
#endif

static inline void updateQuerySetting(bool ignore, IPropertyTree *queryTree, const char *xpath, int value)
{
    if (ignore || !queryTree)
        return;
    if (value!=0)
        queryTree->setPropInt(xpath, value);
    else
        queryTree->removeProp(xpath);
}

static inline void updateTimeLimitSetting(IPropertyTree *queryTree, bool valueNotSet, int value, IConstQuerySetQuery *srcInfo=NULL)
{
    if (valueNotSet && srcInfo && !srcInfo->getTimeLimit_isNull())
    {
        value = srcInfo->getTimeLimit();
        valueNotSet=false;
    }
    updateQuerySetting(valueNotSet, queryTree, "@timeLimit", value);
}

static inline void updateWarnTimeLimitSetting(IPropertyTree *queryTree, bool valueNotSet, int value, IConstQuerySetQuery *srcInfo=NULL)
{
    if (valueNotSet && srcInfo && !srcInfo->getWarnTimeLimit_isNull())
    {
        value = srcInfo->getWarnTimeLimit();
        valueNotSet=false;
    }
    updateQuerySetting(valueNotSet, queryTree, "@warnTimeLimit", value);
}

static inline unsigned __int64 memoryLimitUInt64FromString(const char *value)
{
    if (!value || !*value || !isdigit(*value))
        return 0;
    unsigned __int64 result = (*value - '0');
    const char *s = value+1;
    while (isdigit(*s))
    {
        result = 10 * result + ((*s) - '0');
        s++;
    }
    if (*s)
    {
        const char unit = toupper(*s++);
        if (*s && !strieq("B", s)) //more?
            return 0;
        switch (unit)
        {
            case 'E':
                result <<=60;
                break;
            case 'P':
                result <<=50;
                break;
            case 'T':
                result <<=40;
                break;
            case 'G':
                result <<=30;
                break;
            case 'M':
                result <<=20;
                break;
            case 'K':
                result <<=10;
                break;
            case 'B':
                break;
            default:
                return 0;
        }
    }
    return result;
}

const char memUnitAbbrev[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E'};
#define MAX_MEMUNIT_ABBREV 6

static inline StringBuffer &memoryLimitStringFromUInt64(StringBuffer &s, unsigned __int64 in)
{
    if (!in)
        return s;
    unsigned __int64 value = in;
    unsigned char unit = 0;
    while (!(value & 0x3FF) && unit < MAX_MEMUNIT_ABBREV)
    {
        value >>= 10;
        unit++;
    }
    return s.append(value).append(memUnitAbbrev[unit]);
}

static inline void updateMemoryLimitSetting(IPropertyTree *queryTree, const char *value, IConstQuerySetQuery *srcInfo=NULL)
{
    if (!queryTree)
        return;
    if (!value && srcInfo)
        value = srcInfo->getMemoryLimit();
    if (!value)
        return;
    unsigned __int64 limit = memoryLimitUInt64FromString(value);
    if (0==limit)
        queryTree->removeProp("@memoryLimit");
    else
        queryTree->setPropInt64("@memoryLimit", limit);
}

enum QueryPriority {
    QueryPriorityNone = -1,
    QueryPriorityLow = 0,
    QueryPriorityHigh = 1,
    QueryPrioritySLA = 2,
    QueryPriorityInvalid = 3
};

static inline const char *getQueryPriorityName(int value)
{
    switch (value)
    {
    case QueryPriorityLow:
        return "LOW";
    case QueryPriorityHigh:
        return "HIGH";
    case QueryPrioritySLA:
        return "SLA";
    case QueryPriorityNone:
        return "NONE";
    }
    return "INVALID";
}
static inline void updateQueryPriority(IPropertyTree *queryTree, const char *value, IConstQuerySetQuery *srcInfo=NULL)
{
    if (!queryTree)
        return;
    if ((!value || !*value) && srcInfo)
        value = srcInfo->getPriority();
    if (!value || !*value)
        return;
    int priority = QueryPriorityInvalid;
    if (strieq("LOW", value))
        priority=QueryPriorityLow;
    else if (strieq("HIGH", value))
        priority=QueryPriorityHigh;
    else if (strieq("SLA", value))
        priority=QueryPrioritySLA;
    else if (strieq("NONE", value))
        priority=QueryPriorityNone;

    switch (priority)
    {
    case QueryPriorityInvalid:
        break;
    case QueryPriorityNone:
        queryTree->removeProp("@priority");
        break;
    default:
        queryTree->setPropInt("@priority", priority);
        break;
    }
}

void gatherFileErrors(IReferencedFileList *files, IArrayOf<IConstLogicalFileError> &errors)
{
    if (!files)
        return;

    Owned<IReferencedFileIterator> it = files->getFiles();
    ForEach(*it)
    {
        IReferencedFile &file = it->query();
        unsigned flags = file.getFlags();
        if (!(flags & (RefFileNotFound | RefFileCopyInfoFailed)))
            continue;
        StringBuffer msg;
        if (flags & RefFileOptional)
            msg.append("OPT ");
        if (flags & RefFileNotFound)
            msg.append("Not Found");
        else
            msg.append("Copy Failed");

        Owned<IEspLogicalFileError> error = createLogicalFileError();
        error->setLogicalName(file.getLogicalName());
        error->setError(msg);
        errors.append(*static_cast<IConstLogicalFileError*>(error.getClear()));
    }
}

class QueryFileCopier
{
public:
    QueryFileCopier(const char *target_) : target(target_) {}
    void init(IEspContext &context, bool allowForeignFiles, const char *jobname)
    {
        files.setown(createReferencedFileList(context.queryUserId(), context.queryPassword(), allowForeignFiles, false, jobname));
#ifndef _CONTAINERIZED
        clusterInfo.setown(getTargetClusterInfo(target));
        StringBufferAdaptor sba(process);
        if (clusterInfo && clusterInfo->getPlatform()==RoxieCluster)
            clusterInfo->getRoxieProcess(sba);
        if (!process.length())
            return;
#else
        process.set(target);
#endif

        ps.setown(createPackageSet(process.str()));
        if (ps)
            pm = ps->queryActiveMap(target);
    }
    void copy(StringBuffer &publisherWuid, IConstWorkUnit *cw, unsigned updateFlags)
    {
        StringBuffer queryid;
        if (queryname && *queryname)
            queryname = queryid.append(queryname).append(".0").str(); //prepublish dummy version number to support fuzzy match like queries="myquery.*" in package
        files->addFilesFromQuery(cw, pm, queryname);

        StringArray locations;
#ifdef _CONTAINERIZED
        StringBuffer targetPlane;
        getRoxieDirectAccessPlanes(locations, targetPlane, target, true);
        const char * targetPlaneOrGroup = targetPlane;
#else
        const char * targetPlaneOrGroup = process;
        locations.append(targetPlaneOrGroup);
#endif
        files->resolveFiles(locations, remoteLocation, remotePrefix, srcCluster, !(updateFlags & (DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM | DALI_UPDATEF_SUPERFILES)), true, false, true, (updateFlags & DFU_UPDATEF_REMOTESTORAGE));
        Owned<IDFUhelper> helper = createIDFUhelper();
        files->setDfuQueue(dfu_queue);
#ifdef _CONTAINERIZED
        files->cloneAllInfo(publisherWuid, targetPlaneOrGroup, updateFlags, helper, true, true, 0, 1, 0, nullptr);
#else
        StringBuffer defReplicateFolder;
        getConfigurationDirectory(NULL, "data2", "roxie", process.str(), defReplicateFolder);
        files->cloneAllInfo(publisherWuid, targetPlaneOrGroup, updateFlags, helper, true, true, clusterInfo->getRoxieRedundancy(), clusterInfo->getChannelsPerNode(), clusterInfo->getRoxieReplicateOffset(), defReplicateFolder);
#endif
    }

    void gatherFileErrors(IArrayOf<IConstLogicalFileError> &errors)
    {
        ::gatherFileErrors(files, errors);
    }

    void setKeyCompression(const char * keyCompression)
    {
        files->setKeyCompression(keyCompression);
    }

private:
#ifndef _CONTAINERIZED
    Owned <IConstWUClusterInfo> clusterInfo;
#endif
    Owned<IHpccPackageSet> ps;
    const IHpccPackageMap *pm = nullptr;
    StringAttr target;

public:
    Owned<IReferencedFileList> files;

    StringBuffer process;
    StringAttr remoteLocation;
    StringAttr remotePrefix;
    StringAttr srcCluster;
    StringAttr queryname;
    StringAttr dfu_queue;
};

#ifndef _CONTAINERIZED
bool CWsWorkunitsEx::isQuerySuspended(const char* query, IConstWUClusterInfo* clusterInfo, unsigned wait, StringBuffer& errorMessage)
#else
bool CWsWorkunitsEx::isQuerySuspended(const char* query, const char* target, unsigned wait, StringBuffer& errorMessage)
#endif
{
    try
    {
#ifndef _CONTAINERIZED
        if (0==wait || !clusterInfo || clusterInfo->getPlatform()!=RoxieCluster)
            return false;

        const SocketEndpointArray &addrs = clusterInfo->getRoxieServers();
        if (addrs.length() < 1)
            return false;
#else
        if (0==wait)
            return false;
        ISmartSocketFactory *conn = roxieConnMap.getValue(target);
        if (!conn)
            return false;
        if (!k8s::isActiveService(target))
            return false;
#endif
        StringBuffer control;
        control.appendf("<control:queries><Query id='%s'/></control:queries>",  query);
#ifndef _CONTAINERIZED
        Owned<IPropertyTree> result = sendRoxieControlAllNodes(addrs.item(0), control.str(), false, wait);
#else
        Owned<IPropertyTree> result = sendRoxieControlAllNodes(conn, control, false, wait, ROXIECONNECTIONTIMEOUT);
#endif
        if (!result)
            return false;

        Owned<IPropertyTreeIterator> suspendedQueries = result->getElements("Endpoint/Queries/Query[@suspended='1']");
        if (!suspendedQueries->first())
            return false;

        errorMessage.set(suspendedQueries->query().queryProp("@error"));
        return true;
    }
    catch(IMultiException *me)
    {
        StringBuffer err;
        IERRLOG("ERROR control:queries roxie query info %s", me->errorMessage(err.append(me->errorCode()).append(' ')).str());
        me->Release();
        return false;
    }
    catch(IException *e)
    {
        StringBuffer err;
        IERRLOG("ERROR control:queries roxie query info %s", e->errorMessage(err.append(e->errorCode()).append(' ')).str());
        e->Release();
        return false;
    }
}

template<class TRequest, class TResponse>
static bool handlePublisherResponse(TRequest &req, TResponse &resp, const StringBuffer &publisherWuid)
{
    if (!publisherWuid.isEmpty())
        resp.setDfuPublisherWuid(publisherWuid);

    if (req.getOnlyCopyFiles() || (req.getStopIfFilesCopied() && !publisherWuid.isEmpty()))
    {
        if (!publisherWuid.isEmpty())
        {
            StringBuffer statemsg;
            resp.setDfuPublisherState(encodeDFUstate(DFUstate_started, statemsg));
        }
        return true;
    }

    if (!publisherWuid.isEmpty())
    {
        Owned<IDFUWorkUnitFactory> factory = getDFUWorkUnitFactory();
        Owned<IConstDFUWorkUnit> dfuPublisherWu = factory->openWorkUnit(publisherWuid, false);
        DFUstate state = dfuPublisherWu->pollForCompletion(1000*60*30); //make an option, 30 min timeout for now, tbd
        StringBuffer statemsg;
        resp.setDfuPublisherState(encodeDFUstate(state, statemsg));
        if (state != DFUstate_finished)
            return true;
    }
    return false;
}


bool CWsWorkunitsEx::onWUPublishWorkunit(IEspContext &context, IEspWUPublishWorkunitRequest & req, IEspWUPublishWorkunitResponse & resp)
{
    if (req.getDontCopyFiles() && req.getOnlyCopyFiles())
        throw makeStringException(ECLWATCH_INVALID_INPUT,"Cannot combine 'dont-copy-files' and 'only-copy-files'");

    StringBuffer wuid(req.getWuid());
    WsWuHelpers::checkAndTrimWorkunit("WUPublishWorkunit", wuid);

    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid.str());
    if (!cw)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot find the workunit %s", wuid.str());
    ensureWsWorkunitAccess(context, *cw, SecAccess_Full);

    resp.setWuid(wuid.str());

    StringAttr queryName;
    if (notEmpty(req.getQueryName()))
        queryName.set(req.getQueryName());
    else if (notEmpty(req.getJobName()))
        queryName.set(req.getJobName());
    else
        queryName.set(cw->queryJobName());
    if (!queryName.length())
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Query/Job name not defined for publishing workunit %s", wuid.str());

    StringAttr target;
    if (notEmpty(req.getCluster()))
        target.set(req.getCluster());
    else
        target.set(cw->queryClusterName());
    validateTargetName(target);

    DBGLOG("%s publishing wuid %s to target %s as query %s", nullText(context.queryUserId()), wuid.str(), target.str(), queryName.str());

    StringBuffer daliIP;
    StringBuffer srcCluster;
    StringBuffer srcPrefix;
    splitDerivedDfsLocation(req.getRemoteDali(), srcCluster, daliIP, srcPrefix, req.getSourceProcess(),req.getSourceProcess(), NULL, NULL);

    if (srcCluster.length())
    {
        if (!validateDataPlaneName(daliIP, srcCluster))
            throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Process cluster %s not found on %s DALI", srcCluster.str(), daliIP.length() ? daliIP.str() : "local");
    }
    unsigned updateFlags = 0;
    if (req.getUpdateDfs())
        updateFlags |= (DALI_UPDATEF_SUPERFILES | DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM);
    if (req.getUpdateCloneFrom())
        updateFlags |= DALI_UPDATEF_CLONE_FROM;
    if (req.getUpdateSuperFiles())
        updateFlags |= DALI_UPDATEF_SUPERFILES;
    if (req.getAppendCluster())
        updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;
    if (req.getDfuCopyFiles())
        updateFlags |= DFU_UPDATEF_COPY;
    if (req.getDfuOverwrite())
        updateFlags |= DFU_UPDATEF_OVERWRITE;

    StringBuffer publisherWuid(req.getDfuPublisherWuid());
    if (!req.getDontCopyFiles())
    {
        StringBuffer remoteLocation;
        StringBuffer srcCluster;
        StringBuffer srcPrefix;

        unsigned updateFlags = 0;
        const char *remoteStr = req.getRemoteStorage();
        if (!isEmptyString(remoteStr))
            updateFlags = DFU_UPDATEF_REMOTESTORAGE;
        else
            remoteStr = req.getRemoteDali();

        splitDerivedDfsLocation(remoteStr, srcCluster, remoteLocation, srcPrefix, req.getSourceProcess(),req.getSourceProcess(), NULL, NULL);

        if (srcCluster.length())
        {
            if (!validateDataPlaneName(remoteLocation, srcCluster))
                throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Process cluster %s not found on %s remote", srcCluster.str(), remoteLocation.length() ? remoteLocation.str() : "local");
        }
        if (req.getUpdateDfs())
            updateFlags |= (DALI_UPDATEF_SUPERFILES | DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM);
        if (req.getUpdateCloneFrom())
            updateFlags |= DALI_UPDATEF_CLONE_FROM;
        if (req.getUpdateSuperFiles())
            updateFlags |= DALI_UPDATEF_SUPERFILES;
        if (req.getAppendCluster())
            updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;
        if (req.getDfuCopyFiles())
            updateFlags |= DFU_UPDATEF_COPY;
        if (req.getDfuOverwrite())
            updateFlags |= DFU_UPDATEF_OVERWRITE;

        QueryFileCopier cpr(target);
        cpr.init(context, req.getAllowForeignFiles(), queryName);
        cpr.remoteLocation.set(remoteLocation);
        cpr.remotePrefix.set(srcPrefix);
        cpr.srcCluster.set(srcCluster);
        cpr.queryname.set(queryName);
        cpr.dfu_queue.set(req.getDfuQueue());
        cpr.setKeyCompression(req.getKeyCompression());
        cpr.copy(publisherWuid, cw, updateFlags);

        if (req.getIncludeFileErrors())
            cpr.gatherFileErrors(resp.getFileErrors());
        if (!publisherWuid.isEmpty())
            resp.setDfuPublisherWuid(publisherWuid);
    }

    if (handlePublisherResponse(req, resp, publisherWuid))
        return true;

    WorkunitUpdate wu(&cw->lock());
    if (notEmpty(req.getWorkUnitJobName()))
        wu->setJobName(req.getWorkUnitJobName());
    else if (req.getUpdateWorkUnitName() && notEmpty(req.getJobName()))
        wu->setJobName(req.getJobName());

    StringBuffer queryId;
    WUQueryActivationOptions activate = (WUQueryActivationOptions)req.getActivate();
    addQueryToQuerySet(wu, target.str(), queryName.str(), activate, queryId, context.queryUserId());
    if (req.getMemoryLimit() || !req.getTimeLimit_isNull() || !req.getWarnTimeLimit_isNull() || req.getPriority() || req.getComment())
    {
        Owned<IPropertyTree> queryTree = getQueryById(target.str(), queryId, false);
        updateMemoryLimitSetting(queryTree, req.getMemoryLimit());
        updateQuerySetting(req.getTimeLimit_isNull(), queryTree, "@timeLimit", req.getTimeLimit());
        updateQuerySetting(req.getWarnTimeLimit_isNull(), queryTree, "@warnTimeLimit", req.getWarnTimeLimit());
        updateQueryPriority(queryTree, req.getPriority());
        if (req.getComment())
            queryTree->setProp("@comment", req.getComment());
    }
    wu->commit();
    wu.clear();

    if (queryId.length())
        resp.setQueryId(queryId.str());
    resp.setQueryName(queryName.str());
    resp.setQuerySet(target.str());

    bool reloadFailed = false;
#ifndef _CONTAINERIZED
    Owned <IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target.str());
#endif
    if (0!=req.getWait() && !req.getNoReload())
#ifndef _CONTAINERIZED
        reloadFailed = !reloadCluster(clusterInfo, (unsigned)req.getWait());
#else
        reloadFailed = !reloadCluster(roxieConnMap, target, (unsigned)req.getWait());
#endif
    resp.setReloadFailed(reloadFailed);

    double version = context.getClientVersion();
    if (version > 1.38)
    {
        StringBuffer errorMessage;
#ifndef _CONTAINERIZED
        if (!reloadFailed && !req.getNoReload() && isQuerySuspended(queryName.str(), clusterInfo, (unsigned)req.getWait(), errorMessage))
#else
        if (!reloadFailed && !req.getNoReload() && isQuerySuspended(queryName.str(), target, (unsigned)req.getWait(), errorMessage))
#endif
        {
            resp.setSuspended(true);
            resp.setErrorMessage(errorMessage);
        }
    }

    return true;
}

bool CWsWorkunitsEx::onWUQuerysets(IEspContext &context, IEspWUQuerysetsRequest & req, IEspWUQuerysetsResponse & resp)
{
    IArrayOf<IEspQuerySet> querySets;
#ifdef _CONTAINERIZED
    Owned<IStringIterator> targets = config::getContainerTargets(nullptr, nullptr);
#else
    Owned<IStringIterator> targets = getTargetClusters(nullptr, nullptr);
#endif

    SCMStringBuffer target;
    ForEach(*targets)
    {
        Owned<IEspQuerySet> qs = createQuerySet();
        qs->setQuerySetName(targets->str(target).str());
        querySets.append(*qs.getClear());

    }
    resp.setQuerysets(querySets);
    return true;
}

void addClusterQueryStates(IPropertyTree* queriesOnCluster, const char *target, const char *id, IArrayOf<IEspClusterQueryState>& clusterStates, double version)
{
    if (queriesOnCluster)
        queriesOnCluster = queriesOnCluster->queryPropTree("Endpoint[1]/Queries[1]");
    if (!queriesOnCluster)
        return;

    int reporting = queriesOnCluster->getPropInt("@reporting");

    Owned<IEspClusterQueryState> clusterState = createClusterQueryState();
    clusterState->setCluster(target);

    VStringBuffer xpath("Query[@id='%s']", id);
    IPropertyTree *query = queriesOnCluster->queryPropTree(xpath.str());
    if (!query)
        clusterState->setState("Not Found");
    else
    {
        int suspended = query->getPropInt("@suspended");
        const char* error = query->queryProp("@error");
        if (0==suspended)
            clusterState->setState("Available");
        else
        {
            clusterState->setState("Suspended");
            if (suspended<reporting)
                clusterState->setMixedNodeStates(true);
        }
        if (error && *error)
            clusterState->setErrors(error);
    }

    clusterStates.append(*clusterState.getClear());
}

template<typename T>
void checkAndSetQueryPriority(double version, IPropertyTree *query, T *ret)
{
    if (!query->hasProp("@priority"))
        return;

    int priorityID = query->getPropInt("@priority");
    ret->setPriority(getQueryPriorityName(priorityID));
    if (version >= 1.83)
        ret->setPriorityID(priorityID);
}

void gatherQuerySetQueryDetails(IEspContext &context, IPropertyTree *query, IEspQuerySetQuery *queryInfo, const char *cluster, IPropertyTree *queriesOnCluster)
{
    double version = context.getClientVersion();
    queryInfo->setId(query->queryProp("@id"));
    queryInfo->setName(query->queryProp("@name"));
    queryInfo->setDll(query->queryProp("@dll"));
    queryInfo->setWuid(query->queryProp("@wuid"));
    queryInfo->setSuspended(query->getPropBool("@suspended", false));
    if (query->hasProp("@memoryLimit"))
    {
        StringBuffer s;
        memoryLimitStringFromUInt64(s, query->getPropInt64("@memoryLimit"));
        queryInfo->setMemoryLimit(s);
    }
    if (query->hasProp("@timeLimit"))
        queryInfo->setTimeLimit(query->getPropInt("@timeLimit"));
    if (query->hasProp("@warnTimeLimit"))
        queryInfo->setWarnTimeLimit(query->getPropInt("@warnTimeLimit"));
    checkAndSetQueryPriority(version, query, queryInfo);
    if (query->hasProp("@comment"))
        queryInfo->setComment(query->queryProp("@comment"));
    if (query->hasProp("@snapshot"))
        queryInfo->setSnapshot(query->queryProp("@snapshot"));
    if (version >= 1.46)
    {
        queryInfo->setPublishedBy(query->queryProp("@publishedBy"));
        queryInfo->setIsLibrary(query->getPropBool("@isLibrary"));
    }
    if (queriesOnCluster)
    {
        IArrayOf<IEspClusterQueryState> clusters;
        addClusterQueryStates(queriesOnCluster, cluster, query->queryProp("@id"), clusters, version);
        queryInfo->setClusters(clusters);
    }
}

void gatherQuerySetAliasDetails(IPropertyTree *alias, IEspQuerySetAlias *aliasInfo)
{
    aliasInfo->setName(alias->queryProp("@name"));
    aliasInfo->setId(alias->queryProp("@id"));
}

void retrieveAllQuerysetDetails(IEspContext &context, IPropertyTree *registry, IArrayOf<IEspQuerySetQuery> &queries, IArrayOf<IEspQuerySetAlias> &aliases, const char *cluster=NULL, IPropertyTree *queriesOnCluster=NULL, const char *type=NULL, const char *value=NULL)
{
    Owned<IPropertyTreeIterator> regQueries = registry->getElements("Query");
    ForEach(*regQueries)
    {
        IPropertyTree &query = regQueries->query();
        Owned<IEspQuerySetQuery> q = createQuerySetQuery();
        gatherQuerySetQueryDetails(context, &query, q, cluster, queriesOnCluster);

        if (isEmpty(cluster) || isEmpty(type) || isEmpty(value) || !strieq(type, "Status"))
            queries.append(*q.getClear());
        else
        {
            IArrayOf<IConstClusterQueryState>& cs = q->getClusters();
            ForEachItemIn(i, cs)
            {
                IConstClusterQueryState& c = cs.item(i);
                if (strieq(c.getCluster(), cluster) && (strieq(value, "All") || strieq(c.getState(), value)))
                {
                    queries.append(*q.getClear());
                    break;
                }
            }
        }
    }

    Owned<IPropertyTreeIterator> regAliases = registry->getElements("Alias");
    ForEach(*regAliases)
    {
        IPropertyTree &alias = regAliases->query();
        Owned<IEspQuerySetAlias> a = createQuerySetAlias();
        gatherQuerySetAliasDetails(&alias, a);
        aliases.append(*a.getClear());
    }
}

void retrieveQuerysetDetailsFromAlias(IEspContext &context, IPropertyTree *registry, const char *name, IArrayOf<IEspQuerySetQuery> &queries, IArrayOf<IEspQuerySetAlias> &aliases, const char *cluster, IPropertyTree *queriesOnCluster)
{
    StringBuffer xpath;
    xpath.append("Alias[@name='").append(name).append("']");
    Owned<IPropertyTreeIterator> regAliases = registry->getElements(xpath.str());
    if (!regAliases->first())
    {
        UWARNLOG("Alias %s not found", name);
        return;
    }

    ForEach(*regAliases)
    {
        IPropertyTree& alias = regAliases->query();
        Owned<IEspQuerySetAlias> a = createQuerySetAlias();
        gatherQuerySetAliasDetails(&alias, a);
        xpath.clear().append("Query[@id='").append(a->getId()).append("']");
        aliases.append(*a.getLink());

        IPropertyTree *query = registry->queryPropTree(xpath);
        if (!query)
        {
            UWARNLOG("No matching Query %s found for Alias %s", a->getId(), name);
            return;
        }

        Owned<IEspQuerySetQuery> q = createQuerySetQuery();
        gatherQuerySetQueryDetails(context, query, q, cluster, queriesOnCluster);
        queries.append(*q.getClear());
    }
}

void retrieveQuerysetDetailsFromQuery(IEspContext &context, IPropertyTree *registry, const char *value, const char *type, IArrayOf<IEspQuerySetQuery> &queries, IArrayOf<IEspQuerySetAlias> &aliases, const char *cluster=NULL, IPropertyTree *queriesOnCluster=NULL)
{
    if (!strieq(type, "Id") && !strieq(type, "Name"))
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Unrecognized queryset filter type %s", type);

    StringBuffer attributeName(type);
    StringBuffer xpath;
    xpath.clear().append("Query[@").append(attributeName.toLowerCase()).append("='").append(value).append("']");
    Owned<IPropertyTreeIterator> regQueries = registry->getElements(xpath.str());
    if (!regQueries->first())
    {
        UWARNLOG("No matching Query %s found for %s", value, type);
        return;
    }

    ForEach(*regQueries)
    {
        IPropertyTree& query = regQueries->query();

        Owned<IEspQuerySetQuery> q = createQuerySetQuery();
        gatherQuerySetQueryDetails(context, &query, q, cluster, queriesOnCluster);
        xpath.clear().append("Alias[@id='").append(q->getId()).append("']");
        queries.append(*q.getClear());

        Owned<IPropertyTreeIterator> regAliases = registry->getElements(xpath.str());
        ForEach(*regAliases)
        {
            IPropertyTree &alias = regAliases->query();
            Owned<IEspQuerySetAlias> a = createQuerySetAlias();
            gatherQuerySetAliasDetails(&alias, a);
            aliases.append(*a.getClear());
        }
    }
}

void retrieveQuerysetDetails(IEspContext &context, IPropertyTree *registry, const char *type, const char *value, IArrayOf<IEspQuerySetQuery> &queries, IArrayOf<IEspQuerySetAlias> &aliases, const char *cluster=NULL, IPropertyTree *queriesOnCluster=NULL)
{
    if (strieq(type, "All"))
        return retrieveAllQuerysetDetails(context, registry, queries, aliases, cluster, queriesOnCluster);
    if (!value || !*value)
        return;
    if (strieq(type, "Alias"))
        return retrieveQuerysetDetailsFromAlias(context, registry, value, queries, aliases, cluster, queriesOnCluster);
    if (strieq(type, "Status") && !isEmpty(cluster))
        return retrieveAllQuerysetDetails(context, registry, queries, aliases, cluster, queriesOnCluster, type, value);
    return retrieveQuerysetDetailsFromQuery(context, registry, value, type, queries, aliases, cluster, queriesOnCluster);
}

void retrieveQuerysetDetails(IEspContext &context, IArrayOf<IEspWUQuerySetDetail> &details, IPropertyTree *registry, const char *type, const char *value, const char *cluster=NULL, IPropertyTree *queriesOnCluster=NULL)
{
    if (!registry)
        return;

    IArrayOf<IEspQuerySetQuery> queries;
    IArrayOf<IEspQuerySetAlias> aliases;
    retrieveQuerysetDetails(context, registry, type, value, queries, aliases, cluster, queriesOnCluster);

    Owned<IEspWUQuerySetDetail> detail = createWUQuerySetDetail();
    detail->setQuerySetName(registry->queryProp("@id"));
    detail->setQueries(queries);
    detail->setAliases(aliases);
    details.append(*detail.getClear());
}

void retrieveQuerysetDetails(IEspContext &context, IArrayOf<IEspWUQuerySetDetail> &details, const char *queryset, const char *type, const char *value, const char *cluster=NULL, IPropertyTree *queriesOnCluster=NULL)
{
    if (!queryset || !*queryset)
        return;
    Owned<IPropertyTree> registry = getQueryRegistry(queryset, true);
    if (!registry)
        return;
    retrieveQuerysetDetails(context, details, registry, type, value, cluster, queriesOnCluster);
}

#ifndef _CONTAINERIZED
IPropertyTree *getQueriesOnCluster(const char *target, const char *queryset, StringArray *queryIDs, bool checkAllNodes)
#else
IPropertyTree *getQueriesOnCluster(const char *target, const char *queryset, StringArray *queryIDs, bool checkAllNodes,
    MapStringToMyClass<ISmartSocketFactory> &roxieConnMap)
#endif
{
    if (isEmpty(target))
        target = queryset;
    else if (queryset && *queryset && !strieq(target, queryset))
        throw makeStringExceptionV(ECLWATCH_QUERYSET_NOT_ON_CLUSTER, "Target %s and QuerySet %s should match", target, queryset);

#ifndef _CONTAINERIZED
    Owned<IConstWUClusterInfo> info = getTargetClusterInfo(target);
    if (!info)
        throw MakeStringException(ECLWATCH_CANNOT_RESOLVE_CLUSTER_NAME, "Cluster %s not found", target);
    if (info->getPlatform()!=RoxieCluster)
        return NULL;
    const SocketEndpointArray &eps = info->getRoxieServers();
    if (!eps.length())
        return NULL;
#else
    ISmartSocketFactory *conn = roxieConnMap.getValue(target);
    if (!conn)
        return nullptr;
    if (!k8s::isActiveService(target))
        return nullptr;
#endif

    try
    {
        StringBuffer control;
        if (!queryIDs || (queryIDs->ordinality() == 0))
            control.append("<control:queries/>");
        else
        {
            control.append("<control:queries>");
            ForEachItemIn(i, *queryIDs)
                control.appendf("<Query id='%s'/>",  queryIDs->item(i));
            control.append("</control:queries>");
        }
#ifndef _CONTAINERIZED
        Owned<ISocket> sock = ISocket::connect_timeout(eps.item(0), ROXIECONNECTIONTIMEOUT);
        if (checkAllNodes)
            return sendRoxieControlAllNodes(sock, control, false, ROXIECONTROLQUERIESTIMEOUT);
        else
            return sendRoxieControlQuery(sock, control, ROXIECONTROLQUERIESTIMEOUT);
#else
        if (checkAllNodes)
            return sendRoxieControlAllNodes(conn, control, false, ROXIECONTROLQUERIESTIMEOUT, ROXIECONNECTIONTIMEOUT);
        else
            return sendRoxieControlQuery(conn, control, ROXIECONTROLQUERIESTIMEOUT, ROXIECONNECTIONTIMEOUT);
#endif
    }
    catch(IException* e)
    {
        StringBuffer err;
        DBGLOG("Get exception in control:queries: %s", e->errorMessage(err.append(e->errorCode()).append(' ')).str());
        e->Release();
        return NULL;
    }
}

#ifndef _CONTAINERIZED
void retrieveQuerysetDetailsByCluster(IEspContext &context, IArrayOf<IEspWUQuerySetDetail> &details, const char *target, const char *queryset, const char *type, const char *value, bool checkAllNodes)
{
    Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(target, queryset, nullptr, checkAllNodes);
#else
void retrieveQuerysetDetailsByCluster(IEspContext &context, IArrayOf<IEspWUQuerySetDetail> &details,
    const char *target, const char *queryset, const char *type, const char *value, bool checkAllNodes,
    MapStringToMyClass<ISmartSocketFactory> &roxieConnMap)
{
    Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(target, queryset, nullptr, checkAllNodes, roxieConnMap);
#endif
    retrieveQuerysetDetails(context, details, target, type, value, target, queriesOnCluster);
}

void retrieveAllQuerysetDetails(IEspContext &context, IArrayOf<IEspWUQuerySetDetail> &details, const char *type, const char *value)
{
    Owned<IPropertyTree> root = getQueryRegistryRoot();
    if (!root)
        throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "QuerySet Registry not found");
    Owned<IPropertyTreeIterator> querysets = root->getElements("QuerySet");
    ForEach(*querysets)
        retrieveQuerysetDetails(context, details, &querysets->query(), type, value);
}

bool CWsWorkunitsEx::onWUQuerysetDetails(IEspContext &context, IEspWUQuerySetDetailsRequest & req, IEspWUQuerySetDetailsResponse & resp)
{
    resp.setQuerySetName(req.getQuerySetName());

    double version = context.getClientVersion();
    if (version > 1.36)
    {
        Owned<IPropertyTree> queryRegistry = getQueryRegistry(req.getQuerySetName(), false);
        resp.setClusterName(req.getClusterName());
        resp.setFilter(req.getFilter());
        resp.setFilterType(req.getFilterType());
    }

    Owned<IPropertyTree> registry = getQueryRegistry(req.getQuerySetName(), true);
    if (!registry)
        return false;
    PROGLOG("WUQuerysetDetails for queryset %s", req.getQuerySetName());

    IArrayOf<IEspQuerySetQuery> respQueries;
    IArrayOf<IEspQuerySetAlias> respAliases;

    if (isEmpty(req.getClusterName()) || isEmpty(req.getFilterTypeAsString()) || !strieq(req.getFilterTypeAsString(), "Status") || isEmpty(req.getFilter()))
    {
        const char* cluster = req.getClusterName();
        if (isEmpty(cluster))
            cluster = req.getQuerySetName();
#ifndef _CONTAINERIZED
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(cluster, req.getQuerySetName(), nullptr, req.getCheckAllNodes());
#else
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(cluster, req.getQuerySetName(), nullptr, req.getCheckAllNodes(), roxieConnMap);
#endif
        retrieveQuerysetDetails(context, registry, req.getFilterTypeAsString(), req.getFilter(), respQueries, respAliases, cluster, queriesOnCluster);

        resp.setQuerysetQueries(respQueries);
        resp.setQuerysetAliases(respAliases);
    }
    else
    {
        IArrayOf<IEspWUQuerySetDetail> respDetails;
#ifndef _CONTAINERIZED
        retrieveQuerysetDetailsByCluster(context, respDetails, req.getClusterName(), req.getQuerySetName(), req.getFilterTypeAsString(), req.getFilter(), false);
#else
        retrieveQuerysetDetailsByCluster(context, respDetails, req.getClusterName(), req.getQuerySetName(), req.getFilterTypeAsString(), req.getFilter(), false,
            roxieConnMap);
#endif
        if (respDetails.ordinality())
        {
            IEspWUQuerySetDetail& detail = respDetails.item(0);
            resp.setQuerysetQueries(detail.getQueries());
            resp.setQuerysetAliases(detail.getAliases());
        }
    }

    return true;
}

bool CWsWorkunitsEx::onWUMultiQuerysetDetails(IEspContext &context, IEspWUMultiQuerySetDetailsRequest & req, IEspWUMultiQuerySetDetailsResponse & resp)
{
    IArrayOf<IEspWUQuerySetDetail> respDetails;

    if (notEmpty(req.getClusterName()))
    {
        PROGLOG("WUMultiQuerysetDetails for cluster %s", req.getClusterName());
#ifndef _CONTAINERIZED
        retrieveQuerysetDetailsByCluster(context, respDetails, req.getClusterName(), req.getQuerySetName(), req.getFilterTypeAsString(), req.getFilter(), req.getCheckAllNodes());
#else
        retrieveQuerysetDetailsByCluster(context, respDetails, req.getClusterName(), req.getQuerySetName(), req.getFilterTypeAsString(), req.getFilter(), req.getCheckAllNodes(),
            roxieConnMap);
#endif
    }
    else if (notEmpty(req.getQuerySetName()))
    {
        PROGLOG("WUMultiQuerysetDetails for queryset %s", req.getQuerySetName());
        retrieveQuerysetDetails(context, respDetails, req.getQuerySetName(), req.getFilterTypeAsString(), req.getFilter());
    }
    else
    {
        VStringBuffer logMsg("WUMultiQuerysetDetails: FilterType %s", req.getFilterTypeAsString());
        if (notEmpty(req.getFilter()))
            logMsg.append(", Filter ").append(req.getFilter());
        PROGLOG("%s", logMsg.str());
        retrieveAllQuerysetDetails(context, respDetails, req.getFilterTypeAsString(), req.getFilter());
    }
    resp.setQuerysets(respDetails);

    return true;
}

bool addWUQSQueryFilter(WUQuerySortField *filters, unsigned short &count, MemoryBuffer &buff, const char* value, WUQuerySortField name)
{
    if (isEmpty(value))
        return false;
    filters[count++] = name;
    buff.append(value);
    return true;
}

bool addWUQSQueryFilterInt(WUQuerySortField *filters, unsigned short &count, MemoryBuffer &buff, int value, WUQuerySortField name)
{
    VStringBuffer vBuf("%d", value);
    filters[count++] = name;
    buff.append(vBuf.str());
    return true;
}

bool addWUQSQueryFilterInt64(WUQuerySortField *filters, unsigned short &count, MemoryBuffer &buff, __int64 value, WUQuerySortField name)
{
    VStringBuffer vBuf("%" I64F "d", value);
    filters[count++] = name;
    buff.append(vBuf.str());
    return true;
}

unsigned CWsWorkunitsEx::getGraphIdsByQueryId(const char *target, const char *queryId, StringArray& graphIds)
{
    if (!target || !*target)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Target name required");
    if (!queryId || !*queryId)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Query Id required");

#ifndef _CONTAINERIZED
    Owned<IConstWUClusterInfo> info = getTargetClusterInfo(target);
    if (!info || (info->getPlatform()!=RoxieCluster)) //Only roxie query has query graph.
        return 0;

    const SocketEndpointArray &eps = info->getRoxieServers();
    if (eps.empty())
        return 0;
#else
    ISmartSocketFactory *conn = roxieConnMap.getValue(target);
    if (!conn)
        return 0;
    if (!k8s::isActiveService(target))
        return 0;
#endif

    VStringBuffer xpath("<control:querystats><Query id='%s' action='listGraphNames'/></control:querystats>", queryId);
#ifndef _CONTAINERIZED
    Owned<ISocket> sock = ISocket::connect_timeout(eps.item(0), ROXIECONNECTIONTIMEOUT);
    Owned<IPropertyTree> querystats = sendRoxieControlQuery(sock, xpath.str(), ROXIECONTROLQUERYTIMEOUT);
#else
    Owned<IPropertyTree> querystats = sendRoxieControlQuery(conn, xpath.str(), ROXIECONTROLQUERYTIMEOUT, ROXIECONNECTIONTIMEOUT);
#endif
    if (!querystats)
        return 0;

    Owned<IPropertyTreeIterator> graphs = querystats->getElements("Endpoint/Query/Graphs/Graph");
    if (!graphs->first())
        graphs.setown(querystats->getElements("Endpoint/Query/Graph"));

    ForEach(*graphs)
    {
        IPropertyTree &graph = graphs->query();
        const char* graphId = graph.queryProp("@name");
        if (!graphId)
            graphId = graph.queryProp("@id");
        if (graphId && *graphId)
            graphIds.appendUniq(graphId);
    }
    return graphIds.length();
}

//This method is thread safe because a query belongs to a single queryset. The method may be called by different threads.
//Since one thread is for one queryset and a query only belongs to a single queryset, it is impossible for different threads
//to update the same query object.
void CWsWorkunitsEx::checkAndSetClusterQueryState(IEspContext &context, const char* cluster, const char* querySetId, IArrayOf<IEspQuerySetQuery>& queries, bool checkAllNodes)
{
    try
    {
        double version = context.getClientVersion();
        if (isEmpty(cluster))
            cluster = querySetId;

        StringArray queryIDs;
        ForEachItemIn(j, queries)
        {
            IEspQuerySetQuery& query = queries.item(j);
            const char* queryId = query.getId();
            const char* querySetId0 = query.getQuerySetId();
            if (queryId && querySetId0 && strieq(querySetId0, querySetId))
                queryIDs.append(queryId);
        }

        if (queryIDs.ordinality() == 0)
            return;

#ifndef _CONTAINERIZED
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(cluster, querySetId, &queryIDs, checkAllNodes);
#else
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(cluster, querySetId, &queryIDs, checkAllNodes, roxieConnMap);
#endif
        if (!queriesOnCluster)
        {
            UWARNLOG("getQueriesOnCluster() returns NULL for cluster<%s> and querySetId<%s>", cluster, querySetId);
            return;
        }

        ForEachItemIn(i, queries)
        {
            IEspQuerySetQuery& query = queries.item(i);
            const char* queryId = query.getId();
            const char* querySetId0 = query.getQuerySetId();
            if (!queryId || !querySetId0 || !strieq(querySetId0, querySetId))
                continue;

            IArrayOf<IEspClusterQueryState> clusters;
            addClusterQueryStates(queriesOnCluster, cluster, queryId, clusters, version);
            query.setClusters(clusters);
        }
    }
    catch(IException *e)
    {
        EXCLOG(e, "CWsWorkunitsEx::checkAndSetClusterQueryState: Failed to read Query State On Cluster");
        e->Release();
    }
}

void CWsWorkunitsEx::checkAndSetClusterQueryState(IEspContext &context, const char* cluster, StringArray& querySetIds, IArrayOf<IEspQuerySetQuery>& queries, bool checkAllNodes)
{
    UnsignedArray threadHandles;
    ForEachItemIn(i, querySetIds)
    {
        const char* querySetId = querySetIds.item(i);
        if(!querySetId || !*querySetId)
            continue;

        Owned<CClusterQueryStateParam> threadReq = new CClusterQueryStateParam(this, context, cluster, querySetId, queries, checkAllNodes);
        PooledThreadHandle handle = clusterQueryStatePool->start( threadReq.getClear() );
        threadHandles.append(handle);
    }

    //block for worker threads to finish, if necessary and then collect results
    //Not use joinAll() because multiple threads may call this method. Each call uses the pool to create
    //its own threads of checking query state. Each call should only join the ones created by that call.
    ForEachItemIn(ii, threadHandles)
        clusterQueryStatePool->join(threadHandles.item(ii));
}

bool CWsWorkunitsEx::onWUListQueries(IEspContext &context, IEspWUListQueriesRequest & req, IEspWUListQueriesResponse & resp)
{
    bool descending = req.getDescending();
    const char *sortBy =  req.getSortby();
    WUQuerySortField sortOrder[2] = {WUQSFId, WUQSFterm};
    if(notEmpty(sortBy))
    {
        if (strieq(sortBy, "Name"))
            sortOrder[0] = WUQSFname;
        else if (strieq(sortBy, "WUID"))
            sortOrder[0] = WUQSFwuid;
        else if (strieq(sortBy, "DLL"))
            sortOrder[0] = WUQSFdll;
        else if (strieq(sortBy, "Activated"))
            sortOrder[0] = WUQSFActivited;
        else if (strieq(sortBy, "MemoryLimit"))
            sortOrder[0] = (WUQuerySortField) (WUQSFmemoryLimit | WUQSFnumeric);
        else if (strieq(sortBy, "TimeLimit"))
            sortOrder[0] = (WUQuerySortField) (WUQSFtimeLimit | WUQSFnumeric);
        else if (strieq(sortBy, "WarnTimeLimit"))
            sortOrder[0] = (WUQuerySortField) (WUQSFwarnTimeLimit | WUQSFnumeric);
        else if (strieq(sortBy, "Priority"))
            sortOrder[0] = (WUQuerySortField) (WUQSFpriority | WUQSFnumeric);
        else if (strieq(sortBy, "PublishedBy"))
            sortOrder[0] = WUQSFPublishedBy;
        else if (strieq(sortBy, "QuerySetId"))
            sortOrder[0] = WUQSFQuerySet;
        else
            sortOrder[0] = WUQSFId;

        sortOrder[0] = (WUQuerySortField) (sortOrder[0] | WUQSFnocase);
        if (descending)
            sortOrder[0] = (WUQuerySortField) (sortOrder[0] | WUQSFreverse);
    }

    WUQuerySortField filters[16];
    unsigned short filterCount = 0;
    MemoryBuffer filterBuf;
    const char* clusterReq = req.getClusterName();
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getQuerySetName(), WUQSFQuerySet);
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getQueryID(), (WUQuerySortField) (WUQSFId | WUQSFwild | WUSFnocase));
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getQueryName(), (WUQuerySortField) (WUQSFname | WUQSFwild | WUSFnocase));
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getWUID(), (WUQuerySortField) (WUQSFwuid | WUSFnocase));
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getLibraryName(), (WUQuerySortField) (WUQSFLibrary | WUQSFnocase));
    addWUQSQueryFilter(filters, filterCount, filterBuf, req.getPublishedBy(), (WUQuerySortField) (WUQSFPublishedBy | WUQSFwild | WUSFnocase));
    if (!req.getMemoryLimitLow_isNull())
        addWUQSQueryFilterInt64(filters, filterCount, filterBuf, req.getMemoryLimitLow(), (WUQuerySortField) (WUQSFmemoryLimit | WUQSFnumeric));
    if (!req.getMemoryLimitHigh_isNull())
        addWUQSQueryFilterInt64(filters, filterCount, filterBuf, req.getMemoryLimitHigh(), (WUQuerySortField) (WUQSFmemoryLimitHi | WUQSFnumeric));
    if (!req.getTimeLimitLow_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getTimeLimitLow(), (WUQuerySortField) (WUQSFtimeLimit | WUQSFnumeric));
    if (!req.getTimeLimitHigh_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getTimeLimitHigh(), (WUQuerySortField) (WUQSFtimeLimitHi | WUQSFnumeric));
    if (!req.getWarnTimeLimitLow_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getWarnTimeLimitLow(), (WUQuerySortField) (WUQSFwarnTimeLimit | WUQSFnumeric));
    if (!req.getWarnTimeLimitHigh_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getWarnTimeLimitHigh(), (WUQuerySortField) (WUQSFwarnTimeLimitHi | WUQSFnumeric));
    if (!req.getPriorityLow_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getPriorityLow(), (WUQuerySortField) (WUQSFpriority | WUQSFnumeric));
    if (!req.getPriorityHigh_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getPriorityHigh(), (WUQuerySortField) (WUQSFpriorityHi | WUQSFnumeric));
    if (!req.getActivated_isNull())
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, req.getActivated(), (WUQuerySortField) (WUQSFActivited | WUQSFnumeric));

    MapStringTo<bool> suspendedQueriesByCluster;
    CWUQueryFilterSuspendedType suspendedType = req.getSuspendedFilter();
    if (suspendedType != WUQueryFilterSuspendedType_Undefined)
    {
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, suspendedType, (WUQuerySortField) (WUQSFSuspendedFilter | WUQSFnumeric));
        if (suspendedType == CWUQueryFilterSuspendedType_SUSPDBYFirstNode)
        {
            getSuspendedQueriesByCluster(suspendedQueriesByCluster, req.getQuerySetName(), req.getQueryID(), false);
        }
        else if ((suspendedType == CWUQueryFilterSuspendedType_SUSPDBYAnyNode) || (suspendedType == CWUQueryFilterSuspendedType_NOTSUSPD)
            || (suspendedType == CWUQueryFilterSuspendedType_SUSPD))
        {
            getSuspendedQueriesByCluster(suspendedQueriesByCluster, req.getQuerySetName(), req.getQueryID(), true);
        }
    }
    else if (!req.getSuspendedByUser_isNull()) //For the client before version 1.78
        addWUQSQueryFilterInt(filters, filterCount, filterBuf, CWUQueryFilterSuspendedType_SUSPDBYUSER, (WUQuerySortField) (WUQSFSuspendedFilter | WUQSFnumeric));
    filters[filterCount] = WUQSFterm;

    unsigned numberOfQueries = 0;
    unsigned pageSize = req.getPageSize();
    unsigned pageStartFrom = req.getPageStartFrom();
    if(pageSize < 1)
        pageSize = 100;
    __int64 cacheHint = 0;
    if (!req.getCacheHint_isNull())
        cacheHint = req.getCacheHint();

    Owned<MapStringTo<bool> > queriesUsingFileMap;
    const char *lfn = req.getFileName();
    if (lfn && *lfn)
    {
        queriesUsingFileMap.setown(new MapStringTo<bool>());
        StringAttr dummy;
        Owned<IPropertyTreeIterator> queriesUsingFile = filesInUse.findQueriesUsingFile(clusterReq, lfn, dummy);
        ForEach (*queriesUsingFile)
        {
            IPropertyTree &queryUsingFile = queriesUsingFile->query();
            const char *queryTarget = queryUsingFile.queryProp("@target");
            const char *queryId = queryUsingFile.queryProp("@id");
            if (queryTarget && *queryTarget && queryId && *queryId)
            {
                VStringBuffer targetQuery("%s/%s", queryTarget, queryId);
                queriesUsingFileMap->setValue(targetQuery, true);
            }
        }
    }

    PROGLOG("WUListQueries: getQuerySetQueriesSorted");
    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstQuerySetQueryIterator> it = factory->getQuerySetQueriesSorted(sortOrder, filters, filterBuf.bufferBase(),
        pageStartFrom, pageSize, &cacheHint, &numberOfQueries, queriesUsingFileMap, &suspendedQueriesByCluster);
    resp.setCacheHint(cacheHint);
    PROGLOG("WUListQueries: getQuerySetQueriesSorted done");

    StringArray querySetIds;
    IArrayOf<IEspQuerySetQuery> queries;
    double version = context.getClientVersion();
    ForEach(*it)
    {
        IPropertyTree &query=it->query();
        const char *queryId = query.queryProp("@id");
        const char *queryTarget = query.queryProp("@querySetId");

        Owned<IEspQuerySetQuery> q = createQuerySetQuery();
        q->setId(queryId);
        q->setQuerySetId(queryTarget);
        q->setName(query.queryProp("@name"));
        q->setDll(query.queryProp("@dll"));
        q->setWuid(query.queryProp("@wuid"));
        q->setActivated(query.getPropBool("@activated", false));
        q->setSuspended(query.getPropBool("@suspended", false));

        if (query.hasProp("@memoryLimit"))
        {
            StringBuffer s;
            memoryLimitStringFromUInt64(s, query.getPropInt64("@memoryLimit"));
            q->setMemoryLimit(s);
        }
        if (query.hasProp("@timeLimit"))
            q->setTimeLimit(query.getPropInt("@timeLimit"));
        if (query.hasProp("@warnTimeLimit"))
            q->setWarnTimeLimit(query.getPropInt("@warnTimeLimit"));
        checkAndSetQueryPriority(version, &query, q.get());
        if (query.hasProp("@comment"))
            q->setComment(query.queryProp("@comment"));
        if (version >= 1.46)
        {
            q->setPublishedBy(query.queryProp("@publishedBy"));
            q->setIsLibrary(query.getPropBool("@isLibrary"));
        }

        if (!querySetIds.contains(queryTarget))
            querySetIds.append(queryTarget);
        queries.append(*q.getClear());
    }

    if (queries.ordinality() > 0)
        checkAndSetClusterQueryState(context, clusterReq, querySetIds, queries, req.getCheckAllNodes());

    resp.setQuerysetQueries(queries);
    resp.setNumberOfQueries(numberOfQueries);

    return true;
}

void CWsWorkunitsEx::getSuspendedQueriesByCluster(MapStringTo<bool> &suspendedQueries, const char *querySet, const char *queryID, bool checkAllNodes)
{
    StringArray queryIDs;
    if (!isEmptyString(queryID))
        queryIDs.append(queryID);

    StringArray roxieTargets;
    if (!isEmptyString(querySet))
        roxieTargets.append(querySet);
    else
        getRoxieTargetsSupportingPublishedQueries(roxieTargets);

    ForEachItemIn(i, roxieTargets)
    {
        const char *roxieTarget = roxieTargets.item(i);
#ifndef _CONTAINERIZED
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(roxieTarget, roxieTarget, &queryIDs, checkAllNodes);
#else
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(roxieTarget, roxieTarget, &queryIDs, checkAllNodes, roxieConnMap);
#endif
        addSuspendedQueryIDs(suspendedQueries, queriesOnCluster, roxieTarget);
    }
}

void CWsWorkunitsEx::addSuspendedQueryIDs(MapStringTo<bool> &suspendedQueryIDs, IPropertyTree *queriesOnCluster, const char *querySet)
{
    if (!queriesOnCluster)
        throw makeStringExceptionV(ECLWATCH_INTERNAL_ERROR, "getQueriesOnCluster() returns nullptr for target <%s>", querySet);

    Owned<IPropertyTreeIterator> queries = queriesOnCluster->getElements("Endpoint/Queries/Query");
    ForEach(*queries)
    {
        IPropertyTree &query = queries->query();
        const char *id = query.queryProp("@id");
        if (isEmptyString(id))
            continue; //Should not happen

        if (query.getPropInt("@suspended"))
        {
            VStringBuffer suspendedID("%s/%s", querySet, id);
            suspendedQueryIDs.setValue(suspendedID, true);
        }
    }
}

bool CWsWorkunitsEx::onWUListQueriesUsingFile(IEspContext &context, IEspWUListQueriesUsingFileRequest &req, IEspWUListQueriesUsingFileResponse &resp)
{
    const char *target = req.getTarget();
    const char *process = req.getProcess();

    StringBuffer lfn(req.getFileName());
    resp.setFileName(lfn.toLowerCase());
    resp.setProcess(process);

    if (lfn.isEmpty())
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "FileName required");
    VStringBuffer logMsg("WUListQueriesUsingFile: %s", lfn.str());
    StringArray targets;
    if (target && *target)
    {
        targets.append(target);
        logMsg.append(", target ").append(target);
    }
    else // if (process && *process)
    {
        SCMStringBuffer targetStr;
#ifdef _CONTAINERIZED
        Owned<IStringIterator> targetClusters = config::getContainerTargets("roxie", process);
#else
        Owned<IStringIterator> targetClusters = getTargetClusters("RoxieCluster", process);
#endif
        ForEach(*targetClusters)
            targets.append(targetClusters->str(targetStr).str());
        logMsg.append(", process ").append(process);
    }
    PROGLOG("%s", logMsg.str());

    IArrayOf<IEspTargetQueriesUsingFile> respTargets;
    ForEachItemIn(i, targets)
    {
        target = targets.item(i);
        Owned<IEspTargetQueriesUsingFile> respTarget = createTargetQueriesUsingFile();
        respTarget->setTarget(target);

        StringAttr pmid;
        Owned<IPropertyTreeIterator> queries = filesInUse.findQueriesUsingFile(target, lfn, pmid);
        if (!pmid.isEmpty())
            respTarget->setPackageMap(pmid);
        if (queries)
        {
            IArrayOf<IEspQueryUsingFile> respQueries;
            ForEach(*queries)
            {
                IPropertyTree &query = queries->query();
                Owned<IEspQueryUsingFile> q = createQueryUsingFile();
                q->setId(query.queryProp("@id"));

                VStringBuffer xpath("File[@lfn='%s']/@pkgid", lfn.str());
                if (query.hasProp(xpath))
                    q->setPackage(query.queryProp(xpath));
                respQueries.append(*q.getClear());
            }
            respTarget->setQueries(respQueries);
        }
        respTargets.append(*respTarget.getClear());
    }
    resp.setTargets(respTargets);

    return true;
}

void addQueryFiles(IPropertyTree *queryTree, StringBuffer &queryid, IArrayOf<IEspFileUsedByQuery> &referencedFiles, IArrayOf<IEspQuerySuperFile> &referencedSuperFiles)
{
    queryid.set(queryTree->queryProp("@id"));
    Owned<IPropertyTreeIterator> files = queryTree->getElements("File");
    ForEach(*files)
    {
        IPropertyTree &file = files->query();
        if (file.getPropBool("@super"))
        {
            Owned<IEspQuerySuperFile> superFile = createQuerySuperFile();
            superFile->setName(file.queryProp("@lfn"));
            Owned<IPropertyTreeIterator> subfiles = file.getElements("SubFile");
            ForEach(*subfiles)
                superFile->getSubFiles().append(subfiles->query().queryProp("@lfn"));
            referencedSuperFiles.append(*superFile.getClear());
        }
        else
        {
            Owned<IEspFileUsedByQuery> respFile = createFileUsedByQuery();
            respFile->setFileName(file.queryProp("@lfn"));
            respFile->setFileSize(file.getPropInt64("@size"));
            respFile->setNumberOfParts(file.getPropInt("@numparts"));
            referencedFiles.append(*respFile.getClear());
        }
    }
}

void addQueriesFiles(IPropertyTreeIterator *queriesTrees, IArrayOf<IEspQueryFilesUsed> &queriesFiles)
{
    ForEach(*queriesTrees)
    {
        IPropertyTree &queryTree = queriesTrees->get();

        StringBuffer id;
        IArrayOf<IEspFileUsedByQuery> referencedFiles;
        IArrayOf<IEspQuerySuperFile> referencedSuperFiles;
        addQueryFiles(&queryTree, id, referencedFiles, referencedSuperFiles);

        Owned<IEspQueryFilesUsed> queryFilesUsed = createQueryFilesUsed();
        queryFilesUsed->setQueryId(id);
        queryFilesUsed->setFiles(referencedFiles);
        queryFilesUsed->setSuperFiles(referencedSuperFiles);
        queriesFiles.append(*queryFilesUsed.getClear());
    }
}

bool CWsWorkunitsEx::onWUQueryFiles(IEspContext &context, IEspWUQueryFilesRequest &req, IEspWUQueryFilesResponse &resp)
{
    const char *target = req.getTarget();
    validateTargetName(target);

    StringAttr queryid;
    const char *query = req.getQueryId();
    if (!query || !*query)
    {
        if (context.getClientVersion()<1.86)
            throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND, "Query not specified");
        PROGLOG("WUQueryFiles: target %s, all queries", target);
    }
    else
    {
        Owned<IPropertyTree> registeredQuery = resolveQueryAlias(target, query, true);
        if (!registeredQuery)
            throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND, "Query not found");
        PROGLOG("WUQueryFiles: target %s, query %s", target, query);
        queryid.set(registeredQuery->queryProp("@id"));
    }

    Owned<IPropertyTree> tree = filesInUse.getTree();
    if (queryid.length())
    {
        VStringBuffer xpath("%s/Query[@id='%s']", target, queryid.get());
        IPropertyTree *queryTree = tree->queryPropTree(xpath);
        if (!queryTree)
            throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND, "Query not found in file cache (%s)", xpath.str());

        IArrayOf<IEspFileUsedByQuery> referencedFiles;
        IArrayOf<IEspQuerySuperFile> referencedSuperFiles;
        StringBuffer id;
        addQueryFiles(queryTree, id, referencedFiles, referencedSuperFiles);

        resp.setFiles(referencedFiles);
        resp.setSuperFiles(referencedSuperFiles);
    }
    else
    {
        //return entire queryset
        VStringBuffer xpath("%s/Query", target);
        Owned<IPropertyTreeIterator> queryTrees = tree->getElements(xpath);
        IArrayOf<IEspQueryFilesUsed> queriesFiles;
        addQueriesFiles(queryTrees, queriesFiles);
        resp.setQueries(queriesFiles);
    }
    return true;
}

void copyWorkunitForRecompile(IEspContext &context, IWorkUnitFactory *factory, const char *srcWuid, StringAttr &wuid, StringAttr &jobname)
{
    Owned<IConstWorkUnit> src(factory->openWorkUnit(srcWuid));
    if (!src)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot open workunit %s.", srcWuid);
    WsWuInfo info(context, src);
    StringBuffer archiveText;
    info.getWorkunitArchiveQuery(archiveText); //archive required, fail otherwise
    if (!isArchiveQuery(archiveText))
        throw MakeStringException(ECLWATCH_RESOURCE_NOT_FOUND,"Cannot retrieve workunit ECL archive %s.", srcWuid);

    SCMStringBuffer mainDefinition;
    Owned <IConstWUQuery> query = src->getQuery();
    if (query)
        query->getQueryMainDefinition(mainDefinition);

    NewWsWorkunit wu(factory, context);
    wuid.set(wu->queryWuid());

    wu->setAction(WUActionCompile);

    jobname.set(src->queryJobName());
    if (jobname.length())
        wu->setJobName(jobname);
    wu.setQueryText(archiveText.str());
    if (mainDefinition.length())
        wu.setQueryMain(mainDefinition.str());
    wu->setResultLimit(src->getResultLimit());
    IStringIterator &names = src->getDebugValues();
    ForEach(names)
    {
        SCMStringBuffer name, value;
        names.str(name);
        if (0==strncmp(name.str(), "eclcc", 5))
            wu->setDebugValue(name.str(), src->getDebugValue(name.str(), value).str(), true);
    }
}

bool CWsWorkunitsEx::onWURecreateQuery(IEspContext &context, IEspWURecreateQueryRequest &req, IEspWURecreateQueryResponse &resp)
{
    try
    {
        const char* srcTarget = req.getTarget();
        const char* queryIdOrAlias = req.getQueryId();
        if (!srcTarget || !*srcTarget)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Target not specified");
        if (!queryIdOrAlias || !*queryIdOrAlias)
            throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND, "QueryId not specified");

        const char *target = req.getDestTarget();
        if (isEmptyString(target))
            target = srcTarget;

        Owned<IPropertyTree> queryRegistry = getQueryRegistry(srcTarget, false);
        Owned<IPropertyTree> srcQueryTree = resolveQueryAlias(queryRegistry, queryIdOrAlias);
        if (!srcQueryTree)
        {
            DBGLOG("WURecreateQuery - No matching Query");
            throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND,"No matching query for given id or alias %s.", queryIdOrAlias);
        }

        resp.setPriority(isEmptyString(req.getPriority()) ? srcQueryTree->queryProp("@priority") : req.getPriority());
        resp.setComment(isEmptyString(req.getComment()) ? srcQueryTree->queryProp("@comment") : req.getComment());
        resp.setMemoryLimit(isEmptyString(req.getMemoryLimit()) ? srcQueryTree->queryProp("@memoryLimit") : req.getMemoryLimit());
        resp.setTimeLimit(req.getTimeLimit_isNull() ? srcQueryTree->getPropInt("@timeLimit") : req.getTimeLimit());
        resp.setWarnTimeLimit(req.getWarnTimeLimit_isNull() ? srcQueryTree->getPropInt("@warnTimeLimit") : req.getWarnTimeLimit());

        StringAttr wuid;
        StringAttr jobname;

        const char* srcQueryId = srcQueryTree->queryProp("@id");
        const char* srcQueryName = srcQueryTree->queryProp("@name");
        const char *srcWuid = srcQueryTree->queryProp("@wuid");

        PROGLOG("WURecreateQuery: QuerySet %s, query %s, wuid %s", srcTarget, srcQueryId, srcWuid);

        ensureWsWorkunitAccess(context, srcWuid, SecAccess_Write);

        Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
        copyWorkunitForRecompile(context, factory, srcWuid, wuid, jobname);
        resp.setWuid(wuid);

        WsWuHelpers::submitWsWorkunit(context, wuid.str(), target, nullptr, 0, 0, true, false, false, nullptr, nullptr, &req.getDebugValues(), nullptr);
        waitForWorkUnitToCompile(wuid.str(), req.getWait());

        Owned<IConstWorkUnit> cw(factory->openWorkUnit(wuid.str()));
        if (!cw)
            throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot open recreated workunit %s.",wuid.str());

        if (jobname.length())
        {
            StringBuffer name;
            origValueChanged(jobname.str(), cw->queryJobName(), name, false);
            if (name.length()) //non generated user specified name, so override #Workunit('name')
            {
                WorkunitUpdate wx(&cw->lock());
                wx->setJobName(name.str());
            }
        }
        PROGLOG("WURecreateQuery generated: %s", wuid.str());
        AuditSystemAccess(context.queryUserId(), true, "Updated %s", wuid.str());

        queryRegistry.clear();
        srcQueryTree.clear();

        if (req.getRepublish())
        {
            StringBuffer publisherWuid(req.getDfuPublisherWuid());
            if (!req.getDontCopyFiles())
            {
                StringBuffer remoteLocation;
                StringBuffer srcCluster;
                StringBuffer srcPrefix;

                unsigned updateFlags = 0;
                const char *remoteStr = req.getRemoteStorage();
                if (!isEmptyString(remoteStr))
                    updateFlags = DFU_UPDATEF_REMOTESTORAGE;
                else
                    remoteStr = req.getRemoteDali();

                splitDerivedDfsLocation(remoteStr, srcCluster, remoteLocation, srcPrefix, req.getSourceProcess(),req.getSourceProcess(), NULL, NULL);

                if (srcCluster.length())
                {
                    if (!validateDataPlaneName(remoteLocation, srcCluster))
                        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Process cluster %s not found on %s remote", srcCluster.str(), remoteLocation.length() ? remoteLocation.str() : "local");
                }

                if (req.getUpdateDfs())
                    updateFlags |= (DALI_UPDATEF_SUPERFILES | DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM);
                if (req.getUpdateCloneFrom())
                    updateFlags |= DALI_UPDATEF_CLONE_FROM;
                if (req.getUpdateSuperFiles())
                    updateFlags |= DALI_UPDATEF_SUPERFILES;
                if (req.getAppendCluster())
                    updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;
                if (req.getDfuCopyFiles())
                    updateFlags |= DFU_UPDATEF_COPY;
                if (req.getDfuOverwrite())
                    updateFlags |= DFU_UPDATEF_OVERWRITE;

                QueryFileCopier cpr(target);
                cpr.init(context, req.getAllowForeignFiles(), srcQueryName);
                cpr.remoteLocation.set(remoteLocation);
                cpr.remotePrefix.set(srcPrefix);
                cpr.srcCluster.set(srcCluster);
                cpr.queryname.set(srcQueryName);
                cpr.dfu_queue.set(req.getDfuQueue());
                cpr.setKeyCompression(req.getKeyCompression());
                cpr.copy(publisherWuid, cw, updateFlags);

                if (req.getIncludeFileErrors())
                    cpr.gatherFileErrors(resp.getFileErrors());
                if (!publisherWuid.isEmpty())
                    resp.setDfuPublisherWuid(publisherWuid);
            }

            if (handlePublisherResponse(req, resp, publisherWuid))
                return true;

            StringBuffer queryId;
            WorkunitUpdate wu(&cw->lock());
            WUQueryActivationOptions activate = (WUQueryActivationOptions)req.getActivate();
            addQueryToQuerySet(wu, target, srcQueryName, activate, queryId, context.queryUserId());
            {
                Owned<IPropertyTree> queryTree = getQueryById(target, queryId, false);
                if (queryTree)
                {
                    queryTree->setProp("@priority", resp.getPriority());
                    updateMemoryLimitSetting(queryTree, resp.getMemoryLimit());
                    updateQuerySetting(resp.getTimeLimit_isNull(), queryTree, "@timeLimit", resp.getTimeLimit());
                    updateQuerySetting(resp.getWarnTimeLimit_isNull(), queryTree, "@warnTimeLimit", resp.getWarnTimeLimit());
                    updateQueryPriority(queryTree, resp.getPriority());
                    queryTree->setProp("@comment", resp.getComment());
                }
            }

            wu->commit();
            wu.clear();

            PROGLOG("WURecreateQuery published: %s as %s/%s", wuid.str(), target, queryId.str());

            resp.setQuerySet(target);
            resp.setQueryName(srcQueryName);
            resp.setQueryId(queryId.str());

            bool reloadFailed = false;
#ifndef _CONTAINERIZED
            Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target);
#endif
            if (0!=req.getWait() && !req.getNoReload())
#ifndef _CONTAINERIZED
                reloadFailed = !reloadCluster(clusterInfo, (unsigned)req.getWait());
#else
                reloadFailed = !reloadCluster(roxieConnMap, target, (unsigned)req.getWait());
#endif
            resp.setReloadFailed(reloadFailed);

            StringBuffer errorMessage;
#ifndef _CONTAINERIZED
            if (!reloadFailed && !req.getNoReload() && isQuerySuspended(queryId.str(), clusterInfo, (unsigned)req.getWait(), errorMessage))
#else
            if (!reloadFailed && !req.getNoReload() && isQuerySuspended(queryId, target, (unsigned)req.getWait(), errorMessage))
#endif
            {
                resp.setSuspended(true);
                resp.setErrorMessage(errorMessage);
            }
        }
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsWorkunitsEx::onWUQueryDetails(IEspContext &context, IEspWUQueryDetailsRequest & req, IEspWUQueryDetailsResponse & resp)
{
    try
    {
        CWUQueryDetailsReq request(req);
        getWUQueryDetails(context, request, resp);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsWorkunitsEx::getWUQueryDetails(IEspContext &context, CWUQueryDetailsReq &req, IEspWUQueryDetailsResponse &resp)
{
    const char *querySet = req.getQuerySet();
    if (isEmptyString(querySet))
        throw makeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "QuerySet not specified");
    const char *queryIdOrAlias = req.getQueryIdOrAlias();
    if (isEmptyString(queryIdOrAlias))
        throw makeStringException(ECLWATCH_QUERYID_NOT_FOUND, "QueryId not specified");

    Owned<IPropertyTree> queryRegistry = getQueryRegistry(querySet, false);
    Owned<IPropertyTree> query = resolveQueryAlias(queryRegistry, queryIdOrAlias);
    if (!query)
        throw makeStringExceptionV(ECLWATCH_QUERYID_NOT_FOUND, "No matching query for given id or alias %s.", queryIdOrAlias);

    const char* queryId = query->queryProp("@id");
    resp.setQueryId(queryId);
    resp.setQuerySet(querySet);
    PROGLOG("WUQueryDetails: QuerySet %s, query %s", querySet, queryId);

    const char* wuid = query->queryProp("@wuid");
    resp.setQueryName(query->queryProp("@name"));
    resp.setWuid(wuid);
    resp.setDll(query->queryProp("@dll"));
    resp.setPublishedBy(query->queryProp("@publishedBy"));
    resp.setSuspended(query->getPropBool("@suspended", false));
    resp.setSuspendedBy(query->queryProp("@suspendedBy"));
    resp.setComment(query->queryProp("@comment"));
    double version = context.getClientVersion();
    if (version >= 1.46)
    {
        checkAndSetQueryPriority(version, query, &resp);
        resp.setIsLibrary(query->getPropBool("@isLibrary"));

        if (version < 1.64)
        {
            StringArray graphIds;
            unsigned numGraphIds = getGraphIdsByQueryId(querySet, queryId, graphIds);
            resp.setCountGraphs(numGraphIds);
            if (numGraphIds > 0)
                resp.setGraphIds(graphIds);
        }

        if (req.getIncludeWUDetails())
        {
            Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
            Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid);
            if (!cw)
                throw MakeStringException(ECLWATCH_CANNOT_UPDATE_WORKUNIT,"Cannot open workunit %s.",wuid);
            ensureWsWorkunitAccess(context, *cw, SecAccess_Read);
    
            SCMStringBuffer s;
            resp.setWUSnapShot(cw->getSnapshot(s).str()); //Label
    
            stat_type whenCompiled;
            if (cw->getStatistic(whenCompiled, "", StWhenCompiled))
            {
                formatStatistic(s.s.clear(), whenCompiled, StWhenCompiled);
                resp.setCompileTime(s.str());
            }
    
            StringArray libUsed;
            Owned<IConstWULibraryIterator> libs = &cw->getLibraries();
            ForEach(*libs)
                libUsed.append(libs->query().getName(s).str());
            if (libUsed.length())
                resp.setLibrariesUsed(libUsed);

            if (version >= 1.50)
            {
                WsWuInfo winfo(context, cw);
                resp.setResourceURLCount(winfo.getResourceURLCount());
                if (version >= 1.64)
                {
                    IArrayOf<IEspECLTimer> timers;
                    winfo.doGetTimers(timers); //Graph Duration
                    if (timers.length())
                        resp.setWUTimers(timers);

                    IArrayOf<IEspECLGraph> graphs;
                    winfo.doGetGraphs(graphs); //Graph Name, Label, Started, Finished, Type
                    unsigned numGraphIds = graphs.length();
                    resp.setCountGraphs(numGraphIds);
                    if (numGraphIds > 0)
                        resp.setWUGraphs(graphs);
                }
            }
        }
    }

    if (req.getIncludeWUQueryFiles())
    {
        StringArray logicalFiles;
        IArrayOf<IEspQuerySuperFile> superFiles;
        getQueryFiles(context, wuid, queryId, querySet, logicalFiles, req.getIncludeSuperFiles() ? &superFiles : nullptr);
        if (logicalFiles.length())
            resp.setLogicalFiles(logicalFiles);
        if (superFiles.length())
            resp.setSuperFiles(superFiles);
    }

    if (version >= 1.42)
    {
        VStringBuffer xpath("Alias[@id='%s']", queryId);
        IPropertyTree *alias = queryRegistry->queryPropTree(xpath.str());
        if (!alias)
            resp.setActivated(false);
        else
            resp.setActivated(true);
    }
    
    if (req.getIncludeStateOnClusters() && (version >= 1.43))
    {
        StringArray queryIds;
        queryIds.append(queryId);

#ifndef _CONTAINERIZED
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(querySet, querySet, &queryIds, req.getCheckAllNodes());
#else
        Owned<IPropertyTree> queriesOnCluster = getQueriesOnCluster(querySet, querySet, &queryIds, req.getCheckAllNodes(), roxieConnMap);
#endif
        if (queriesOnCluster)
        {
            IArrayOf<IEspClusterQueryState> clusterStates;
            addClusterQueryStates(queriesOnCluster, querySet, queryId, clusterStates, version);
            resp.setClusters(clusterStates);
        }
    }

    if (req.getIncludeWsEclAddresses())
    {
        StringArray wseclAddresses;
#ifndef _CONTAINERIZED
        Owned<IEnvironmentFactory> factory = getEnvironmentFactory(true);
        Owned<IConstEnvironment> env = factory->openEnvironment();
        Owned<IPropertyTree> root = &env->getPTree();
        Owned<IPropertyTreeIterator> services = root->getElements("Software/EspService[Properties/@type='ws_ecl']");
        StringArray serviceNames;
        VStringBuffer xpath("Target[@name='%s']", querySet);
        ForEach(*services)
        {
            IPropertyTree &service = services->query();
            if (!service.hasProp("Target") || service.hasProp(xpath))
                serviceNames.append(service.queryProp("@name"));
        }

        Owned<IPropertyTreeIterator> processes = root->getElements("Software/EspProcess");
        ForEach(*processes)
        {
            StringArray netAddrs;
            IPropertyTree &process = processes->query();
            Owned<IPropertyTreeIterator> instances = process.getElements("Instance");
            ForEach(*instances)
            {
                IPropertyTree &instance = instances->query();
                const char *netAddr = instance.queryProp("@netAddress");
                if (!netAddr || !*netAddr)
                    continue;
                if (streq(netAddr, "."))
                    netAddrs.appendUniq(envLocalAddress); //not necessarily local to this server
                else
                    netAddrs.appendUniq(netAddr);
            }
            Owned<IPropertyTreeIterator> bindings = process.getElements("EspBinding");
            ForEach(*bindings)
            {
                IPropertyTree &binding = bindings->query();
                const char *srvName = binding.queryProp("@service");
                if (!serviceNames.contains(srvName))
                    continue;
                const char *port = binding.queryProp("@port"); //should always be an integer, but we're just concatenating strings
                if (!port || !*port)
                    continue;
                ForEachItemIn(i, netAddrs)
                {
                    VStringBuffer wseclAddr("%s:%s", netAddrs.item(i), port);
                    wseclAddresses.append(wseclAddr);
                }
            }
        }
#else
        IArrayOf<IConstHPCCService> eclservices;
        CTpWrapper tpWrapper;
        tpWrapper.getServices(version, "eclqueries", nullptr, eclservices);
        ForEachItemIn(i, eclservices)
        {
            IConstHPCCService& eclservice = eclservices.item(i);
            VStringBuffer wseclAddr("%s:%u", eclservice.getName(), eclservice.getPort());
            wseclAddresses.append(wseclAddr);
        }
#endif
        resp.setWsEclAddresses(wseclAddresses);
    }
}

bool CWsWorkunitsEx::onWUQueryDetailsLightWeight(IEspContext &context, IEspWUQueryDetailsLightWeightRequest & req, IEspWUQueryDetailsResponse & resp)
{
    try
    {
        CWUQueryDetailsReq request(req);
        getWUQueryDetails(context, request, resp);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

CWUQueryDetailsReq::CWUQueryDetailsReq(IEspWUQueryDetailsRequest &req)
{
    querySet = req.getQuerySet();
    queryIdOrAlias = req.getQueryId();
    includeWUDetails = req.getIncludeWUDetails();
    IncludeWUQueryFiles = req.getIncludeWUQueryFiles();
    includeSuperFiles = req.getIncludeSuperFiles();
    includeWsEclAddresses = req.getIncludeWsEclAddresses();
    includeStateOnClusters = req.getIncludeStateOnClusters();
    checkAllNodes = req.getCheckAllNodes();
}

CWUQueryDetailsReq::CWUQueryDetailsReq(IEspWUQueryDetailsLightWeightRequest &req)
{
    querySet = req.getQuerySet();
    queryIdOrAlias = req.getQueryId();
    includeWUDetails = req.getIncludeWUDetails();
    IncludeWUQueryFiles = req.getIncludeWUQueryFiles();
    includeSuperFiles = req.getIncludeSuperFiles();
    includeWsEclAddresses = req.getIncludeWsEclAddresses();
    includeStateOnClusters = req.getIncludeStateOnClusters();
    checkAllNodes = req.getCheckAllNodes();
}

int EspQuerySuperFileCompareFunc(IInterface * const *i1, IInterface * const *i2)
{
    if (!i1 || !*i1 || !i2 || !*i2)
        return 0;
    IEspQuerySuperFile *sf1 = QUERYINTERFACE(*i1, IEspQuerySuperFile);
    IEspQuerySuperFile *sf2 = QUERYINTERFACE(*i2, IEspQuerySuperFile);
    if (!sf1 || !sf2)
        return 0;
    const char *name1 = sf1->getName();
    const char *name2 = sf2->getName();
    if (!name1 || !name2)
        return 0;
    return strcmp(name1, name2);
}

IReferencedFile* CWsWorkunitsEx::getReferencedFileByName(const char* name, IReferencedFileList* wufiles)
{
    Owned<IReferencedFileIterator> refFileItr = wufiles->getFiles();
    ForEach(*refFileItr)
    {
        IReferencedFile& rf = refFileItr->query();
        const char* lfn = rf.getLogicalName();
        if (lfn && strieq(lfn, name))
            return &rf;
    }
    return NULL;
}

void CWsWorkunitsEx::readSuperFiles(IEspContext &context, IReferencedFile* rf, const char* fileName, IReferencedFileList* wufiles, IArrayOf<IEspQuerySuperFile>* files)
{
    double version = context.getClientVersion();
    StringArray subFiles;
    IArrayOf<IEspQuerySuperFile> superFiles;
    const StringArray& subFileNames = rf->getSubFileNames();
    ForEachItemIn(i, subFileNames)
    {
        const char* name = subFileNames.item(i);
        if (!name || !*name)
            continue;
        IReferencedFile* pRF = getReferencedFileByName(name, wufiles);
        if (!pRF)
            continue;
        if (!(pRF->getFlags() & RefFileSuper))
        {
            subFiles.append(name);
        }
        else if (version >= 1.57)
        {
            readSuperFiles(context, pRF, name, wufiles, &superFiles);
        }
    }

    Owned<IEspQuerySuperFile> newSuperFile = createQuerySuperFile();
    newSuperFile->setName(fileName);
    if (subFiles.length())
    {
        subFiles.sortAscii();
        newSuperFile->setSubFiles(subFiles);
    }
    if ((version >= 1.57) && superFiles.length())
    {
        superFiles.sort(EspQuerySuperFileCompareFunc);
        newSuperFile->setSuperFiles(superFiles);
    }
    files->append(*newSuperFile.getClear());
}

bool CWsWorkunitsEx::getQueryFiles(IEspContext &context, const char* wuid, const char* query, const char* target, StringArray& logicalFiles, IArrayOf<IEspQuerySuperFile> *respSuperFiles)
{
    try
    {
        Owned<IConstWUClusterInfo> info = getWUClusterInfoByName(target);
        if (!info || (info->getPlatform()!=RoxieCluster))
            return false;

        SCMStringBuffer process;
        info->getRoxieProcess(process);
        if (!process.length())
            return false;

        Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
        Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid);
        if (!cw)
            return false;

        double version = context.getClientVersion();
        StringArray superFileNames;
        Owned<IHpccPackageSet> ps = createPackageSet(process.str());
        Owned<IReferencedFileList> wufiles = createReferencedFileList(context.queryUserId(),
            context.queryPassword(), true, true);
        wufiles->addFilesFromQuery(cw, (ps) ? ps->queryActiveMap(target) : NULL, query);
        StringArray locations;
        locations.append(process.str());
        wufiles->resolveFiles(locations, NULL, NULL, NULL, true, true, true, true, false);
        Owned<IReferencedFileIterator> refFileItr = wufiles->getFiles();
        ForEach(*refFileItr)
        {
            IReferencedFile &rf = refFileItr->query();
            const char *lfn = rf.getLogicalName();
            if (lfn && *lfn)
            {
                bool isSuper = rf.getFlags() & RefFileSuper;
                if (!isSuper || (version < 1.78))
                    logicalFiles.append(lfn);
                if (respSuperFiles && isSuper)
                    readSuperFiles(context, &rf, lfn, wufiles, respSuperFiles);
            }
        }

        logicalFiles.sortAscii();
        if (respSuperFiles)
            respSuperFiles->sort(EspQuerySuperFileCompareFunc);
        return true;
    }
    catch(IMultiException *me)
    {
        StringBuffer err;
        IERRLOG("ERROR control:getQueryXrefInfo roxie query info %s", me->errorMessage(err.append(me->errorCode()).append(' ')).str());
        me->Release();
        return false;
    }
    catch(IException *e)
    {
        StringBuffer err;
        IERRLOG("ERROR control:getQueryXrefInfo roxie query info %s", e->errorMessage(err.append(e->errorCode()).append(' ')).str());
        e->Release();
        return false;
    }
}

inline void verifyQueryActionAllowsWild(bool &allowWildChecked, CQuerySetQueryActionTypes action)
{
    if (allowWildChecked)
        return;
    switch (action)
    {
        case CQuerySetQueryActionTypes_ToggleSuspend:
            throw MakeStringException(ECLWATCH_INVALID_ACTION, "Wildcards not supported for toggling suspended state");
        case CQuerySetQueryActionTypes_Activate:
            throw MakeStringException(ECLWATCH_INVALID_ACTION, "Wildcards not supported for Activating queries");
    }
    allowWildChecked=true;
}

inline bool verifyQueryActionAllowsAlias(CQuerySetQueryActionTypes action)
{
    return (action!=CQuerySetQueryActionTypes_Activate && action!=CQuerySetQueryActionTypes_RemoveAllAliases);
}

void setQueryActionTargetList(IProperties *queryIds, const char *queryId, bool suspendedByUser, const char *wuidToDelete, StringArray *wuidsToDelete)
{
    queryIds->setProp(queryId, suspendedByUser);
    if (wuidsToDelete && wuidToDelete)
        wuidsToDelete->appendUniq(wuidToDelete);
}

void findActiveQueries(IPropertyTree *queryset, StringArray &queryIds)
{
    Owned<IPropertyTreeIterator> active = queryset->getElements("Alias");
    ForEach(*active)
        queryIds.append(active->query().queryProp("@id"));
}

void expandQueryActionTargetList(IProperties *queryIds, StringArray *wuidsToDelete, IPropertyTree *queryset, IArrayOf<IConstQuerySetQueryActionItem> &items, CQuerySetQueryActionTypes action)
{
    StringArray activeQueryIds;
    bool activeQueryIdsFound = false;
    bool allowWildChecked = false;
    ForEachItemIn(i, items)
    {
        const char *itemId = items.item(i).getQueryId();
        if (!isWildString(itemId))
        {
            bool suspendedByUser = false;
            const char *itemSuspendState = items.item(i).getClientState().getSuspended();
            if (itemSuspendState && (strieq(itemSuspendState, "By User") || strieq(itemSuspendState, "1")))
                suspendedByUser = true;
            if (!verifyQueryActionAllowsAlias(action))
            {
                Owned<IPropertyTree> query = getQueryById(queryset, itemId);
                setQueryActionTargetList(queryIds, itemId, suspendedByUser, wuidsToDelete ? query->queryProp("@wuid") : nullptr, wuidsToDelete);
            }
            else
            {
                Owned<IPropertyTree> query = resolveQueryAlias(queryset, itemId);
                if (query)
                {
                    const char *id = query->queryProp("@id");
                    if (id && *id)
                        setQueryActionTargetList(queryIds, id, suspendedByUser, wuidsToDelete ? query->queryProp("@wuid") : nullptr, wuidsToDelete);
                }
            }
        }
        else
        {
            verifyQueryActionAllowsWild(allowWildChecked, action);

            bool checkActivated = !items.item(i).getActivated_isNull();
            bool activated = checkActivated && items.item(i).getActivated();
            bool suspendedByUser = items.item(i).getSuspendedByUser();

            if (checkActivated && !activated && !activeQueryIdsFound)
            {
                activeQueryIdsFound = true;
                findActiveQueries(queryset, activeQueryIds);
            }

            StringArray activeQueryInActionTargetList;
            if (verifyQueryActionAllowsAlias(action) && (!checkActivated || activated))
            {
                Owned<IPropertyTreeIterator> active = queryset->getElements("Alias");
                ForEach(*active)
                {
                    const char *name = active->query().queryProp("@name");
                    const char *id = active->query().queryProp("@id");
                    if (name && id && WildMatch(name, itemId))
                    {
                        Owned<IPropertyTree> query = getQueryById(queryset, id);
                        if (!suspendedByUser || query->getPropBool("@suspended", false))
                        {
                            setQueryActionTargetList(queryIds, id, 0, wuidsToDelete ? query->queryProp("@wuid") : nullptr, wuidsToDelete);
                            activeQueryInActionTargetList.append(id);
                        }
                    }
                }
            }
            Owned<IPropertyTreeIterator> queries = queryset->getElements("Query");
            ForEach(*queries)
            {
                const char *id = queries->query().queryProp("@id");
                if (id && WildMatch(id, itemId) && (activeQueryInActionTargetList.find(id) == NotFound))
                {
                    if (!checkActivated || (activated == (activeQueryIds.find(id) != NotFound)))
                    {
                        if (!suspendedByUser || queries->query().getPropBool("@suspended", false))
                            setQueryActionTargetList(queryIds, id, 0, wuidsToDelete ? queries->query().queryProp("@wuid") : nullptr, wuidsToDelete);
                    }
                }
            }
        }
    }
}

void expandQueryActionTargetList(IProperties *queryIds, IPropertyTree *queryset, const char *id, CQuerySetQueryActionTypes action)
{
    IArrayOf<IConstQuerySetQueryActionItem> items;
    Owned<IEspQuerySetQueryActionItem> item = createQuerySetQueryActionItem();
    item->setQueryId(id);
    items.append(*(IConstQuerySetQueryActionItem*)item.getClear());
    expandQueryActionTargetList(queryIds, nullptr, queryset, items, action);
}

bool CWsWorkunitsEx::onWUQueryConfig(IEspContext &context, IEspWUQueryConfigRequest & req, IEspWUQueryConfigResponse & resp)
{
    StringAttr target(req.getTarget());
    validateTargetName(target);

    Owned<IPropertyTree> queryset = getQueryRegistry(target.get(), false);
    if (!queryset)
        throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Target Queryset %s not found", req.getTarget());
    PROGLOG("WUQueryConfig: target %s", target.get());

    Owned<IProperties> queryIds = createProperties();
    expandQueryActionTargetList(queryIds, queryset, req.getQueryId(), QuerySetQueryActionTypes_Undefined);

    IArrayOf<IEspWUQueryConfigResult> results;
    Owned<IPropertyIterator> it = queryIds->getIterator();
    ForEach(*it)
    {
        Owned<IEspWUQueryConfigResult> result = createWUQueryConfigResult();
        result->setQueryId(it->getPropKey());

        VStringBuffer xpath("Query[@id='%s']", it->getPropKey());
        IPropertyTree *queryTree = queryset->queryPropTree(xpath);
        if (queryTree)
        {
            updateMemoryLimitSetting(queryTree, req.getMemoryLimit());
            updateQueryPriority(queryTree, req.getPriority());
            updateQuerySetting(req.getTimeLimit_isNull(), queryTree, "@timeLimit", req.getTimeLimit());
            updateQuerySetting(req.getWarnTimeLimit_isNull(), queryTree, "@warnTimeLimit", req.getWarnTimeLimit());
            if (req.getComment())
                queryTree->setProp("@comment", req.getComment());
        }

        results.append(*result.getClear());
    }
    resp.setResults(results);

    bool reloadFailed = false;
    if (0!=req.getWait() && !req.getNoReload())
#ifndef _CONTAINERIZED
        reloadFailed = !reloadCluster(target.get(), (unsigned)req.getWait());
#else
        reloadFailed = !reloadCluster(roxieConnMap, target.get(), (unsigned)req.getWait());
#endif
    resp.setReloadFailed(reloadFailed);

    return true;
}

bool CWsWorkunitsEx::onWUQuerysetQueryAction(IEspContext &context, IEspWUQuerySetQueryActionRequest & req, IEspWUQuerySetQueryActionResponse & resp)
{
    resp.setQuerySetName(req.getQuerySetName());
    resp.setAction(req.getAction());

    if (isEmpty(req.getQuerySetName()))
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Queryset name required");
    Owned<IPropertyTree> queryset = getQueryRegistry(req.getQuerySetName(), true);
    if (!queryset)
        throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Queryset %s not found", req.getQuerySetName());

    CQuerySetQueryActionTypes action = req.getAction();
    Owned<IProperties> queryIds = createProperties();
    StringArray wuidsToDelete;
    if (action == CQuerySetQueryActionTypes_DeleteQueriesAndWUs)
        expandQueryActionTargetList(queryIds, &wuidsToDelete, queryset, req.getQueries(), action);
    else
        expandQueryActionTargetList(queryIds, nullptr, queryset, req.getQueries(), action);
    if (action == CQuerySetQueryActionTypes_ResetQueryStats)
        return resetQueryStats(context, req.getQuerySetName(), queryIds, resp);

    IArrayOf<IEspQuerySetQueryActionResult> results;
    Owned<IPropertyIterator> it = queryIds->getIterator();
    ForEach(*it)
    {
        const char *id = it->getPropKey();
        Owned<IEspQuerySetQueryActionResult> result = createQuerySetQueryActionResult();
        result->setQueryId(id);
        try
        {
            Owned<IPropertyTree> query = getQueryById(queryset, id);
            if (!query)
                throw MakeStringException(ECLWATCH_QUERYID_NOT_FOUND, "Query %s/%s not found.", req.getQuerySetName(), id);
            const char* strAction = (action > -1) && (action < NumOfQuerySetQueryActionTypes) ? QuerySetQueryActionTypes[action] : "Undefined";
            PROGLOG("%s: queryset %s, query %s", strAction, req.getQuerySetName(), id);
            switch (action)
            {
                case CQuerySetQueryActionTypes_ToggleSuspend:
                    setQuerySuspendedState(queryset, id, !queryIds->getPropBool(id), context.queryUserId());
                    break;
                case CQuerySetQueryActionTypes_Suspend:
                    setQuerySuspendedState(queryset, id, true, context.queryUserId());
                    break;
                case CQuerySetQueryActionTypes_Unsuspend:
                    setQuerySuspendedState(queryset, id, false, NULL);
                    break;
                case CQuerySetQueryActionTypes_Activate:
                    setQueryAlias(queryset, query->queryProp("@name"), id);
                    break;
                case CQuerySetQueryActionTypes_Delete:
                case CQuerySetQueryActionTypes_DeleteQueriesAndWUs:
                    removeNamedQuery(queryset, id);
                    query.clear();
                    break;
                case CQuerySetQueryActionTypes_RemoveAllAliases:
                    removeAliasesFromNamedQuery(queryset, id);
                    break;
            }
            result->setSuccess(true);
            if (query)
                result->setSuspended(query->getPropBool("@suspended"));
        }
        catch(IException *e)
        {
            StringBuffer msg;
            result->setMessage(e->errorMessage(msg).str());
            result->setCode(e->errorCode());
            result->setSuccess(false);
            e->Release();
        }
        results.append(*result.getClear());
    }

    if (action == CQuerySetQueryActionTypes_DeleteQueriesAndWUs)
    {
        queryset.clear();

        Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
        ForEachItemIn(i, wuidsToDelete)
        {
            const char *wuid = wuidsToDelete.item(i);
            Owned<IEspQuerySetQueryActionResult> result = createQuerySetQueryActionResult();
            result->setWUID(wuid);
            try
            {
                factory->deleteWorkUnitEx(wuid, true, context.querySecManager(), context.queryUser());
                result->setSuccess(true);
            }
            catch(IException *e)
            {
                VStringBuffer msg("Deleting %s. ", wuid);
                result->setMessage(e->errorMessage(msg).str());
                result->setCode(e->errorCode());
                result->setSuccess(false);
                e->Release();
            }
            results.append(*result.getClear());
        }
    }
    resp.setResults(results);
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetAliasAction(IEspContext &context, IEspWUQuerySetAliasActionRequest &req, IEspWUQuerySetAliasActionResponse &resp)
{
    resp.setQuerySetName(req.getQuerySetName());
    resp.setAction(req.getAction());

    if (isEmpty(req.getQuerySetName()))
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Queryset name required");
    Owned<IPropertyTree> queryset = getQueryRegistry(req.getQuerySetName(), true);
    if (!queryset)
        throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Queryset %s not found", req.getQuerySetName());

    IArrayOf<IEspQuerySetAliasActionResult> results;
    ForEachItemIn(i, req.getAliases())
    {
        IConstQuerySetAliasActionItem& item=req.getAliases().item(i);
        Owned<IEspQuerySetAliasActionResult> result = createQuerySetAliasActionResult();
        try
        {
            VStringBuffer xpath("Alias[@name='%s']", item.getName());
            IPropertyTree *alias = queryset->queryPropTree(xpath.str());
            if (!alias)
                throw MakeStringException(ECLWATCH_ALIAS_NOT_FOUND, "Alias %s/%s not found.", req.getQuerySetName(), item.getName());
            CQuerySetAliasActionTypes action = req.getAction();
            const char* strAction = (action > -1) && (action < NumOfQuerySetAliasActionTypes) ? QuerySetAliasActionTypes[action] : "Undefined";
            PROGLOG("%s: queryset %s, alias %s", strAction, req.getQuerySetName(), item.getName());
            switch (action)
            {
                case CQuerySetAliasActionTypes_Deactivate:
                    removeQuerySetAlias(req.getQuerySetName(), item.getName());
                    break;
            }
            result->setSuccess(true);
        }
        catch(IException *e)
        {
            StringBuffer msg;
            result->setMessage(e->errorMessage(msg).str());
            result->setCode(e->errorCode());
            result->setSuccess(false);
            e->Release();
        }
        results.append(*result.getClear());
    }
    resp.setResults(results);
    return true;
}

#define QUERYPATH_SEP_CHAR '/'

bool nextQueryPathNode(const char *&path, StringBuffer &node)
{
    if (*path==QUERYPATH_SEP_CHAR)
        path++;
    while (*path && *path!=QUERYPATH_SEP_CHAR)
        node.append(*path++);
    return (*path && *++path);
}

bool splitQueryPath(const char *path, StringBuffer &netAddress, StringBuffer &queryset, StringBuffer *query)
{
    if (!path || !*path)
        return false;
    if (*path==QUERYPATH_SEP_CHAR && path[1]==QUERYPATH_SEP_CHAR)
    {
        path+=2;
        if (!nextQueryPathNode(path, netAddress))
            return false;
    }
    if (!nextQueryPathNode(path, queryset))
        return (query==NULL);
    if (!query)
        return false;
    if (nextQueryPathNode(path, *query))
        return false; //query path too deep
    return true;
}

IPropertyTree *fetchRemoteQuerySetInfo(IEspContext *context, const char *srcAddress, const char *srcTarget, bool useSSL)
{
    if (!srcAddress || !*srcAddress || !srcTarget || !*srcTarget)
        return NULL;

    VStringBuffer url("%s://%s%s/WsWorkunits/WUQuerysetDetails.xml?ver_=1.51&QuerySetName=%s&FilterType=All", useSSL ? "https" : "http", srcAddress, (!strchr(srcAddress, ':')) ? ":8010" : "", srcTarget);

    Owned<IHttpClientContext> httpCtx = getHttpClientContext();
    Owned<IHttpClient> httpclient = httpCtx->createHttpClient(NULL, url);

    const char *user = context->queryUserId();
    if (user && *user)
        httpclient->setUserID(user);

    const char *pw = context->queryPassword();
    if (pw && *pw)
         httpclient->setPassword(pw);

    StringBuffer request; //empty
    StringBuffer response;
    StringBuffer status;
    if (0 > httpclient->sendRequest("GET", NULL, request, response, status) || !response.length() || strncmp("200", status, 3))
         throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Error fetching remote queryset information: %s %s %s", srcAddress, srcTarget, status.str());

    return createPTreeFromXMLString(response);
}

class QueryCloner
{
public:
    QueryCloner(IEspContext *_context, const char *address, const char *source, const char *_target, bool _useSSL) :
        context(_context), srcAddress(address), target(_target), useSSL(_useSSL)
    {
        if (srcAddress.length())
            srcQuerySet.setown(fetchRemoteQuerySetInfo(context, srcAddress, source, useSSL));
        else
            srcQuerySet.setown(getQueryRegistry(source, true));
        if (!srcQuerySet)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Source Queryset %s %s not found", srcAddress.str(), source);

        destQuerySet.setown(getQueryRegistry(target, false));
        if (!destQuerySet) // getQueryRegistry should have created if not found
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Destination Queryset %s could not be created, or found", target.str());

        factory.setown(getWorkUnitFactory(context->querySecManager(), context->queryUser()));
    }

    QueryCloner(IEspContext *_context, IPropertyTree *srcTree, const char *_target) :
        context(_context), target(_target)
    {
        srcQuerySet.set(srcTree);
        destQuerySet.setown(getQueryRegistry(target, false));
        if (!destQuerySet) // getQueryRegistry should have created if not found
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Destination Queryset %s could not be created, or found", target.str());

        factory.setown(getWorkUnitFactory(context->querySecManager(), context->queryUser()));
    }

    void setQueryDirectory(const char *dir)
    {
        queryDirectory.set(dir);
    }

    void addToBePublished(const char *wuid, const char *name, bool makeActive, const char *userid, IPropertyTree *query)
    {
        IPropertyTree *entry = toBePublished->addPropTree("Publish");
        entry->setProp("@wuid", wuid);
        entry->setProp("@name", name);
        entry->setPropBool("@makeActive", makeActive);
        entry->setProp("@userid", userid);
        entry->addPropTree("Info", createPTreeFromIPT(query));

        Owned<IAttributeIterator> aiter = query->getAttributes();
        IPropertyTree *attrs = entry->addPropTree("Attrs");
        ForEach(*aiter)
        {
            const char *atname = aiter->queryName();
            attrs->setProp(atname, aiter->queryValue());
        }
    }

    void publish()
    {
        Owned<IPropertyTreeIterator> entries = toBePublished->getElements("Publish");
        ForEach(*entries)
        {
            IPropertyTree &entry = entries->query();
            StringBuffer newQueryId;
            Owned<IWorkUnit> workunit = factory->updateWorkUnit(entry.queryProp("@wuid"));
            addQueryToQuerySet(workunit, destQuerySet, entry.queryProp("@name"), entry.getPropBool("@makeActive") ? ACTIVATE_SUSPEND_PREVIOUS : DO_NOT_ACTIVATE, newQueryId, entry.queryProp("@userid"));
            copiedQueryIds.append(newQueryId);
            IPropertyTree *info = entry.queryPropTree("Info");
            if (info)
            {
                Owned<IPropertyTree> destQuery = getQueryById(destQuerySet, newQueryId);
                if (destQuery)
                {
                    Owned<IAttributeIterator> aiter = info->getAttributes();
                    ForEach(*aiter)
                    {
                        const char *atname = aiter->queryName();
                        if (!destQuery->hasProp(atname))
                            destQuery->setProp(atname, aiter->queryValue());
                    }
                    Owned<IPropertyTreeIterator> children = info->getElements("*");
                    ForEach(*children)
                    {
                        IPropertyTree &child = children->query();
                        destQuery->addPropTree(child.queryName(), createPTreeFromIPT(&child));
                    }
                }
            }
        }
    }

    void cloneQueryRemote(IPropertyTree *query, bool makeActive)
    {
        StringBuffer wuid(query->queryProp("Wuid"));
        if (!wuid.length())
            return;
        const char *queryName = query->queryProp("Name");
        if (!queryName || !*queryName)
            return;

        StringBuffer xml;
        MemoryBuffer dll;
        StringBuffer dllname;
        StringBuffer fetchedName;
        StringBuffer remoteDfs;
        fetchRemoteWorkunit(NULL, context, srcAddress.str(), NULL, NULL, wuid, fetchedName, xml, dllname, dll, remoteDfs, useSSL);
        deploySharedObject(*context, wuid, dllname, target, queryName, dll, queryDirectory, xml.str(), false);

        SCMStringBuffer existingQueryId;
        queryIdFromQuerySetWuid(destQuerySet, wuid, queryName, existingQueryId);
        if (existingQueryId.length())
        {
            existingQueryIds.append(existingQueryId.str());
            if (makeActive)
                activateQuery(destQuerySet, ACTIVATE_SUSPEND_PREVIOUS, queryName, existingQueryId.str(), context->queryUserId());
            return;
        }
        addToBePublished(wuid, queryName, makeActive, context->queryUserId(), query);

        if (cloneFilesEnabled && wufiles)
        {
            VStringBuffer queryPmMatch("%s.0", queryName);
            Owned<IConstWorkUnit> workunit = factory->openWorkUnit(wuid);
            wufiles->addFilesFromQuery(workunit, pm, queryPmMatch);
        }
    }

    void cloneQueryLocal(IPropertyTree *query, bool makeActive)
    {
        const char *wuid = query->queryProp("@wuid");
        if (!wuid || !*wuid)
            return;
        const char *queryName = query->queryProp("@name");
        if (!queryName || !*queryName)
            return;
        SCMStringBuffer existingQueryId;
        queryIdFromQuerySetWuid(destQuerySet, wuid, queryName, existingQueryId);
        if (existingQueryId.length())
        {
            existingQueryIds.append(existingQueryId.str());
            if (makeActive)
                activateQuery(destQuerySet, ACTIVATE_SUSPEND_PREVIOUS, queryName, existingQueryId.str(), context->queryUserId());
            return;
        }
        StringBuffer newQueryId;
        Owned<IWorkUnit> workunit = factory->updateWorkUnit(wuid);
        if (!workunit)
        {
            StringBuffer msg(wuid);
            msg.append(": ").append(query->queryProp("@id"));
            missingWuids.append(msg);
            return;
        }
        addToBePublished(wuid, queryName, makeActive, context->queryUserId(), query);
        if (cloneFilesEnabled && wufiles)
            wufiles->addFilesFromQuery(workunit, pm, newQueryId);
        if (cloneFilesEnabled && wufiles)
        {
            VStringBuffer queryPmMatch("%s.0", queryName);
            Owned<IConstWorkUnit> workunit = factory->openWorkUnit(wuid);
            wufiles->addFilesFromQuery(workunit, pm, queryPmMatch);
        }
    }

    void cloneActiveRemote(bool makeActive)
    {
        Owned<IPropertyTreeIterator> activeQueries = srcQuerySet->getElements("QuerysetAliases/QuerySetAlias");
        ForEach(*activeQueries)
        {
            IPropertyTree &alias = activeQueries->query();
            VStringBuffer xpath("QuerysetQueries/QuerySetQuery[Id='%s'][1]", alias.queryProp("Id"));
            IPropertyTree *query = srcQuerySet->queryPropTree(xpath);
            if (!query)
                continue;
            cloneQueryRemote(query, makeActive);
        }
    }

    void cloneActiveLocal(bool makeActive, const char *mask)
    {
        StringBuffer xpath("Alias");
        if (mask && *mask)
            xpath.appendf("[@id='%s']", mask);
        Owned<IPropertyTreeIterator> activeQueries = srcQuerySet->getElements(xpath);
        ForEach(*activeQueries)
        {
            IPropertyTree &alias = activeQueries->query();
            Owned<IPropertyTree> query = getQueryById(srcQuerySet, alias.queryProp("@id"));
            if (!query)
                return;
            cloneQueryLocal(query, makeActive);
        }
    }

    void cloneActive(bool makeActive)
    {
        if (srcAddress.length())
            cloneActiveRemote(makeActive);
        else
            cloneActiveLocal(makeActive, nullptr);
    }

    void cloneAllRemote(bool cloneActiveState)
    {
        Owned<IPropertyTreeIterator> allQueries = srcQuerySet->getElements("QuerysetQueries/QuerySetQuery");
        ForEach(*allQueries)
        {
            IPropertyTree &query = allQueries->query();
            bool makeActive = false;
            if (cloneActiveState)
            {
                VStringBuffer xpath("QuerysetAliases/QuerySetAlias[Id='%s']", query.queryProp("Id"));
                makeActive = srcQuerySet->hasProp(xpath);
            }
            cloneQueryRemote(&query, makeActive);
        }
    }
    void cloneAllLocal(bool cloneActiveState, const char *mask)
    {
        StringBuffer xpath("Query");
        if (mask && *mask)
            xpath.appendf("[@id='%s']", mask);
        Owned<IPropertyTreeIterator> allQueries = srcQuerySet->getElements(xpath);
        ForEach(*allQueries)
        {
            IPropertyTree &query = allQueries->query();
            bool makeActive = false;
            if (cloneActiveState)
            {
                VStringBuffer xpath("Alias[@id='%s']", query.queryProp("@id"));
                makeActive = srcQuerySet->hasProp(xpath);
            }
            cloneQueryLocal(&query, makeActive);
        }
    }
    void cloneAll(bool cloneActiveState)
    {
        if (srcAddress.length())
            cloneAllRemote(cloneActiveState);
        else
            cloneAllLocal(cloneActiveState, nullptr);
    }
    void enableFileCloning(unsigned _updateFlags, const char *dfsServer, const char *destProcess, const char *sourceProcess, bool allowForeign, const char * keyCompression)
    {
        cloneFilesEnabled = true;
        updateFlags = _updateFlags;
        splitDerivedDfsLocation(dfsServer, srcCluster, dfsIP, srcPrefix, sourceProcess, sourceProcess, NULL, NULL);
        wufiles.setown(createReferencedFileList(context->queryUserId(), context->queryPassword(), allowForeign, false));
        wufiles->setKeyCompression(keyCompression);
        Owned<IHpccPackageSet> ps = createPackageSet(destProcess);
        pm.set(ps->queryActiveMap(target));
        if (isContainerized())
        {
            StringAttrBuilder builder(process);
            getRoxieDirectAccessPlanes(locations, builder, target, true);
        }
        else
            process.set(destProcess);
    }
    void cloneFiles(StringBuffer &publisherWuid)
    {
        if (cloneFilesEnabled)
        {
            wufiles->resolveFiles(locations, dfsIP, srcPrefix, srcCluster, !(updateFlags & (DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM)), true, false, true, false);
            Owned<IDFUhelper> helper = createIDFUhelper();
            Owned <IConstWUClusterInfo> cl = getWUClusterInfoByName(target);
            if (cl)
            {
                wufiles->setDfuQueue(dfu_queue);
#ifdef _CONTAINERIZED
                wufiles->cloneAllInfo(publisherWuid, process.str(), updateFlags, helper, true, true, 0, 1, 0, nullptr);
#else
                SCMStringBuffer process;
                StringBuffer defReplicateFolder;
                getConfigurationDirectory(NULL, "data2", "roxie", cl->getRoxieProcess(process).str(), defReplicateFolder);
                wufiles->cloneAllInfo(publisherWuid, process.str(), updateFlags, helper, true, true, cl->getRoxieRedundancy(), cl->getChannelsPerNode(), cl->getRoxieReplicateOffset(), defReplicateFolder);
#endif
            }
        }
    }
    void gatherFileErrors(IArrayOf<IConstLogicalFileError> &errors)
    {
        if (wufiles)
            ::gatherFileErrors(wufiles, errors);
    }

private:
    Linked<IEspContext> context;
    Linked<IWorkUnitFactory> factory;
    Owned<IPropertyTree> destQuerySet;
    Owned<IPropertyTree> srcQuerySet;
    Owned<IPropertyTree> toBePublished = createPTree();
    Owned<IReferencedFileList> wufiles;
    Owned<const IHpccPackageMap> pm;
    StringBuffer dfsIP;
    StringBuffer srcAddress;
    StringBuffer srcCluster;
    StringBuffer srcPrefix;
    StringAttr target;
    StringAttr process;
    StringAttr queryDirectory;
    bool cloneFilesEnabled = false;
    bool useSSL = false;
    unsigned updateFlags = 0;
    StringArray locations;

public:
    StringArray existingQueryIds;
    StringArray copiedQueryIds;
    StringArray missingWuids;
    StringBuffer dfu_jobname;
    StringAttr dfu_queue;
};

bool CWsWorkunitsEx::onWUCopyQuerySet(IEspContext &context, IEspWUCopyQuerySetRequest &req, IEspWUCopyQuerySetResponse &resp)
{
    const char *source = req.getSource();
    if (!source || !*source)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "No source target specified");

    StringBuffer srcAddress;
    StringBuffer srcTarget;
    if (!splitQueryPath(source, srcAddress, srcTarget, NULL))
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Invalid source target");
    //don't validate source target if it's remote (has srcAddress)
    if (srcAddress.isEmpty() && !srcTarget.isEmpty())
        validateTargetName(srcTarget);

    const char *target = req.getTarget();
    validateTargetName(target);

    DBGLOG("%s copying queryset %s from %s target %s", context.queryUserId(), target, srcAddress.str(), srcTarget.str());

    QueryCloner cloner(&context, srcAddress, srcTarget, target, req.getSourceSSL());
    cloner.setQueryDirectory(queryDirectory);

    SCMStringBuffer process;
    StringBuffer publisherWuid(req.getDfuPublisherWuid());
    if (req.getCopyFiles())
    {
        Owned <IConstWUClusterInfo> clusterInfo = getWUClusterInfoByName(target);
        if (clusterInfo && clusterInfo->getPlatform()==RoxieCluster)
        {
            clusterInfo->getRoxieProcess(process);
            if (!process.length())
                throw MakeStringException(ECLWATCH_INVALID_CLUSTER_INFO, "DFS process cluster not found for destination target %s", target);
            unsigned updateFlags = 0;
            if (req.getOverwriteDfs())
                updateFlags |= (DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM | DALI_UPDATEF_SUPERFILES);
            if (req.getUpdateCloneFrom())
                updateFlags |= DALI_UPDATEF_CLONE_FROM;
            if (req.getUpdateSuperFiles())
                updateFlags |= DALI_UPDATEF_SUPERFILES;
            if (req.getAppendCluster())
                updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;
            if (req.getDfuCopyFiles())
            {
                updateFlags |= DFU_UPDATEF_COPY;
                cloner.dfu_jobname.append("copy queryset ").append(srcTarget);
            }
            if (req.getDfuOverwrite())
                updateFlags |= DFU_UPDATEF_OVERWRITE;
            cloner.dfu_queue.set(req.getDfuQueue());

            cloner.enableFileCloning(updateFlags, req.getDfsServer(), process.str(), req.getSourceProcess(), req.getAllowForeignFiles(), req.getKeyCompression());
        }
    }

    if (req.getActiveOnly())
        cloner.cloneActive(req.getCloneActiveState());
    else
        cloner.cloneAll(req.getCloneActiveState());

    cloner.cloneFiles(publisherWuid);
    if (req.getIncludeFileErrors())
        cloner.gatherFileErrors(resp.getFileErrors());

    if (handlePublisherResponse(req, resp, publisherWuid))
        return true;

    cloner.publish();

    resp.setCopiedQueries(cloner.copiedQueryIds);
    resp.setExistingQueries(cloner.existingQueryIds);

    return true;
}

bool CWsWorkunitsEx::onWUQuerysetCopyQuery(IEspContext &context, IEspWUQuerySetCopyQueryRequest &req, IEspWUQuerySetCopyQueryResponse &resp)
{
    if (req.getDontCopyFiles() && req.getOnlyCopyFiles())
        throw makeStringException(ECLWATCH_INVALID_INPUT,"Cannot combine 'dont-copy-files' and 'only-copy-files'");

    unsigned start = msTick();
    const char *source = req.getSource();
    if (!source || !*source)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "No source query specified");
    const char *target = req.getTarget();
    if (!target || !*target)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "No destination specified");
    if (strchr(target, '/')) //for future use
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Invalid target queryset name");
    if (req.getCluster() && *req.getCluster() && !strieq(req.getCluster(), target)) //backward compatability check
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Invalid target cluster and queryset must match");
    validateTargetName(target);

    StringBuffer srcAddress, srcQuerySet, srcQuery;
    if (!splitQueryPath(source, srcAddress, srcQuerySet, &srcQuery))
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Invalid source query path");

    StringAttr targetQueryName(req.getDestName());
    Owned<IClientWUQuerySetDetailsResponse> sourceQueryInfoResp;
    IConstQuerySetQuery *srcInfo=NULL;

    DBGLOG("%s copying query %s to target %s from %s target %s", context.queryUserId(), srcQuery.str(), target, srcAddress.str(), srcQuerySet.str());

    StringBuffer remoteIP;
    StringBuffer wuid;
    if (srcAddress.length())
    {
        StringBuffer xml;
        MemoryBuffer dll;
        StringBuffer dllname;
        StringBuffer queryName;
        fetchRemoteWorkunitAndQueryDetails(NULL, &context, srcAddress.str(), srcQuerySet.str(), srcQuery.str(), NULL, queryName, xml, dllname, dll, remoteIP, sourceQueryInfoResp, req.getSourceSSL());
        if (sourceQueryInfoResp && sourceQueryInfoResp->getQuerysetQueries().ordinality())
            srcInfo = &sourceQueryInfoResp->getQuerysetQueries().item(0);
        if (srcInfo)
            wuid.set(srcInfo->getWuid());
        if (targetQueryName.isEmpty())
            targetQueryName.set(queryName);
        deploySharedObject(context, wuid, dllname.str(), target, targetQueryName.get(), dll, queryDirectory.str(), xml.str(), false);
    }
    else
    {
        //Could get the atributes without soap call, but this creates a common data structure shared with fetching remote query info
        //Get query attributes before resolveQueryAlias, to avoid deadlock
        sourceQueryInfoResp.setown(fetchQueryDetails(NULL, &context, NULL, srcQuerySet, srcQuery, req.getSourceSSL()));
        if (sourceQueryInfoResp && sourceQueryInfoResp->getQuerysetQueries().ordinality())
            srcInfo = &sourceQueryInfoResp->getQuerysetQueries().item(0);

        Owned<IPropertyTree> queryset = getQueryRegistry(srcQuerySet.str(), true);
        if (!queryset)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Source Queryset %s not found", srcQuery.str());

        Owned<IPropertyTree> query = resolveQueryAlias(queryset, srcQuery.str());
        if (!query)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Source query %s not found", source);
        wuid.set(query->queryProp("@wuid"));
        if (targetQueryName.isEmpty())
            targetQueryName.set(query->queryProp("@name"));
    }

    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstWorkUnit> cw = factory->openWorkUnit(wuid.str());

    StringBuffer publisherWuid(req.getDfuPublisherWuid());
    if (!req.getDontCopyFiles())
    {
        StringBuffer remoteLocation;
        StringBuffer srcCluster;
        StringBuffer srcPrefix;

        unsigned updateFlags = 0;
        const char *remoteStr = req.getRemoteStorage();
        if (!isEmptyString(remoteStr))
            updateFlags = DFU_UPDATEF_REMOTESTORAGE;
        else
            remoteStr = req.getDaliServer();

        splitDerivedDfsLocation(remoteStr, srcCluster, remoteLocation, srcPrefix, req.getSourceProcess(),req.getSourceProcess(), remoteIP.str(), NULL);

        if (srcCluster.length())
        {
            if (!validateDataPlaneName(remoteLocation, srcCluster))
                throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Process cluster %s not found on %s remote", srcCluster.str(), remoteLocation.length() ? remoteLocation.str() : "local");
        }

        if (req.getOverwrite())
            updateFlags |= (DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM | DALI_UPDATEF_SUPERFILES);
        if (req.getUpdateCloneFrom())
            updateFlags |= DALI_UPDATEF_CLONE_FROM;
        if (req.getUpdateSuperFiles())
            updateFlags |= DALI_UPDATEF_SUPERFILES;
        if (req.getAppendCluster())
            updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;
        if (req.getDfuCopyFiles())
            updateFlags |= DFU_UPDATEF_COPY;
        if (req.getDfuOverwrite())
            updateFlags |= DFU_UPDATEF_OVERWRITE;

        QueryFileCopier cpr(target);
        cpr.init(context, req.getAllowForeignFiles(), targetQueryName);
        cpr.remoteLocation.set(remoteLocation);
        cpr.remotePrefix.set(srcPrefix);
        cpr.srcCluster.set(srcCluster);
        cpr.queryname.set(targetQueryName);
        cpr.dfu_queue.set(req.getDfuQueue());
        cpr.setKeyCompression(req.getKeyCompression());

        cpr.copy(publisherWuid, cw, updateFlags);

        if (req.getIncludeFileErrors())
            cpr.gatherFileErrors(resp.getFileErrors());
        if (!publisherWuid.isEmpty())
            resp.setDfuPublisherWuid(publisherWuid);
    }

    if (handlePublisherResponse(req, resp, publisherWuid))
        return true;

    WorkunitUpdate wu(&cw->lock());
    if (!wu)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT, "Error opening wuid %s for query %s", wuid.str(), source);

    StringBuffer targetQueryId;
    WUQueryActivationOptions activate = (WUQueryActivationOptions)req.getActivate();
    addQueryToQuerySet(wu, target, targetQueryName.get(), activate, targetQueryId, context.queryUserId());

    Owned<IPropertyTree> queryTree = getQueryById(target, targetQueryId, false);
    if (queryTree)
    {
        updateMemoryLimitSetting(queryTree, req.getMemoryLimit(), srcInfo);
        updateQueryPriority(queryTree, req.getPriority(), srcInfo);
        updateTimeLimitSetting(queryTree, req.getTimeLimit_isNull(), req.getTimeLimit(), srcInfo);
        updateWarnTimeLimitSetting(queryTree, req.getWarnTimeLimit_isNull(), req.getWarnTimeLimit(), srcInfo);
        if (req.getComment())
            queryTree->setProp("@comment", req.getComment());
        else if (srcInfo && srcInfo->getComment())
            queryTree->setProp("@comment", srcInfo->getComment());
        if (srcInfo && srcInfo->getSnapshot())
            queryTree->setProp("@snapshot", srcInfo->getSnapshot());
    }
    wu.clear();

    resp.setQueryId(targetQueryId.str());

    if (0!=req.getWait() && !req.getNoReload())
#ifndef _CONTAINERIZED
        reloadCluster(target, remainingMsWait(req.getWait(), start));
#else
        reloadCluster(roxieConnMap, target, (unsigned)req.getWait());
#endif
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetImport(IEspContext &context, IEspWUQuerysetImportRequest &req, IEspWUQuerysetImportResponse &resp)
{
    try
    {
        const char* target = req.getTarget();
        if (!target || !*target)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Target not specified");

        Owned <IConstWUClusterInfo> clusterInfo = getWUClusterInfoByName(target);
        if (!clusterInfo)
            throw MakeStringException(ECLWATCH_CANNOT_RESOLVE_CLUSTER_NAME, "Target not found");

        if (req.getCopyFiles() && clusterInfo->getPlatform()!=RoxieCluster)
            throw MakeStringException(ECLWATCH_INVALID_ACTION, "Copy files option only supported for Roxie");

        MemoryBuffer &mb = const_cast<MemoryBuffer &>(req.getData()); //for efficiency, content of request shouldn't matter after
        if (req.getCompressed())
        {
            MemoryBuffer decompressed;
            fastLZDecompressToBuffer(decompressed, mb);
            mb.swapWith(decompressed);
        }
        mb.append('\0');

        Owned<IPropertyTree> srcTree = createPTreeFromXMLString(mb.toByteArray());
        const char *archivedTarget = srcTree->queryProp("@target");
        if (archivedTarget && *archivedTarget) //support simple queryset or with archived (exported) root format
        {
            VStringBuffer xpath("QuerySet[@id='%s']", archivedTarget);
            IPropertyTree *qsTree = srcTree->queryPropTree(xpath);
            if (qsTree)
                srcTree.setown(LINK(qsTree));
        }
        if (req.getReplace())
        {
            Owned<IPropertyTree> queryRegistry = getQueryRegistry(target, false);
            queryRegistry->removeProp("*");
            resp.setClearedExisting(true);
        }

        const bool activate = CQuerysetImportActivation_ImportedActive == req.getActivation(); //only two options now but may evolve

        QueryCloner cloner(&context, srcTree, target);

        SCMStringBuffer process;
        if (req.getCopyFiles())
        {
            clusterInfo->getRoxieProcess(process); //checked if roxie when copying files above
            if (!process.length())
                throw MakeStringException(ECLWATCH_INVALID_CLUSTER_INFO, "DFS process cluster not found for destination target %s", target);
            unsigned updateFlags = 0;
            if (req.getOverwriteDfs())
                updateFlags |= (DALI_UPDATEF_REPLACE_FILE | DALI_UPDATEF_CLONE_FROM | DALI_UPDATEF_SUPERFILES);
            if (req.getUpdateCloneFrom())
                updateFlags |= DALI_UPDATEF_CLONE_FROM;
            if (req.getUpdateSuperFiles())
                updateFlags |= DALI_UPDATEF_SUPERFILES;
            if (req.getAppendCluster())
                updateFlags |= DALI_UPDATEF_APPEND_CLUSTER;

            cloner.enableFileCloning(updateFlags, req.getDfsServer(), process.str(), req.getSourceProcess(), req.getAllowForeignFiles(), req.getKeyCompression());
        }

        if (req.getActiveOnly())
            cloner.cloneActiveLocal(activate, req.getQueryMask());
        else
            cloner.cloneAllLocal(activate, req.getQueryMask());

        StringBuffer publisherWuid(req.getDfuPublisherWuid());
        cloner.cloneFiles(publisherWuid);
        if (req.getIncludeFileErrors())
            cloner.gatherFileErrors(resp.getFileErrors());

        if (handlePublisherResponse(req, resp, publisherWuid))
            return true;

        cloner.publish();

        resp.setImportedQueries(cloner.copiedQueryIds);
        resp.setExistingQueries(cloner.existingQueryIds);
        resp.setMissingWuids(cloner.missingWuids);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetExport(IEspContext &context, IEspWUQuerysetExportRequest &req, IEspWUQuerysetExportResponse &resp)
{
    try
    {
        const char* target = req.getTarget();
        if (!target || !*target)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Target not specified");

        Owned<IPropertyTree> queryRegistry = getQueryRegistry(target, true);
        if (req.getActiveOnly())
        {
            Owned<IPropertyTree> activeOnly = createPTree("QuerySet");
            Owned<IAttributeIterator> attrs = queryRegistry->getAttributes();
            ForEach(*attrs)
                activeOnly->setProp(attrs->queryName(), attrs->queryValue());

            Owned<IPropertyTreeIterator> aliases = queryRegistry->getElements("Alias");
            ForEach(*aliases)
            {
                IPropertyTree &alias = aliases->query();
                const char *id = alias.queryProp("@id");
                if (id && *id)
                {
                    VStringBuffer xpath("Query[@id='%s']", id);
                    IPropertyTree *query = queryRegistry->queryPropTree(xpath);
                    if (query)
                    {
                        activeOnly->addPropTree("Query", LINK(query));
                        activeOnly->addPropTree("Alias", LINK(&alias));
                    }
                }
            }
            queryRegistry.setown(activeOnly.getClear());
        }

        if (req.getProtect())
        {
            StringArray wuids;
            Owned<IPropertyTreeIterator> queries = queryRegistry->getElements("Query");
            ForEach(*queries)
            {
                IPropertyTree &query = queries->query();
                const char *wuid = query.queryProp("@wuid");
                if (wuid && *wuid)
                    wuids.append(wuid);
            }
            if (wuids.length())
                doProtectWorkunits(context, wuids, nullptr);
        }
        CDateTime dt;
        dt.setNow();
        StringBuffer dts;
        VStringBuffer qs("<QuerySetArchive exported='%s' target='%s' activeOnly='%s'>\n", dt.getString(dts, true).str(), target, req.getActiveOnly() ? "true" : "false");
        toXML(queryRegistry, qs);
        qs.append("</QuerySetArchive>");

        MemoryBuffer content;
        if (req.getCompress())
            fastLZCompressToBuffer(content, qs.length()+1, qs);
        else
            content.append(qs.str());

        resp.setTarget(target);
        resp.setCompressed(req.getCompress());
        resp.setData(content);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsWorkunitsEx::getGraphsByQueryId(const char *target, const char *queryId, const char *graphId, const char *subGraphId, IArrayOf<IEspECLGraphEx>& ECLGraphs)
{
    if (!target || !*target)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Target name required");
    if (!queryId || !*queryId)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Query Id required");

#ifndef _CONTAINERIZED
    Owned<IConstWUClusterInfo> info = getTargetClusterInfo(target);
    if (!info || (info->getPlatform()!=RoxieCluster)) //Only support roxie for now
        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Invalid Roxie name");

    PROGLOG("getGraphsByQueryId: target %s, query %s", target, queryId);

    const SocketEndpointArray &eps = info->getRoxieServers();
    if (eps.empty())
        return;

    VStringBuffer control("<control:querystats><Query id='%s'/></control:querystats>", queryId);
    Owned<IPropertyTree> querystats = sendRoxieControlAllNodes(eps.item(0), control.str(), false, ROXIELOCKCONNECTIONTIMEOUT);
#else
    ISmartSocketFactory *conn = roxieConnMap.getValue(target);
    if (!conn)
        throw makeStringExceptionV(ECLWATCH_INVALID_CLUSTER_NAME, "Invalid Roxie name %s", target);

    PROGLOG("getGraphsByQueryId: target %s, query %s", target, queryId);
    VStringBuffer control("<control:querystats><Query id='%s'/></control:querystats>", queryId);
    Owned<IPropertyTree> querystats = sendRoxieControlAllNodes(conn, control.str(), false, ROXIELOCKCONNECTIONTIMEOUT, ROXIECONNECTIONTIMEOUT);
#endif
    if (!querystats)
        return;

    Owned<IPropertyTreeIterator> graphs = querystats->getElements("Endpoint/Query/Graphs/Graph");
    if (!graphs->first())
        graphs.setown(querystats->getElements("Endpoint/Query/Graph"));

    ForEach(*graphs)
    {
        IPropertyTree &graph = graphs->query();
        const char* aGraphId = graph.queryProp("@name");
        if (!aGraphId)
            aGraphId = graph.queryProp("@id");
        if (graphId && *graphId && !strieq(graphId, aGraphId))
            continue;

        IPropertyTree* xgmml = graph.getBranch("xgmml/graph");
        if (!xgmml)
            continue;

        Owned<IEspECLGraphEx> g = createECLGraphEx("","");
        g->setName(aGraphId);

        StringBuffer xml;
        if (!subGraphId || !*subGraphId)
            toXML(xgmml, xml);
        else
        {
            VStringBuffer xpath("//node[@id='%s']", subGraphId);
            toXML(xgmml->queryPropTree(xpath.str()), xml);
        }

        g->setGraph(xml.str());
        ECLGraphs.append(*g.getClear());
    }
    return;
}

bool CWsWorkunitsEx::onWUQueryGetGraph(IEspContext& context, IEspWUQueryGetGraphRequest& req, IEspWUQueryGetGraphResponse& resp)
{
    try
    {
        IArrayOf<IEspECLGraphEx> graphs;
        getGraphsByQueryId(req.getTarget(), req.getQueryId(), req.getGraphName(), req.getSubGraphId(), graphs);
        resp.setGraphs(graphs);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsWorkunitsEx::resetQueryStats(IEspContext& context, const char* target, IProperties* queryIds, IEspWUQuerySetQueryActionResponse& resp)
{
    IArrayOf<IEspQuerySetQueryActionResult> results;
    Owned<IEspQuerySetQueryActionResult> result = createQuerySetQueryActionResult();
    try
    {
        StringBuffer control;
        Owned<IPropertyIterator> it = queryIds->getIterator();
        ForEach(*it)
        {
            const char *queryId = it->getPropKey();
            if (queryId && *queryId)
            {
                appendXMLOpenTag(control, "Query", NULL, false);
                appendXMLAttr(control, "id", queryId);
                if (target && *target)
                    appendXMLAttr(control, "target", target);
                control.append("/>");
            }
        }
        if (!control.length())
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "CWsWorkunitsEx::resetQueryStats: Query ID not specified");

        control.insert(0, "<control:resetquerystats>");
        control.append("</control:resetquerystats>");

        if (!sendControlQuery(context, target, control.str(), ROXIECONNECTIONTIMEOUT))
            throw MakeStringException(ECLWATCH_INTERNAL_ERROR, "CWsWorkunitsEx::resetQueryStats: Failed to send roxie control query");

        result->setMessage("Query stats reset succeeded");
        result->setSuccess(true);;
    }
    catch(IMultiException *me)
    {
        StringBuffer msg;
        result->setMessage(me->errorMessage(msg).str());
        result->setCode(me->errorCode());
        result->setSuccess(false);
        me->Release();
    }
    catch(IException *e)
    {
        StringBuffer msg;
        result->setMessage(e->errorMessage(msg).str());
        result->setCode(e->errorCode());
        result->setSuccess(false);
        e->Release();
    }
    results.append(*result.getClear());
    resp.setResults(results);
    return true;
}

IPropertyTree* CWsWorkunitsEx::sendControlQuery(IEspContext& context, const char* target, const char* query, unsigned timeout)
{
    if (!target || !*target)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "CWsWorkunitsEx::sendControlQuery: target not specified");

    if (!query || !*query)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "CWsWorkunitsEx::sendControlQuery: Control query not specified");

#ifndef _CONTAINERIZED
    Owned<IConstWUClusterInfo> info = getTargetClusterInfo(target);
    if (!info || (info->getPlatform()!=RoxieCluster)) //Only support roxie for now
        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "CWsWorkunitsEx::sendControlQuery: Invalid target name %s", target);

    const SocketEndpointArray &eps = info->getRoxieServers();
    if (eps.empty())
        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "CWsWorkunitsEx::sendControlQuery: Server not found for %s", target);

    Owned<ISocket> sock = ISocket::connect_timeout(eps.item(0), timeout);
    return sendRoxieControlQuery(sock, query, timeout);
#else
    ISmartSocketFactory *conn = roxieConnMap.getValue(target);
    if (!conn)
        throw makeStringExceptionV(ECLWATCH_CANNOT_GET_ENV_INFO, "roxie target cluster not mapped: %s", target);

    return sendRoxieControlQuery(conn, query, timeout, ROXIECONNECTIONTIMEOUT);
#endif
}

bool CWsWorkunitsEx::onWUUpdateQueryEntry(IEspContext& context, IEspWUUpdateQueryEntryRequest& req, IEspWUUpdateQueryEntryResponse& resp)
{
    try
    {
        StringBuffer querySetName, query;
        ensureInputString(req.getQuerySet(), true, querySetName, ECLWATCH_QUERYSET_NOT_FOUND, "Query Set not specified");
        ensureInputString(req.getQueryId(), true, query, ECLWATCH_QUERYID_NOT_FOUND, "Query not specified");

        Owned<IPropertyTree> querySet = getQueryRegistry(querySetName.str(), true);
        if (!querySet)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Queryset %s not found", querySetName.str());
        VStringBuffer xpath("Query[@id=\"%s\"]", query.str());
        IPropertyTree *tree = querySet->queryPropTree(xpath);
        if (!tree)
            throw MakeStringException(ECLWATCH_QUERYSET_NOT_FOUND, "Query %s not found", query.str());

        StringBuffer comment(req.getComment());
        if (comment.isEmpty())
            tree->removeProp("@comment");
        else
            tree->setProp("@comment", comment.str());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsWorkunitsEx::onWUGetNumFileToCopy(IEspContext& context, IEspWUGetNumFileToCopyRequest& req, IEspWUGetNumFileToCopyResponse& resp)
{
    class CWUGetNumFileToCopyPager : public CSimpleInterface, implements IElementsPager
    {
        StringAttr clusterName;
        StringAttr sortOrder;
        MapStringToMyClass<ISmartSocketFactory> *roxieConnMap;
    public:
        IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

#ifndef _CONTAINERIZED
        CWUGetNumFileToCopyPager(const char* _clusterName, const char *_sortOrder)
            : clusterName(_clusterName), sortOrder(_sortOrder) { };
#else
        CWUGetNumFileToCopyPager(const char* _clusterName, const char* _sortOrder, MapStringToMyClass<ISmartSocketFactory>* _roxieConnMap)
            : clusterName(_clusterName), sortOrder(_sortOrder), roxieConnMap(_roxieConnMap) { };
#endif

        virtual IRemoteConnection* getElements(IArrayOf<IPropertyTree> &elements)
        {
#ifndef _CONTAINERIZED
            SocketEndpointArray servers;
            getRoxieProcessServers(clusterName.get(), servers);
            if (servers.length() < 1)
            {
                PROGLOG("WUGetNumFileToCopy: Process Server not found for %s", clusterName.get());
                return NULL;
            }
            Owned<IPropertyTree> result = sendRoxieControlAllNodes(servers.item(0), "<control:numfilestoprocess/>", false, ROXIELOCKCONNECTIONTIMEOUT);
#else
            ISmartSocketFactory *conn = roxieConnMap->getValue(clusterName);
            if (!conn)
            {
                PROGLOG("WUGetNumFileToCopy: Process Server not found for %s", clusterName.get());
                return nullptr;
            }
            Owned<IPropertyTree> result = sendRoxieControlAllNodes(conn, "<control:numfilestoprocess/>", false, ROXIELOCKCONNECTIONTIMEOUT, ROXIECONNECTIONTIMEOUT);
#endif
            if (!result)
            {
                PROGLOG("WUGetNumFileToCopy: Empty result received for cluster %s", clusterName.get());
                return NULL;
            }
            Owned<IPropertyTreeIterator> iter = result->getElements("*");
            if (!iter)
                return NULL;

            StringArray unknownAttributes;
            sortElements(iter, sortOrder.get(), NULL, NULL, unknownAttributes, elements);
            return NULL;
        }
        virtual bool allMatchingElementsReceived() { return true; } //For now, roxie always returns all of matched items.
    };

    try
    {
#ifndef _CONTAINERIZED
        StringBuffer clusterName(req.getClusterName());
        if (clusterName.isEmpty())
            throw MakeStringException(ECLWATCH_CANNOT_RESOLVE_CLUSTER_NAME, "Cluster not specified");
#else
        StringBuffer targetName(req.getTargetName());
        if (targetName.isEmpty())
            targetName.set(req.getClusterName()); //for backward compatible
        if (targetName.isEmpty())
            throw makeStringException(ECLWATCH_CANNOT_RESOLVE_CLUSTER_NAME, "Target not specified");
#endif
        StringBuffer so;
        bool descending = req.getDescending();
        if (descending)
            so.set("-");
        const char *sortBy =  req.getSortby();
        if (!isEmptyString(sortBy) && strieq(sortBy, "URL"))
            so.append("?@ep");
        else if (!isEmptyString(sortBy) && strieq(sortBy, "Status"))
            so.append("?Status");
        else
            so.append("#FilesToProcess/@value");

        unsigned pageSize = req.getPageSize();
        unsigned pageStartFrom = req.getPageStartFrom();
        if(pageSize < 1)
            pageSize = 100;

        __int64 cacheHint = 0;
        if (!req.getCacheHint_isNull())
            cacheHint = req.getCacheHint();

        unsigned numberOfEndpoints = 0;
        IArrayOf<IPropertyTree> results;
#ifndef _CONTAINERIZED
        Owned<IElementsPager> elementsPager = new CWUGetNumFileToCopyPager(clusterName, so);
#else
        Owned<IElementsPager> elementsPager = new CWUGetNumFileToCopyPager(targetName, so, &roxieConnMap);
#endif
        getElementsPaged(elementsPager, pageStartFrom, pageSize, NULL, "", &cacheHint, results, &numberOfEndpoints, NULL, false);

        IArrayOf<IEspClusterEndpoint> endpoints;
        ForEachItemIn(i, results)
        {
            IPropertyTree &item = results.item(i);

            Owned<IEspClusterEndpoint> endpoint = createClusterEndpoint();
            endpoint->setURL(item.queryProp("@ep"));
            endpoint->setStatus(item.queryProp("Status"));
            endpoint->setNumQueryFileToCopy(item.getPropInt("FilesToProcess/@value", 0));
            endpoints.append(*endpoint.getClear());
        }

        resp.setEndpoints(endpoints);
        resp.setCacheHint(cacheHint);
        resp.setTotal(numberOfEndpoints);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsWorkunitsEx::onWUQueryGetSummaryStats(IEspContext& context, IEspWUQueryGetSummaryStatsRequest& req, IEspWUQueryGetSummaryStatsResponse& resp)
{
    try
    {
        const char *target = req.getTarget();
        if (isEmptyString(target))
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Target name required");

#ifndef _CONTAINERIZED
        Owned<IConstWUClusterInfo> info = getTargetClusterInfo(target);
        if (!info || (info->getPlatform()!=RoxieCluster)) //Only support roxie for now
            throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Roxie name not found");
#else
        ISmartSocketFactory *conn = roxieConnMap.getValue(target);
        if (!conn)
            throw makeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Roxie name not found");
        if (!k8s::isActiveService(target))
            throw makeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "Roxie cluster has no active servers");
#endif

        double version = context.getClientVersion();
        const char *queryId = req.getQueryId();
        if (!isEmptyString(queryId))
            PROGLOG("WUQueryGetSummaryStats: target %s, query %s", target, queryId);
        else
            PROGLOG("WUQueryGetSummaryStats: target %s", target);

#ifndef _CONTAINERIZED
        const SocketEndpointArray &eps = info->getRoxieServers();
        if (eps.empty())
        {
            IERRLOG("WUQueryGetSummaryStats: Failed to getRoxieServers for %s", target);
            return true;
        }
#endif

        bool includeRawStats = req.getIncludeRawStats();
        const char *fromTime = req.getFromTime();
        const char *toTime = req.getToTime();
        VStringBuffer control("<control:queryAggregates");
        if (!isEmpty(fromTime))
            control.appendf(" from='%s'", fromTime);
        if (!isEmpty(toTime))
            control.appendf(" to='%s'", toTime);
        if (includeRawStats)
        {
            if (!isEmptyString(queryId))
                control.appendf(" rawStats='1'><Query id='%s'/></control:queryAggregates>", queryId);
            else
                control.append(" all='1' rawStats='1' />");
        }
        else if (!isEmptyString(queryId))
        {
            control.appendf("><Query id='%s'/></control:queryAggregates>", queryId);
        }
        else
            control.append(" />");

#ifndef _CONTAINERIZED
        Owned<IPropertyTree> queryAggregates = sendRoxieControlAllNodes(eps.item(0), control.str(), false, ROXIELOCKCONNECTIONTIMEOUT);
#else
        Owned<IPropertyTree> queryAggregates = sendRoxieControlAllNodes(conn, control, false, ROXIELOCKCONNECTIONTIMEOUT, ROXIECONNECTIONTIMEOUT);
#endif
        if (!queryAggregates)
        {
            PROGLOG("WUQueryGetSummaryStats: %s returns empty for %s", control.str(), target);
            return true;
        }

        if (getEspLogLevel() >= LogMax)
        {
            StringBuffer sb;
            toXML(queryAggregates, sb);
            DBGLOG("getQueryStats(): '%s' => '%s'", control.str(), sb.str());
        }

        IArrayOf<IEspQuerySummaryStats> querySummaryStatsList;
        IArrayOf<IEspEndpointQueryStats> queryStatsList;
        Owned<IPropertyTreeIterator> queryStatsOnEndpointItr = queryAggregates->getElements("Endpoint");
        ForEach(*queryStatsOnEndpointItr)
        {
            IPropertyTree &queryStatsOnEndpoint = queryStatsOnEndpointItr->query();
            const char *status = queryStatsOnEndpoint.queryProp("Status");
            const char *ep = queryStatsOnEndpoint.queryProp("@ep");
            if (isEmptyString(ep))
                continue;

            if (version >= 1.75)
            {
                if (includeRawStats)
                    readQueryStatsList(&queryStatsOnEndpoint, status, ep, includeRawStats, queryStatsList);
                else
                    readQueryAggregateStats(queryStatsOnEndpoint.queryPropTree("Query"), status, ep, querySummaryStatsList);
            }
            else if (!isEmptyString(queryId))
                readQueryAggregateStats(queryStatsOnEndpoint.queryPropTree("Query"), status, ep, querySummaryStatsList);
        }

        resp.setStatsList(querySummaryStatsList);
        resp.setQueryStatsList(queryStatsList);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsWorkunitsEx::readQueryAggregateStats(IPropertyTree *queryStats, const char *status, const char *ep,
    IArrayOf<IEspQuerySummaryStats> &querySummaryStatsList)
{
    if (!queryStats)
        return;

    Owned<IEspQuerySummaryStats> querySummaryStats = createQuerySummaryStats();
    if (!isEmptyString(ep))
        querySummaryStats->setEndpoint(ep);
    if (queryStats->hasProp("countFailed"))
        querySummaryStats->setCountFailed(queryStats->getPropInt("countFailed"));
    if (queryStats->hasProp("countTotal"))
        querySummaryStats->setCountTotal(queryStats->getPropInt("countTotal"));
    if (queryStats->hasProp("averageBytesOut"))
        querySummaryStats->setAverageBytesOut(queryStats->getPropInt64("averageBytesOut"));
    if (queryStats->hasProp("averageMemUsed"))
        querySummaryStats->setSizeAvgPeakMemory(queryStats->getPropInt64("averageMemUsed"));
    if (queryStats->hasProp("averageSlavesReplyLen"))
        querySummaryStats->setAverageSlavesReplyLen(queryStats->getPropInt("averageSlavesReplyLen"));
    if (queryStats->hasProp("averageTimeMs"))
        querySummaryStats->setTimeAvgTotalExecuteMinutes(queryStats->getPropInt64("averageTimeMs"));
    if (queryStats->hasProp("minTimeMs"))
        querySummaryStats->setTimeMinTotalExecuteMinutes(queryStats->getPropInt64("minTimeMs"));
    if (queryStats->hasProp("maxTimeMs"))
        querySummaryStats->setTimeMaxTotalExecuteMinutes(queryStats->getPropInt64("maxTimeMs"));
    if (queryStats->hasProp("percentile97"))
    {
        querySummaryStats->setPercentile97(queryStats->getPropInt("percentile97"));
        if (queryStats->hasProp("percentile97/@estimate"))
            querySummaryStats->setPercentile97Estimate(queryStats->getPropBool("percentile97/@estimate"));
    }
    const char *startTime = queryStats->queryProp("startTime");
    const char *endTime = queryStats->queryProp("endTime");
    if (!isEmptyString(startTime))
        querySummaryStats->setStartTime(startTime);
    if (!isEmptyString(endTime))
        querySummaryStats->setEndTime(endTime);
    if (!isEmptyString(status))
        querySummaryStats->setStatus(status);
    querySummaryStatsList.append(*querySummaryStats.getLink());
}

void CWsWorkunitsEx::readQueryStatsList(IPropertyTree *queryStatsTree, const char *status, const char *ep,
    bool includeRawStats, IArrayOf<IEspEndpointQueryStats> &endpointQueryStatsList)
{
    if (!queryStatsTree)
        return;

    IArrayOf<IEspQueryStats> queryStatsList;
    Owned<IPropertyTreeIterator> queryItr = queryStatsTree->getElements("QueryStats/Query");
    ForEach(*queryItr)
    {
        IPropertyTree &query = queryItr->query();
        readQueryStats(&query, query.queryProp("@id"), includeRawStats, queryStatsList);
    }

    IPropertyTree *globalStats = queryStatsTree->queryPropTree("QueryStats/Global");
    if (globalStats)
        readQueryStats(globalStats, "Global", includeRawStats, queryStatsList);

    if (queryStatsList.ordinality() == 0)
        return;

    Owned<IEspEndpointQueryStats> endpointQueryStats = createEndpointQueryStats();
    endpointQueryStats->setEndpoint(ep);
    if (!isEmptyString(status))
        endpointQueryStats->setStatus(status);

    endpointQueryStats->setQueryStatsList(queryStatsList);
    endpointQueryStatsList.append(*endpointQueryStats.getLink());
}

void CWsWorkunitsEx::readQueryStats(IPropertyTree *queryStatsTree, const char *id, bool includeRawStats,
    IArrayOf<IEspQueryStats> &queryStatsList)
{
    if (!queryStatsTree)
        return;

    Owned<IEspQueryStats> queryStats = createQueryStats();
    if (!isEmptyString(id))
        queryStats->setID(id);

    if (!includeRawStats)
    {
        IArrayOf<IEspQuerySummaryStats> aggregateQueryStatsList;
        readQueryAggregateStats(queryStatsTree, nullptr, nullptr, aggregateQueryStatsList);
        queryStats->setAggregateQueryStatsList(aggregateQueryStatsList);
        queryStatsList.append(*queryStats.getLink());
        return;
    }

    IArrayOf<IEspQuerySummaryStats> aggregateQueryStatsList;
    Owned<IPropertyTreeIterator> aggregateRecordItr = queryStatsTree->getElements("QueryStatsAggregateRecord");
    ForEach(*aggregateRecordItr)
    {
        IPropertyTree &query = aggregateRecordItr->query();
        readQueryAggregateStats(&query, nullptr, nullptr, aggregateQueryStatsList);
    }
    queryStats->setAggregateQueryStatsList(aggregateQueryStatsList);

    IArrayOf<IEspQueryStatsRecord> recordList;
    Owned<IPropertyTreeIterator> recordItr = queryStatsTree->getElements("QueryStatsRecord");
    ForEach(*recordItr)
    {
        IPropertyTree &query = recordItr->query();
        readQueryStatsRecord(&query, recordList);
    }
    queryStats->setQueryStatsRecordList(recordList);

    queryStatsList.append(*queryStats.getLink());
}

void CWsWorkunitsEx::readQueryStatsRecord(IPropertyTree *queryRecord, IArrayOf<IEspQueryStatsRecord> &recordList)
{
    if (!queryRecord)
        return;

    Owned<IEspQueryStatsRecord> record = createQueryStatsRecord();
    const char *startTime = queryRecord->queryProp("@startTime");
    if (!isEmptyString(startTime))
        record->setStartTime(startTime);

    if (queryRecord->hasProp("elapsedTimeMs"))
        record->setElapsedTimeMs(queryRecord->getPropInt64("elapsedTimeMs"));
    if (queryRecord->hasProp("memUsed"))
        record->setMemoryUsed(queryRecord->getPropInt64("memUsed"));
    if (queryRecord->hasProp("bytesOut"))
        record->setBytesOut(queryRecord->getPropInt64("bytesOut"));
    if (queryRecord->hasProp("slavesReplyLen"))
        record->setSlavesReplyLen(queryRecord->getPropInt("slavesReplyLen"));

    recordList.append(*record.getLink());
}
