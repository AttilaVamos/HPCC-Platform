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

#include <math.h>

#include "jcontainerized.hpp"
#include "daclient.hpp"
#include "daft.hpp"
#include "daftcfg.hpp"
#include "fterror.hpp"
#include "fverror.hpp"
#include "daftprogress.hpp"
#include "mpbase.hpp"
#include "daclient.hpp"
#include "dadfs.hpp"
#include "dafdesc.hpp"
#include "dasds.hpp"
#include "danqs.hpp"
#include "dalienv.hpp"
#include "dautils.hpp"
#include "jfile.hpp"
#include "wshelpers.hpp"
#include "LogicFileWrapper.hpp"
#include "rmtfile.hpp"
#include "dfuutil.hpp"
#include "TpWrapper.hpp"
#include "WUWrapper.hpp"
#include "portlist.h"
#include "dfuwu.hpp"
#include "fverror.hpp"
#include "nbcd.hpp"
#include "thorcommon.hpp"
#include "jstats.h"
#include "thorfile.hpp"

#include "jstring.hpp"
#include "exception_util.hpp"

#include "hqlerror.hpp"
#include "hqlexpr.hpp"
#include "hqlutil.hpp"
#include "eclrtl.hpp"
#include "package.h"
#include "daaudit.hpp"

#include "jflz.hpp"
#include "digisign.hpp"

#include "wsdfuaccess/wsdfuaccess.hpp"

#include "ws_dfuService.hpp"

using namespace wsdfuaccess;
using namespace cryptohelper;


#define     Action_Delete           "Delete"
#define     Action_AddtoSuperfile   "Add To Superfile"
static const char* FEATURE_URL="DfuAccess";
#define     FILE_NEWEST     1
#define     FILE_OLDEST     2
#define     FILE_LARGEST    3
#define     FILE_SMALLEST   4
#define     COUNTBY_SCOPE   "Scope"
#define     COUNTBY_OWNER   "Owner"
#define     COUNTBY_DATE    "Date"
#define     COUNTBY_YEAR    "Year"
#define     COUNTBY_QUARTER "Quarter"
#define     COUNTBY_MONTH   "Month"
#define     COUNTBY_DAY     "Day"

#define REMOVE_FILE_SDS_CONNECT_TIMEOUT (1000*15)  // 15 seconds

static const char *DFUFileIdSeparator = "|";
static const char *DFUFileCreate_FileNamePostfix = ".wsdfucreate.tmp";
static const char *DFUFileCreate_GroupNamePrefix = "wsdfucreate";
static const char *ConfigurationDirectoryForDataCategory = "data";
static std::atomic<unsigned> dfuCreateUniqId{1};

const unsigned NODE_GROUP_CACHE_DEFAULT_TIMEOUT = 30*60*1000; //30 minutes

const unsigned MAX_VIEWKEYFILE_ROWS = 1000;
const unsigned MAX_KEY_ROWS = 20;

short days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};


CThorNodeGroup* CThorNodeGroupCache::readNodeGroup(const char* _groupName)
{
#ifndef _CONTAINERIZED
    Owned<IEnvironmentFactory> factory = getEnvironmentFactory(true);
    Owned<IConstEnvironment> env = factory->openEnvironment();
    Owned<IPropertyTree> root = &env->getPTree();
    Owned<IPropertyTreeIterator> it= root->getElements("Software/ThorCluster");
    ForEach(*it)
    {
        IPropertyTree& cluster = it->query();
        StringBuffer groupName;
        getClusterGroupName(cluster, groupName);
        if (groupName.length() && strieq(groupName.str(), _groupName))
            return new CThorNodeGroup(_groupName, cluster.getCount("ThorSlaveProcess"), cluster.getPropBool("@replicateOutputs", false));
    }
#endif

    return NULL;
}

CThorNodeGroup* CThorNodeGroupCache::lookup(const char* groupName, unsigned timeout)
{
    CriticalBlock block(sect);
    CThorNodeGroup* item=SuperHashTableOf<CThorNodeGroup, const char>::find(groupName);
    if (item && !item->checkTimeout(timeout))
        return LINK(item);

    Owned<CThorNodeGroup> e = readNodeGroup(groupName);
    if (e)
        replace(*e.getLink()); //if not exists, will be added.

    return e.getClear();
}

void CWsDfuEx::init(IPropertyTree *cfg, const char *process, const char *service)
{

    DBGLOG("Initializing %s service [process = %s]", service, process);

    espProcess.set(process);

    VStringBuffer xpath("Software/EspProcess[@name=\"%s\"]", process);
    IPropertyTree *processTree = cfg->queryPropTree(xpath);
    if (!processTree)
        throw MakeStringException(-1, "config not found for process %s", process);

    xpath.clear().appendf("EspService[@name=\"%s\"]", service);
    IPropertyTree *serviceTree = processTree->queryPropTree(xpath);
    if (!serviceTree)
        throw MakeStringException(-1, "config not found for service %s", service);

    serviceTree->getProp("DefaultScope", defaultScope_);
    serviceTree->getProp("User", user_);
    serviceTree->getProp("Password", password_);

    StringBuffer disableUppercaseTranslation;
    serviceTree->getProp("DisableUppercaseTranslation", disableUppercaseTranslation);

    m_clusterName.clear();
    serviceTree->getProp("ClusterName", m_clusterName);

    const char * plugins = serviceTree->queryProp("Plugins/@path");
    if (plugins)
        queryTransformerRegistry().addPlugins(plugins);

    m_disableUppercaseTranslation = false;
    if (streq(disableUppercaseTranslation.str(), "true"))
        m_disableUppercaseTranslation = true;

    if (processTree->hasProp("@PageCacheTimeoutSeconds"))
        setPageCacheTimeoutMilliSeconds(processTree->getPropInt("@PageCacheTimeoutSeconds"));
    if (processTree->hasProp("@MaxPageCacheItems"))
        setMaxPageCacheItems(processTree->getPropInt("@MaxPageCacheItems"));

    int timeout = serviceTree->getPropInt("NodeGroupCacheMinutes", -1);
    if (timeout > -1)
        nodeGroupCacheTimeout = (unsigned) timeout*60*1000;
    else
        nodeGroupCacheTimeout = NODE_GROUP_CACHE_DEFAULT_TIMEOUT;
    thorNodeGroupCache.setown(new CThorNodeGroupCache());

    if (!daliClientActive())
        throw MakeStringException(-1, "No Dali Connection Active. Please Specify a Dali to connect to in you configuration file");

    setDaliServixSocketCaching(true);

#ifdef _CONTAINERIZED
    IERRLOG("CONTAINERIZED(CWsDfuEx::init)");
    maxFileAccessExpirySeconds = defaultMaxFileAccessExpirySeconds;
#else
    factory.setown(getEnvironmentFactory(true));
    env.setown(factory->openEnvironment());
#endif
    maxFileAccessExpirySeconds = serviceTree->getPropInt("@maxFileAccessExpirySeconds", defaultMaxFileAccessExpirySeconds);
}

bool CWsDfuEx::onDFUSearch(IEspContext &context, IEspDFUSearchRequest & req, IEspDFUSearchResponse & resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUSearch: Permission denied.");

        StringBuffer username;
        context.getUserID(username);

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        CTpWrapper dummy;
        IArrayOf<IEspTpCluster> clusters;
        dummy.getClusterProcessList(eqThorCluster, clusters);
        dummy.getHthorClusterList(clusters);

        StringArray dfuclusters;
        ForEachItemIn(k, clusters)
        {
            IEspTpCluster& cluster = clusters.item(k);
            dfuclusters.append(cluster.getName());
        }

        IArrayOf<IEspTpCluster> clusters1;
        dummy.getClusterProcessList(eqRoxieCluster, clusters1);
        ForEachItemIn(k1, clusters1)
        {
            IEspTpCluster& cluster = clusters1.item(k1);
            StringBuffer slaveName(cluster.getName());
            dfuclusters.append(slaveName.str());
        }

        StringArray ftarray;
        ftarray.append("Logical Files and Superfiles");
        ftarray.append("Logical Files Only");
        ftarray.append("Superfiles Only");
        ftarray.append("Not in Superfiles");

        if (req.getShowExample() && *req.getShowExample())
            resp.setShowExample(req.getShowExample());
        resp.setClusterNames(dfuclusters);
        resp.setFileTypes(ftarray);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void addToQueryString(StringBuffer &queryString, const char *name, const char *value)
{
    if (queryString.length() > 0)
    {
        queryString.append("&");
    }

    queryString.append(name);
    queryString.append("=");
    queryString.append(value);
}

void addToQueryStringFromInt(StringBuffer &queryString, const char *name, __int64 value)
{
    if (queryString.length() > 0)
    {
        queryString.append("&");
    }

    queryString.append(name);
    queryString.append("=");
    queryString.append(value);
}

void parseTwoStringArrays(const char *input, StringArray& strarray1, StringArray& strarray2)
{
    if (!input || strlen(input) <= 2)
        return;

    char c0[2], c1[2];
    c0[0] = input[0], c0[1] = 0;
    c1[0] = input[1], c1[1] = 0;

    //the first string usually is a name; the second is a value
    unsigned int name_len = atoi(c0);
    unsigned int value_len = atoi(c1);
    if (name_len > 0 && value_len > 0)
    {
        char * inputText = (char *) input;
        inputText += 2; //skip 2 chars

        for (;;)
        {
            if (!inputText || strlen(inputText) < name_len + value_len)
                break;

            StringBuffer columnNameLenStr, columnValueLenStr;
            for (unsigned i_name = 0; i_name < name_len; i_name++)
            {
                columnNameLenStr.append(inputText[0]);
                inputText++;
            }
            for (unsigned i_value = 0; i_value < value_len; i_value++)
            {
                columnValueLenStr.append(inputText[0]);
                inputText++;
            }

            unsigned columnNameLen = atoi(columnNameLenStr.str());
            unsigned columnValueLen = atoi(columnValueLenStr.str());

            if (!inputText || strlen(inputText) < columnNameLen + columnValueLen)
                break;

            char * colon = inputText + columnNameLen;
            if (!colon)
                break;

            StringAttr tmp;
            tmp.set(inputText, columnNameLen);
            strarray1.append(tmp.get());
            tmp.set(colon, columnValueLen);
            //tmp.toUpperCase();
            strarray2.append(tmp.get());

            inputText = colon + columnValueLen;
        }
    }

    return;
}

bool CWsDfuEx::onDFUGetMetaInquiry(IEspContext &context, IEspDFUMetaInquiryRequest & req, IEspDFUMetaInquiryResponse & resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::onDFUGetMetaInquiry: Permission denied.");

        DFUQResultField field = DFUQResultField::name; // 1st enum
        IArrayOf<IEspDFUMetaFieldInfo> fields;
        while (true)
        {
            Owned<IEspDFUMetaFieldInfo> result = createDFUMetaFieldInfo();
            const char *fieldName = getDFUQResultFieldName(field);
            DFUQResultFieldType type = getDFUQResultFieldType(field);
            const char *typeName = getDFUQResultFieldTypeName(type);
            result->setName(fieldName);
            result->setType(typeName);
            fields.append(*result.getClear());
            field = (DFUQResultField) ((unsigned)field + 1);
            if (field == DFUQResultField::term)
                break;
        }
        resp.setFields(fields);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }

    return true;
}

bool CWsDfuEx::onDFUQuery(IEspContext &context, IEspDFUQueryRequest & req, IEspDFUQueryResponse & resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUQuery: Permission denied.");

        StringBuffer username;
        context.getUserID(username);

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        doLogicalFileSearch(context, userdesc.get(), req, resp);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }

    return true;
}

bool CWsDfuEx::onDFUInfo(IEspContext &context, IEspDFUInfoRequest &req, IEspDFUInfoResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUInfo: Permission denied.");

        StringBuffer username;
        context.getUserID(username);

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        bool forceIndexInfo = req.getForceIndexInfo();
        if (req.getUpdateDescription())
        {
            double version = context.getClientVersion();
            if (version < 1.38)
                doGetFileDetails(context, userdesc.get(), req.getFileName(), req.getCluster(), req.getQuerySet(), req.getQuery(), req.getFileDesc(),
                    req.getIncludeJsonTypeInfo(), req.getIncludeBinTypeInfo(), req.getProtect(), req.getRestrict(), resp.updateFileDetail(), forceIndexInfo);
            else
                doGetFileDetails(context, userdesc.get(), req.getName(), req.getCluster(), req.getQuerySet(), req.getQuery(), req.getFileDesc(),
                    req.getIncludeJsonTypeInfo(), req.getIncludeBinTypeInfo(), req.getProtect(), req.getRestrict(), resp.updateFileDetail(), forceIndexInfo);
        }
        else
        {
            doGetFileDetails(context, userdesc.get(), req.getName(), req.getCluster(), req.getQuerySet(), req.getQuery(), NULL,
                    req.getIncludeJsonTypeInfo(), req.getIncludeBinTypeInfo(), req.getProtect(), req.getRestrict(), resp.updateFileDetail(), forceIndexInfo);
        }
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }

    return true;
}

bool CWsDfuEx::onDFUSpace(IEspContext &context, IEspDFUSpaceRequest & req, IEspDFUSpaceResponse & resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUSpace: Permission denied.");

        StringBuffer username;
        context.getUserID(username);

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        const char *countby = req.getCountBy();
        if (!countby || strlen(countby) < 1)
            return true;

        char *scopeName = NULL;
        StringBuffer filter;
        if(req.getScopeUnder() && *req.getScopeUnder())
        {
            scopeName = (char *) req.getScopeUnder();
            filter.appendf("%s::*", scopeName);
            resp.setScopeUnder(req.getScopeUnder());
        }
        else
        {
            filter.append("*");
        }

        PROGLOG("DFUSpace: filter %s ", filter.str());
        Owned<IPropertyTreeIterator> fi = queryDistributedFileDirectory().getDFAttributesIterator(filter, userdesc.get(), true, false, nullptr);
        if(!fi)
            throw MakeStringException(ECLWATCH_CANNOT_GET_FILE_ITERATOR,"Cannot get information from file system.");

        const char *ownerUnder = NULL;
        if(req.getOwnerUnder() && *req.getOwnerUnder())
        {
            ownerUnder = req.getOwnerUnder();
            resp.setOwnerUnder(ownerUnder);
        }

        StringBuffer wuFrom, wuTo, interval;
        unsigned yearFrom = 0, monthFrom, dayFrom, yearTo = 0, monthTo, dayTo, hour, minute, second, nano;
        if(req.getStartDate() && *req.getStartDate())
        {
            CDateTime wuTime;
            wuTime.setString(req.getStartDate(),NULL,true);

            wuTime.getDate(yearFrom, monthFrom, dayFrom, true);
            wuTime.getTime(hour, minute, second, nano, true);
            wuFrom.appendf("%4d-%02d-%02d %02d:%02d:%02d",yearFrom,monthFrom,dayFrom,hour,minute,second);

            StringBuffer startDate;
            startDate.appendf("%02d/%02d/%04d", monthFrom, dayFrom, yearFrom);
            resp.setStartDate(startDate.str());
        }

        if(req.getEndDate() && *req.getEndDate())
        {
            CDateTime wuTime;
            wuTime.setString(req.getEndDate(),NULL,true);

            wuTime.getDate(yearTo, monthTo, dayTo, true);
            wuTime.getTime(hour, minute, second, nano, true);
            wuTo.appendf("%4d-%02d-%02d %02d:%02d:%02d",yearTo,monthTo,dayTo,hour,minute,second);

            StringBuffer endDate;
            endDate.appendf("%02d/%02d/%04d", monthTo, dayTo, yearTo);
            resp.setEndDate(endDate.str());
        }

        unsigned i = 0;
        IArrayOf<IEspSpaceItem> SpaceItems64;
        if (!stricmp(countby, COUNTBY_DATE))
        {
            if (yearFrom < 1 || yearTo < 1)
            {
                StringBuffer wuFrom, wuTo;
                bool bFirst = true;
                ForEach(*fi)
                {
                    IPropertyTree &attr=fi->query();
                    StringBuffer modf(attr.queryProp("@modified"));
                    //char* t=strchr(modf.str(),'T');
                    //if(t) *t=' ';

                    if (bFirst)
                    {
                        bFirst = false;
                        wuFrom = modf.str();
                        wuTo = modf.str();
                        continue;
                    }

                    if (strcmp(modf.str(),wuFrom.str())<0)
                        wuFrom = modf.str();

                    if (strcmp(modf.str(),wuTo.str())>0)
                        wuTo = modf.str();
                }

                if (yearFrom < 1)
                {
                    CDateTime wuTime;
                    wuTime.setString(wuFrom.str(),NULL,true);
                    wuTime.getDate(yearFrom, monthFrom, dayFrom, true);
                }
                if (yearTo < 1)
                {
                    CDateTime wuTime;
                    wuTime.setString(wuTo.str(),NULL,true);
                    wuTime.getDate(yearTo, monthTo, dayTo, true);
                }
            }
            interval = req.getInterval();
            resp.setInterval(interval);

            createSpaceItemsByDate(SpaceItems64, interval, yearFrom, monthFrom, dayFrom, yearTo, monthTo, dayTo);
        }
        else
        {
            Owned<IEspSpaceItem> item64 = createSpaceItem();
            if (!strieq(countby, COUNTBY_OWNER))
            {
                if (scopeName)
                    item64->setName(scopeName);
                else
                    item64->setName("(root)");
            }
            else
            {
                item64->setName("(empty)");
            }

            item64->setNumOfFilesInt(0);
            item64->setNumOfFilesIntUnknown(0);
            item64->setTotalSizeInt(0);
            item64->setLargestSizeInt(0);
            item64->setSmallestSizeInt(0);
            item64->setLargestFile("");
            item64->setSmallestFile("");
            SpaceItems64.append(*item64.getClear());
        }

        ForEach(*fi)
        {
            IPropertyTree &attr=fi->query();

            if (attr.hasProp("@numsubfiles"))
                continue; //exclude superfiles

            if (ownerUnder)
            {
                const char* owner=attr.queryProp("@owner");
                if (owner && stricmp(owner, ownerUnder))
                    continue;
            }

              StringBuffer modf(attr.queryProp("@modified"));
              char* t= (char *) strchr(modf.str(),'T');
              if(t) *t=' ';

            if (wuFrom.length() && strcmp(modf.str(),wuFrom.str())<0)
                    continue;

             if (wuTo.length() && strcmp(modf.str(),wuTo.str())>0)
                    continue;

            if (!stricmp(countby, COUNTBY_DATE))
            {
                setSpaceItemByDate(SpaceItems64, interval, attr.queryProp("@modified"), attr.queryProp("@name"), attr.getPropInt64("@size",-1));
            }
            else if (!stricmp(countby, COUNTBY_OWNER))
            {
                setSpaceItemByOwner(SpaceItems64, attr.queryProp("@owner"), attr.queryProp("@name"), attr.getPropInt64("@size",-1));
            }
            else
            {
                setSpaceItemByScope(SpaceItems64, scopeName, attr.queryProp("@name"), attr.getPropInt64("@size",-1));
            }
        }

        i = 0;
        IEspSpaceItem& item0 = SpaceItems64.item(0);
        if (item0.getNumOfFilesInt() < 1)
        {
            i++;
        }

        double version = context.getClientVersion();
        IArrayOf<IEspDFUSpaceItem> SpaceItems;
        for(; i < SpaceItems64.length();i++)
        {
            IEspSpaceItem& item64 = SpaceItems64.item(i);
            if (item64.getNumOfFilesInt() < 1)
                continue;

            StringBuffer buf;
            Owned<IEspDFUSpaceItem> item1 = createDFUSpaceItem("","");

            __int64 numOfFiles = item64.getNumOfFilesInt();
            __int64 numOfFilesIntUnknown = item64.getNumOfFilesIntUnknown();
            __int64 totalSize = item64.getTotalSizeInt();
            __int64 largestSize = item64.getLargestSizeInt();
            __int64 smallestSize = item64.getSmallestSizeInt();
            if (version >= 1.38)
            {
                item1->setNumOfFilesInt64(numOfFiles);
                item1->setNumOfFilesUnknownInt64(numOfFilesIntUnknown);
                item1->setTotalSizeInt64(totalSize);
                item1->setLargestSizeInt64(largestSize);
                item1->setSmallestSizeInt64(smallestSize);
            }

            item1->setName(item64.getName());
            buf << comma(numOfFiles);
            item1->setNumOfFiles(buf.str());
            buf.clear();
            buf << comma(numOfFilesIntUnknown);
            item1->setNumOfFilesUnknown(buf.str());
            buf.clear();
            buf << comma(totalSize);
            item1->setTotalSize(buf.str());
            buf.clear();
            buf << comma(largestSize);
            item1->setLargestSize(buf.str());
            buf.clear();
            buf << comma(smallestSize);
            item1->setSmallestSize(buf.str());
            item1->setLargestFile(item64.getLargestFile());
            item1->setSmallestFile(item64.getSmallestFile());
            SpaceItems.append(*item1.getClear());
        }

        resp.setDFUSpaceItems(SpaceItems);
        resp.setCountBy(req.getCountBy());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::setSpaceItemByScope(IArrayOf<IEspSpaceItem>& SpaceItems64, const char*scopeName, const char*logicalName, __int64 size)
{
    char scope[1024];
    scope[0] = 0;

    const char* pName = NULL;
    if (!scopeName)
    {
        pName = strstr(logicalName, "::");
        if (!pName)
            return false;

        strncpy(scope, logicalName, pName - logicalName);
        scope[pName - logicalName] = 0;
    }
    else
    {
        if (strlen(logicalName) <= strlen(scopeName)+2)
            return false;

        char* ppName = (char*) logicalName + strlen(scopeName) + 2;
        pName = strstr(ppName, "::");
        if (pName)
        {
            strncpy(scope, logicalName, pName - logicalName);
            scope[pName - logicalName] = 0;
        }
    }

    if (strlen(scope) > 0)
    {
        IEspSpaceItem *item0 = NULL;
        for(unsigned i = 0; i < SpaceItems64.length();i++)
        {
            IEspSpaceItem& item1 = SpaceItems64.item(i);
            if (!stricmp(item1.getName(), scope))
            {
                item0 = &item1;
                break;
            }
        }

        if (!item0)
        {
            Owned<IEspSpaceItem> item1 = createSpaceItem();
            item1->setName(scope);
            item1->setNumOfFilesInt(1);
            if (size < 0)
            {
                item1->setNumOfFilesIntUnknown(1);
                item1->setTotalSizeInt(0);
                item1->setLargestSizeInt(0);
                item1->setSmallestSizeInt(0);
                item1->setLargestFile("");
                item1->setSmallestFile("");
            }
            else
            {
                item1->setNumOfFilesIntUnknown(0);
                item1->setTotalSizeInt(size);
                item1->setLargestSizeInt(size);
                item1->setSmallestSizeInt(size);
                item1->setLargestFile(logicalName);
                item1->setSmallestFile(logicalName);
            }
            SpaceItems64.append(*item1.getClear());
        }
        else if (size < 0)
        {
            item0->setNumOfFilesIntUnknown(item0->getNumOfFilesIntUnknown() + 1);
            item0->setNumOfFilesInt(item0->getNumOfFilesInt() + 1);
        }
        else
        {
            if (item0->getNumOfFilesInt() == item0->getNumOfFilesIntUnknown() || size > item0->getLargestSizeInt())
            {
                item0->setLargestSizeInt(size);
                item0->setLargestFile(logicalName);
            }
            if (item0->getNumOfFilesInt() == item0->getNumOfFilesIntUnknown() || size < item0->getSmallestSizeInt())
            {
                item0->setSmallestSizeInt(size);
                item0->setSmallestFile(logicalName);
            }

            item0->setNumOfFilesInt(item0->getNumOfFilesInt() + 1);
            item0->setTotalSizeInt(item0->getTotalSizeInt() + size);
        }
    }
    else
    {
        IEspSpaceItem& item0 = SpaceItems64.item(0);
        if (size < 0)
        {
            item0.setNumOfFilesInt(item0.getNumOfFilesInt() + 1);
            item0.setNumOfFilesIntUnknown(item0.getNumOfFilesIntUnknown() + 1);
        }
        else
        {
            if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size > item0.getLargestSizeInt()))
            {
                item0.setLargestSizeInt(size);
                item0.setLargestFile(logicalName);
            }
            if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size < item0.getSmallestSizeInt()))
            {
                item0.setSmallestSizeInt(size);
                item0.setSmallestFile(logicalName);
            }

            item0.setNumOfFilesInt(item0.getNumOfFilesInt() + 1);
            item0.setTotalSizeInt(item0.getTotalSizeInt() + size);
        }
    }

    return true;
}

bool CWsDfuEx::setSpaceItemByOwner(IArrayOf<IEspSpaceItem>& SpaceItems64, const char *owner, const char *logicalName, __int64 size)
{
    if (owner && *owner)
    {
        IEspSpaceItem *item0 = NULL;
        for(unsigned i = 0; i < SpaceItems64.length();i++)
        {
            IEspSpaceItem& item1 = SpaceItems64.item(i);
            if (!stricmp(item1.getName(), owner))
            {
                item0 = &item1;
                break;
            }
        }

        if (!item0)
        {
            Owned<IEspSpaceItem> item1 = createSpaceItem();
            item1->setName(owner);
            item1->setNumOfFilesInt(1);
            if (size < 0)
            {
                item1->setNumOfFilesIntUnknown(1);
                item1->setTotalSizeInt(0);
                item1->setLargestSizeInt(0);
                item1->setSmallestSizeInt(0);
                item1->setLargestFile("");
                item1->setSmallestFile("");
            }
            else
            {
                item1->setNumOfFilesIntUnknown(0);
                item1->setTotalSizeInt(size);
                item1->setLargestSizeInt(size);
                item1->setSmallestSizeInt(size);
                item1->setLargestFile(logicalName);
                item1->setSmallestFile(logicalName);
            }
            SpaceItems64.append(*item1.getClear());
        }
        else if (size < 0)
        {
            item0->setNumOfFilesIntUnknown(item0->getNumOfFilesIntUnknown() + 1);
            item0->setNumOfFilesInt(item0->getNumOfFilesInt() + 1);
        }
        else
        {
            if (item0->getNumOfFilesInt() == item0->getNumOfFilesIntUnknown() || size > item0->getLargestSizeInt())
            {
                item0->setLargestSizeInt(size);
                item0->setLargestFile(logicalName);
            }
            if (item0->getNumOfFilesInt() == item0->getNumOfFilesIntUnknown() || size < item0->getSmallestSizeInt())
            {
                item0->setSmallestSizeInt(size);
                item0->setSmallestFile(logicalName);
            }

            item0->setNumOfFilesInt(item0->getNumOfFilesInt() + 1);
            item0->setTotalSizeInt(item0->getTotalSizeInt() + size);
        }
    }
    else
    {
        IEspSpaceItem& item0 = SpaceItems64.item(0);
        if (size < 0)
        {
            item0.setNumOfFilesInt(item0.getNumOfFilesInt() + 1);
            item0.setNumOfFilesIntUnknown(item0.getNumOfFilesIntUnknown() + 1);
        }
        else
        {
            if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size > item0.getLargestSizeInt()))
            {
                item0.setLargestSizeInt(size);
                item0.setLargestFile(logicalName);
            }
            if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size < item0.getSmallestSizeInt()))
            {
                item0.setSmallestSizeInt(size);
                item0.setSmallestFile(logicalName);
            }

            item0.setNumOfFilesInt(item0.getNumOfFilesInt() + 1);
            item0.setTotalSizeInt(item0.getTotalSizeInt() + size);
        }
    }

    return true;
}

bool CWsDfuEx::createSpaceItemsByDate(IArrayOf<IEspSpaceItem>& SpaceItems, const char *interval, unsigned& yearFrom,
    unsigned& monthFrom, unsigned& dayFrom, unsigned& yearTo, unsigned& monthTo, unsigned& dayTo)
{
    if (!stricmp(interval, COUNTBY_YEAR))
    {
        for (unsigned i = yearFrom; i <= yearTo; i++)
        {
            Owned<IEspSpaceItem> item64 = createSpaceItem();
            StringBuffer name;
            name.appendf("%04d", i);
            item64->setName(name.str());
            item64->setNumOfFilesInt(0);
            item64->setNumOfFilesIntUnknown(0);
            item64->setTotalSizeInt(0);
            item64->setLargestSizeInt(0);
            item64->setSmallestSizeInt(0);
            item64->setLargestFile("");
            item64->setSmallestFile("");
            SpaceItems.append(*item64.getClear());
        }
    }
    else if (!stricmp(interval, COUNTBY_QUARTER))
    {
        for (unsigned i = yearFrom; i <= yearTo; i++)
        {
            int quartStart = 1;
            int quartEnd = 4;
            if (i == yearFrom)
            {
                if (monthFrom > 9)
                {
                    quartStart = 4;
                }
                else if (monthFrom > 6)
                {
                    quartStart = 3;
                }
                else if (monthFrom > 3)
                {
                    quartStart = 2;
                }
            }
            if (i == yearTo)
            {
                if (monthTo > 9)
                {
                    quartEnd = 4;
                }
                else if (monthTo > 6)
                {
                    quartEnd = 3;
                }
                else if (monthTo > 3)
                {
                    quartEnd = 2;
                }
            }

            for (int j = quartStart; j <= quartEnd; j++)
            {
                Owned<IEspSpaceItem> item64 = createSpaceItem();
                StringBuffer name;
                name.appendf("%04d quarter: %d", i, j);
                item64->setName(name.str());
                item64->setNumOfFilesInt(0);
                item64->setNumOfFilesIntUnknown(0);
                item64->setTotalSizeInt(0);
                item64->setLargestSizeInt(0);
                item64->setSmallestSizeInt(0);
                item64->setLargestFile("");
                item64->setSmallestFile("");
                SpaceItems.append(*item64.getClear());
            }
        }
    }
    else if (!stricmp(interval, COUNTBY_MONTH))
    {
        for (unsigned i = yearFrom; i <= yearTo; i++)
        {
            int jFrom = (i != yearFrom) ? 1 : monthFrom;
            int jTo =  (i != yearTo) ? 12 : monthTo;
            for (int j = jFrom; j <= jTo; j++)
            {
                Owned<IEspSpaceItem> item64 = createSpaceItem();
                StringBuffer name;
                name.appendf("%04d-%02d", i, j);
                item64->setName(name.str());
                item64->setNumOfFilesInt(0);
                item64->setNumOfFilesIntUnknown(0);
                item64->setTotalSizeInt(0);
                item64->setLargestSizeInt(0);
                item64->setSmallestSizeInt(0);
                item64->setLargestFile("");
                item64->setSmallestFile("");
                SpaceItems.append(*item64.getClear());
            }
        }
    }
    else
    {
        for (unsigned i = yearFrom; i <= yearTo; i++)
        {
            int jFrom = (i != yearFrom) ? 1 : monthFrom;
            int jTo =  (i != yearTo) ? 12 : monthTo;
            for (int j = jFrom; j <= jTo; j++)
            {
                int dayStart = 1;
                int dayEnd = days[j-1];
                if (i == yearFrom && j == monthFrom)
                {
                    dayStart = dayFrom;
                }
                else if (i == yearTo && j == monthTo)
                {
                    dayEnd = dayTo;
                }
                for (int k = dayStart; k <= dayEnd; k++)
                {
                    Owned<IEspSpaceItem> item64 = createSpaceItem();
                    StringBuffer name;
                    name.appendf("%04d-%02d-%02d", i, j, k);
                    item64->setName(name.str());
                    item64->setNumOfFilesInt(0);
                    item64->setNumOfFilesIntUnknown(0);
                    item64->setTotalSizeInt(0);
                    item64->setLargestSizeInt(0);
                    item64->setSmallestSizeInt(0);
                    item64->setLargestFile("");
                    item64->setSmallestFile("");
                    SpaceItems.append(*item64.getClear());
                }
            }
        }
    }

    return true;
}

bool CWsDfuEx::setSpaceItemByDate(IArrayOf<IEspSpaceItem>& SpaceItems, const char * interval, const char * mod, const char*logicalName, __int64 size)
{
    unsigned year, month, day;
    CDateTime wuTime;
    wuTime.setString(mod,NULL,true);
    wuTime.getDate(year, month, day, true);

    StringBuffer name;
    if (!stricmp(interval, COUNTBY_YEAR))
    {
        name.appendf("%04d", year);
    }
    else if (!stricmp(interval, COUNTBY_QUARTER))
    {
        int quart = 1;
        if (month > 9)
        {
            quart = 4;
        }
        else if (month > 6)
        {
            quart = 3;
        }
        else if (month > 3)
        {
            quart = 2;
        }
        name.appendf("%04d quarter: %d", year, quart);
    }
    else if (!stricmp(interval, COUNTBY_MONTH))
    {
        name.appendf("%04d-%02d", year, month);
    }
    else
    {
        name.appendf("%04d-%02d-%02d", year, month, day);
    }

    for (unsigned i = 0; i < SpaceItems.length(); i++)
    {
        IEspSpaceItem& item0 = SpaceItems.item(i);
        if (!stricmp(item0.getName(), name))
        {
            if (size < 0)
            {
                item0.setNumOfFilesIntUnknown(item0.getNumOfFilesIntUnknown() + 1);
            }
            else
            {
                if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size > item0.getLargestSizeInt()))
                {
                    item0.setLargestSizeInt(size);
                    item0.setLargestFile(logicalName);
                }
                if ((item0.getNumOfFilesInt() == item0.getNumOfFilesIntUnknown()) || (size < item0.getSmallestSizeInt()))
                {
                    item0.setSmallestSizeInt(size);
                    item0.setSmallestFile(logicalName);
                }

                item0.setTotalSizeInt(item0.getTotalSizeInt() + size);
            }

            item0.setNumOfFilesInt(item0.getNumOfFilesInt() + 1);
            break;
        }
    }

    return true;
}

void CWsDfuEx::parseStringArray(const char *input, StringArray& strarray)
{
    if (!input || !*input)
        return;

    const char *ptr = input;
    const char *pptr = ptr;
    while (pptr[0])
    {
        if (pptr[0] == ',')
        {
            StringAttr tmp;
            tmp.set(ptr, pptr-ptr);
            strarray.append(tmp.get());
            ptr = pptr + 1;
        }
        pptr++;
    }
    if (pptr > ptr)
    {
        StringAttr tmp;
        tmp.set(ptr, pptr-ptr);
        strarray.append(tmp.get());
    }
}

int CWsDfuEx::superfileAction(IEspContext &context, const char* action, const char* superfile, StringArray& subfiles,
                               const char* beforeSubFile, bool existingSuperfile, bool autocreatesuper, bool deleteFile, bool removeSuperfile)
{
    if (!action || !*action)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Superfile action not specified");

    if(!strieq(action, "add") && !strieq(action, "remove"))
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Only Add or Remove is allowed.");

    if (!superfile || !*superfile)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Superfile name not specified");

    StringBuffer username;
    context.getUserID(username);
    Owned<IUserDescriptor> userdesc;
    if(username.length() > 0)
    {
        userdesc.setown(createUserDescriptor());
        userdesc->set(username.str(), context.queryPassword(), context.querySignature());
    }

    if (!autocreatesuper)
    {//a file lock created by the lookup() will be released after '}'
        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(superfile, userdesc.get(), AccessMode::tbdWrite, false, false, nullptr, defaultPrivilegedUser);
        if (existingSuperfile)
        {
            if (!df)
                throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file %s.",superfile);
            if(!df->querySuperFile())
                throw MakeStringException(ECLWATCH_NOT_SUPERFILE,"%s is not a superfile.",superfile);
        }
        else if (df)
            throw MakeStringException(ECLWATCH_FILE_ALREADY_EXISTS,"The file %s already exists.",superfile);
    }

    PointerArrayOf<char> subfileArray;
    unsigned num = subfiles.length();
    if (num > 0)
    {
        StringBuffer msgHead;
        if(username.length() > 0)
            msgHead.appendf("%s: Superfile:%s, Subfile(s): ", action, superfile);
        else
            msgHead.appendf("%s: Superfile:%s, Subfile(s): ", action, superfile);

        unsigned filesInMsgBuf = 0;
        StringBuffer msgBuf(msgHead);
        for(unsigned i = 0; i < num; i++)
        {
            subfileArray.append((char*) subfiles.item(i));
            msgBuf.appendf("%s, ", subfiles.item(i));
            filesInMsgBuf++;
            if (filesInMsgBuf > 9)
            {
                PROGLOG("%s",msgBuf.str());
                msgBuf = msgHead;
                filesInMsgBuf = 0;
            }
        }

        if (filesInMsgBuf > 0)
            PROGLOG("%s", msgBuf.str());
    }
    else
        PROGLOG("%s: %s", action, superfile);

    Owned<IDFUhelper> dfuhelper = createIDFUhelper();

    synchronized block(m_superfilemutex);
    if(strieq(action, "add"))
        dfuhelper->addSuper(superfile, userdesc.get(), num, (const char**) subfileArray.getArray(), beforeSubFile, true);
    else
        dfuhelper->removeSuper(superfile, userdesc.get(), num, (const char**) subfileArray.getArray(), deleteFile, removeSuperfile);
    PROGLOG("%s done", action);

    return num;
}

bool CWsDfuEx::onAddtoSuperfile(IEspContext &context, IEspAddtoSuperfileRequest &req, IEspAddtoSuperfileResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Write, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::AddtoSuperfile: Permission denied.");

        double version = context.getClientVersion();
        if (version > 1.17)
        {
            const char* backTo = req.getBackToPage();
            if (backTo && *backTo)
                resp.setBackToPage(backTo);
        }
        resp.setSubfiles(req.getSubfiles());

        const char* superfile = req.getSuperfile();
        if (!superfile || !*superfile)
        {
            if (version > 1.15)
            {//Display the subfiles inside a table
                const char* files = req.getSubfiles();
                if (files && *files)
                {
                    StringArray subfileNames;
                    parseStringArray(files, subfileNames);
                    if (subfileNames.length() > 0)
                        resp.setSubfileNames(subfileNames);
                }
            }

            return true;//Display a form for user to specify superfile
        }

        if (version > 1.15)
        {
            superfileAction(context, "add", superfile, req.getNames(), NULL, req.getExistingFile(), false, false);
        }
        else
        {
            StringArray subfileNames;
            const char *subfilesStr = req.getSubfiles();
            if (subfilesStr && *subfilesStr)
                parseStringArray(subfilesStr, subfileNames);

            superfileAction(context, "add", superfile, subfileNames, NULL, req.getExistingFile(), false, false);
        }

        resp.setRedirectUrl(StringBuffer("/WsDFU/DFUInfo?Name=").append(superfile));
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void setDeleteFileResults(const char* fileName, const char* nodeGroup, bool failed, const char *start, const char* text, StringBuffer& resultString,
    IArrayOf<IEspDFUActionInfo>& actionResults)
{
    if (!fileName || !*fileName)
        return;
    Owned<IEspDFUActionInfo> resultObj = createDFUActionInfo("", "");
    resultObj->setFileName(fileName);
    resultObj->setFailed(failed);
    if (nodeGroup && *nodeGroup)
        resultObj->setNodeGroup(nodeGroup);

    StringBuffer message;
    if (start)
        message.append(start).append(' ');
    message.append(fileName);
    if (nodeGroup && *nodeGroup)
        message.append(" on ").append(nodeGroup);
    if (text && *text)
        message.append(failed ? ": " : " ").append(text);
    resultObj->setActionResult(message);
    resultString.appendf("<Message><Value>%s</Value></Message>", message.str());

    actionResults.append(*resultObj.getClear());
}

void cleanLogicalFileNames(StringArray &fileNames, StringArray &uniqueFileNames)
{
    ForEachItemIn(i, fileNames)
    {
        const char *fn = fileNames.item(i);
        if (isEmptyString(fn))
            continue;

        uniqueFileNames.appendUniq(fn);
    }
}

bool CWsDfuEx::DFUDeleteFiles(IEspContext &context, IEspDFUArrayActionRequest &req, IEspDFUArrayActionResponse &resp)
{
    if (isDetachedFromDali())
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "ESP server is detached from Dali. Please try later.");

    class CDFUDeleteFilesHelper
    {
        IEspContext &context;
        Owned<IUserDescriptor> userdesc;
        IArrayOf<IEspDFUActionInfo> actionResults;
        StringBuffer returnStr, auditStr;
        StringAttr espProcess;

        void deleteFile(const char *fn)
        {
            CDfsLogicalFileName dfsLFN;
            dfsLFN.set(fn);

            StringBuffer group;
            dfsLFN.getCluster(group);
            if (!group.isEmpty() && strieq(group.str(), "null")) //null is used by new ECLWatch for a superfile
                group.clear();

            try
            {
                PROGLOG("Deleting %s", fn);
                queryDistributedFileDirectory().removeEntry(fn, userdesc, nullptr, REMOVE_FILE_SDS_CONNECT_TIMEOUT, true);

                LOG(MCauditInfo, "%s,%s", auditStr.str(), fn);
                VStringBuffer message("File %s deleted.", fn);
                addResult(dfsLFN.get(), group, false, message);
            }
            catch(IException *e)
            {
                StringBuffer emsg;
                e->errorMessage(emsg);
                if (e->errorCode() == DFSERR_CreateAccessDenied)
                    emsg.replaceString("Create ", "Delete ");

                VStringBuffer message("Could not delete file %s: %s.", fn, emsg.str());
                addResult(dfsLFN.get(), group, true, message);
                e->Release();
            }
            catch(...)
            {
                VStringBuffer message("Could not delete file %s: unknown exception.", fn);
                addResult(dfsLFN.get(), group, true, message);
            }
        }

        void addResult(const char* fileName, const char* nodeGroup, bool failed, const char *message)
        {
            double version = context.getClientVersion();
            if (version < 1.33)
                returnStr.appendf("<Message><Value>%s</Value></Message>", message);

            PROGLOG("%s", message);
            if (version < 1.27)
                return;

            Owned<IEspDFUActionInfo> resultObj = createDFUActionInfo();
            resultObj->setFileName(fileName);
            resultObj->setFailed(failed);
            if (!isEmptyString(nodeGroup))
                resultObj->setNodeGroup(nodeGroup);
            resultObj->setActionResult(message);
            actionResults.append(*resultObj.getClear());
        }
    public:
        CDFUDeleteFilesHelper(IEspContext &ctx, const char *process) : context(ctx), espProcess(process)
        {
            const char *user = context.queryUserId();
            if (!isEmptyString(user))
            {
                userdesc.setown(createUserDescriptor());
                userdesc->set(user, context.queryPassword(), context.querySignature());
            }

            auditStr.set(",FileAccess,WsDfu,DELETED,");
            auditStr.append(espProcess.get());
            auditStr.append(',');
            if (!isEmptyString(user))
                auditStr.append(user).append('@');
            context.getPeer(auditStr);
        }

        const char *getReturnStr() const { return returnStr.str(); }
        IArrayOf<IEspDFUActionInfo> &getActionResults() { return actionResults; }

        void deleteFiles(StringArray &logicalFileNames, bool removeFromSuperFiles, bool removeEmptyOwningSuperFiles)
        {
            StringArray emptyOwningSuperFiles;
            IDistributedFileDirectory &fdir = queryDistributedFileDirectory();
            ForEachItemIn(i, logicalFileNames)
            {
                CDfsLogicalFileName dfsLFN;
                dfsLFN.set(logicalFileNames.item(i));

                StringBuffer group;
                dfsLFN.getCluster(group);
                if (!group.isEmpty() && strieq(group.str(), "null")) //null is used by new ECLWatch for a superfile
                    group.clear();

                StringBuffer normalizeLFN;
                normalizeLFN.append(dfsLFN.get());
                if (group.length())
                {
                    normalizeLFN.append("@");
                    dfsLFN.getCluster(normalizeLFN);
                }
                const char *lfn = normalizeLFN;
                if (emptyOwningSuperFiles.contains(lfn))
                    continue;

                Owned<IDistributedFile> df = fdir.lookup(lfn, userdesc, AccessMode::tbdWrite, false, false, nullptr, defaultPrivilegedUser, 30000); // 30 sec timeout
                if (!df)
                {
                    VStringBuffer message("Could not delete file %s: file not found.", lfn);
                    addResult(dfsLFN.get(), group, true, message);
                    continue;
                }

                Owned<IDistributedSuperFileIterator> superOwners = df->getOwningSuperFiles();
                if (!removeFromSuperFiles && superOwners->first())
                {
                    VStringBuffer message("The file %s can not be deleted because it is owned by one or more superfiles.", lfn);
                    addResult(dfsLFN.get(), group, true, message);
                    continue;
                }

                bool fileCanBeDeleted = true;
                ForEach(*superOwners)
                {
                    IDistributedSuperFile &superOwner = superOwners->query();
                    try
                    {
                        superOwner.removeSubFile(lfn, false, false, nullptr);
                        PROGLOG("File %s is removed from superfile %s", lfn, superOwner.queryLogicalName());
                    }
                    catch(IException *e)
                    {
                        VStringBuffer message("Could not delete file %s from superfile %s: ", lfn, superOwner.queryLogicalName());
                        e->errorMessage(message);
                        addResult(dfsLFN.get(), group, true, message);
                        e->Release();
                        fileCanBeDeleted = false;
                        break;
                    }
                    catch(...)
                    {
                        VStringBuffer message("Could not delete file %s from superfile %s: unknown exception", lfn, superOwner.queryLogicalName());
                        addResult(dfsLFN.get(), group, true, message);
                        fileCanBeDeleted = false;
                        break;
                    }

                    if (removeEmptyOwningSuperFiles && (superOwner.numSubFiles(false)==0))
                        emptyOwningSuperFiles.appendUniq(superOwner.queryLogicalName());
                }

                if (fileCanBeDeleted)
                {
                    superOwners.clear();
                    df.clear();
                    deleteFile(lfn);
                }
            }

            ForEachItemIn(j, emptyOwningSuperFiles)
            {
                deleteFile(emptyOwningSuperFiles.item(j));
            }
        }
    };

    StringArray logicalFileNames;
    cleanLogicalFileNames(req.getLogicalFiles(), logicalFileNames);
    if (!logicalFileNames.length())
        return true;

    CDFUDeleteFilesHelper helper(context, espProcess);
    helper.deleteFiles(logicalFileNames, req.getRemoveFromSuperfiles(), req.getRemoveRecursively());

    double version = context.getClientVersion();
    if (version >= 1.27)
        resp.setActionResults(helper.getActionResults());
    if (version < 1.33)
        resp.setDFUArrayActionResult(helper.getReturnStr());
    return true;
}

bool CWsDfuEx::onDFUArrayAction(IEspContext &context, IEspDFUArrayActionRequest &req, IEspDFUArrayActionResponse &resp)
{
    try
    {
        CDFUArrayActions action = req.getType();
        if (action == DFUArrayActions_Undefined)
            throw MakeStringException(ECLWATCH_INVALID_INPUT,"Action not defined.");

        if (action == CDFUArrayActions_ChangeRestriction)
            return  changeFileRestrictions(context, req, resp);

        if (action == CDFUArrayActions_ChangeProtection)
            return  changeFileProtections(context, req, resp);

        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Write, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUArrayAction: Permission denied.");

        double version = context.getClientVersion();
        if (version > 1.03)
        {
            StringBuffer backToPage(req.getBackToPage());
            if (backToPage.length() > 0)
            {
                const char* oldStr = "&";
                const char* newStr = "&amp;";
                backToPage.replaceString(oldStr, newStr);
                resp.setBackToPage(backToPage.str());
            }
        }

        if (action == CDFUArrayActions_Delete)
            return  DFUDeleteFiles(context, req, resp);

        //the code below is only for legacy ECLWatch. Other application should use AddtoSuperfile.
        StringBuffer username;
        context.getUserID(username);

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        IArrayOf<IEspDFUActionInfo> actionResults;
        StringBuffer errorStr, subfiles;
        for(unsigned i = 0; i < req.getLogicalFiles().length();i++)
        {
            const char* file = req.getLogicalFiles().item(i);
            if(!file || !*file)
                continue;

            unsigned len = strlen(file);
            char* curfile = new char[len+1];
            const char* cluster = NULL;
            const char *pCh = strchr(file, '@');
            if (pCh)
            {
                len = pCh - file;
                if (len+1 < strlen(file))
                    cluster = pCh + 1;
            }

            strncpy(curfile, file, len);
            curfile[len] = 0;

            try
            {
                Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(curfile, userdesc.get(), AccessMode::tbdWrite, false, false, nullptr, defaultPrivilegedUser);
                if (df)
                {
                    if (subfiles.length() > 0)
                        subfiles.append(",");
                    subfiles.append(curfile);
                }
                else
                    setDeleteFileResults(file, NULL, true, NULL, "not found", errorStr, actionResults);
            }
            catch(IException* e)
            {
                StringBuffer emsg;
                e->errorMessage(emsg);
                if (e->errorCode() == DFSERR_CreateAccessDenied)
                    emsg.replaceString("Create ", "AddtoSuperfile ");

                setDeleteFileResults(file, NULL, true, NULL, emsg.str(), errorStr, actionResults);
                e->Release();
            }
            catch(...)
            {
                setDeleteFileResults(file, NULL, true, NULL, "unknown exception", errorStr, actionResults);
            }
            delete [] curfile;
        }

        if (version >= 1.27)
            resp.setActionResults(actionResults);
        if (errorStr.length())
        {
            if (version < 1.33)
                resp.setDFUArrayActionResult(errorStr.str());
            return false;
        }

        if (version < 1.18)
            resp.setRedirectUrl(StringBuffer("/WsDFU/AddtoSuperfile?Subfiles=").append(subfiles.str()));
        else
            resp.setRedirectTo(subfiles.str());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsDfuEx::addFileActionResult(const char* fileName, const char* nodeGroup, bool failed, const  char* msg,
    IArrayOf<IEspDFUActionInfo>& actionResults)
{
    Owned<IEspDFUActionInfo> result = createDFUActionInfo();
    result->setFileName(fileName);
    if (!isEmptyString(nodeGroup))
        result->setNodeGroup(nodeGroup);
    result->setFailed(failed);
    result->setActionResult(msg);
    actionResults.append(*result.getClear());
}

bool CWsDfuEx::changeFileProtections(IEspContext &context, IEspDFUArrayActionRequest &req, IEspDFUArrayActionResponse &resp)
{
    CDFUChangeProtection protect = req.getProtect();
    if (protect == CDFUChangeProtection_NoChange)
        return true;

#ifdef _USE_OPENLDAP
    if (protect == CDFUChangeProtection_UnprotectAll)
        context.ensureSuperUser(ECLWATCH_SUPER_USER_ACCESS_DENIED, "Access denied, administrators only.");
#endif

    if (isDetachedFromDali())
        throw makeStringException(ECLWATCH_INVALID_INPUT, "ESP server is detached from Dali. Please try later.");

    StringBuffer userID;
    context.getUserID(userID);
    Owned<IUserDescriptor> userDesc;
    if (userID.isEmpty())
        userID.set("hpcc"); //The userID is needed for setProtect().
    else
    {
        userDesc.setown(createUserDescriptor());
        userDesc->set(userID, context.queryPassword(), context.querySignature());
    }

    IArrayOf<IEspDFUActionInfo> actionResults;
    for (unsigned i = 0; i < req.getLogicalFiles().length(); i++)
    {
        const char *file = req.getLogicalFiles().item(i);
        if (isEmptyString(file))
            continue;

        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(file, userDesc, AccessMode::tbdRead, false, true, nullptr, defaultPrivilegedUser); // lock super-owners
        if (!df)
        {
            addFileActionResult(file, nullptr, true, "Cannot find file.", actionResults);
            continue;
        }

        if (protect != CDFUChangeProtection_UnprotectAll)
            df->setProtect(userID, protect == CDFUChangeProtection_Protect ? true : false);
        else
            clearFileProtections(df);
        addFileActionResult(file, nullptr, false, protect == CDFUChangeProtection_Protect ? "File protected" : "File unprotected", actionResults);
    }
    resp.setActionResults(actionResults);
    return true;
}

void CWsDfuEx::clearFileProtections(IDistributedFile *df)
{
    StringArray owners;
    Owned<IPropertyTreeIterator> itr = df->queryAttributes().getElements("Protect");
    ForEach(*itr)
    {
        const char *owner = itr->query().queryProp("@name");
        if (!isEmptyString(owner))
            owners.append(owner);
    }

    ForEachItemIn(i, owners)
        df->setProtect(owners.item(i), false);
}

bool CWsDfuEx::changeFileRestrictions(IEspContext &context, IEspDFUArrayActionRequest &req, IEspDFUArrayActionResponse &resp)
{
    CDFUChangeRestriction changeRestriction = req.getRestrict();
    if (changeRestriction == CDFUChangeRestriction_NoChange)
        return true;

    if (changeRestriction == CDFUChangeRestriction_NotRestricted)
        context.ensureFeatureAccess("CodeSignAccess", SecAccess_Full, ECLWATCH_DFU_ACCESS_DENIED, "CodeSignAccess: Permission denied.");

    if (isDetachedFromDali())
        throw makeStringException(ECLWATCH_INVALID_INPUT, "ESP server is detached from Dali. Please try later.");

    StringBuffer userID;
    context.getUserID(userID);
    Owned<IUserDescriptor> userDesc;
    if (!userID.isEmpty())
    {
        userDesc.setown(createUserDescriptor());
        userDesc->set(userID, context.queryPassword(), context.querySignature());
    }

    IArrayOf<IEspDFUActionInfo> actionResults;
    for (unsigned i = 0; i < req.getLogicalFiles().length();i++)
    {
        const char *file = req.getLogicalFiles().item(i);
        if (isEmptyString(file))
            continue;

        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(file, userDesc, AccessMode::tbdRead, false, true, nullptr, defaultPrivilegedUser); // lock super-owners
        if (!df)
        {
            addFileActionResult(file, nullptr, true, "Cannot find file.", actionResults);
            continue;
        }

        df->setRestrictedAccess(changeRestriction==CDFUChangeRestriction_Restrict);
        addFileActionResult(file, nullptr, false, changeRestriction == CDFUChangeRestriction_Restrict ? "Changed to restricted" : "Changed to unrestricted", actionResults);
    }
    resp.setActionResults(actionResults);
    return true;
}

bool CWsDfuEx::onDFUDefFile(IEspContext &context,IEspDFUDefFileRequest &req, IEspDFUDefFileResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUDefFile: Permission denied.");

        CDFUDefFileFormat format = req.getFormat();
        if (format == DFUDefFileFormat_Undefined)
            throw MakeStringException(ECLWATCH_INVALID_INPUT,"Invalid format");

        const char* fileName = req.getName();
        if (!fileName || !*fileName)
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "File name required");
        PROGLOG("DFUDefFile: %s", fileName);

        StringBuffer username;
        context.getUserID(username);

        StringBuffer rawStr,returnStr;

        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        getDefFile(userdesc.get(), req.getName(),rawStr);
        StringBuffer xsltFile;
        xsltFile.append(getCFD()).append("smc_xslt/").append(req.getFormatAsString()).append("_def_file.xslt");
        xsltTransformer(xsltFile.str(),rawStr,returnStr);

        //set the file
        MemoryBuffer buff;
        buff.setBuffer(returnStr.length(), (void*)returnStr.str());
        resp.setDefFile(buff);

        //set the type
        StringBuffer type("text/");
        if (format == CDFUDefFileFormat_xml)
            type.append("xml");
        else
            type.append("plain");

        resp.setDefFile_mimetype(type.str());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

IHqlExpression * getEclRecordDefinition(const char * ecl)
{
    MultiErrorReceiver errs;
    OwnedHqlExpr record = parseQuery(ecl, &errs);
    if (errs.errCount())
    {
        StringBuffer errtext;
        IError *first = errs.firstError();
        first->toString(errtext);
        throw MakeStringException(ECLWATCH_CANNOT_PARSE_ECL_QUERY, "Failed in parsing ECL record definition: %s @ %d:%d.", errtext.str(), first->getColumn(), first->getLine());
    }
    if(!record)
        throw MakeStringException(ECLWATCH_CANNOT_PARSE_ECL_QUERY, "Failed in parsing ECL record definition.");
    return record.getClear();
}

IHqlExpression * getEclRecordDefinition(IUserDescriptor* udesc, const char* FileName)
{
    Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(FileName, udesc, AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
    if(!df)
        throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file %s.",FileName);
    if(!df->queryAttributes().hasProp("ECL"))
        throw MakeStringException(ECLWATCH_MISSING_PARAMS,"No record definition for file %s.",FileName);

    return getEclRecordDefinition(df->queryAttributes().queryProp("ECL"));
}

bool getRecordFormatFromRtlType(MemoryBuffer &binLayout, StringBuffer &jsonLayout, const IPropertyTree &attr, bool includeBin, bool includeJson)
{
    if (!attr.hasProp("_rtlType"))
        return false;
    try
    {
        MemoryBuffer mb;
        attr.getPropBin("_rtlType", mb);
        if (includeJson)
        {
            Owned<IRtlFieldTypeDeserializer> deserializer(createRtlFieldTypeDeserializer());
            const RtlTypeInfo *typeInfo = deserializer->deserialize(mb);
            dumpTypeInfo(jsonLayout, typeInfo);
        }
        if (includeBin)
            mb.swapWith(binLayout);
        return true;
    }
    catch (IException *e)
    {
        EXCLOG(e, "Failed to process _rtlType");
        e->Release();
    }
    return false;
}

bool getRecordFormatFromECL(MemoryBuffer &binLayout, StringBuffer &jsonLayout, const IPropertyTree &attr, bool includeBin, bool includeJson)
{
    if (!attr.hasProp("ECL"))
        return false;
    try
    {
        const char * kind = attr.queryProp("@kind");
        bool isIndex = (kind && streq(kind, "key"));
        OwnedHqlExpr record = getEclRecordDefinition(attr.queryProp("ECL"));
        MemoryBuffer mb;
        if (attr.hasProp("_record_layout"))
        {
            attr.getPropBin("_record_layout", mb);
            record.setown(patchEclRecordDefinitionFromRecordLayout(record, mb));
        }
        if (includeJson)
            exportJsonType(jsonLayout, record, isIndex);
        if (includeBin)
            exportBinaryType(binLayout, record, isIndex);
        return true;
    }
    catch (IException *e)
    {
        EXCLOG(e, "Failed to process ECL record");
        e->Release();
    }
    return false;
}

bool CWsDfuEx::onDFURecordTypeInfo(IEspContext &context, IEspDFURecordTypeInfoRequest &req, IEspDFURecordTypeInfoResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFURecordTypeInfo: Permission denied.");

        const char* fileName = req.getName();
        if (!fileName || !*fileName)
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "File name required");
        PROGLOG("DFURecordTypeInfo file: %s", fileName);

        Owned<IDistributedFile> df = lookupLogicalName(context, fileName, AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
        if (!df)
            throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file %s.",fileName);

        MemoryBuffer binLayout;
        StringBuffer jsonLayout;
        if (getRecordFormatFromRtlType(binLayout, jsonLayout, df->queryAttributes(), req.getIncludeBinTypeInfo(), req.getIncludeJsonTypeInfo()) ||
            getRecordFormatFromECL(binLayout.clear(), jsonLayout.clear(), df->queryAttributes(), req.getIncludeBinTypeInfo(), req.getIncludeJsonTypeInfo()))
        {
            if (req.getIncludeBinTypeInfo())
                resp.setBinInfo(binLayout);
            if (req.getIncludeJsonTypeInfo())
                resp.setJsonInfo(jsonLayout);
        }
    }
    catch (IException* e)
    {
        FORWARDEXCEPTION(context, e, ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onEclRecordTypeInfo(IEspContext &context, IEspEclRecordTypeInfoRequest &req, IEspEclRecordTypeInfoResponse &resp)
{
    try
    {
        OwnedHqlExpr record = getEclRecordDefinition(req.getEcl());
        if (req.getIncludeJsonTypeInfo())
        {
            StringBuffer jsonFormat;
            exportJsonType(jsonFormat, record, false);  // MORE - could allow isIndex to be passed in?
            resp.setJsonInfo(jsonFormat);
        }
        if (req.getIncludeBinTypeInfo())
        {
            MemoryBuffer binFormat;
            exportBinaryType(binFormat, record, false);
            resp.setBinInfo(binFormat);
        }
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsDfuEx::xsltTransformer(const char* xsltPath,StringBuffer& source,StringBuffer& returnStr)
{
    if (m_xsl.get() == 0)
    {
        m_xsl.setown(getXslProcessor());
    }

    Owned<IXslTransform> xform = m_xsl->createXslTransform();

    xform->loadXslFromFile(xsltPath);
    xform->setXmlSource(source.str(), source.length()+1);
    xform->transform(returnStr.clear());
}

void CWsDfuEx::getDefFile(IUserDescriptor* udesc, const char* FileName,StringBuffer& returnStr)
{
    OwnedHqlExpr record = getEclRecordDefinition(udesc, FileName);
    Owned<IPropertyTree> data = createPTree("Table", ipt_caseInsensitive);
    exportData(data, record);

    const char* fname=strrchr(FileName,':');

    data->setProp("filename",fname ? fname+1 : FileName);

    toXML(data, returnStr, 0, 0);
}

bool CWsDfuEx::checkFileContent(IEspContext &context, IUserDescriptor* udesc, const char * logicalName, const char * cluster)
{
    Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(logicalName, udesc, AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
    if (!df)
        return false;

    if (!cluster || !stricmp(cluster, ""))
    {
        StringAttr eclCluster;
        const char* wuid = df->queryAttributes().queryProp("@workunit");
        if (wuid && *wuid)
        {
            try
            {
                Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
                if (factory)
                {
                    IConstWorkUnit* wu = factory->openWorkUnit(wuid, context.querySecManager(), context.queryUser());
                    if (wu)
                        eclCluster.set(wu->queryClusterName());
                }
            }
            catch(...)
            {
                return false;
            }
        }

        if (!eclCluster.length())
            return false;
    }

    bool blocked;
    if (df->isCompressed(&blocked) && !blocked)
        return false;

    IPropertyTree & properties = df->queryAttributes();
    const char * format = properties.queryProp("@format");
    if (format && (stricmp(format,"csv")==0 || memicmp(format, "utf", 3) == 0))
    {
        return true;
    }
    const char * recordEcl = properties.queryProp("ECL");
    if (!recordEcl)
        return false;

    MultiErrorReceiver errs;
    Owned< IHqlExpression> ret = ::parseQuery(recordEcl, &errs);
    return errs.errCount() == 0;
}

bool FindInStringArray(StringArray& clusters, const char *cluster)
{
    bool bFound = false;
    if(cluster && *cluster)
    {
        if (clusters.ordinality())
        {
            ForEachItemIn(i, clusters)
            {
                const char* cluster0 = clusters.item(i);
                if(cluster0 && *cluster0 && !stricmp(cluster, cluster0))
                    return true;
            }
        }
    }
    else
    {
#if 0 //Comment out since clusters are not set for some old files
        if (!clusters.ordinality())
            return true;

        ForEachItemIn(i, clusters)
        {
            const char* cluster0 = clusters.item(i);
            if(cluster0 && !*cluster0)
            {
                return true;
            }
        }
#else
        return true;
#endif
    }

    return bFound;
}

static void getFilePermission(CDfsLogicalFileName &dlfn, ISecUser & user, IUserDescriptor* udesc, ISecManager* secmgr, SecAccessFlags& permission)
{
    if (dlfn.isMulti())
    {
        if (!dlfn.isExpanded())
            dlfn.expand(udesc);
        unsigned i = dlfn.multiOrdinality();
        while (i--)
        {
            getFilePermission((CDfsLogicalFileName &)dlfn.multiItem(i), user, udesc, secmgr, permission);
        }
    }
    else
    {
        SecAccessFlags permissionTemp;
        if (dlfn.isForeign())
        {
            permissionTemp = queryDistributedFileDirectory().getFilePermissions(dlfn.get(), udesc);
        }
        else
        {
            StringBuffer scopes;
            dlfn.getScopes(scopes);

            permissionTemp = secmgr->authorizeFileScope(user, scopes.str());
        }

        //Descrease the permission whenever a component has a lower permission.
        if (permissionTemp < permission)
            permission = permissionTemp;
    }

    return;
}

bool CWsDfuEx::getUserFilePermission(IEspContext &context, IUserDescriptor* udesc, const char* logicalName, SecAccessFlags& permission)
{
    ISecManager* secmgr = context.querySecManager();
    if (!secmgr)
    {
        return false;
    }

    CDfsLogicalFileName dlfn;
    dlfn.set(logicalName);

    //Start from the SecAccess_Full. Decrease the permission whenever a component has a lower permission.
    permission = SecAccess_Full;
    getFilePermission(dlfn, *context.queryUser(), udesc, secmgr, permission);

    return true;
}

void CWsDfuEx::getFilePartsOnClusters(IEspContext &context, const char *clusterReq, StringArray &clusters,
    IDistributedFile *df, IEspDFUFileDetail &fileDetails)
{
    double version = context.getClientVersion();
    bool compressedFile = isFileKey(df) || df->isCompressed();
    IArrayOf<IConstDFUFilePartsOnCluster>& partsOnClusters = fileDetails.getDFUFilePartsOnClusters();
    ForEachItemIn(i, clusters)
    {
        const char* clusterName = clusters.item(i);
        if (isEmptyString(clusterName) || (!isEmptyString(clusterReq) && !strieq(clusterReq, clusterName)))
            continue;

        Owned<IEspDFUFilePartsOnCluster> partsOnCluster = createDFUFilePartsOnCluster();
        partsOnCluster->setCluster(clusterName);

        IArrayOf<IConstDFUPart> &filePartList = partsOnCluster->getDFUFileParts();

        Owned<IFileDescriptor> fdesc = df->getFileDescriptor(clusterName);
        Owned<IPartDescriptorIterator> pi = fdesc->getIterator();
        ForEach(*pi)
        {
            IPartDescriptor &part = pi->query();
            unsigned partIndex = part.queryPartIndex();

            __int64 size = -1;
            __int64 compressedSize = -1;
            StringBuffer partSizeStr;
            IPropertyTree *partPropertyTree = &part.queryProperties();
            if (!partPropertyTree)
                partSizeStr.set("<N/A>");
            else
            {
                size = partPropertyTree->getPropInt64("@size", -1);
                comma c4(size);
                partSizeStr<<c4;
                if (compressedFile && (version >= 1.58))
                    compressedSize = partPropertyTree->getPropInt64("@compressedSize", -1);
            }

            for (unsigned i=0; i<part.numCopies(); i++)
            {
                StringBuffer url;
                part.queryNode(i)->endpoint().getEndpointHostText(url);

                Owned<IEspDFUPart> FilePart = createDFUPart();
                FilePart->setId(partIndex+1);
                FilePart->setPartsize(partSizeStr.str());
                if (version >= 1.38)
                    FilePart->setPartSizeInt64(size);
                if (compressedFile && (version >= 1.58))
                {
                    if (compressedSize != -1)
                        FilePart->setCompressedSize(compressedSize);
                    else
                        FilePart->setCompressedSize(size);
                }
                FilePart->setIp(url.str());
                FilePart->setCopy(i+1);

                filePartList.append(*FilePart.getClear());
            }
        }

        if (version >= 1.31)
        {
            IClusterInfo *clusterInfo = fdesc->queryCluster(clusterName);
            if (clusterInfo) //Should be valid. But, check it just in case.
            {
                partsOnCluster->setReplicate(clusterInfo->queryPartDiskMapping().isReplicated());
#ifndef _CONTAINERIZED
                Owned<CThorNodeGroup> nodeGroup = thorNodeGroupCache->lookup(clusterName, nodeGroupCacheTimeout);
                if (nodeGroup)
                    partsOnCluster->setCanReplicate(nodeGroup->queryCanReplicate());
#endif
                const char *defaultDir = fdesc->queryDefaultDir();
                if (!isEmptyString(defaultDir))
                {
                    DFD_OS os = SepCharBaseOs(getPathSepChar(defaultDir));
                    StringBuffer baseDir, repDir;
                    clusterInfo->getBaseDir(baseDir, os);
                    clusterInfo->getReplicateDir(repDir, os);
                    partsOnCluster->setBaseDir(baseDir.str());
                    partsOnCluster->setReplicateDir(baseDir.str());
                }
            }
        }
        partsOnClusters.append(*partsOnCluster.getClear());
    }
}

void CWsDfuEx::parseFieldMask(unsigned __int64 fieldMask, unsigned &fieldCount, IntArray &fieldIndexArray)
{
    while (fieldMask > 0)
    {
        if (fieldMask & 1)
            fieldIndexArray.append(fieldCount); //index from 0
        fieldMask >>= 1;
        fieldCount++;
    }
}

void CWsDfuEx::queryFieldNames(IEspContext &context, const char *fileName, const char *cluster,
    unsigned __int64 fieldMask, StringArray &fieldNames)
{
    if (!fileName || !*fileName)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "File name required");

    Owned<IResultSetFactory> resultSetFactory = getSecResultSetFactory(context.querySecManager(), context.queryUser(), context.queryUserId(), context.queryPassword());
    Owned<INewResultSet> result = resultSetFactory->createNewFileResultSet(fileName, cluster);
    if (!result)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Failed to access FileResultSet for %s.", fileName);

    unsigned fieldCount = 0;
    IntArray fieldIndexArray;
    parseFieldMask(fieldMask, fieldCount, fieldIndexArray);

    const IResultSetMetaData& metaData = result->getMetaData();
    unsigned totalColumns = (unsigned) metaData.getColumnCount();
    if (fieldCount > totalColumns)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "Invalid FieldMask %" I64F "u: total fields %u, ask for %u.",
            fieldMask, totalColumns, fieldCount);

    ForEachItemIn(i, fieldIndexArray)
    {
        int fieldIndex = fieldIndexArray.item(i);

        SCMStringBuffer columnLabel;
        if (metaData.hasSetTranslation(fieldIndex))
            metaData.getNaturalColumnLabel(columnLabel, fieldIndex);
        if (columnLabel.length() < 1)
            metaData.getColumnLabel(columnLabel, fieldIndex);
        fieldNames.append(columnLabel.str());
    }
}

void CWsDfuEx::doGetFileDetails(IEspContext &context, IUserDescriptor *udesc, const char *name, const char *cluster,
    const char *querySet, const char *query, const char *description, bool includeJsonTypeInfo, bool includeBinTypeInfo,
    CDFUChangeProtection protect, CDFUChangeRestriction changeRestriction, IEspDFUFileDetail &FileDetails, bool forceIndexInfo)
{
    if (!name || !*name)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "File name required");
    PROGLOG("doGetFileDetails: %s", name);

    double version = context.getClientVersion();
    if ((version >= 1.38) && !isEmptyString(querySet) && !isEmptyString(query))
    {
        if (getQueryFile(name, querySet, query, FileDetails))
            return;
    }

    Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(name, udesc, AccessMode::tbdRead, false, true, nullptr, defaultPrivilegedUser); // lock super-owners
    if(!df)
        throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file %s.",name);

    StringArray clusters;
    df->getClusterNames(clusters);
    if (cluster && *cluster && !FindInStringArray(clusters, cluster))
        throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file %s on %s.", name, cluster);

    if (protect != CDFUChangeProtection_NoChange)
    {
#ifdef _USE_OPENLDAP
        if (protect == CDFUChangeProtection_UnprotectAll)
            context.ensureSuperUser(ECLWATCH_SUPER_USER_ACCESS_DENIED, "Access denied, administrators only.");
#endif

        if (protect == CDFUChangeProtection_UnprotectAll)
            clearFileProtections(df);
        else
        {
            StringBuffer userID;
            context.getUserID(userID);
            if (userID.isEmpty())
                userID.set("hpcc");
            df->setProtect(userID.str(), protect == CDFUChangeProtection_Protect ? true : false);
        }
    }
    if (changeRestriction != CDFUChangeRestriction_NoChange)
    {
        if (changeRestriction == CDFUChangeRestriction_NotRestricted)
            context.ensureFeatureAccess("CodeSignAccess", SecAccess_Full, ECLWATCH_DFU_ACCESS_DENIED, "CodeSignAccess: Permission denied.");
        df->setRestrictedAccess(changeRestriction==CDFUChangeRestriction_Restrict);
    }

    offset_t size=df->getFileSize(true, false), recordSize=df->queryAttributes().getPropInt64("@recordSize",0);

    CDateTime dt;
    df->getModificationTime(dt);
    const char* lname=df->queryLogicalName(), *fname=strrchr(lname,':');
    FileDetails.setName(lname);
    FileDetails.setFilename(fname ? fname+1 : lname);
    FileDetails.setDir(df->queryDefaultDir());
    FileDetails.setPathMask(df->queryPartMask());
    if (version >= 1.28)
    {
        StringBuffer buf;
        FileDetails.setPrefix(WsDFUHelpers::getPrefixFromLogicalName(lname, buf));
        if (cluster && *cluster)
            FileDetails.setNodeGroup(cluster);
        else if (clusters.length() == 1)
            FileDetails.setNodeGroup(clusters.item(0));

        IArrayOf<IEspDFUFileProtect> protectList;
        Owned<IPropertyTreeIterator> itr= df->queryAttributes().getElements("Protect");
        ForEach(*itr)
        {
            IPropertyTree &tree = itr->query();
            const char *owner = tree.queryProp("@name");
            const char *modified = tree.queryProp("@modified");
            Owned<IEspDFUFileProtect> protect= createDFUFileProtect();
            if(owner && *owner)
                protect->setOwner(owner);
            if(modified && *modified)
                protect->setModified(modified);
            if (version < 1.54)
                protect->setCount(1);
            protectList.append(*protect.getLink());
        }
        FileDetails.setProtectList(protectList);
    }
    StringBuffer strDesc(df->queryAttributes().queryProp("@description"));
    if (description)
    {
        DistributedFilePropertyLock lock(df);
        lock.queryAttributes().setProp("@description",description);
        strDesc = description;
    }

    FileDetails.setDescription(strDesc);

    if (version >= 1.38)
        FileDetails.setFileSizeInt64(size);
    comma c1(size);
    StringBuffer tmpstr;
    tmpstr<<c1;
    FileDetails.setFilesize(tmpstr.str());

    bool isKeyFile = isFileKey(df);
    if (isKeyFile || df->isCompressed())
    {
        if (version < 1.22)
            FileDetails.setZipFile(true);
        else
        {
            FileDetails.setIsCompressed(true);
            if (df->queryAttributes().hasProp("@compressedSize"))
            {
                __int64 compressedSize = df->queryAttributes().getPropInt64("@compressedSize");
                FileDetails.setCompressedFileSize(compressedSize);
                if (version >= 1.34)
                {
                    Decimal d(((double) compressedSize)/size*100);
                    d.round(2);
                    FileDetails.setPercentCompressed(d.getCString());
                }
            }
            else if (isKeyFile)
                FileDetails.setCompressedFileSize(size);

            if (isKeyFile && (version >= 1.41))
            {
                if (isFilePartitionKey(df))
                    FileDetails.setKeyType("Partitioned");
                else if (isFileLocalKey(df))
                    FileDetails.setKeyType("Local");
                else
                    FileDetails.setKeyType("Distributed");
            }
        }
    }
    if (version >= 1.53)
        FileDetails.setIsRestricted(df->isRestrictedAccess());
    if (version >= 1.38)
        FileDetails.setRecordSizeInt64(recordSize);
    comma c2(recordSize);
    tmpstr.clear();
    tmpstr<<c2;
    FileDetails.setRecordSize(tmpstr.str());

    tmpstr.clear();
    __int64 recordCount = -1;
    if (df->queryAttributes().hasProp("@recordCount"))
    {
        recordCount = df->queryAttributes().getPropInt64("@recordCount");
    }
    else if (recordSize)
    {
        recordCount = size/recordSize;
    }
    if (version >= 1.38)
        FileDetails.setRecordCountInt64(recordCount);

    if (recordCount != -1)
    {
        comma c3(recordCount);
        tmpstr<<c3;
    }
    FileDetails.setRecordCount(tmpstr.str());

    FileDetails.setOwner(df->queryAttributes().queryProp("@owner"));
    FileDetails.setJobName(df->queryAttributes().queryProp("@job"));

    if (version >= 1.39)
    {
        if (df->queryAttributes().hasProp("@partitionFieldMask"))
        {
            StringArray partitionFieldNames;
            unsigned __int64 partitionFieldMask = df->queryAttributes().getPropInt64("@partitionFieldMask");
            queryFieldNames(context, name, cluster, partitionFieldMask, partitionFieldNames);

            IEspDFUFilePartition &partition = FileDetails.updatePartition();
            partition.setFieldMask(partitionFieldMask);
            partition.setFieldNames(partitionFieldNames);
        }

        IArrayOf<IEspDFUFileBloom> bloomList;
        Owned<IPropertyTreeIterator> itr= df->queryAttributes().getElements("Bloom");
        ForEach(*itr)
        {
            IPropertyTree &bloomTree = itr->query();

            StringArray bloomFieldNames;
            unsigned __int64 bloomFieldMask = bloomTree.getPropInt64("@bloomFieldMask");
            queryFieldNames(context, name, cluster, bloomFieldMask, bloomFieldNames);

            Owned<IEspDFUFileBloom> bloom= createDFUFileBloom();
            bloom->setFieldMask(bloomFieldMask);
            bloom->setFieldNames(bloomFieldNames);
            bloom->setLimit(bloomTree.getPropInt64("@bloomLimit"));
            bloom->setProbability(bloomTree.queryProp("@bloomProbability"));
            bloomList.append(*bloom.getLink());
        }
        if (bloomList.ordinality())
            FileDetails.setBlooms(bloomList);
    }

    if (version >= 1.40)
    {
        StringBuffer expirationDate;
        int expireDays = df->getExpire(&expirationDate);
        if (expireDays != -1)
        {
            FileDetails.setExpireDays(expireDays);
            if ((version >= 1.63) && !expirationDate.isEmpty())
                FileDetails.setExpirationDate(expirationDate);
        }
    }

    //#14280
    IDistributedSuperFile *sf = df->querySuperFile();
    if(sf)
    {
        StringArray farray;
        Owned<IDistributedFileIterator> iter=sf->getSubFileIterator();
        ForEach(*iter)
        {
            StringBuffer subfileName;
            iter->getName(subfileName);
            farray.append(subfileName.str());
        }

        unsigned numSubFiles = farray.length();
        if(numSubFiles > 0)
        {
            FileDetails.setSubfiles(farray);
        }
        if ((version >= 1.28) && (numSubFiles > 1))
            FileDetails.setBrowseData(false); //ViewKeyFile Cannot handle superfile with multiple subfiles

        FileDetails.setIsSuperfile(true);
        return;
    }
    //#14280

    FileDetails.setWuid(df->queryAttributes().queryProp("@workunit"));
    if (version >= 1.28)
        FileDetails.setNumParts(df->numParts());

    //#17430
    {
        IArrayOf<IEspDFULogicalFile> LogicalFiles;
        Owned<IDistributedSuperFileIterator> iter = df->getOwningSuperFiles();
        if(iter.get() != NULL)
        {
            ForEach(*iter)
            {
                //printf("%s,%s\n",iter->query().queryLogicalName(),lname);
                Owned<IEspDFULogicalFile> File = createDFULogicalFile("","");
                File->setName(iter->queryName());
                LogicalFiles.append(*File.getClear());
            }
        }

        if(LogicalFiles.length() > 0)
        {
            FileDetails.setSuperfiles(LogicalFiles);
        }
    }
    //#17430

    //new (optional) attribute on a logical file (@persistent)
    //indicates the ESP page that shows the details of a file.  It indicates
    //whether the file was created with a PERSIST() ecl attribute.
    FileDetails.setPersistent(df->queryAttributes().queryProp("@persistent"));

    //@format - what format the file is (if not fixed with)
    FileDetails.setFormat(df->queryAttributes().queryProp("@format"));

    if ((version >= 1.21) && (df->queryAttributes().hasProp("@kind")))
        FileDetails.setContentType(df->queryAttributes().queryProp("@kind"));

    //@maxRecordSize - what the maximum length of records is
    FileDetails.setMaxRecordSize(df->queryAttributes().queryProp("@maxRecordSize"));

    //@csvSeparate - separators between fields for a CSV/utf file
    FileDetails.setCsvSeparate(df->queryAttributes().queryProp("@csvSeparate"));

    //@csvQuote - character used to quote fields for a csv/utf file.
    FileDetails.setCsvQuote(df->queryAttributes().queryProp("@csvQuote"));

    //@csvTerminate - characters used to terminate a record in a csv.utf file
    FileDetails.setCsvTerminate(df->queryAttributes().queryProp("@csvTerminate"));

    //@csvEscape - character used to define escape for a csv/utf file.
    if (version >= 1.20)
        FileDetails.setCsvEscape(df->queryAttributes().queryProp("@csvEscape"));


    //Time and date of the file
    tmpstr.clear();
    dt.getDateString(tmpstr);
    tmpstr.append(" ");
    dt.getTimeString(tmpstr);
    FileDetails.setModified(tmpstr.str());

    if(df->queryAttributes().hasProp("ECL"))
        FileDetails.setEcl(df->queryAttributes().queryProp("ECL"));

    StringBuffer clusterStr;
    ForEachItemIn(i, clusters)
    {
        if (!clusterStr.length())
            clusterStr.append(clusters.item(i));
        else
            clusterStr.append(",").append(clusters.item(i));
    }

    if (clusterStr.length() > 0)
    {
        if (!checkFileContent(context, udesc, name, clusterStr.str()))
            FileDetails.setShowFileContent(false);

        if (version > 1.05)
        {
            bool fromRoxieCluster = false;

            StringArray roxieClusterNames;
            IArrayOf<IEspTpCluster> roxieclusters;
            CTpWrapper dummy;
            dummy.getClusterProcessList(eqRoxieCluster, roxieclusters);
            ForEachItemIn(k, roxieclusters)
            {
                IEspTpCluster& r_cluster = roxieclusters.item(k);
                StringBuffer sName(r_cluster.getName());
                if (FindInStringArray(clusters, sName.str()))
                {
                    fromRoxieCluster = true;
                    break;
                }
            }
            FileDetails.setFromRoxieCluster(fromRoxieCluster);
        }
    }

    if (version >= 1.25)
        getFilePartsOnClusters(context, cluster, clusters, df, FileDetails);
    else
    {
        FileDetails.setCluster(clusters.item(0));
        IArrayOf<IConstDFUPart>& PartList = FileDetails.getDFUFileParts();

        Owned<IDistributedFilePartIterator> pi = df->getIterator();
        ForEach(*pi)
        {
            Owned<IDistributedFilePart> part = &pi->get();
            for (unsigned int i=0; i<part->numCopies(); i++)
            {
                Owned<IEspDFUPart> FilePart = createDFUPart();

                StringBuffer url;
                part->queryNode(i)->endpoint().getEndpointHostText(url);

                FilePart->setId(part->getPartIndex()+1);
                FilePart->setCopy(i+1);
                FilePart->setIp(url.str());
                FilePart->setPartsize("<N/A>");

                try
                {
                    offset_t size = part->getFileSize(true, false);
                    if (version >= 1.38)
                        FilePart->setPartSizeInt64(size);

                    comma c4(size);
                    tmpstr.clear();
                    tmpstr<<c4;
                    FilePart->setPartsize(tmpstr.str());
                }
                catch(IException *e)
                {
                    StringBuffer msg;
                    IERRLOG("Exception %d:%s in WS_DFU getFileSize()", e->errorCode(), e->errorMessage(msg).str());
                    e->Release();
                }
                catch(...)
                {
                    IERRLOG("Unknown exception in WS_DFU getFileSize");
                }

                PartList.append(*FilePart.getClear());
            }
        }
    }

    unsigned maxSkew, minSkew, maxSkewPart, minSkewPart;
    if (df->getSkewInfo(maxSkew, minSkew, maxSkewPart, minSkewPart, true))
    {
        IEspDFUFileStat& Stat = FileDetails.updateStat();
        if (version >= 1.38)
        {
            Stat.setMinSkewInt64(minSkew);
            Stat.setMaxSkewInt64(maxSkew);
        }
        if (version >= 1.52)
        {
            Stat.setMinSkewPart(minSkewPart);
            Stat.setMaxSkewPart(maxSkewPart);
        }

        VStringBuffer minSkewString("%s%.2f", (minSkew>0) ? "-" : "", ((double)minSkew)/100);
        Stat.setMinSkew(minSkewString.str());
        VStringBuffer maxSkewString("%.2f", ((double)maxSkew)/100);
        Stat.setMaxSkew(maxSkewString.str());
    }

    if (version > 1.06)
    {
        const char *wuid = df->queryAttributes().queryProp("@workunit");
        if (wuid && *wuid && (wuid[0]=='W'))
        {
            try
            {
                CWUWrapper wu(wuid, context);

                StringArray graphs;
                Owned<IPropertyTreeIterator> f=&wu->getFileIterator();
                ForEach(*f)
                {
                    IPropertyTree &query = f->query();
                    const char *fileName = query.queryProp("@name");
                    const char *graphName = query.queryProp("@graph");
                    if (!fileName || !graphName || !*graphName || stricmp(fileName, name))
                        continue;

                    graphs.append(graphName);
                }
                FileDetails.setGraphs(graphs);
            }
            catch(...)
            {
                IERRLOG("Failed in retrieving graphs from workunit %s", wuid);
            }
        }
    }

    if (version > 1.08 && udesc)
    {
        SecAccessFlags permission;
        if (getUserFilePermission(context, udesc, name, permission))
        {
            switch (permission)
            {
            case SecAccess_Full:
                FileDetails.setUserPermission("Full Access Permission");
                break;
            case SecAccess_Write:
                FileDetails.setUserPermission("Write Access Permission");
                break;
            case SecAccess_Read:
                FileDetails.setUserPermission("Read Access Permission");
                break;
            case SecAccess_Access:
                FileDetails.setUserPermission("Access Permission");
                break;
            case SecAccess_None:
                FileDetails.setUserPermission("None Access Permission");
                break;
            default:
                FileDetails.setUserPermission("Permission Unknown");
                break;
            }
        }
    }
    if (includeJsonTypeInfo||includeBinTypeInfo)
    {
        MemoryBuffer binLayout;
        StringBuffer jsonLayout;
        if (getRecordFormatFromRtlType(binLayout, jsonLayout, df->queryAttributes(), includeBinTypeInfo, includeJsonTypeInfo) ||
            getRecordFormatFromECL(binLayout.clear(), jsonLayout.clear(), df->queryAttributes(), includeBinTypeInfo, includeJsonTypeInfo))
        {
            if (includeBinTypeInfo)
                FileDetails.setBinInfo(binLayout);
            if (includeJsonTypeInfo)
                FileDetails.setJsonInfo(jsonLayout);
        }
    }
    if (version >= 1.60)
    {
        cost_type atRestCost, accessCost;
        df->getCost(cluster, atRestCost, accessCost);

        if (version <= 1.61)
            FileDetails.setCost(cost_type2money(atRestCost+accessCost));
        else
        {
            FileDetails.setAccessCost(cost_type2money(accessCost));
            FileDetails.setAtRestCost(cost_type2money(atRestCost));
        }
    }
    if (version >= 1.65)
    {
        if (isFileKey(df))
        {
            DerivedIndexInformation info;
            if (calculateDerivedIndexInformation(info, df, forceIndexInfo))
            {
                IEspDFUIndexInfo & extended = FileDetails.updateExtendedIndexInfo();
                extended.setIsLeafCountEstimated(!info.knownLeafCount);
                extended.setNumLeafNodes(info.numLeafNodes);
                if (info.knownLeafCount)
                    extended.setNumBlobNodes(info.numBlobNodes);
                extended.setNumBranchNodes(info.numBranchNodes);
                extended.setSizeDiskLeaves(info.sizeDiskLeaves);
                if (info.knownLeafCount)
                    extended.setSizeDiskBlobs(info.sizeDiskBlobs);
                extended.setSizeDiskBranches(info.sizeDiskBranches);
                if (info.sizeOriginalData)
                    extended.setSizeOriginalData(info.sizeOriginalData);
                extended.setSizeOriginalBranches(info.sizeOriginalBranches);
                extended.setSizeMemoryLeaves(info.sizeMemoryLeaves);
                extended.setSizeMemoryBranches(info.sizeMemoryBranches);
                extended.setBranchCompressionPercent(info.getBranchCompression()*100);
                double dataCompression = info.getDataCompression();
                if (dataCompression != 0)
                    extended.setDataCompressionPercent(dataCompression*100);
            }
        }
    }
    PROGLOG("doGetFileDetails: %s done", name);
}

bool CWsDfuEx::getQueryFile(const char *logicalName, const char *querySet, const char *queryID, IEspDFUFileDetail &fileDetails)
{
    Owned<IConstWUClusterInfo> info = getWUClusterInfoByName(querySet);
    if (!info || (info->getPlatform()!=RoxieCluster))
        return false;

    SCMStringBuffer process;
    info->getRoxieProcess(process);
    if (!process.length())
        return false;

    Owned<IHpccPackageSet> ps = createPackageSet(process.str());
    if (!ps)
        return false;

    const IHpccPackageMap *pm = ps->queryActiveMap(querySet);
    if (!pm)
        return false;

    const IHpccPackage *pkg = pm->matchPackage(queryID);
    if (!pkg)
        return false;

    const char *pkgid = pkg->locateSuperFile(logicalName);
    if (!pkgid)
        return false;

    fileDetails.setName(logicalName);
    fileDetails.setIsSuperfile(true);
    fileDetails.setPackageID(pkgid);
    StringArray subFiles;

    Owned<ISimpleSuperFileEnquiry> ssfe = pkg->resolveSuperFile(logicalName);
    if (ssfe && ssfe->numSubFiles()>0)
    {
        unsigned count = ssfe->numSubFiles();
        while (count--)
        {
            StringBuffer subfile;
            ssfe->getSubFileName(count, subfile);
            subFiles.append(subfile.str());
        }
        if (!subFiles.empty())
            fileDetails.setSubfiles(subFiles);
    }
    return true;
}

void CWsDfuEx::getLogicalFileAndDirectory(IEspContext &context, IUserDescriptor* udesc, const char *dirname,
    bool includeSuperOwner, IArrayOf<IEspDFULogicalFile>& logicalFiles, int& numFiles, int& numDirs)
{
    double version = context.getClientVersion();
    if (dirname && *dirname)
        PROGLOG("getLogicalFileAndDirectory: %s", dirname);
    else
        PROGLOG("getLogicalFileAndDirectory: folder not specified");

    numFiles = 0;
    numDirs = 0;
    if (dirname && *dirname)
    {
        StringBuffer filterBuf;
        setFileNameFilter(NULL, dirname, filterBuf);
        std::vector<DFUQResultField> fields = {DFUQResultField::includeAll};
        if (!includeSuperOwner)
            fields.push_back(DFUQResultField::superowners|DFUQResultField::exclude);
        fields.push_back(DFUQResultField::term);

        DFUQResultField sortOrder[] = {DFUQResultField::term};

        __int64 cacheHint = 0; //No page
        unsigned totalFiles = 0;
        bool allMatchingFilesReceived = true;
        Owned<IPropertyTreeIterator> it = queryDistributedFileDirectory().getLogicalFiles(udesc, sortOrder, filterBuf.str(),
            nullptr, fields.data(), 0, (unsigned)-1, &cacheHint, &totalFiles, &allMatchingFilesReceived, false, false);
        if(!it)
            throw MakeStringException(ECLWATCH_CANNOT_GET_FILE_ITERATOR,"Cannot get LogicalFile information from file system.");

        ForEach(*it)
            WsDFUHelpers::addToLogicalFileList(it->query(), NULL, version, logicalFiles);
        numFiles = totalFiles;
    }

    Owned<IDFScopeIterator> iter = queryDistributedFileDirectory().getScopeIterator(udesc,dirname,false);
    if(iter)
    {
        ForEach(*iter)
        {
            const char *scope = iter->query();
            if (scope && *scope)
            {
                Owned<IEspDFULogicalFile> file = createDFULogicalFile("","");
                file->setDirectory(scope);
                file->setIsDirectory(true);
                logicalFiles.append(*file.getClear());
                numDirs++;
            }
        }
    }
}

bool CWsDfuEx::onDFUFileView(IEspContext &context, IEspDFUFileViewRequest &req, IEspDFUFileViewResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUFileView: Permission denied.");

        Owned<IUserDescriptor> userdesc;
        StringBuffer username;
        context.getUserID(username);

        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        int numDirs = 0;
        int numFiles = 0;
        IArrayOf<IEspDFULogicalFile> logicalFiles;
        getLogicalFileAndDirectory(context, userdesc.get(), req.getScope(), !req.getIncludeSuperOwner_isNull() && req.getIncludeSuperOwner(), logicalFiles, numFiles, numDirs);

        if (numFiles > 0)
            resp.setNumFiles(numFiles);

        if (req.getScope() && *req.getScope())
            resp.setScope(req.getScope());
        else
            resp.setScope("");
        resp.setDFULogicalFiles(logicalFiles);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}


__int64 CWsDfuEx::findPositionBySize(const __int64 size, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    __int64 addToPos = -1;

    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char* sSize = File.getLongSize();
        __int64 nSize = atoi64_l(sSize,strlen(sSize));
        if (descend && size > nSize)
        {
            addToPos = i;
            break;
        }
        if (!descend && size < nSize)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByParts(const __int64 parts, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    __int64 addToPos = -1;

    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char* sParts = File.getParts();
        __int64 nParts = atoi64_l(sParts,strlen(sParts));
        if (descend && parts > nParts)
        {
            addToPos = i;
            break;
        }
        if (!descend && parts < nParts)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByRecords(const __int64 records, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    __int64 addToPos = -1;

    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char* sRecords = File.getLongRecordCount();
        __int64 nRecords = atoi64_l(sRecords,strlen(sRecords));
        if (descend && records > nRecords)
        {
            addToPos = i;
            break;
        }
        if (!descend && records < nRecords)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByName(const char *name, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    if (!name || (strlen(name) < 1))
    {
        if (descend)
            return -1;
        else
            return 0;
    }

    __int64 addToPos = -1;
    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char *Name = File.getName();
        if (!Name)
            continue;

        if (descend && strcmp(name, Name)>0)
        {
            addToPos = i;
            break;
        }
        if (!descend && strcmp(name, Name)<0)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByNodeGroup(double version, const char *node, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    if (!node || !*node)
    {
        if (descend)
            return -1;
        else
            return 0;
    }

    __int64 addToPos = -1;
    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char *nodeGroup = NULL;
        if (version < 1.26)
            nodeGroup = File.getClusterName();
        else
            nodeGroup = File.getNodeGroup();
        if (!nodeGroup)
            continue;

        if (descend && strcmp(node, nodeGroup)>0)
        {
            addToPos = i;
            break;
        }
        if (!descend && strcmp(node, nodeGroup)<0)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByOwner(const char *owner, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    if (!owner || (strlen(owner) < 1))
    {
        if (descend)
            return -1;
        else
            return 0;
    }

    __int64 addToPos = -1;
    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char *Owner = File.getOwner();
        if (!Owner)
            continue;

        if (descend && strcmp(owner, Owner)>0)
        {
            addToPos = i;
            break;
        }
        if (!descend && strcmp(owner, Owner)<0)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByDate(const char *datetime, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    if (!datetime || (strlen(datetime) < 1))
    {
        if (descend)
            return -1;
        else
            return 0;
    }

    __int64 addToPos = -1;
    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char *modDate = File.getModified();
        if (!modDate)
            continue;

        if (descend && strcmp(datetime, modDate)>0)
        {
            addToPos = i;
            break;
        }
        if (!descend && strcmp(datetime, modDate)<0)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

__int64 CWsDfuEx::findPositionByDescription(const char *description, bool descend, IArrayOf<IEspDFULogicalFile>& LogicalFiles)
{
    if (!description || (strlen(description) < 1))
    {
        if (descend)
            return -1;
        else
            return 0;
    }

    __int64 addToPos = -1;
    ForEachItemIn(i, LogicalFiles)
    {
        IEspDFULogicalFile& File = LogicalFiles.item(i);
        const char *Description = File.getDescription();
        if (!Description)
            continue;

        if (descend && strcmp(description, Description)>0)
        {
            addToPos = i;
            break;
        }
        if (!descend && strcmp(description, Description)<0)
        {
            addToPos = i;
            break;
        }
    }

    return addToPos;
}

//The code inside this method is copied from previous code for legacy (< 5.0) dali support
void CWsDfuEx::getAPageOfSortedLogicalFile(IEspContext &context, IUserDescriptor* udesc, IEspDFUQueryRequest & req, IEspDFUQueryResponse & resp)
{
    double version = context.getClientVersion();

    IArrayOf<IEspDFULogicalFile> LogicalFiles;
    StringBuffer filter;
    const char* fname = req.getLogicalName();
    if(fname && *fname)
    {
        filter.append(fname);
    }
    else
    {
        if(req.getPrefix() && *req.getPrefix())
        {
            filter.append(req.getPrefix());
            filter.append("::");
        }
        filter.append("*");
    }

    Owned<IPropertyTreeIterator> fi;
    bool bNotInSuperfile = false;
    const char* sFileType = req.getFileType();
    if (sFileType && !stricmp(sFileType, "Not in Superfiles"))
    {
        bNotInSuperfile = true;
    }

    if (bNotInSuperfile)
    {
        fi.setown(createSubFileFilter(
            queryDistributedFileDirectory().getDFAttributesIterator(filter.toLowerCase().str(),udesc,true,true, NULL),udesc,false)); // NB wrapper owns wrapped iterator
    }
    else
    {
        fi.setown(queryDistributedFileDirectory().getDFAttributesIterator(filter.toLowerCase().str(), udesc,true,true, NULL));
    }
    if(!fi)
        throw MakeStringException(ECLWATCH_CANNOT_GET_FILE_ITERATOR,"Cannot get information from file system.");

    StringBuffer wuFrom, wuTo;
    if(req.getStartDate() && *req.getStartDate())
    {
        CDateTime wuTime;
        wuTime.setString(req.getStartDate(),NULL,true);

        unsigned year, month, day, hour, minute, second, nano;
        wuTime.getDate(year, month, day, true);
        wuTime.getTime(hour, minute, second, nano, true);
        wuFrom.appendf("%4d-%02d-%02d %02d:%02d:%02d",year,month,day,hour,minute,second);
    }

    if(req.getEndDate() && *req.getEndDate())
    {
        CDateTime wuTime;
        wuTime.setString(req.getEndDate(),NULL,true);

        unsigned year, month, day, hour, minute, second, nano;
        wuTime.getDate(year, month, day, true);
        wuTime.getTime(hour, minute, second, nano, true);
        wuTo.appendf("%4d-%02d-%02d %02d:%02d:%02d",year,month,day,hour,minute,second);
    }

    StringBuffer sortBy;
    if(req.getSortby() && *req.getSortby())
    {
        sortBy.append(req.getSortby());
    }

    unsigned pagesize = req.getPageSize();
    if (pagesize < 1)
    {
        pagesize = 100;
    }

    __int64 displayStartReq = 1;
    if (req.getPageStartFrom() > 0)
        displayStartReq = req.getPageStartFrom();

    __int64 displayStart = displayStartReq - 1;
    __int64 displayEnd = displayStart + pagesize;

    bool descending = req.getDescending();
    const int nFirstN = req.getFirstN();
    const char* sFirstNType = req.getFirstNType();
    const __int64 nFileSizeFrom = req.getFileSizeFrom();
    const __int64 nFileSizeTo = req.getFileSizeTo();
    if (nFirstN > 0)
    {
        displayStart = 0;
        displayEnd = nFirstN;
        if (!stricmp(sFirstNType, "newest"))
        {
            sortBy.set("Modified");
            descending = true;
        }
        else if (!stricmp(sFirstNType, "oldest"))
        {
            sortBy.set("Modified");
            descending = false;
        }
        else if (!stricmp(sFirstNType, "largest"))
        {
            sortBy.set("FileSize");
            descending = true;
        }
        else if (!stricmp(sFirstNType, "smallest"))
        {
            sortBy.set("FileSize");
            descending = false;
        }
        pagesize = nFirstN;
    }

    StringArray roxieClusterNames;
    IArrayOf<IEspTpCluster> roxieclusters;
    CTpWrapper dummy;
    dummy.getClusterProcessList(eqRoxieCluster, roxieclusters);
    ForEachItemIn(k, roxieclusters)
    {
        IEspTpCluster& cluster = roxieclusters.item(k);
        StringBuffer sName(cluster.getName());
        roxieClusterNames.append(sName.str());
    }

    StringArray nodeGroupsReq;
    const char* nodeGroupsReqString = req.getNodeGroup();
    if (nodeGroupsReqString && *nodeGroupsReqString)
        nodeGroupsReq.appendListUniq(nodeGroupsReqString, ",");

    StringBuffer size;
    __int64 totalFiles = 0;
    IArrayOf<IEspDFULogicalFile> LogicalFileList;
    ForEach(*fi)
    {
        IPropertyTree &attr=fi->query();

        const char* logicalName=attr.queryProp("@name");
        if (!logicalName || (logicalName[0] == 0))
            continue;

        try
        {
            StringBuffer pref;
            const char *c=strstr(logicalName, "::");
            if (c)
                pref.append(c-logicalName, logicalName);
            else
                pref.append(logicalName);

            const char* owner=attr.queryProp("@owner");
            if (req.getOwner() && *req.getOwner()!=0)
            {
                if (!owner || stricmp(owner, req.getOwner()))
                    continue;
            }
            StringArray nodeGroups;
            StringArray fileNodeGroups;
            if (getFileGroups(&attr,fileNodeGroups)==0)
            {
                if (!nodeGroupsReq.length())
                    nodeGroups.append("");
            }
            else if (nodeGroupsReq.length() > 0) // check specified cluster name in list
            {
                ForEachItemIn(ii,nodeGroupsReq)
                {
                    const char* nodeGroupReq = nodeGroupsReq.item(ii);
                    ForEachItemIn(i,fileNodeGroups)
                    {
                        if (strieq(fileNodeGroups.item(i), nodeGroupReq))
                        {
                            nodeGroups.append(nodeGroupReq);
                            break;
                        }
                    }
                }
            }
            else if (fileNodeGroups.length())
            {
                ForEachItemIn(i,fileNodeGroups)
                    nodeGroups.append(fileNodeGroups.item(i));
            }

            if (sFileType && *sFileType)
            {
                bool bHasSubFiles = attr.hasProp("@numsubfiles");
                if (bHasSubFiles && (bNotInSuperfile || !stricmp(sFileType, "Logical Files Only")))
                    continue;
                else if (!bHasSubFiles && !stricmp(sFileType, "Superfiles Only"))
                    continue;
            }

            __int64 recordSize=attr.getPropInt64("@recordSize",0), size=attr.getPropInt64("@size",-1);

            if (nFileSizeFrom > 0 && size < nFileSizeFrom)
                continue;
            if (nFileSizeTo > 0 && size > nFileSizeTo)
                continue;

            StringBuffer modf(attr.queryProp("@modified"));
            char* t=(char *) strchr(modf.str(),'T');
            if(t) *t=' ';

            if (wuFrom.length() && strcmp(modf.str(),wuFrom.str())<0)
                continue;

            if (wuTo.length() && strcmp(modf.str(),wuTo.str())>0)
                continue;

            __int64 parts = 0;
            if(!attr.hasProp("@numsubfiles"))
                parts = attr.getPropInt64("@numparts");

            __int64 records = 0;
            if (attr.hasProp("@recordCount"))
                records = attr.getPropInt64("@recordCount");
            else if(recordSize)
                records = size/recordSize;

            const char* desc = attr.queryProp("@description");
            ForEachItemIn(i, nodeGroups)
            {
                const char* nodeGroup = nodeGroups.item(i);
                __int64 addToPos = -1; //Add to tail
                if (stricmp(sortBy, "FileSize")==0)
                {
                    addToPos = findPositionBySize(size, descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "Parts")==0)
                {
                    addToPos = findPositionByParts(parts, descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "Owner")==0)
                {
                    addToPos = findPositionByOwner(owner, descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "NodeGroup")==0)
                {
                    addToPos = findPositionByNodeGroup(version, nodeGroup, descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "Records")==0)
                {
                    addToPos = findPositionByRecords(records, descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "Modified")==0)
                {
                    addToPos = findPositionByDate(modf.str(), descending, LogicalFileList);
                }
                else if (stricmp(sortBy, "Description")==0)
                {
                    addToPos = findPositionByDescription(desc, descending, LogicalFileList);
                }
                else
                {
                    addToPos = findPositionByName(logicalName, descending, LogicalFileList);
                }

                totalFiles++;
                if (addToPos < 0 && (totalFiles > displayEnd))
                    continue;

                Owned<IEspDFULogicalFile> File = createDFULogicalFile("","");

                File->setPrefix(pref);
                if (version < 1.26)
                    File->setClusterName(nodeGroup);
                else
                    File->setNodeGroup(nodeGroup);
                File->setName(logicalName);
                File->setOwner(owner);
                File->setDescription(desc);
                File->setModified(modf.str());
                File->setReplicate(true);

                ForEachItemIn(j, roxieClusterNames)
                {
                    const char* roxieClusterName = roxieClusterNames.item(j);
                    if (roxieClusterName && nodeGroup && strieq(roxieClusterName, nodeGroup))
                    {
                        File->setFromRoxieCluster(true);
                        break;
                    }
                }

                bool bSuperfile = false;
                int numSubFiles = attr.hasProp("@numsubfiles");
                if(!numSubFiles)
                {
                    File->setDirectory(attr.queryProp("@directory"));
                    File->setParts(attr.queryProp("@numparts"));
                }
                else
                {
                    bSuperfile = true;
                }
                File->setIsSuperfile(bSuperfile);

                if (version < 1.22)
                    File->setIsZipfile(isCompressed(attr));
                else
                {
                    File->setIsCompressed(isCompressed(attr));
                    if (attr.hasProp("@compressedSize"))
                        File->setCompressedFileSize(attr.getPropInt64("@compressedSize"));
                }

                //File->setBrowseData(bKeyFile); //Bug: 39750 - All files should be viewable through ViewKeyFile function
                if (numSubFiles > 1) //Bug 41379 - ViewKeyFile Cannot handle superfile with multiple subfiles
                    File->setBrowseData(false);
                else
                    File->setBrowseData(true);

                if (version > 1.13)
                {
                    bool bKeyFile = false;
                    const char * kind = attr.queryProp("@kind");
                    if (kind && (stricmp(kind, "key") == 0))
                    {
                        bKeyFile = true;
                    }

                    if (version < 1.24)
                        File->setIsKeyFile(bKeyFile);
                    else if (kind && *kind)
                        File->setContentType(kind);
                }

                StringBuffer buf;
                buf << comma(size);
                File->setTotalsize(buf.str());
                char temp[64];
                numtostr(temp, size);
                File->setLongSize(temp);
                numtostr(temp, records);
                File->setLongRecordCount(temp);
                if (records > 0)
                    File->setRecordCount((buf.clear()<<comma(records)).str());

                if (addToPos < 0)
                    LogicalFileList.append(*File.getClear());
                else
                    LogicalFileList.add(*File.getClear(), (int) addToPos);

                if (LogicalFileList.length() > displayEnd)
                    LogicalFileList.pop();
            }
        }
        catch(IException* e)
        {
            VStringBuffer msg("Failed to retrieve data for logical file %s: ", logicalName);
            int code = e->errorCode();
            e->errorMessage(msg);
            e->Release();
            throw MakeStringException(code, "%s", msg.str());
        }
    }

    if (displayEnd > LogicalFileList.length())
        displayEnd = LogicalFileList.length();

    for (int i = (int) displayStart; i < (int) displayEnd; i++)
    {
        Owned<IEspDFULogicalFile> File = createDFULogicalFile("","");
        IEspDFULogicalFile& File0 = LogicalFileList.item(i);
        File->copy(File0);
        LogicalFiles.append(*File.getClear());
    }

    resp.setNumFiles(totalFiles);
    resp.setPageSize(pagesize);
    resp.setPageStartFrom(displayStart+1);
    resp.setPageEndAt(displayEnd);

    if (displayStart - pagesize > 0)
        resp.setPrevPageFrom(displayStart - pagesize + 1);
    else if(displayStart > 0)
        resp.setPrevPageFrom(1);

    if(displayEnd < totalFiles)
    {
        resp.setNextPageFrom(displayEnd+1);
        resp.setLastPageFrom((int)(pagesize * floor((double) ((totalFiles-1) / pagesize)) + 1));
    }

    StringBuffer basicQuery;
    if (req.getNodeGroup() && *req.getNodeGroup())
    {
        if (version < 1.26)
            resp.setClusterName(req.getNodeGroup());
        else
            resp.setNodeGroup(req.getNodeGroup());
        addToQueryString(basicQuery, "NodeGroup", req.getNodeGroup());
    }
    if (req.getOwner() && *req.getOwner())
    {
        resp.setOwner(req.getOwner());
        addToQueryString(basicQuery, "Owner", req.getOwner());
    }
    if (req.getPrefix() && *req.getPrefix())
    {
        resp.setPrefix(req.getPrefix());
        addToQueryString(basicQuery, "Prefix", req.getPrefix());
    }
    if (req.getLogicalName() && *req.getLogicalName())
    {
        resp.setLogicalName(req.getLogicalName());
        addToQueryString(basicQuery, "LogicalName", req.getLogicalName());
    }
    if (req.getStartDate() && *req.getStartDate())
    {
        resp.setStartDate(req.getStartDate());
        addToQueryString(basicQuery, "StartDate", req.getStartDate());
    }
    if (req.getEndDate() && *req.getEndDate())
    {
        resp.setEndDate(req.getEndDate());
        addToQueryString(basicQuery, "EndDate", req.getEndDate());
    }
    if (req.getFileType() && *req.getFileType())
    {
        resp.setFileType(req.getFileType());
        addToQueryString(basicQuery, "FileType", req.getFileType());
    }
    if (req.getFileSizeFrom())
    {
        resp.setFileSizeFrom(req.getFileSizeFrom());
        addToQueryStringFromInt(basicQuery, "FileSizeFrom", req.getFileSizeFrom());
    }
    if (req.getFileSizeTo())
    {
        resp.setFileSizeTo(req.getFileSizeTo());
        addToQueryStringFromInt(basicQuery, "FileSizeTo", req.getFileSizeTo());
    }

    StringBuffer ParametersForFilters(basicQuery);
    StringBuffer ParametersForPaging(basicQuery);

    addToQueryStringFromInt(ParametersForFilters, "PageSize",pagesize);
    addToQueryStringFromInt(ParametersForPaging, "PageSize", pagesize);

    if (ParametersForFilters.length() > 0)
        resp.setFilters(ParametersForFilters.str());

    sortBy.clear();
    descending = false;
    if ((req.getFirstN() > 0) && req.getFirstNType() && *req.getFirstNType())
    {
        const char *sFirstNType = req.getFirstNType();
        if (!stricmp(sFirstNType, "newest"))
        {
            sortBy.set("Modified");
            descending = true;
        }
        else if (!stricmp(sFirstNType, "oldest"))
        {
            sortBy.set("Modified");
            descending = false;
        }
        else if (!stricmp(sFirstNType, "largest"))
        {
            sortBy.set("FileSize");
            descending = true;
        }
        else if (!stricmp(sFirstNType, "smallest"))
        {
            sortBy.set("FileSize");
            descending = false;
        }

    }
    else if (req.getSortby() && *req.getSortby())
    {
        sortBy.set(req.getSortby());
        if (req.getDescending())
            descending = req.getDescending();
    }

    if (sortBy.length())
    {
        resp.setSortby(sortBy);
        resp.setDescending(descending);

        StringBuffer strbuf(sortBy);
        strbuf.append("=");
        String str1(strbuf.str());
        String str(basicQuery.str());
        if (str.indexOf(str1) < 0)
        {
            addToQueryString(ParametersForPaging, "Sortby", sortBy);
            addToQueryString(basicQuery, "Sortby", sortBy);
            if (descending)
            {
                addToQueryString(ParametersForPaging, "Descending", "1");
                addToQueryString(basicQuery, "Descending", "1");
            }
        }
    }

    if (basicQuery.length() > 0)
        resp.setBasicQuery(basicQuery.str());
    if (ParametersForPaging.length() > 0)
        resp.setParametersForPaging(ParametersForPaging.str());
    resp.setDFULogicalFiles(LogicalFiles);
    return;
}

void CWsDfuEx::setFileTypeFilter(const char* fileType, StringBuffer& filterBuf)
{
    DFUQFileTypeFilter fileTypeFilter = DFUQFFTall;
    if (!fileType || !*fileType)
    {
        filterBuf.append(DFUQFTspecial).append(DFUQFilterSeparator).append(DFUQSFFileType).append(DFUQFilterSeparator).append(fileTypeFilter).append(DFUQFilterSeparator);
        return;
    }
    bool notInSuperfile = false;
    if (strieq(fileType, "Superfiles Only"))
        fileTypeFilter = DFUQFFTsuperfileonly;
    else if (strieq(fileType, "Logical Files Only"))
        fileTypeFilter = DFUQFFTnonsuperfileonly;
    else if (strieq(fileType, "Not in Superfiles"))
        notInSuperfile = true;
    else
        fileTypeFilter = DFUQFFTall;
    filterBuf.append(DFUQFTspecial).append(DFUQFilterSeparator).append(DFUQSFFileType).append(DFUQFilterSeparator).append(fileTypeFilter).append(DFUQFilterSeparator);
    if (notInSuperfile)
        WsDFUHelpers::appendDFUQueryFilter(getDFUQFilterFieldName(DFUQFFsuperowner), DFUQFThasProp, "0", filterBuf);
}

void CWsDfuEx::setFileNameFilter(const char* fname, const char* prefix, StringBuffer &filterBuf)
{
    StringBuffer fileNameFilter;
    if(fname && *fname)
        fileNameFilter.append(fname);//ex. *part_of_file_name*
    else
    {
        if(prefix && *prefix)
        {
            fileNameFilter.append(prefix);
            fileNameFilter.append("::");
        }
        fileNameFilter.append("*");
    }
    fileNameFilter.toLowerCase();
    filterBuf.append(DFUQFTspecial).append(DFUQFilterSeparator).append(DFUQSFFileNameWithPrefix).append(DFUQFilterSeparator).append(fileNameFilter.str()).append(DFUQFilterSeparator);
}

void CWsDfuEx::setFileIterateFilter(unsigned maxFiles, StringBuffer &filterBuf)
{
    filterBuf.append(DFUQFTspecial).append(DFUQFilterSeparator).append(DFUQSFMaxFiles).append(DFUQFilterSeparator)
        .append(maxFiles).append(DFUQFilterSeparator);
}

void CWsDfuEx::setDFUQueryFilters(IEspDFUQueryRequest& req, StringBuffer& filterBuf)
{
    setFileNameFilter(req.getLogicalName(), req.getPrefix(), filterBuf);
    setFileTypeFilter(req.getFileType(), filterBuf);
    WsDFUHelpers::appendDFUQueryFilter(getDFUQFilterFieldName(DFUQFFattrowner), DFUQFTwildcardMatch, req.getOwner(), filterBuf);

    if (req.getInvertContent_isNull() || !req.getInvertContent())
    {
        WsDFUHelpers::appendDFUQueryFilter(getDFUQFilterFieldName(DFUQFFkind), DFUQFTwildcardMatch, req.getContentType(), filterBuf);
    }
    else
    {
        WsDFUHelpers::appendDFUQueryFilter(getDFUQFilterFieldName(DFUQFFkind), DFUQFTinverseWildcardMatch, req.getContentType(), filterBuf);
    }
    WsDFUHelpers::appendDFUQueryFilter(getDFUQFilterFieldName(DFUQFFgroup), DFUQFTcontainString, req.getNodeGroup(), ",", filterBuf);

    setInt64RangeFilter(req.getFileSizeFrom(), req.getFileSizeTo(), DFUQFFattrsize, filterBuf);
    setInt64RangeFilter(req.getMaxSkewFrom(), req.getMaxSkewTo(), DFUQFFattrmaxskew, filterBuf);
    setInt64RangeFilter(req.getMinSkewFrom(), req.getMinSkewTo(), DFUQFFattrminskew, filterBuf);

    setTimeRangeFilter(req.getStartDate(), req.getEndDate(), DFUQFFtimemodified, filterBuf);
    setTimeRangeFilter(req.getStartAccessedTime(), req.getEndAccessedTime(), DFUQFFaccessed, filterBuf);
}

void CWsDfuEx::setInt64RangeFilter(__int64 from, __int64 to, DFUQFilterField filterID, StringBuffer &filterBuf)
{
    if ((from == 0) && (to == 0))
        return;

    filterBuf.append(DFUQFTinteger64Range).append(DFUQFilterSeparator);
    filterBuf.append(getDFUQFilterFieldName(filterID)).append(DFUQFilterSeparator);
    StringBuffer buf;
    if (from > 0)
        filterBuf.append(from);
    filterBuf.append(DFUQFilterSeparator);
    if (to > 0)
        filterBuf.append(to);
    filterBuf.append(DFUQFilterSeparator);
}

void CWsDfuEx::setTimeRangeFilter(const char *from, const char *to, DFUQFilterField filterID, StringBuffer &filterBuf)
{
    if (isEmptyString(from) && isEmptyString(to))
        return;

    StringBuffer filterValue;
    if (!isEmptyString(from))
        appendTimeString(from, filterValue);
    filterValue.append(DFUQFilterSeparator);
    if (!isEmptyString(to))
        appendTimeString(to, filterValue);

    filterBuf.append(DFUQFTstringRange).append(DFUQFilterSeparator).append(getDFUQFilterFieldName(filterID));
    filterBuf.append(DFUQFilterSeparator).append(filterValue).append(DFUQFilterSeparator);
}

void CWsDfuEx::appendTimeString(const char *in, StringBuffer &out)
{
    CDateTime wuTime;
    wuTime.setString(in, nullptr);
    wuTime.getString(out);
}

void CWsDfuEx::setDFUQuerySortOrder(IEspDFUQueryRequest& req, StringBuffer& sortBy, bool& descending, DFUQResultField* sortOrder)
{
    const char* sortByReq = req.getSortby();
    if (!sortByReq || !*sortByReq)
        return;

    sortBy.set(sortByReq);
    if (req.getDescending())
        descending = req.getDescending();

    // These mappings do not conform to DFS mapping (see dadfs getDFUQResultField())
    static const std::unordered_map<std::string_view, std::string_view> legacyMappings =
    {
        {"FileSize", "@DFUSFsize"},
        {"ContentType", "@kind"},
        {"IsCompressed", "@compressed"},
        {"Records", "@recordcount"},
        {"Parts", "@numparts"},
        {"NodeGroup", "@DFUSFcluster"}
    };

    const char* sortByPtr = sortBy.str();
    auto it = legacyMappings.find(sortByPtr);
    if (it != legacyMappings.end())
    {
        const std::string_view &dfsFieldName = it->second;
        sortOrder[0] = getDFUQResultFieldAndType(dfsFieldName.data());
    }
    else
    {
        VStringBuffer key("@%s", sortByPtr);
        DFUQResultField fieldAndType = getDFUQResultFieldAndType(key);
        if (DFUQResultField::unknown != fieldAndType) // odd, but previous semantics
            sortOrder[0] = fieldAndType;
        else
            sortOrder[0] = DFUQResultField::name | DFUQResultField::stringType;
    }

    sortOrder[0] = (DFUQResultField) (sortOrder[0] | DFUQResultField::nocase);
    if (descending)
        sortOrder[0] = (DFUQResultField) (sortOrder[0] | DFUQResultField::reverse);
    return;
}

void CWsDfuEx::setDFUQueryResponse(IEspContext &context, unsigned totalFiles, StringBuffer& sortBy, bool descending, unsigned pageStart, unsigned pageSize,
                                   IEspDFUQueryRequest& req, IEspDFUQueryResponse& resp)
{
    //for legacy
    double version = context.getClientVersion();
    unsigned pageEnd = pageStart + pageSize;
    if (pageEnd > totalFiles)
        pageEnd = totalFiles;
    resp.setNumFiles(totalFiles);
    resp.setPageSize(pageSize);
    resp.setPageStartFrom(pageStart+1);
    resp.setPageEndAt(pageEnd);
    if (pageStart > pageSize)
        resp.setPrevPageFrom(pageStart - pageSize + 1);
    else if(pageStart > 0)
        resp.setPrevPageFrom(1);
    if(pageEnd < totalFiles)
    {
        resp.setNextPageFrom(pageEnd+1);
        resp.setLastPageFrom((int)(pageSize * floor((double) ((totalFiles-1) / pageSize)) + 1));
    }

    StringBuffer queryReq;
    if (req.getNodeGroup() && *req.getNodeGroup())
    {
        if (version < 1.26)
            resp.setClusterName(req.getNodeGroup());
        else
            resp.setNodeGroup(req.getNodeGroup());
        addToQueryString(queryReq, "NodeGroup", req.getNodeGroup());
    }
    if (req.getOwner() && *req.getOwner())
    {
        resp.setOwner(req.getOwner());
        addToQueryString(queryReq, "Owner", req.getOwner());
    }
    if (req.getPrefix() && *req.getPrefix())
    {
        resp.setPrefix(req.getPrefix());
        addToQueryString(queryReq, "Prefix", req.getPrefix());
    }
    if (req.getLogicalName() && *req.getLogicalName())
    {
        resp.setLogicalName(req.getLogicalName());
        addToQueryString(queryReq, "LogicalName", req.getLogicalName());
    }
    if (req.getStartDate() && *req.getStartDate())
    {
        resp.setStartDate(req.getStartDate());
        addToQueryString(queryReq, "StartDate", req.getStartDate());
    }
    if (req.getEndDate() && *req.getEndDate())
    {
        resp.setEndDate(req.getEndDate());
        addToQueryString(queryReq, "EndDate", req.getEndDate());
    }
    if (req.getFileType() && *req.getFileType())
    {
        resp.setFileType(req.getFileType());
        addToQueryString(queryReq, "FileType", req.getFileType());
    }
    if (req.getFileSizeFrom())
    {
        resp.setFileSizeFrom(req.getFileSizeFrom());
        addToQueryStringFromInt(queryReq, "FileSizeFrom", req.getFileSizeFrom());
    }
    if (req.getFileSizeTo())
    {
        resp.setFileSizeTo(req.getFileSizeTo());
        addToQueryStringFromInt(queryReq, "FileSizeTo", req.getFileSizeTo());
    }

    StringBuffer queryReqNoPageSize(queryReq);
    addToQueryStringFromInt(queryReq, "PageSize", pageSize);
    resp.setFilters(queryReq.str());

    if (sortBy.length())
    {
        resp.setSortby(sortBy.str());
        resp.setDescending(descending);
        addToQueryString(queryReq, "Sortby", sortBy.str());
        addToQueryString(queryReqNoPageSize, "Sortby", sortBy.str());
        if (descending)
        {
            addToQueryString(queryReq, "Descending", "1");
            addToQueryString(queryReqNoPageSize, "Descending", "1");
        }
    }
    resp.setBasicQuery(queryReqNoPageSize.str());
    resp.setParametersForPaging(queryReq.str());
    return;
}

bool CWsDfuEx::doLogicalFileSearch(IEspContext &context, IUserDescriptor* udesc, IEspDFUQueryRequest & req, IEspDFUQueryResponse & resp)
{
    double version = context.getClientVersion();

    if (req.getOneLevelDirFileReturn())
    {
        int numDirs = 0;
        int numFiles = 0;
        IArrayOf<IEspDFULogicalFile> logicalFiles;
        getLogicalFileAndDirectory(context, udesc, req.getLogicalName(), !req.getIncludeSuperOwner_isNull() && req.getIncludeSuperOwner(), logicalFiles, numFiles, numDirs);
        return true;
    }

    if (queryDaliServerVersion().compare("3.11") < 0)
    {//Dali server does not support Filtered File Query. Use legacy code.
        PROGLOG("DFUQuery: getAPageOfSortedLogicalFile");
        getAPageOfSortedLogicalFile(context, udesc, req, resp);
        return true;
    }

    StringBuffer filterBuf;
    setDFUQueryFilters(req, filterBuf);
    std::vector<DFUQResultField> fields;
    const char *fieldsReq = req.getFields();
    if (isEmptyString(fieldsReq))
    {
        // legacy default = all except super owners. NB: the server defaults to this too if fields is empty
        // but we're being explicit.
        fields = {DFUQResultField::includeAll};
        if (req.getIncludeSuperOwner_isNull() || !req.getIncludeSuperOwner())
            fields.push_back(DFUQResultField::superowners|DFUQResultField::exclude);
        fields.push_back(DFUQResultField::term);
    }
    else
    {
        StringArray fieldsArray;
        fieldsArray.appendList(fieldsReq, ",");
        ForEachItemIn(f, fieldsArray)
        {
            const char *fieldString = fieldsArray.item(f);
            DFUQResultField field = getDFUQResultField(fieldString);
            if (DFUQResultField::unknown != field) // if unknown, ignore it.
                fields.push_back(field);
        }
        fields.push_back(DFUQResultField::term);
    }

    //Now, set filters which are used to filter query result received from dali server.
    StringBuffer localFilterBuf;
    const char *reqNodeGroup = req.getNodeGroup();
    if (!isEmptyString(reqNodeGroup))
        localFilterBuf.append(DFUQFTwildcardMatch).append(DFUQFilterSeparator).append(getDFUQFilterFieldName(DFUQFFgroup)).append(DFUQFilterSeparator).append(reqNodeGroup);

    StringBuffer sortBy;
    bool descending = false;
    DFUQResultField sortOrder[2] = {DFUQResultField::name, DFUQResultField::term};
    setDFUQuerySortOrder(req, sortBy, descending, sortOrder);

    unsigned pageStart = 0;
    if (req.getPageStartFrom() > 0)
        pageStart = req.getPageStartFrom() - 1;
    unsigned pageSize = req.getPageSize();
    if (pageSize < 1)
        pageSize = 100;
    const int firstN = req.getFirstN();
    if (firstN > 0)
    {
        pageStart = 0;
        pageSize = firstN;
    }

    unsigned maxFiles = 0;
    if(!req.getMaxNumberOfFiles_isNull())
        maxFiles = req.getMaxNumberOfFiles();
    if (maxFiles == 0)
        maxFiles = ITERATE_FILTEREDFILES_LIMIT;
    if (maxFiles != ITERATE_FILTEREDFILES_LIMIT)
        setFileIterateFilter(maxFiles, filterBuf);

    __int64 cacheHint = 0;
    if (!req.getCacheHint_isNull())
        cacheHint = req.getCacheHint();

    bool allMatchingFilesReceived = true;
    unsigned totalFiles = 0;
    PROGLOG("DFUQuery: getLogicalFilesSorted");
    Owned<IPropertyTreeIterator> it = queryDistributedFileDirectory().getLogicalFilesSorted(udesc, sortOrder, filterBuf.str(),
        localFilterBuf, fields.data(), pageStart, pageSize, &cacheHint, &totalFiles, &allMatchingFilesReceived);
    if(!it)
        throw MakeStringException(ECLWATCH_CANNOT_GET_FILE_ITERATOR,"Cannot get information from file system.");
    PROGLOG("DFUQuery: getLogicalFilesSorted done");

    IArrayOf<IEspDFULogicalFile> logicalFiles;
    ForEach(*it)
        WsDFUHelpers::addToLogicalFileList(it->query(), NULL, version, logicalFiles);

    if (!allMatchingFilesReceived)
    {
        VStringBuffer warning("The returned results (%d files) represent a subset of the total number of matches. Using a correct filter may reduce the number of matches.",
            maxFiles);
        resp.setWarning(warning.str());
        resp.setIsSubsetOfFiles(!allMatchingFilesReceived);
    }
    resp.setCacheHint(cacheHint);
    resp.setDFULogicalFiles(logicalFiles);
    setDFUQueryResponse(context, totalFiles, sortBy, descending, pageStart, pageSize, req, resp); //This call may be removed after 5.0

    return true;
}

bool CWsDfuEx::onSuperfileList(IEspContext &context, IEspSuperfileListRequest &req, IEspSuperfileListResponse &resp)
{
    try
    {
        const char* superfile = req.getSuperfile();
        if (!superfile || !*superfile)
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Superfile name required");
        PROGLOG("SuperfileList: %s", superfile);

        StringBuffer username;
        context.getUserID(username);
        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        Owned<IDFUhelper> dfuhelper = createIDFUhelper();
        StringArray farray;
        StringAttrArray subfiles;
        dfuhelper->listSubFiles(req.getSuperfile(), subfiles, userdesc.get());
        for(unsigned i = 0; i < subfiles.length(); i++)
        {
            StringAttrItem& subfile = subfiles.item(i);
            farray.append(subfile.text);
        }
        if(farray.length() > 0)
            resp.setSubfiles(farray);

        resp.setSuperfile(req.getSuperfile());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onSuperfileAction(IEspContext &context, IEspSuperfileActionRequest &req, IEspSuperfileActionResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Write, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::SuperfileAction: Permission denied.");

        const char* action = req.getAction();
        const char* superfile = req.getSuperfile();
        superfileAction(context, action, superfile, req.getSubfiles(), req.getBefore(), true, true, req.getDelete(), req.getRemoveSuperfile());

        resp.setRetcode(0);
        if (superfile && *superfile && action && strieq(action, "remove"))
        {
            Owned<IUserDescriptor> udesc;
            udesc.setown(createUserDescriptor());
            udesc->set(context.queryUserId(), context.queryPassword(), context.querySignature());
            Owned<IDistributedSuperFile> fp = queryDistributedFileDirectory().lookupSuperFile(superfile, udesc, AccessMode::tbdWrite);
            if (!fp)
                resp.setRetcode(-1); //Superfile has been removed.
        }
        resp.setSuperfile(req.getSuperfile());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onSavexml(IEspContext &context, IEspSavexmlRequest &req, IEspSavexmlResponse &resp)
{
    try
    {
        StringBuffer username;
        context.getUserID(username);
        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        if (!req.getName() || !*req.getName())
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Name required");
        PROGLOG("getFileXML: %s", req.getName());

        Owned<IDFUhelper> dfuhelper = createIDFUhelper();
        StringBuffer out;
        dfuhelper->getFileXML(req.getName(), out, userdesc.get());
        MemoryBuffer xmlmap;
        int len = out.length();
        xmlmap.setBuffer(len, out.detach(), true);
        resp.setXmlmap(xmlmap);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onAdd(IEspContext &context, IEspAddRequest &req, IEspAddResponse &resp)
{
    try
    {
        StringBuffer username;
        context.getUserID(username);
        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        if (!req.getDstname() || !*req.getDstname())
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Dstname required.");
        PROGLOG("addFileXML: %s", req.getDstname());

        Owned<IDFUhelper> dfuhelper = createIDFUhelper();
        StringBuffer xmlstr(req.getXmlmap().length(),(const char*)req.getXmlmap().bufferBase());
        dfuhelper->addFileXML(req.getDstname(), xmlstr, req.getDstcluster(), userdesc.get());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onAddRemote(IEspContext &context, IEspAddRemoteRequest &req, IEspAddRemoteResponse &resp)
{
    try
    {
        StringBuffer username;
        context.getUserID(username);
        Owned<IUserDescriptor> userdesc;
        if(username.length() > 0)
        {
            userdesc.setown(createUserDescriptor());
            userdesc->set(username.str(), context.queryPassword(), context.querySignature());
        }

        const char* srcusername = req.getSrcusername();
        Owned<IUserDescriptor> srcuserdesc;
        if(srcusername && *srcusername)
        {
            srcuserdesc.setown(createUserDescriptor());
            srcuserdesc->set(srcusername, req.getSrcpassword(), context.querySignature());
        }

        const char* srcname = req.getSrcname();
        if(srcname == NULL || *srcname == '\0')
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "srcname can't be empty.");
        const char* srcdali = req.getSrcdali();
        if(srcdali == NULL || *srcdali == '\0')
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "srcdali can't be empty.");
        const char* dstname = req.getDstname();
        if(dstname == NULL || *dstname == '\0')
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "dstname can't be empty.");

        PROGLOG("addFileRemote: Srcdali %s, Srcname %s, Dstname %s", srcdali, srcname, dstname);

        SocketEndpoint ep(srcdali);
        Owned<IDFUhelper> dfuhelper = createIDFUhelper();
        dfuhelper->addFileRemote(dstname, ep, srcname, srcuserdesc.get(), userdesc.get());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

const int INTEGELSIZE = 20;
const int REALSIZE = 32;
const int STRINGSIZE = 128;

bool CWsDfuEx::onDFUGetDataColumns(IEspContext &context, IEspDFUGetDataColumnsRequest &req, IEspDFUGetDataColumnsResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUGetDataColumns: Permission denied.");

        StringBuffer logicalNameStr;
        char* logicalName0 = (char*) req.getOpenLogicalName();
        if (logicalName0 && *logicalName0)
        {
            logicalNameStr.append(logicalName0);
            logicalNameStr.trim();
        }

        if (logicalNameStr.length() > 0)
        {
            PROGLOG("DFUGetDataColumns: %s", logicalNameStr.str());

            __int64 startIndex = req.getStartIndex();
            __int64 endIndex = req.getEndIndex();
            if (startIndex < 1)
                startIndex = 1;
            if (endIndex < 1)
                endIndex = 100;

            StringArray filterByNames, filterByValues;
            double version = context.getClientVersion();
            if (version > 1.04)
            {
                const char* filterBy = req.getFilterBy();
                if (filterBy && *filterBy)
                {
                    parseTwoStringArrays(filterBy, filterByNames, filterByValues);
                }

                const char* showColumns = req.getShowColumns();
                if (showColumns && *showColumns)
                {
                    resp.setShowColumns(showColumns);
                }
            }

            Owned<IResultSetFactory> resultSetFactory = getSecResultSetFactory(context.querySecManager(), context.queryUser(), context.queryUserId(), context.queryPassword());
            Owned<INewResultSet> result;
            if (m_clusterName.length() > 0)
            {
                result.setown(resultSetFactory->createNewFileResultSet(logicalNameStr.str(), m_clusterName.str()));
            }
            else
            {
                result.setown(resultSetFactory->createNewFileResultSet(logicalNameStr.str(), NULL));
            }

            __int64 total=result->getNumRows();
            {
                IArrayOf<IEspDFUDataColumn> dataKeyedColumns[MAX_KEY_ROWS];
                IArrayOf<IEspDFUDataColumn> dataNonKeyedColumns[MAX_KEY_ROWS];
               const IResultSetMetaData & meta = result->getMetaData();
                int columnCount = meta.getColumnCount();
                int keyedColumnCount = meta.getNumKeyedColumns();
                unsigned columnSize = 0;
                int lineSizeCount = 0;
                int lineCount = 0;
                for (int i = 0; i < keyedColumnCount; i++)
                {
                    Owned<IEspDFUDataColumn> item = createDFUDataColumn("","");

                    bool bNaturalColumn = true;
                    SCMStringBuffer columnLabel;
                    if (meta.hasSetTranslation(i))
                    {
                        meta.getNaturalColumnLabel(columnLabel, i);
                    }

                    if (columnLabel.length() < 1)
                    {
                        meta.getColumnLabel(columnLabel, i);
                        bNaturalColumn = false;
                    }

                    item->setColumnLabel(columnLabel.str());

                    if (version > 1.04 && filterByNames.length() > 0)
                    {
                        for (unsigned ii = 0; ii < filterByNames.length(); ii++)
                        {
                            const char* name = filterByNames.item(ii);
                            if (name && !stricmp(name, columnLabel.str()))
                            {
                                const char* value = filterByValues.item(ii);
                                if (value && *value)
                                {
                                    item->setColumnValue(value);
                                    break;
                                }
                            }
                        }
                    }

                    DisplayType columnType = meta.getColumnDisplayType(i);
                    if (bNaturalColumn)
                    {
                        item->setColumnType("Others");
                        item->setColumnSize(STRINGSIZE);
                        columnSize = STRINGSIZE;
                        item->setMaxSize(columnSize);

                    }
                    else if (columnType == TypeBoolean)
                    {
                        item->setColumnType("Boolean");
                        item->setMaxSize(1);
                        item->setColumnSize(strlen(columnLabel.str()));
                        columnSize = 2;
                    }
                    else
                    {
                        if (columnType == TypeInteger || columnType == TypeUnsignedInteger)
                        {
                            item->setColumnType("Integer");
                            item->setMaxSize(INTEGELSIZE);
                            columnSize = INTEGELSIZE;
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeReal)
                        {
                            item->setColumnType("Real");
                            item->setMaxSize(REALSIZE);
                            columnSize = REALSIZE;
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeString)
                        {
                            columnSize = meta.getColumnRawSize(i);
                            columnSize = rtlQStrLength(columnSize);
                            if (columnSize < 1)
                                columnSize = STRINGSIZE;
                            else if (columnSize > STRINGSIZE)
                                columnSize = STRINGSIZE;

                            item->setColumnType("String");
                            item->setMaxSize(columnSize);
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeUnicode)
                        {
                            item->setColumnType("Others");
                            columnSize = (int) (meta.getColumnRawSize(i) * 0.5);
                            if (columnSize > STRINGSIZE)
                                columnSize = STRINGSIZE;
                            item->setColumnSize(columnSize);
                            item->setMaxSize(columnSize);
                        }
                        else
                        {
                            item->setColumnType("Others");
                            columnSize = STRINGSIZE;
                            item->setColumnSize(columnSize);
                            item->setMaxSize(columnSize);
                        }
                    }

                    columnSize += 7;
                    if ((lineSizeCount == 0) && (columnSize > STRINGSIZE)) //One field is big enough to use one line
                    {
                        if (lineCount >= MAX_KEY_ROWS)
                            break;

                        dataKeyedColumns[lineCount].append(*item.getLink());
                        lineCount++;
                    }
                    else
                    {
                        if (lineSizeCount + columnSize < STRINGSIZE)
                        {
                            lineSizeCount += columnSize;
                        }
                        else //too big in this line...so, switch to another line
                        {
                            lineCount++;
                            lineSizeCount = columnSize;
                        }

                        if (lineCount >= MAX_KEY_ROWS)
                            break;

                        dataKeyedColumns[lineCount].append(*item.getLink());
                    }
                }

                columnSize = 0;
                lineSizeCount = 0;
                lineCount = 0;
                for (int ii = keyedColumnCount; ii < columnCount; ii++)
                {
                    Owned<IEspDFUDataColumn> item = createDFUDataColumn("","");

                    bool bNaturalColumn = true;
                    SCMStringBuffer columnLabel;
                    if (meta.hasSetTranslation(ii))
                    {
                        meta.getNaturalColumnLabel(columnLabel, ii);
                    }

                    if (columnLabel.length() < 1)
                    {
                        meta.getColumnLabel(columnLabel, ii);
                        bNaturalColumn = false;
                    }

                    item->setColumnLabel(columnLabel.str());

                    if (version > 1.04 && filterByNames.length() > 0)
                    {
                        for (unsigned ii = 0; ii < filterByNames.length(); ii++)
                        {
                            const char* name = filterByNames.item(ii);
                            if (name && !stricmp(name, columnLabel.str()))
                            {
                                const char* value = filterByValues.item(ii);
                                if (value && *value)
                                {
                                    item->setColumnValue(value);
                                    break;
                                }
                            }
                        }
                    }

                    DisplayType columnType = meta.getColumnDisplayType(ii);
                    if (bNaturalColumn)
                    {
                        item->setColumnType("Others");
                        item->setColumnSize(STRINGSIZE);
                        columnSize = STRINGSIZE;
                    }
                    else if (columnType == TypeBoolean)
                    {
                        item->setColumnType("Boolean");
                        item->setMaxSize(1);
                        item->setColumnSize(strlen(columnLabel.str()));
                        columnSize = 2;
                    }
                    else
                    {
                        if (columnType == TypeInteger || columnType == TypeUnsignedInteger)
                        {
                            item->setColumnType("Integer");
                            item->setMaxSize(INTEGELSIZE);
                            columnSize = INTEGELSIZE;
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeReal)
                        {
                            item->setColumnType("Real");
                            item->setMaxSize(REALSIZE);
                            columnSize = REALSIZE;
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeString)
                        {
                            columnSize = meta.getColumnRawSize(ii);
                            columnSize = rtlQStrLength(columnSize);
                            if (columnSize < 1)
                                columnSize = STRINGSIZE;
                            else if (columnSize > STRINGSIZE)
                                columnSize = STRINGSIZE;

                            item->setColumnType("String");
                            item->setMaxSize(columnSize);
                            if (strlen(columnLabel.str()) > columnSize)
                                columnSize = strlen(columnLabel.str());
                            item->setColumnSize(columnSize);
                        }
                        else if (columnType == TypeUnicode)
                        {
                            item->setColumnType("Others");
                            columnSize = (int) (meta.getColumnRawSize(ii) * 0.5);
                            if (columnSize > STRINGSIZE)
                                columnSize = STRINGSIZE;
                            item->setColumnSize(columnSize);
                            item->setMaxSize(columnSize);
                        }
                        else
                        {
                            item->setColumnType("Others");
                            columnSize = STRINGSIZE;
                            item->setColumnSize(columnSize);
                            item->setMaxSize(columnSize);
                        }
                    }

                    columnSize += 7;
                    if ((lineSizeCount == 0) && (columnSize > STRINGSIZE))
                    {
                        if (lineCount >= MAX_KEY_ROWS)
                            break;

                        dataNonKeyedColumns[lineCount].append(*item.getLink());
                        lineCount++;
                    }
                    else
                    {
                        if (lineSizeCount + columnSize < STRINGSIZE)
                        {
                            lineSizeCount += columnSize;
                        }
                        else
                        {
                            lineCount++;
                            lineSizeCount = columnSize;
                        }

                        if (lineCount >= MAX_KEY_ROWS)
                            break;

                        dataNonKeyedColumns[lineCount].append(*item.getLink());
                    }
                }

                if (dataKeyedColumns[0].length() > 0)
                    resp.setDFUDataKeyedColumns1(dataKeyedColumns[0]);
                if (dataKeyedColumns[1].length() > 0)
                    resp.setDFUDataKeyedColumns2(dataKeyedColumns[1]);
                if (dataKeyedColumns[2].length() > 0)
                    resp.setDFUDataKeyedColumns3(dataKeyedColumns[2]);
                if (dataKeyedColumns[3].length() > 0)
                    resp.setDFUDataKeyedColumns4(dataKeyedColumns[3]);
                if (dataKeyedColumns[4].length() > 0)
                    resp.setDFUDataKeyedColumns5(dataKeyedColumns[4]);
                if (dataKeyedColumns[5].length() > 0)
                    resp.setDFUDataKeyedColumns6(dataKeyedColumns[5]);
                if (dataKeyedColumns[6].length() > 0)
                    resp.setDFUDataKeyedColumns7(dataKeyedColumns[6]);
                if (dataKeyedColumns[7].length() > 0)
                    resp.setDFUDataKeyedColumns8(dataKeyedColumns[7]);
                if (dataKeyedColumns[8].length() > 0)
                    resp.setDFUDataKeyedColumns9(dataKeyedColumns[8]);
                if (dataKeyedColumns[9].length() > 0)
                    resp.setDFUDataKeyedColumns10(dataKeyedColumns[9]);
                if (version > 1.14)
                {
                    if (dataKeyedColumns[10].length() > 0)
                        resp.setDFUDataKeyedColumns11(dataKeyedColumns[10]);
                    if (dataKeyedColumns[11].length() > 0)
                        resp.setDFUDataKeyedColumns12(dataKeyedColumns[11]);
                    if (dataKeyedColumns[12].length() > 0)
                        resp.setDFUDataKeyedColumns13(dataKeyedColumns[12]);
                    if (dataKeyedColumns[13].length() > 0)
                        resp.setDFUDataKeyedColumns14(dataKeyedColumns[13]);
                    if (dataKeyedColumns[14].length() > 0)
                        resp.setDFUDataKeyedColumns15(dataKeyedColumns[14]);
                    if (dataKeyedColumns[15].length() > 0)
                        resp.setDFUDataKeyedColumns16(dataKeyedColumns[15]);
                    if (dataKeyedColumns[16].length() > 0)
                        resp.setDFUDataKeyedColumns17(dataKeyedColumns[16]);
                    if (dataKeyedColumns[17].length() > 0)
                        resp.setDFUDataKeyedColumns18(dataKeyedColumns[17]);
                    if (dataKeyedColumns[18].length() > 0)
                        resp.setDFUDataKeyedColumns19(dataKeyedColumns[18]);
                    if (dataKeyedColumns[19].length() > 0)
                        resp.setDFUDataKeyedColumns20(dataKeyedColumns[19]);
                }
                if (dataNonKeyedColumns[0].length() > 0)
                    resp.setDFUDataNonKeyedColumns1(dataNonKeyedColumns[0]);
                if (dataNonKeyedColumns[1].length() > 0)
                    resp.setDFUDataNonKeyedColumns2(dataNonKeyedColumns[1]);
                if (dataNonKeyedColumns[2].length() > 0)
                    resp.setDFUDataNonKeyedColumns3(dataNonKeyedColumns[2]);
                if (dataNonKeyedColumns[3].length() > 0)
                    resp.setDFUDataNonKeyedColumns4(dataNonKeyedColumns[3]);
                if (dataNonKeyedColumns[4].length() > 0)
                    resp.setDFUDataNonKeyedColumns5(dataNonKeyedColumns[4]);
                if (dataNonKeyedColumns[5].length() > 0)
                    resp.setDFUDataNonKeyedColumns6(dataNonKeyedColumns[5]);
                if (dataNonKeyedColumns[6].length() > 0)
                    resp.setDFUDataNonKeyedColumns7(dataNonKeyedColumns[6]);
                if (dataNonKeyedColumns[7].length() > 0)
                    resp.setDFUDataNonKeyedColumns8(dataNonKeyedColumns[7]);
                if (dataNonKeyedColumns[8].length() > 0)
                    resp.setDFUDataNonKeyedColumns9(dataNonKeyedColumns[8]);
                if (dataNonKeyedColumns[9].length() > 0)
                    resp.setDFUDataNonKeyedColumns10(dataNonKeyedColumns[9]);
                if (version > 1.14)
                {
                    if (dataNonKeyedColumns[10].length() > 0)
                        resp.setDFUDataNonKeyedColumns11(dataNonKeyedColumns[10]);
                    if (dataNonKeyedColumns[11].length() > 0)
                        resp.setDFUDataNonKeyedColumns12(dataNonKeyedColumns[11]);
                    if (dataNonKeyedColumns[12].length() > 0)
                        resp.setDFUDataNonKeyedColumns13(dataNonKeyedColumns[12]);
                    if (dataNonKeyedColumns[13].length() > 0)
                        resp.setDFUDataNonKeyedColumns14(dataNonKeyedColumns[13]);
                    if (dataNonKeyedColumns[14].length() > 0)
                        resp.setDFUDataNonKeyedColumns15(dataNonKeyedColumns[14]);
                    if (dataNonKeyedColumns[15].length() > 0)
                        resp.setDFUDataNonKeyedColumns16(dataNonKeyedColumns[15]);
                    if (dataNonKeyedColumns[16].length() > 0)
                        resp.setDFUDataNonKeyedColumns17(dataNonKeyedColumns[16]);
                    if (dataNonKeyedColumns[17].length() > 0)
                        resp.setDFUDataNonKeyedColumns18(dataNonKeyedColumns[17]);
                    if (dataNonKeyedColumns[18].length() > 0)
                        resp.setDFUDataNonKeyedColumns19(dataNonKeyedColumns[18]);
                    if (dataNonKeyedColumns[19].length() > 0)
                        resp.setDFUDataNonKeyedColumns20(dataNonKeyedColumns[19]);
                }
                //resp.setColumnCount(columnCount);
                resp.setRowCount(total);
            }

            resp.setLogicalName(logicalNameStr.str());
            resp.setStartIndex(startIndex);
            resp.setEndIndex(endIndex);

            if (version > 1.11)
            {
                if (req.getCluster() && *req.getCluster())
                {
                    resp.setCluster(req.getCluster());
                }
                if (req.getClusterType() && *req.getClusterType())
                {
                    resp.setClusterType(req.getClusterType());
                }
            }
        }

        if (req.getChooseFile())
            resp.setChooseFile(1);
        else
            resp.setChooseFile(0);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onDFUSearchData(IEspContext &context, IEspDFUSearchDataRequest &req, IEspDFUSearchDataResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUSearchData: Permission denied.");

        double version = context.getClientVersion();

        resp.setCluster(req.getCluster());
        resp.setClusterType(req.getClusterType());
        resp.setFile(req.getFile());
        resp.setKey(req.getKey());

        const char* selectedKey = req.getSelectedKey();

        if (strlen(selectedKey) > 0)
        {
            resp.setSelectedKey(req.getSelectedKey());
        }
        else
        {
            resp.setSelectedKey(req.getKey());
        }
        resp.setParentName(req.getParentName());
        resp.setRoxieSelections(req.getRoxieSelections());
        resp.setDisableUppercaseTranslation(req.getDisableUppercaseTranslation());

        const char* openLogicalName = req.getOpenLogicalName();

        if (strlen(openLogicalName) > 0)
        {
            PROGLOG("DFUSearchData: %s", openLogicalName);

            Owned<IEspDFUGetDataColumnsRequest> DataColumnsRequest = createDFUGetDataColumnsRequest();
            Owned<IEspDFUGetDataColumnsResponse> DataColumnsResponse = createDFUGetDataColumnsResponse();

            DataColumnsRequest->setOpenLogicalName(req.getOpenLogicalName());
            DataColumnsRequest->setFilterBy(req.getFilterBy());
            DataColumnsRequest->setShowColumns(req.getShowColumns());
            DataColumnsRequest->setChooseFile(req.getChooseFile());
            DataColumnsRequest->setCluster(req.getCluster());
            DataColumnsRequest->setClusterType(req.getClusterType());

            try
            {
                onDFUGetDataColumns(context, *DataColumnsRequest, *DataColumnsResponse);
            }
            catch(IException* e)
            {
                if (version < 1.08)
                    throw e;

                StringBuffer emsg;
                e->errorMessage(emsg);
                e->Release();
                resp.setMsgToDisplay(emsg);
                return true;
            }

            resp.setOpenLogicalName(req.getOpenLogicalName());
            resp.setLogicalName(DataColumnsResponse->getLogicalName());
            resp.setStartIndex(DataColumnsResponse->getStartIndex());
            resp.setEndIndex(DataColumnsResponse->getEndIndex());
            resp.setDFUDataKeyedColumns1(DataColumnsResponse->getDFUDataKeyedColumns1());
            resp.setDFUDataKeyedColumns2(DataColumnsResponse->getDFUDataKeyedColumns2());
            resp.setDFUDataKeyedColumns3(DataColumnsResponse->getDFUDataKeyedColumns3());
            resp.setDFUDataKeyedColumns4(DataColumnsResponse->getDFUDataKeyedColumns4());
            resp.setDFUDataKeyedColumns5(DataColumnsResponse->getDFUDataKeyedColumns5());
            resp.setDFUDataKeyedColumns6(DataColumnsResponse->getDFUDataKeyedColumns6());
            resp.setDFUDataKeyedColumns7(DataColumnsResponse->getDFUDataKeyedColumns7());
            resp.setDFUDataKeyedColumns8(DataColumnsResponse->getDFUDataKeyedColumns8());
            resp.setDFUDataKeyedColumns9(DataColumnsResponse->getDFUDataKeyedColumns9());
            resp.setDFUDataKeyedColumns10(DataColumnsResponse->getDFUDataKeyedColumns10());
            if (version > 1.14)
            {
                resp.setDFUDataKeyedColumns11(DataColumnsResponse->getDFUDataKeyedColumns11());
                resp.setDFUDataKeyedColumns12(DataColumnsResponse->getDFUDataKeyedColumns12());
                resp.setDFUDataKeyedColumns13(DataColumnsResponse->getDFUDataKeyedColumns13());
                resp.setDFUDataKeyedColumns14(DataColumnsResponse->getDFUDataKeyedColumns14());
                resp.setDFUDataKeyedColumns15(DataColumnsResponse->getDFUDataKeyedColumns15());
                resp.setDFUDataKeyedColumns16(DataColumnsResponse->getDFUDataKeyedColumns16());
                resp.setDFUDataKeyedColumns17(DataColumnsResponse->getDFUDataKeyedColumns17());
                resp.setDFUDataKeyedColumns18(DataColumnsResponse->getDFUDataKeyedColumns18());
                resp.setDFUDataKeyedColumns19(DataColumnsResponse->getDFUDataKeyedColumns19());
                resp.setDFUDataKeyedColumns20(DataColumnsResponse->getDFUDataKeyedColumns20());
            }
            resp.setDFUDataNonKeyedColumns1(DataColumnsResponse->getDFUDataNonKeyedColumns1());
            resp.setDFUDataNonKeyedColumns2(DataColumnsResponse->getDFUDataNonKeyedColumns2());
            resp.setDFUDataNonKeyedColumns3(DataColumnsResponse->getDFUDataNonKeyedColumns3());
            resp.setDFUDataNonKeyedColumns4(DataColumnsResponse->getDFUDataNonKeyedColumns4());
            resp.setDFUDataNonKeyedColumns5(DataColumnsResponse->getDFUDataNonKeyedColumns5());
            resp.setDFUDataNonKeyedColumns6(DataColumnsResponse->getDFUDataNonKeyedColumns6());
            resp.setDFUDataNonKeyedColumns7(DataColumnsResponse->getDFUDataNonKeyedColumns7());
            resp.setDFUDataNonKeyedColumns8(DataColumnsResponse->getDFUDataNonKeyedColumns8());
            resp.setDFUDataNonKeyedColumns9(DataColumnsResponse->getDFUDataNonKeyedColumns9());
            resp.setDFUDataNonKeyedColumns10(DataColumnsResponse->getDFUDataNonKeyedColumns10());
            if (version > 1.14)
            {
                resp.setDFUDataNonKeyedColumns11(DataColumnsResponse->getDFUDataNonKeyedColumns11());
                resp.setDFUDataNonKeyedColumns12(DataColumnsResponse->getDFUDataNonKeyedColumns12());
                resp.setDFUDataNonKeyedColumns13(DataColumnsResponse->getDFUDataNonKeyedColumns13());
                resp.setDFUDataNonKeyedColumns14(DataColumnsResponse->getDFUDataNonKeyedColumns14());
                resp.setDFUDataNonKeyedColumns15(DataColumnsResponse->getDFUDataNonKeyedColumns15());
                resp.setDFUDataNonKeyedColumns16(DataColumnsResponse->getDFUDataNonKeyedColumns16());
                resp.setDFUDataNonKeyedColumns17(DataColumnsResponse->getDFUDataNonKeyedColumns17());
                resp.setDFUDataNonKeyedColumns18(DataColumnsResponse->getDFUDataNonKeyedColumns18());
                resp.setDFUDataNonKeyedColumns19(DataColumnsResponse->getDFUDataNonKeyedColumns19());
                resp.setDFUDataNonKeyedColumns20(DataColumnsResponse->getDFUDataNonKeyedColumns20());
            }
            resp.setRowCount(DataColumnsResponse->getRowCount());
            resp.setShowColumns(DataColumnsResponse->getShowColumns());
            resp.setChooseFile(DataColumnsResponse->getChooseFile());
        }

        const char* logicalName = req.getLogicalName();

        if (strlen(logicalName) == 0 && strlen(openLogicalName) > 0)
        {
            logicalName = openLogicalName;
        }

        if (strlen(logicalName) > 0)
        {
            Owned<IEspDFUBrowseDataRequest> browseDataRequest = createDFUBrowseDataRequest();
            Owned<IEspDFUBrowseDataResponse> browseDataResponse = createDFUBrowseDataResponse();

            browseDataRequest->setLogicalName(logicalName);
            const char* parentName = req.getParentName();
            if (parentName && *parentName)
                browseDataRequest->setParentName(parentName);

            browseDataRequest->setFilterBy(req.getFilterBy());
            browseDataRequest->setShowColumns(req.getShowColumns());
            browseDataRequest->setStartForGoback(req.getStartForGoback());
            browseDataRequest->setCountForGoback(req.getCountForGoback());
            browseDataRequest->setChooseFile(req.getChooseFile());
            browseDataRequest->setStart(req.getStart());
            browseDataRequest->setCount(req.getCount());
            browseDataRequest->setSchemaOnly(req.getSchemaOnly());
            browseDataRequest->setCluster(req.getCluster());
            browseDataRequest->setClusterType(req.getClusterType());
            browseDataRequest->setDisableUppercaseTranslation(req.getDisableUppercaseTranslation());

            onDFUBrowseData(context, *browseDataRequest, *browseDataResponse);

            resp.setName(browseDataResponse->getName());
            resp.setLogicalName(browseDataResponse->getLogicalName());
            resp.setFilterBy(browseDataResponse->getFilterBy());
            resp.setFilterForGoBack(browseDataResponse->getFilterForGoBack());
            resp.setColumnsHidden(browseDataResponse->getColumnsHidden());
            resp.setColumnsHidden(browseDataResponse->getColumnsHidden());
            resp.setColumnCount(browseDataResponse->getColumnCount());
            resp.setStartForGoback(browseDataResponse->getStartForGoback());
            resp.setCountForGoback(browseDataResponse->getCountForGoback());
            resp.setChooseFile(browseDataResponse->getChooseFile());
            resp.setStart(browseDataResponse->getStart());
            resp.setCount(browseDataResponse->getCount());
            resp.setPageSize(browseDataResponse->getPageSize());
            resp.setTotal(browseDataResponse->getTotal());
            resp.setResult(browseDataResponse->getResult());
            resp.setMsgToDisplay(browseDataResponse->getMsgToDisplay());
            resp.setSchemaOnly(browseDataResponse->getSchemaOnly());
            resp.setAutoUppercaseTranslation(!m_disableUppercaseTranslation);

        }
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

static const char * const columnTypes[] = { "Boolean", "Integer", "Unsigned Integer", "Real", "String",
    "Data", "Unicode", "Unknown", "BeginIfBlock", "EndIfBlock", "BeginRecord", "EndRecord", "Set", "Dataset", NULL };

bool CWsDfuEx::onDFUGetFileMetaData(IEspContext &context, IEspDFUGetFileMetaDataRequest & req, IEspDFUGetFileMetaDataResponse & resp)
{
    class CDFUFileMetaDataReader
    {
        unsigned totalColumnCount;
        unsigned keyedColumnCount;
        StringBuffer XmlSchema, XmlXPathSchema;
        IArrayOf<IEspDFUDataColumn> dataColumns;
        const IResultSetMetaData& metaRoot;
        bool readRootLevelColumns;
        IEspContext &context;

        bool readColumnLabel(const IResultSetMetaData* meta, unsigned columnID, IEspDFUDataColumn* out)
        {
            SCMStringBuffer columnLabel;
            bool isNaturalColumn = true;
            if (meta->hasSetTranslation(columnID))
                meta->getNaturalColumnLabel(columnLabel, columnID);
            if (columnLabel.length() < 1)
            {
                meta->getColumnLabel(columnLabel, columnID);
                isNaturalColumn = false;
            }
            out->setColumnLabel(columnLabel.str());
            out->setIsNaturalColumn(isNaturalColumn);
            return isNaturalColumn;
        }
        void readColumn(const IResultSetMetaData* meta, unsigned& columnID, const bool isKeyed,
            IArrayOf<IEspDFUDataColumn>& dataColumns)
        {
            double version = context.getClientVersion();
            Owned<IEspDFUDataColumn> dataItem = createDFUDataColumn();
            dataItem->setColumnID(columnID+1);
            dataItem->setIsKeyedColumn(isKeyed);

            SCMStringBuffer s;
            dataItem->setColumnEclType(meta->getColumnEclType(s, columnID).str());

            DisplayType columnType = meta->getColumnDisplayType(columnID);
            if ((columnType == TypeUnicode) || columnType == TypeString)
                dataItem->setColumnRawSize(meta->getColumnRawSize(columnID));

            if (readColumnLabel(meta, columnID, dataItem))
                dataItem->setColumnType("Others");
            else if (columnType == TypeBeginRecord)
                dataItem->setColumnType("Record");
            else
                dataItem->setColumnType(columnTypes[columnType]);

            if ((version >= 1.31) && ((columnType == TypeSet) || (columnType == TypeDataset) || (columnType == TypeBeginRecord)))
                checkAndReadNestedColumn(meta, columnID, columnType, dataItem);

            dataColumns.append(*dataItem.getClear());
        }
        void readColumns(const IResultSetMetaData* meta, IArrayOf<IEspDFUDataColumn>& dataColumnArray)
        {
            if (!meta)
                return;

            if (readRootLevelColumns)
            {
                readRootLevelColumns = false;
                totalColumnCount = (unsigned)meta->getColumnCount();
                keyedColumnCount = meta->getNumKeyedColumns();
                unsigned i = 0;
                for (; i < keyedColumnCount; i++)
                    readColumn(meta, i, true, dataColumnArray);
                for (i = keyedColumnCount; i < totalColumnCount; i++)
                    readColumn(meta, i, false, dataColumnArray);
            }
            else
            {
                unsigned columnCount = (unsigned)meta->getColumnCount();
                for (unsigned i = 0; i < columnCount; i++)
                    readColumn(meta, i, false, dataColumnArray);
            }
        }
        void checkAndReadNestedColumn(const IResultSetMetaData* meta, unsigned& columnID,
            DisplayType columnType, IEspDFUDataColumn* dataItem)
        {
            IArrayOf<IEspDFUDataColumn> curDataColumnArray;
            if (columnType == TypeBeginRecord)
            {
                columnID++;
                do
                {
                    readColumn(meta, columnID, false, curDataColumnArray);
                } while (meta->getColumnDisplayType(++columnID) != TypeEndRecord);
            }
            else
                readColumns(meta->getChildMeta(columnID), curDataColumnArray);
            dataItem->setDataColumns(curDataColumnArray);
        }

    public:
        CDFUFileMetaDataReader(IEspContext& _context, const IResultSetMetaData& _meta)
            : metaRoot(_meta), readRootLevelColumns(true), context(_context)
        {
            readColumns(&metaRoot, dataColumns);
        };
        inline unsigned getTotalColumnCount() { return totalColumnCount; }
        inline unsigned getKeyedColumnCount() { return keyedColumnCount; }
        inline IArrayOf<IEspDFUDataColumn>& getDataColumns() { return dataColumns; }
        inline StringBuffer& getXmlSchema(StringBuffer& s, const bool addHeader)
        {
            StringBufferAdaptor schema(s);
            metaRoot.getXmlSchema(schema, addHeader);
            return s;
        }
        inline StringBuffer& getXmlXPathSchema(StringBuffer& s, const bool addHeader)
        {
            StringBufferAdaptor XPathSchema(s);
            metaRoot.getXmlXPathSchema(XPathSchema, addHeader);
            return s;
        }
    };

    try
    {
        StringBuffer fileNameStr(req.getLogicalFileName());
        const char* fileName = fileNameStr.trim().str();
        if (!fileName || !*fileName)
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "CWsDfuEx::onDFUGetFileMetaData: LogicalFileName not set");

        const char* cluster = NULL;
        StringBuffer clusterNameStr(req.getClusterName());
        if (clusterNameStr.trim().length() > 0)
            cluster = clusterNameStr.str();

        PROGLOG("DFUGetFileMetaData: %s", fileName);
        Owned<IResultSetFactory> resultSetFactory = getSecResultSetFactory(context.querySecManager(), context.queryUser(), context.queryUserId(), context.queryPassword());
        Owned<INewResultSet> result = resultSetFactory->createNewFileResultSet(fileName, cluster);
        if (!result)
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "CWsDfuEx::onDFUGetFileMetaData: Failed to access FileResultSet for %s.", fileName);

        CDFUFileMetaDataReader dataReader(context, result->getMetaData());
        resp.setTotalColumnCount(dataReader.getTotalColumnCount());
        resp.setKeyedColumnCount(dataReader.getKeyedColumnCount());
        resp.setDataColumns(dataReader.getDataColumns());

        StringBuffer s, s1;
        if (req.getIncludeXmlSchema())
            resp.setXmlSchema(dataReader.getXmlSchema(s, req.getAddHeaderInXmlSchema()).str());
        if (req.getIncludeXmlXPathSchema())
            resp.setXmlXPathSchema(dataReader.getXmlXPathSchema(s1, req.getAddHeaderInXmlXPathSchema()).str());

        resp.setTotalResultRows(result->getNumRows());
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onDFUBrowseData(IEspContext &context, IEspDFUBrowseDataRequest &req, IEspDFUBrowseDataResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Read, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::DFUBrowseData: Permission denied.");

        const char* logicalName0 = req.getLogicalName();
        const char* parentName = req.getParentName();
        if (!logicalName0 || !*logicalName0)
             throw MakeStringException(ECLWATCH_INVALID_INPUT,"No LogicalName defined.");

        StringBuffer logicalNameStr;
        if (logicalName0 && *logicalName0)
        {
            logicalNameStr.append(logicalName0);
            logicalNameStr.trim();
            if (logicalNameStr.length() < 1)
                 throw MakeStringException(ECLWATCH_INVALID_INPUT,"No LogicalName defined.");
        }
        PROGLOG("DFUBrowseData: %s", logicalNameStr.str());

        __int64 start = req.getStart() > 0 ? req.getStart() : 0;
        __int64 count=req.getCount() ? req.getCount() : 20, requested=count;
        if (count > MAX_VIEWKEYFILE_ROWS)
            throw MakeStringException(ECLWATCH_TOO_MANY_DATA_ROWS,"Browser Cannot display more than %d data rows.", MAX_VIEWKEYFILE_ROWS);

        bool bSchemaOnly=req.getSchemaOnly() ? req.getSchemaOnly() : false;
        bool bDisableUppercaseTranslation = req.getDisableUppercaseTranslation() ? req.getDisableUppercaseTranslation() : false;

        const char* filterBy = req.getFilterBy();
        const char* showColumns = req.getShowColumns();

        __int64 read=0;
        __int64 total = 0;
        StringBuffer msg;
        StringArray columnLabels, columnLabelsType;
        IArrayOf<IEspDFUData> DataList;

        int iRet = GetIndexData(context, bSchemaOnly, logicalNameStr.str(), parentName, filterBy, start, count, read, total, msg, columnLabels, columnLabelsType, DataList, bDisableUppercaseTranslation);
        if (iRet > 0)
            resp.setMsgToDisplay("This search has timed out due to the restrictive filter. There may be more records.");
        //GetIndexData(context, bSchemaOnly, logicalNameStr.str(), "roxie::thor_data400::key::bankruptcyv2::20090721::search::tmsid", filterBy, start, count, read, total, msg, columnLabels, columnLabelsType, DataList);
        resp.setResult(DataList.item(0).getData());

        unsigned int max_name_length = 3; //max length for name length
        unsigned int max_value_length = 4; //max length for value length:
        StringBuffer filterByStr, filterByStr0;
        filterByStr0.appendf("%d%d", max_name_length, max_value_length);

        unsigned columnCount = columnLabels.length();
        IArrayOf<IEspDFUDataColumn> dataColumns;

        double version = context.getClientVersion();
        if (version > 1.04 && columnCount > 0)
        {
            //Find out which columns need to be displayed
            int lenShowCols = 0, showCols[1024];
            const char* showColumns = req.getShowColumns();
            char *pShowColumns =  (char*) showColumns;
            while (pShowColumns && *pShowColumns)
            {
                StringBuffer buf;
                while (pShowColumns && isdigit(pShowColumns[0]))
                {
                    buf.append(pShowColumns[0]);
                    pShowColumns++;
                }
                if (buf.length() > 0)
                {
                    showCols[lenShowCols] = atoi(buf.str());
                    lenShowCols++;
                }

                if (!pShowColumns || !*pShowColumns)
                    break;
                pShowColumns++;
            }

            for(unsigned col = 0; col < columnCount; col++)
            {
                const char* label = columnLabels.item(col);
                const char* type = columnLabelsType.item(col);
                if (!label || !*label || !type || !*type)
                    continue;

                Owned<IEspDFUDataColumn> item = createDFUDataColumn("","");

                item->setColumnLabel(label);
                item->setColumnType(type);
                item->setColumnSize(0); //not show this column

                if (!showColumns || !*showColumns)
                {
                    item->setColumnSize(1); //Show this column
                }
                else
                {
                    for(int col1 = 0; col1 < lenShowCols; col1++)
                    {
                        if (col == showCols[col1])
                        {
                            item->setColumnSize(1); //Show this column
                            break;
                        }
                    }
                }
                dataColumns.append(*item.getLink());
            }

            //Re-build filters
            if (filterBy && *filterBy)
            {
                StringArray filterByNames, filterByValues;
                parseTwoStringArrays(filterBy, filterByNames, filterByValues);
                if (filterByNames.length() > 0)
                {
                    for (unsigned ii = 0; ii < filterByNames.length(); ii++)
                    {
                        const char* columnName = filterByNames.item(ii);
                        const char* columnValue = filterByValues.item(ii);
                        if (columnName && *columnName && columnValue && *columnValue)
                        {
                            filterByStr.appendf("%s[%s]", columnName, columnValue);
                            filterByStr0.appendf("%03d%04d%s%s", (int) strlen(columnName), (int) strlen(columnValue), columnName, columnValue);
                        }
                    }
                }
            }

            if (req.getStartForGoback())
                resp.setStartForGoback(req.getStartForGoback());
            if (req.getCountForGoback())
                resp.setCountForGoback(req.getCountForGoback());
        }

        //resp.setFilterBy(filterByStr.str());
        if (filterByStr.length() > 0)
        {
            const char* oldStr = "&";
            const char* newStr = "&amp;";
            filterByStr.replaceString(oldStr, newStr);
            resp.setFilterBy(filterByStr.str());
        }
        if (version > 1.04)
        {
            //resp.setFilterForGoBack(filterByStr0.str());
            if (filterByStr0.length() > 0)
            {
                const char* oldStr = "&";
                const char* newStr = "&amp;";
                filterByStr0.replaceString(oldStr, newStr);
                resp.setFilterForGoBack(filterByStr0.str());
            }
            resp.setColumnCount(columnCount);
            if (dataColumns.length() > 0)
                resp.setColumnsHidden(dataColumns);
        }
        if (version > 1.10)
        {
            resp.setSchemaOnly(bSchemaOnly);
        }
        //resp.setName(name.str());
        resp.setLogicalName(logicalNameStr.str());
        resp.setStart(start);
        //if (requested > read)
        //  requested = read;
        resp.setPageSize(requested);
        if (count > read)
        {
            count = read;
        }
        resp.setCount(count);
        if (total != UNKNOWN_NUM_ROWS)
            resp.setTotal(total);
        else
            resp.setTotal(-1);
        if (req.getChooseFile())
            resp.setChooseFile(1);
        else
            resp.setChooseFile(0);

        if (version > 1.11)
        {
            if (req.getCluster() && *req.getCluster())
            {
                resp.setCluster(req.getCluster());
            }
            if (req.getClusterType() && *req.getClusterType())
            {
                resp.setClusterType(req.getClusterType());
            }
        }

        if ((version > 1.12) && parentName && *parentName)
        {
            resp.setParentName(parentName);
        }
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void storeHistoryTreeToArray(IPropertyTree *history, IArrayOf<IEspHistory>& arrHistory)
{
    Owned<IPropertyTreeIterator> historyIter = history->getElements("*");
    ForEach(*historyIter)
    {
        Owned<IEspHistory> historyRecord = createHistory();

        IPropertyTree & item = historyIter->query();
        historyRecord->setIP(item.queryProp("@ip"));
        historyRecord->setName(item.queryProp("@name"));
        historyRecord->setOperation(item.queryProp("@operation"));
        historyRecord->setOwner(item.queryProp("@owner"));
        historyRecord->setPath(item.queryProp("@path"));
        historyRecord->setTimestamp(item.queryProp("@timestamp"));
        historyRecord->setWorkunit(item.queryProp("@workunit"));

        arrHistory.append(*historyRecord.getClear());
    }
}

bool CWsDfuEx::onListHistory(IEspContext &context, IEspListHistoryRequest &req, IEspListHistoryResponse &resp)
{
    try
    {
        if (!req.getName() || !*req.getName())
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Name required");
        PROGLOG("onListHistory: %s", req.getName());

        MemoryBuffer xmlmap;
        IArrayOf<IEspHistory> arrHistory;
        Owned<IDistributedFile> file = lookupLogicalName(context, req.getName(), AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
        if (file)
        {
            IPropertyTree *history = file->queryHistory();
            if (history)
            {
                storeHistoryTreeToArray(history, arrHistory);
                if (context.getClientVersion() < 1.36)
                    history->serialize(xmlmap);
            }

            if (arrHistory.ordinality())
                resp.setHistory(arrHistory);
        }
        else
            throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"CWsDfuEx::onListHistory: Could not find file '%s'.", req.getName());

        if (xmlmap.length())
            resp.setXmlmap(xmlmap);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onEraseHistory(IEspContext &context, IEspEraseHistoryRequest &req, IEspEraseHistoryResponse &resp)
{
    try
    {
        context.ensureFeatureAccess(FEATURE_URL, SecAccess_Full, ECLWATCH_DFU_ACCESS_DENIED, "WsDfu::EraseHistory: Permission denied.");

        if (!req.getName() || !*req.getName())
            throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Name required");
        PROGLOG("onEraseHistory: %s", req.getName());

        MemoryBuffer xmlmap;
        IArrayOf<IEspHistory> arrHistory;
        Owned<IDistributedFile> file = lookupLogicalName(context, req.getName(), AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser);
        if (file)
        {
            IPropertyTree *history = file->queryHistory();
            if (history)
            {
                storeHistoryTreeToArray(history, arrHistory);
                if (context.getClientVersion() < 1.36)
                    history->serialize(xmlmap);

                file->resetHistory();
            }
            if (arrHistory.ordinality())
                resp.setHistory(arrHistory);
        }
        else
            throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"CWsDfuEx::onEraseHistory: Could not find file '%s'.", req.getName());

        if (xmlmap.length())
            resp.setXmlmap(xmlmap);
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

//////////////////////HPCC Browser//////////////////////////

static const char* SCHEMANAME = "myschema";

//void CWsDfuEx::setRootFilter(INewResultSet* result, const char* filterBy, IFilteredResultSet* filter)
void CWsDfuEx::setRootFilter(INewResultSet* result, const char* filterBy, IResultSetFilter* filter, bool disableUppercaseTranslation)
{
    if (!filterBy || !*filterBy || !result)
        return;

    //Owned<IFilteredResultSet> filter = result->createFiltered();

    filter->clearFilters();

    const IResultSetMetaData &meta = result->getMetaData();
    unsigned columnCount = meta.getColumnCount();
    if (columnCount < 1)
        return;

    StringArray filterByNames, filterByValues;
    parseTwoStringArrays(filterBy, filterByNames, filterByValues);
    if (filterByNames.length() < 1)
        return;

    for (unsigned ii = 0; ii < filterByNames.length(); ii++)
    {
        const char* columnName = filterByNames.item(ii);
        const char* columnValue0 = filterByValues.item(ii);
        if (!columnName || !*columnName || !columnValue0 || !*columnValue0)
            continue;

        StringBuffer buf(columnValue0);
        if (!disableUppercaseTranslation)
            buf.toUpperCase();

        const char* columnValue = buf.str();

        for(unsigned col = 0; col < columnCount; col++)
        {
            bool hasSetTranslation = false;

            SCMStringBuffer scmbuf;
            if (meta.hasSetTranslation(col))
            {
                hasSetTranslation = true;
                meta.getNaturalColumnLabel(scmbuf, col);
            }

            if (scmbuf.length() < 1)
            {
                meta.getColumnLabel(scmbuf, col);
            }

            if (!stricmp(scmbuf.str(), columnName))
            {
                //filter->addFilter(col, columnValue);
                //filterByStr.appendf("%s[%s]", columnName, columnValue);
                //filterByStr0.appendf("%03d%04d%s%s", strlen(columnName), strlen(columnValue), columnName, columnValue);
                if (hasSetTranslation)
                    filter->addNaturalFilter(col, strlen(columnValue), columnValue);
                else
                    filter->addFilter(col, strlen(columnValue), columnValue);

                break;
            }
        }
    }

    //result.setown(filter->create());
    return;
}

void CWsDfuEx::getMappingColumns(IRelatedBrowseFile * file, bool isPrimary, UnsignedArray& cols)
{
    const char* logicalName = file->queryDefinition()->queryDistributedFile()->queryLogicalName();
    const char* primaryName = file->queryParentRelation()->queryFileRelationship()->queryPrimaryFilename();
    if (!logicalName || !primaryName || strcmp(logicalName, primaryName))
        return;

    IViewRelation* parentRelation = file->queryParentRelation();
    for (unsigned i=0; i < parentRelation->numMappingFields(); i++)
    {
        //find out the column numbers to remove
        unsigned col = parentRelation->queryMappingField(i, isPrimary);
        cols.append(col);
    }

#ifdef TESTDATASET
    cols.kill();
    cols.append(2);
    cols.append(3);
#endif
    return;
}

void CWsDfuEx::readColumnsForDisplay(StringBuffer& schemaText, StringArray& columnsDisplay, StringArray& columnsDisplayType)
{
    if (schemaText.length() < 1)
        return;

    Owned<IPropertyTree> schema = createPTreeFromXMLString(schemaText.str());
    if (!schema)
        return;

    //Find out labels from the second schema used for column mapping
    columnsDisplay.kill();
    Owned<IPropertyTreeIterator> rows4 = schema->getElements("xs:element[@name=\"Dataset\"]/xs:complexType/xs:sequence/xs:element[@name=\"Row\"]/xs:complexType/xs:sequence/*");
    ForEach(*rows4)
    {
        IPropertyTree &e = rows4->query();
        const char* name = e.queryProp("@name");
        const char* type = e.queryProp("@type");
        bool hasChildren = e.hasChildren();
        if (!name || !*name)
            continue;

        columnsDisplay.append(name); //Display this column
        if (type && *type)
            columnsDisplayType.append(type);
        else if (hasChildren)
            columnsDisplayType.append("Object");
        else
            columnsDisplayType.append("Unknown");
    }

    return;
}

void CWsDfuEx::mergeSchema(IRelatedBrowseFile * file, StringBuffer& schemaText, const char * schemaText2,
                                    StringArray& columnsDisplay, StringArray& columnsDisplayType, StringArray& columnsHide)
{
    if (schemaText.length() < 1)
        return;
    if (isEmptyString(schemaText2))
        return;

    Owned<IPropertyTree> schema = createPTreeFromXMLString(schemaText.str());
    Owned<IPropertyTree> schema2 = createPTreeFromXMLString(schemaText2);
    if (!schema || !schema2)
        return;

    //Process simpleType part
    Owned<IPropertyTreeIterator> rows1 = schema->getElements("xs:simpleType");
    Owned<IPropertyTreeIterator> rows2 = schema2->getElements("xs:simpleType");
    if (!rows1 || !rows2)
        return;

    ForEach(*rows2)
    {
        IPropertyTree &e = rows2->query();
        const char* name = e.queryProp("@name");
        if (!name || !*name)
            continue;

        bool bFound = false;
        ForEach(*rows1)
        {
            IPropertyTree &e1 = rows1->query();
            const char* name1 = e1.queryProp("@name");
            if (!name1 || !*name1 || stricmp(name1, name))
                continue;

            bFound = true;
            break;
        }

        if (!bFound)
            schema->addPropTree(e.queryName(), LINK(&e));
    }

    IPropertyTree*  rows = schema->queryBranch("xs:element[@name=\"Dataset\"]/xs:complexType/xs:sequence/xs:element[@name=\"Row\"]/xs:complexType/xs:sequence");
    if (!rows)
        return;

    //Find out labels used for column mapping
    columnsDisplay.kill();
    columnsDisplayType.kill();
    columnsHide.kill();
    Owned<IPropertyTreeIterator> rows4 = schema->getElements("xs:element[@name=\"Dataset\"]/xs:complexType/xs:sequence/xs:element[@name=\"Row\"]/xs:complexType/xs:sequence/*");
    ForEach(*rows4)
    {
        IPropertyTree &e = rows4->query();
        const char* name = e.queryProp("@name");
        const char* type = e.queryProp("@type");
        bool hasChildren = e.hasChildren();
        if (!name || !*name)
            continue;

        columnsDisplay.append(name); //Display this column
        if (type && *type)
            columnsDisplayType.append(type);
        else if (hasChildren)
            columnsDisplayType.append("Object");
        else
            columnsDisplayType.append("Unknown");
    }

    UnsignedArray cols;
    bool isPrimary = true;
    getMappingColumns(file, isPrimary, cols);

    //Process complexType part for labels
    unsigned col0 = 0;
    Owned<IPropertyTreeIterator> rows3 = schema2->getElements("xs:element[@name=\"Dataset\"]/xs:complexType/xs:sequence/xs:element[@name=\"Row\"]/xs:complexType/xs:sequence/*");
    ForEach(*rows3)
    {
        IPropertyTree &e = rows3->query();
        const char* name = e.queryProp("@name");
        const char* type = e.queryProp("@type");
        bool hasChildren = e.hasChildren();
        if (!name || !*name)
            continue;

        bool bAdd = true;
        bool bRename = false;
        if (cols.ordinality() != 0)
        {
            ForEachItemIn(i1,cols)
            {
                unsigned col = cols.item(i1);
                if (col == col0)
                {
                    bAdd = false;
                    break;
                }
            }
        }

#define RENAMESAMECOLUMN
#ifdef RENAMESAMECOLUMN
        if (columnsDisplay.length() > 0)
        {
            for (unsigned i = 0; i < columnsDisplay.length(); i++)
            {
                const char* label = columnsDisplay.item(i);
                if (!label || strcmp(label, name))
                    continue;

                bRename = true;
                break;
            }
        }
#endif

        if (!bAdd)
        {
            columnsHide.append(name); //hide this column
        }
        else
        {
            if (type && *type)
                columnsDisplayType.append(type);
            else if (hasChildren)
                columnsDisplayType.append("Object");
            else
                columnsDisplayType.append("Unknown");
#ifdef RENAMESAMECOLUMN
            if (bRename)
            {
                StringBuffer newName(name);
                newName.append("-2");
                columnsDisplay.append(newName.str()); //Display this column
                e.setProp("@name", newName.str());
                rows->addPropTree(e.queryName(), LINK(&e));
            }
            else
            {
#endif
            columnsDisplay.append(name); //Display this column
            rows->addPropTree(e.queryName(), LINK(&e));
#ifdef RENAMESAMECOLUMN
            }
#endif
        }

        col0++;
    }

    //Convert schema tree to schame now
    schemaText.clear();
    toXML(schema, schemaText);
    return;
}

void CWsDfuEx::mergeDataRow(StringBuffer& newRow, int depth, IPropertyTreeIterator* it, StringArray& columnsHide, StringArray& columnsUsed)
{
    if (!it)
        return;

    it->first();
    while(it->isValid())
    {
        IPropertyTree* e = &it->query();
        if (e)
        {
            const char* label = e->queryName();
            if (label && *label)
            {
#ifdef RENAMESAMECOLUMN
                if (depth < 1)
                    columnsUsed.append(label);
#endif

                bool bHide = false;
                if (columnsHide.length() > 0)
                {
                    for (unsigned i = 0 ; i < columnsHide.length(); i++)
                    {
                        const char* key = columnsHide.item(i);
                        if (!key || strcmp(key, label))
                            continue;

                        bHide = true;
                        break;
                    }
                }

#ifdef RENAMESAMECOLUMN
                if (!bHide && depth > 0 && columnsUsed.length() > 0)
                {
                    for (unsigned i = 0 ; i < columnsUsed.length(); i++)
                    {
                        const char* key = columnsUsed.item(i);
                        if (!key || strcmp(key, label))
                            continue;

                        StringBuffer newName(label);
                        newName.append("-2");
                        e->renameProp("/", newName.str());
                        break;
                    }
                }
#endif
                if (!bHide)
                {
                    StringBuffer dataRow;
                    toXML(e, dataRow);
                    newRow.append(dataRow);
                }
            }
        }
        it->next();
    }

    return;
}

void CWsDfuEx::mergeDataRow(StringBuffer& newRow, const char * dataRow1, const char * dataRow2, StringArray& columnsHide)
{
    if (isEmptyString(dataRow1))
        return;
    if (isEmptyString(dataRow2))
        return;

    Owned<IPropertyTree> data1 = createPTreeFromXMLString(dataRow1);
    Owned<IPropertyTree> data2 = createPTreeFromXMLString(dataRow2);
    if (!data1 || !data2)
        return;

    newRow.clear();
    newRow.append("<Row>");

    StringArray columnLabels;
    Owned<IPropertyTreeIterator> it = data1->getElements("*");
    if (it)
    {
        StringArray columnLabels0;
        mergeDataRow(newRow, 0, it, columnLabels0, columnLabels);
    }

    Owned<IPropertyTreeIterator> it2 = data2->getElements("*");
    if (it2)
    {
        mergeDataRow(newRow, 1, it2, columnsHide, columnLabels);
    }

    newRow.append("</Row>");

    return;
}

void CWsDfuEx::browseRelatedFileSchema(IRelatedBrowseFile * file, const char* parentName, unsigned depth, StringBuffer& schemaText,
                                                    StringArray& columnsDisplay, StringArray& columnsDisplayType, StringArray& columnsHide)
{
    //if (file in set of files to display or iterate)
    IResultSetCursor * cursor = file->queryCursor();
    if (cursor && cursor->first())
    {
        if (depth < 1)
        {
            const IResultSetMetaData & meta = cursor->queryResultSet()->getMetaData();
            StringBufferAdaptor adaptor(schemaText);
            meta.getXmlSchema(adaptor, false);

#ifdef TESTDATASET
schemaText.clear();
schemaText.append("<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" elementFormDefault=\"qualified\"");
schemaText.append(" attributeFormDefault=\"unqualified\">");
schemaText.append("<xs:element name=\"Dataset\"><xs:complexType><xs:sequence minOccurs=\"0\" maxOccurs=\"unbounded\">");
schemaText.append("<xs:element name=\"Row\"><xs:complexType><xs:sequence>");
schemaText.append("<xs:element name=\"state\" type=\"string2\"/>");
schemaText.append("<xs:element name=\"rtype\" type=\"string2\"/>");
schemaText.append("<xs:element name=\"id\" type=\"string20\"/>");
schemaText.append("<xs:element name=\"seq\" type=\"xs:nonNegativeInteger\"/>");
schemaText.append("<xs:element name=\"num\" type=\"xs:nonNegativeInteger\"/>");
schemaText.append("<xs:element name=\"date\" type=\"string8\"/>");
schemaText.append("<xs:element name=\"imglength\" type=\"xs:nonNegativeInteger\"/>");
schemaText.append("<xs:element name=\"__filepos\" type=\"xs:nonNegativeInteger\"/>");
schemaText.append("</xs:sequence></xs:complexType></xs:element>");
schemaText.append("</xs:sequence></xs:complexType></xs:element>");
schemaText.append("<xs:simpleType name=\"string2\"><xs:restriction base=\"xs:string\"><xs:maxLength value=\"2\"/>");
schemaText.append("</xs:restriction></xs:simpleType>");
schemaText.append("<xs:simpleType name=\"string20\"><xs:restriction base=\"xs:string\"><xs:maxLength value=\"20\"/>");
schemaText.append("</xs:restriction></xs:simpleType>");
schemaText.append("<xs:simpleType name=\"string8\"><xs:restriction base=\"xs:string\"><xs:maxLength value=\"8\"/>");
schemaText.append("</xs:restriction></xs:simpleType>");
schemaText.append("</xs:schema>");
#endif
            readColumnsForDisplay(schemaText, columnsDisplay, columnsDisplayType);
        }
        else
        {
            StringBuffer schemaText0;
            const IResultSetMetaData & meta = cursor->queryResultSet()->getMetaData();
            StringBufferAdaptor adaptor(schemaText0);
            meta.getXmlSchema(adaptor, false);
#ifdef TESTDATASET
schemaText0.clear();
schemaText0.append("<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" elementFormDefault=\"qualified\"");
schemaText0.append(" attributeFormDefault=\"unqualified\">");
schemaText0.append("<xs:element name=\"Dataset\"><xs:complexType><xs:sequence minOccurs=\"0\" maxOccurs=\"unbounded\">");
schemaText0.append("<xs:element name=\"Row\"><xs:complexType><xs:sequence>");
schemaText0.append("<xs:element name=\"date_first_reported\" type=\"string12\"/>");
schemaText0.append("<xs:element name=\"msa\" type=\"string8\"/>");
schemaText0.append("<xs:element name=\"sid\" type=\"string20\"/>");
schemaText0.append("<xs:element name=\"seq\" type=\"xs:nonNegativeInteger\"/>"); //not add
schemaText0.append("</xs:sequence></xs:complexType></xs:element>");
schemaText0.append("</xs:sequence></xs:complexType></xs:element>");
schemaText0.append("<xs:simpleType name=\"string12\"><xs:restriction base=\"xs:string\"><xs:maxLength value=\"12\"/>");
schemaText0.append("</xs:restriction></xs:simpleType>");
schemaText0.append("</xs:schema>");
#endif
            mergeSchema(file, schemaText, schemaText0, columnsDisplay, columnsDisplayType, columnsHide);
        }

        if (parentName && *parentName)
        {
            for (unsigned i = 0;;i++)
            {
                IRelatedBrowseFile * next = file->queryChild(i);
                if (!next)
                    break;

                IViewRelatedFile * viewRelatedFile = next->queryDefinition();
                if (!viewRelatedFile)
                    continue;

                IDistributedFile * file = viewRelatedFile->queryDistributedFile();
                if (!file)
                    continue;

                const char* logicName0 = file->queryLogicalName();
                if (logicName0 && !strcmp(logicName0, parentName))
                    browseRelatedFileSchema(next, NULL, depth+1, schemaText, columnsDisplay, columnsDisplayType, columnsHide);
            }
        }
    }

    return;
}

int CWsDfuEx::browseRelatedFileDataSet(double version, IRelatedBrowseFile * file, const char* parentName, unsigned depth, __int64 start, __int64& count, __int64& read,
                                        StringArray& columnsHide, StringArray& dataSetOutput)
{
    int iRet = 0;
    int rows = 0;

    try
    {
        //if (file in set of files to display or iterate)
        IResultSetCursor * cursor = file->queryCursor();
        if (cursor->first())
        {
            for(bool ok=cursor->absolute(start);ok;ok=cursor->next())
            {

                StringBuffer text;
                StringBufferAdaptor adaptor2(text);
                cursor->getXmlRow(adaptor2);

                StringArray dataSetOutput0;
                if (parentName && *parentName)
                {
                    for (unsigned i = 0;;i++)
                    {
                        IRelatedBrowseFile * next = file->queryChild(i);
                        if (!next)
                            break;

                        IViewRelatedFile * viewRelatedFile = next->queryDefinition();
                        if (!viewRelatedFile)
                            continue;

                        IDistributedFile * file = viewRelatedFile->queryDistributedFile();
                        if (!file)
                            continue;

                        const char* logicName0 = file->queryLogicalName();
                        if (logicName0 && !strcmp(logicName0, parentName))
                            iRet = browseRelatedFileDataSet(version, next, NULL, depth+1, 0, count, read, columnsHide, dataSetOutput0);
                    }
                }

                if (dataSetOutput0.length() < 1)
                {
                    dataSetOutput.append(text);
                }
                else
                {
                    for (unsigned ii = 0; ii<dataSetOutput0.length(); ii++)
                    {
                        StringBuffer text0;
                        StringBuffer text1(dataSetOutput0.item(ii));
                        if (text1.length() > 0)
                        {
                            mergeDataRow(text0, text, text1, columnsHide);
                        }
                        dataSetOutput.append(text0);
                    }
                }

                if (depth < 1)
                {
                    read++;
                    if(read>=count)
                        break;
                }
            }

            if (depth < 1)
            {
                if (count > read)
                    count = read;
            }
        }
    }
    catch(IException* e)
    {
        if ((version < 1.08) || (e->errorCode() != FVERR_FilterTooRestrictive))
            throw e;

        e->Release();
        iRet = 1;
    }
    return iRet;
}

//sample filterBy: 340020001id1
//sample data: <XmlSchema name="myschema">...</XmlSchema><Dataset xmlSchema="myschema">...</Dataset>
int CWsDfuEx::GetIndexData(IEspContext &context, bool bSchemaOnly, const char* indexName, const char* parentName, const char* filterBy, __int64 start,
                                        __int64& count, __int64& read, __int64& total, StringBuffer& message, StringArray& columnLabels,
                                        StringArray& columnLabelsType, IArrayOf<IEspDFUData>& DataList, bool webDisableUppercaseTranslation)
{
    if (!indexName || !*indexName)
        return -1;

    double version = context.getClientVersion();

    StringBuffer cluster;
    bool disableUppercaseTranslation = false;
    Owned<IDistributedFile> df;
    try
    {
        df.setown(lookupLogicalName(context, indexName, AccessMode::tbdRead, false, false, nullptr, defaultPrivilegedUser));
        if(!df)
            throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Could not find file %s.", indexName);

        //Check disableUppercaseTranslation
        StringBuffer mapping;
        df->getColumnMapping(mapping);
        if (mapping.length() > 37 && strstr(mapping.str(), "word{set(stringlib.StringToLowerCase)}"))
            disableUppercaseTranslation = true;
        else if (webDisableUppercaseTranslation)
            disableUppercaseTranslation = webDisableUppercaseTranslation;
        else
            disableUppercaseTranslation = m_disableUppercaseTranslation;

        const char* wuid = df->queryAttributes().queryProp("@workunit");
        if (wuid && *wuid)
        {
            CWUWrapper wu(wuid, context);
            if (wu)
                cluster.append(wu->queryClusterName());
        }
    }
    catch (IException *e)
    {
        IERRLOG(e);
        e->Release();
    }
    catch(...)
    {
        IERRLOG("Unknown Exception - view data file: %s", indexName);
    }

    Owned<IResultSetFactory> resultSetFactory = getSecResultSetFactory(context.querySecManager(), context.queryUser(), context.queryUserId(), context.queryPassword());
    Owned<IViewFileWeb> web;

    Owned<IUserDescriptor> udesc;
    ISecUser * secUser = context.queryUser();
    if(secUser && secUser->getName() && *secUser->getName())
    {
        udesc.setown(createUserDescriptor());
        udesc->set(secUser->getName(), secUser->credentials().getPassword(), context.querySignature());
    }

    if (cluster.length())
    {
        web.setown(createViewFileWeb(*resultSetFactory, cluster, udesc.getLink()));
    }
    else if (m_clusterName.length() > 0)
    {
        web.setown(createViewFileWeb(*resultSetFactory, m_clusterName.str(), udesc.getLink()));
    }
    else
    {
        web.setown(createViewFileWeb(*resultSetFactory, NULL, udesc.getLink()));
    }

    ViewGatherOptions options;
    options.primaryDepth = 100;             // we want to traverse secondary->primary, but not the reverse
    options.secondaryDepth = 0;
    options.setPayloadFilter(true);         // we're only interested in payload links

    char *indexName0 = (char *) indexName;
    Owned<IFileTreeBrowser> browser;
    try
    {
        web->gatherWeb(indexName0, df, options);
        browser.setown(web->createBrowseTree(indexName0));
    }
    catch(IException* e)
    {
        if ((e->errorCode() != FVERR_CouldNotResolveX) || (indexName[0] != '~'))
        {
            throw e;
        }
        else
        {
            e->Release();

            indexName0 = (char *) (indexName+1);
            web->gatherWeb(indexName0, df, options);
            browser.setown(web->createBrowseTree(indexName0));
        }
    }

    Owned<INewResultSet> result;
    if (cluster && *cluster)
    {
        result.setown(resultSetFactory->createNewFileResultSet(indexName0, cluster));
    }
    else if (m_clusterName.length() > 0)
    {
        result.setown(resultSetFactory->createNewFileResultSet(indexName0, m_clusterName.str()));
    }
    else
    {
        result.setown(resultSetFactory->createNewFileResultSet(indexName0, NULL));
    }

    // Apply the filter to the root node
    if (filterBy && *filterBy)
    {
        //Owned<IFilteredResultSet> filter = result->createFiltered();
        IResultSetFilter* filter = browser->queryRootFilter();
        setRootFilter(result, filterBy, filter, disableUppercaseTranslation);
        ///result.setown(filter->create());
    }

    StringBuffer text, schemaText;
    StringArray columnsHide;
    browseRelatedFileSchema(browser->queryRootFile(), parentName, 0, schemaText, columnLabels, columnLabelsType, columnsHide);

    text.appendf("<XmlSchema name=\"%s\">", SCHEMANAME);
    text.append(schemaText);
    text.append("</XmlSchema>").newline();

    int iRet = 0;
    if (!bSchemaOnly)
    {
        StringArray dataSetOutput;
        iRet = browseRelatedFileDataSet(version, browser->queryRootFile(), parentName, 0, start, count, read, columnsHide, dataSetOutput);

        StringBuffer dataSetText;
        dataSetText.appendf("<Dataset xmlSchema=\"%s\" >", SCHEMANAME);
        dataSetText.newline();
        for (unsigned i = 0; i<dataSetOutput.length(); i++)
        {
            StringBuffer text0(dataSetOutput.item(i));
            if (text0.length() > 0)
            {
                dataSetText.append(text0);
                dataSetText.newline();
            }
        }
        dataSetText.append("</Dataset>");

        text.append(dataSetText.str());
    }

    MemoryBuffer data;
    struct MemoryBuffer2IStringVal : public CInterface, implements IStringVal
   {
       MemoryBuffer2IStringVal(MemoryBuffer & _buffer) : buffer(_buffer) {}
       IMPLEMENT_IINTERFACE;

       virtual const char * str() const { UNIMPLEMENTED;  }
       virtual void set(const char *val) { buffer.append(strlen(val),val); }
       virtual void clear() { } // clearing when appending does nothing
       virtual void setLen(const char *val, unsigned length) { buffer.append(length, val); }
       virtual unsigned length() const { return buffer.length(); };
       MemoryBuffer & buffer;
   } adaptor0(data);

    adaptor0.set(text.str());
   data.append(0);

    total=result->getNumRows();

    Owned<IEspDFUData> item = createDFUData("","");
    item->setName(indexName);
    item->setNumRows(total);
    item->setData(data.toByteArray());

    DataList.append(*item.getClear());

    return iRet;
}

void CWsDfuEx::getFilePartsInfo(IEspContext &context, const char *dafilesrvHost, IFileDescriptor &fileDesc, bool forFileCreate, IEspDFUFileAccessInfo &accessInfo)
{
    double version = context.getClientVersion();

    IArrayOf<IEspDFUFilePart> dfuParts;
    IArrayOf<IEspDFUPartLocation> dfuPartLocations;

    StringBuffer host(dafilesrvHost);
    unsigned newLocationIndex = 0;
    MapStringTo<unsigned> partLocationMap;
    // NB: both CopyIndex and PartIndex are 1 based in response.
    Owned<IPartDescriptorIterator> pi = fileDesc.getIterator();
    ForEach(*pi)
    {
        IPartDescriptor& part = pi->query();

        bool isTLKPart = isPartTLK(&part);
        if (version <= 1.55 && isTLKPart)
            continue;

        IArrayOf<IEspDFUFileCopy> fileCopies;
        for (unsigned int i=0; i<part.numCopies(); i++)
        {
            StringBuffer groupHost, path;
            part.getPath(path, i);

            Owned<IEspDFUFileCopy> fileCopy = createDFUFileCopy();
            if (forFileCreate)
                fileCopy->setPath(path.str());
            fileCopy->setCopyIndex(i + 1);

            /* In cloud the [load-balanced] dafilesrv external service name will be used/passed in as dafilesrvHost
             * In bare metal the host names from the group will be used.
             */
            if (!dafilesrvHost)
                part.queryNode(i)->endpoint().getEndpointHostText(host.clear());
            unsigned *locationIndex = partLocationMap.getValue(host.str());
            if (locationIndex)
                fileCopy->setLocationIndex(*locationIndex);
            else
            {
                partLocationMap.setValue(host.str(), ++newLocationIndex);
                fileCopy->setLocationIndex(newLocationIndex);

                Owned<IEspDFUPartLocation> partLocation = createDFUPartLocation();
                partLocation->setLocationIndex(newLocationIndex);
                partLocation->setHost(host.str());
                dfuPartLocations.append(*partLocation.getClear());
            }

            fileCopies.append(*fileCopy.getClear());
        }

        Owned<IEspDFUFilePart> filePart = createDFUFilePart();
        filePart->setPartIndex(part.queryPartIndex() + 1);
        filePart->setCopies(fileCopies);

        if (version > 1.55)
            filePart->setTopLevelKey(isTLKPart);

        dfuParts.append(*filePart.getClear());
    }

    accessInfo.setNumParts(fileDesc.numParts());
    accessInfo.setFileParts(dfuParts);
    accessInfo.setFileLocations(dfuPartLocations);
}

void CWsDfuEx::getFileDafilesrvConfiguration(StringBuffer &keyPairName, unsigned &port, bool &secure, const char *group)
{
    port = DEFAULT_ROWSERVICE_PORT;
    secure = false;
#ifdef _CONTAINERIZED
    IERRLOG("CONTAINERIZED(CWsDfuEx::getFileDafilesrvConfiguration)");
#else
    keyPairName.set(env->getClusterGroupKeyPairName(group));
    Owned<IConstDaFileSrvInfo> daFileSrvInfo = env->getDaFileSrvGroupInfo(group);
    if (daFileSrvInfo)
    {
        port = daFileSrvInfo->getPort();
        secure = daFileSrvInfo->getSecure();
    }
#endif
}

void CWsDfuEx::getFileDafilesrvConfiguration(StringBuffer &keyPairName, unsigned &retPort, bool &retSecure, const char *fileName, std::vector<std::string> &groups)
{
    retPort = DEFAULT_ROWSERVICE_PORT;
    retSecure = false;
    bool firstGroup = true;
    for (auto &group: groups)
    {
        StringBuffer _keyPairName;
        unsigned port;
        bool secure;
        getFileDafilesrvConfiguration(_keyPairName, port, secure, group.c_str());
        if (firstGroup)
        {
            firstGroup = false;
            keyPairName.set(_keyPairName);
            retPort = port;
            retSecure = secure;
        }
        else
        {
            if (!strsame(keyPairName, _keyPairName))
                throwStringExceptionV(0, "Configuration issue - file '%s' is on multiple clusters, keys for file access must match", fileName);
            if (retPort != port)
                throwStringExceptionV(0, "Configuration issue - file '%s' is on multiple clusters, dafilesrv's port for file access must match", fileName);
            if (retSecure != secure)
                throwStringExceptionV(0, "Configuration issue - file '%s' is on multiple clusters, dafilesrv's security setting for file access must match", fileName);
        }
    }
}

static void getJsonTypeInfo(IFileDescriptor &fileDesc, IEspDFUFileAccessInfo &accessInfo)
{
    MemoryBuffer binLayout;
    StringBuffer jsonLayout;
    if (!getRecordFormatFromRtlType(binLayout, jsonLayout, fileDesc.queryProperties(), false, true))
        getRecordFormatFromECL(binLayout, jsonLayout, fileDesc.queryProperties(), false, true);
    if (jsonLayout.length())
        accessInfo.setRecordTypeInfoJson(jsonLayout.str());
}

SecAccessFlags translateToSecAccessFlags(CSecAccessType from)
{
    switch (from)
    {
        case CSecAccessType_Access:
            return SecAccess_Access;
        case CSecAccessType_Read:
            return SecAccess_Read;
        case CSecAccessType_Write:
            return SecAccess_Write;
        case CSecAccessType_Full:
            return SecAccess_Full;
        case CSecAccessType_None:
        default:
            return SecAccess_None;
    }
}

void CWsDfuEx::dFUFileAccessCommon(IEspContext &context, const CDfsLogicalFileName &lfn, SessionId clientSessionId, const char *requestId, unsigned expirySecs, bool returnTextResponse, unsigned lockTimeoutMs, IEspDFUFileAccessResponse &resp)
{
    double version = context.getClientVersion();
    StringBuffer fileName;
    lfn.get(fileName, false, true);
    if (0 == fileName.length())
        throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFU File lookup: No Name defined. (requestId=%s, expirySecs=%u)", requestId, expirySecs);

    StringBuffer userID;
    context.getUserID(userID);

    Owned<IUserDescriptor> userDesc;
    if (!userID.isEmpty())
    {
        userDesc.setown(createUserDescriptor());
        userDesc->set(userID.str(), context.queryPassword(), context.querySignature());
    }

    checkLogicalName(fileName, userDesc, true, false, false, nullptr); // check for read permissions

    Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(fileName, userDesc, AccessMode::tbdRead, false, true, nullptr, defaultPrivilegedUser, lockTimeoutMs); // lock super-owners
    if (!df)
        throw MakeStringException(ECLWATCH_FILE_NOT_EXIST,"Cannot find file '%s'.", fileName.str());

    StringBuffer cluster;
    lfn.getCluster(cluster);
    Owned<IFileDescriptor> fileDesc = df->getFileDescriptor(cluster);

    StringBuffer keyPairName;
    unsigned port;
    bool secure;
    StringBuffer dafilesrvHost;
#ifdef _CONTAINERIZED
    keyPairName.set("signing");
    if (!hasIssuerTlsConfig(keyPairName))
        throw makeStringExceptionV(-1, "dFUFileAccessCommon: file signing certificate ('%s') not defined in configuration.", keyPairName.str());

    auto externalService = k8s::getDafileServiceFromConfig("stream", true, true);
    dafilesrvHost.set(externalService.first.c_str());
    port = externalService.second;
    secure = true;
#else
    // NB: if file has copies on >1 cluster, they must share the same key

    std::vector<std::string> groups;
    unsigned numClusters = df->numClusters();
    for (unsigned c=0; c<numClusters; c++)
    {
        StringBuffer clusterName;
        const char *clusterGroup = df->getClusterName(c, clusterName.clear());
        groups.push_back(clusterGroup);
    }

    getFileDafilesrvConfiguration(keyPairName, port, secure, fileName, groups);
 #ifdef _USE_OPENSSL
    if (secure && keyPairName.isEmpty())
        throw makeStringExceptionV(-1, "No keyPairName is found for '%s' in environment settings: /EnvSettings/Keys/ClusterGroup.", cluster.str());
 #endif
#endif

    IEspDFUFileAccessInfo &accessInfo = resp.updateAccessInfo();

    Owned<IPropertyTree> metaInfo = createDFUFileMetaInfo(fileName, fileDesc, requestId, "READ", expirySecs, userDesc, keyPairName, port, secure, maxFileAccessExpirySeconds);
    StringBuffer metaInfoBlob;
    encodeDFUFileMeta(metaInfoBlob, metaInfo, env);
    accessInfo.setMetaInfoBlob(metaInfoBlob);

    CDFUFileType kind = CDFUFileType_Unset;
    if (returnTextResponse)
    {
        getFilePartsInfo(context, nullIfEmptyString(dafilesrvHost), *fileDesc, false, accessInfo);
        getJsonTypeInfo(*fileDesc, accessInfo);

        accessInfo.setExpiryTime(metaInfo->queryProp("expiryTime"));
        accessInfo.setFileAccessPort(metaInfo->getPropInt("port"));
        accessInfo.setFileAccessSSL(metaInfo->getPropBool("secure"));

        if (isFileKey(fileDesc))
        {
            kind = CDFUFileType_Index;
            if (version > 1.55)
            {
                if (isFileLocalKey(fileDesc))
                    kind = CDFUFileType_IndexLocal;
                else if (isFilePartitionKey(fileDesc))
                    kind = CDFUFileType_IndexPartitioned;
            }
        }
        else
        {
            const char *kindStr = queryFileKind(fileDesc);
            if (!isEmptyString(kindStr))
            {
                if (streq("flat", kindStr))
                    kind = CDFUFileType_Flat;
                else if (streq("csv", kindStr))
                    kind = CDFUFileType_Csv;
                else if (streq("xml", kindStr))
                    kind = CDFUFileType_Xml;
                else if (streq("json", kindStr))
                    kind = CDFUFileType_Json;
            }
        }
    }
    resp.setType(kind);

    df->setAccessed();

    LOG(MCauditInfo,",FileAccess,EspProcess,READ,%s,%s,%s,jobid=%s,expirySecs=%d", cluster.str(), userID.str(), fileName.str(), requestId, expirySecs);
}

// NB: deprecated from ver >= 1.50
bool CWsDfuEx::onDFUFileAccess(IEspContext &context, IEspDFUFileAccessRequest &req, IEspDFUFileAccessResponse &resp)
{
    try
    {
        IConstDFUFileAccessRequestBase &requestBase = req.getRequestBase();

        bool returnTextResponse = CFileAccessRole_External == requestBase.getAccessRole();

        CDfsLogicalFileName lfn;
        lfn.set(requestBase.getName());
        lfn.setCluster(requestBase.getCluster());

        dFUFileAccessCommon(context, lfn, 0, requestBase.getJobId(), requestBase.getExpirySeconds(), returnTextResponse, 0, resp);
    }
    catch (IException *e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onDFUFileAccessV2(IEspContext &context, IEspDFUFileAccessV2Request &req, IEspDFUFileAccessResponse &resp)
{
    try
    {
        CDfsLogicalFileName lfn;
        lfn.set(req.getName());
        lfn.setCluster(req.getCluster());

        dFUFileAccessCommon(context, lfn, req.getSessionId(), req.getRequestId(), req.getExpirySeconds(), req.getReturnTextResponse(), req.getLockTimeoutMs(), resp);
    }
    catch (IException *e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

// NB: deprecated from ver >= 1.50
static IGroup *getDFUFileIGroup(const char *clusterName, ClusterType clusterType, const char *clusterTypeEx, StringArray &locations, StringBuffer &groupName)
{
#ifdef _CONTAINERIZED
    UNIMPLEMENTED_X("CONTAINERIZED(getDFUFileIGroup)"); // call to getClusterGroupName() is not available.
#else
    GroupType groupType;
    StringBuffer basedir;
    getClusterGroupName(groupName, clusterName);
    Owned<IGroup> groupFound = queryNamedGroupStore().lookup(groupName.str(), basedir, groupType);
    if (!groupFound)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "getDFUFileIGroup: Failed to get Group for Group %s.", groupName.str());

    if (!locations.ordinality())
        return groupFound.getClear();

    StringBuffer locationStr;
    SocketEndpointArray epa;
    ForEachItemIn(i, locations)
    {
        SocketEndpoint ep(locations.item(i));
        if (ep.isNull())
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "getDFUFileIGroup: Invalid location '%s'.", locations.item(i));
        epa.append(ep);
        locationStr.append(locations.item(i)).append(";");
    }

    Owned<IGroup> group = createIGroup(epa);
    if (group->equals(groupFound))
        return group.getClear();

    groupName.set(DFUFileCreate_GroupNamePrefix).append(hashc((unsigned char*) locationStr.str(), locationStr.length(), 0));

    bool foundGroup = false;
    unsigned groupNameLength = groupName.length();
    while (true)
    {
        GroupType groupType;
        StringBuffer basedir;
        Owned<IGroup> groupFound = queryNamedGroupStore().lookup(groupName.str(), basedir, groupType);
        if (!groupFound)
            break;

        if (group->equals(groupFound))
        {
            foundGroup = true;
            ESPLOG(LogMax, "Found DFUFileIGroup %s", groupName.str());
            break;
        }
        //The original group name is used by another group. Rename it to: 'original name + _ + a random number'.
        //We have to check if the new name is also used by another group. If yes, rename it to:
        //'the original name + _ + another random number' until we find out a name which is not used.
        groupName.setLength(groupNameLength); //remove _rand if any
        groupName.append("_").append(rand());
    }

    if (!foundGroup)
    {
        StringBuffer defaultDir;
        if (!getConfigurationDirectory(nullptr, ConfigurationDirectoryForDataCategory, clusterTypeEx, groupName.str(), defaultDir))
            throw MakeStringException(ECLWATCH_INVALID_INPUT, "getDFUFileIGroup: Failed to get ConfigurationDirectory: %s.", groupName.str());

        GroupType grpType = (clusterType == ThorLCRCluster) ? grp_thor : ((clusterType == HThorCluster) ? grp_hthor : grp_roxie);

        std::vector<std::string> hosts;
        ForEachItemIn(l, locations)
            hosts.push_back(locations.item(l));
        queryNamedGroupStore().add(groupName, hosts, false, defaultDir, grpType);
        ESPLOG(LogMin, "DFUFileIGroup %s added", groupName.str());
    }
    return group.getClear();
#endif
}

void CWsDfuEx::exportRecordDefinitionBinaryType(const char *recordDefinition, MemoryBuffer &layoutBin)
{
    MultiErrorReceiver errs;
    Owned<IHqlExpression> expr = parseQuery(recordDefinition, &errs);
    if (errs.errCount() > 0)
    {
        StringBuffer errorMsg;
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "exportRecordDefinitionBinaryType: Failed in parsing ECL %s: %s.", recordDefinition, errs.toString(errorMsg).str());
    }
    if (!expr)
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "exportRecordDefinitionBinaryType: Failed in parsing ECL: %s.", recordDefinition);
    
    if (!exportBinaryType(layoutBin, expr, false))
        throw MakeStringException(ECLWATCH_INVALID_INPUT, "exportRecordDefinitionBinaryType: Failed in exportBinaryType.");
}

// NB: deprecated from ver >= 1.50
bool CWsDfuEx::onDFUFileCreate(IEspContext &context, IEspDFUFileCreateRequest &req, IEspDFUFileCreateResponse &resp)
{
    try
    {
#ifdef _CONTAINERIZED
        UNIMPLEMENTED_X("CONTAINERIZED(CWsDfuEx::onDFUFileCreate)");
#else
        IConstDFUFileAccessRequestBase &requestBase = req.getRequestBase();
        const char *fileName = requestBase.getName();
        const char *clusterName = requestBase.getCluster();
        const char *recordDefinition = req.getECLRecordDefinition();
        StringBuffer requestId(requestBase.getJobId());
        unsigned expirySecs = requestBase.getExpirySeconds();
        bool returnTextResponse = true;

        if (isEmptyString(fileName))
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreate: No Name defined.");
        if (isEmptyString(clusterName))
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreate: No Cluster defined.");
        if (isEmptyString(recordDefinition))
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreate: No ECLRecordDefinition defined.");

        StringBuffer userId;
        context.getUserID(userId);
        Owned<IUserDescriptor> userDesc;
        if (!userId.isEmpty())
        {
            userDesc.setown(createUserDescriptor());
            userDesc->set(userId.str(), context.queryPassword(), context.querySignature());
        }

        ClusterType clusterType = getClusterTypeByClusterName(clusterName);
        if (clusterType == NoCluster)
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreate: Cluster %s not found.", clusterName);

        CDfsLogicalFileName lfn;
        lfn.set(fileName);
        StringBuffer normalizedFileName(lfn.get());

        checkLogicalName(normalizedFileName, userDesc, false, true, false, nullptr);

        const char *clusterTypeEx = clusterTypeString(clusterType, false);
        StringBuffer groupName;
        Owned<IGroup> group = getDFUFileIGroup(clusterName, clusterType, clusterTypeEx, req.getPartLocations(), groupName);

        StringBuffer tempFileName(normalizedFileName);
        tempFileName.append(DFUFileCreate_FileNamePostfix);

        //create FileId
        StringBuffer fileId;
        fileId.set(groupName.str()).append(DFUFileIdSeparator).append(clusterName).append(DFUFileIdSeparator).append(tempFileName.str());
        resp.setFileId(fileId.str());

        if (requestId.isEmpty())
            requestId.appendf("Create %s on %s", normalizedFileName.str(), clusterName);

        Owned<IFileDescriptor> fileDesc = createFileDescriptor(tempFileName, clusterTypeString(clusterType, false), groupName, group);
        fileDesc->queryProperties().setProp("@job", requestId);
        if (!userId.isEmpty())
            fileDesc->queryProperties().setProp("@owner", userId);

        MemoryBuffer layoutBin;
        exportRecordDefinitionBinaryType(recordDefinition, layoutBin);
        if (0 == layoutBin.length())
            throw makeStringExceptionV(ECLWATCH_INVALID_ECLRECDEF, "DFUFileCreate: Failed to parse ECL record definition: %s.", recordDefinition);
        fileDesc->queryProperties().setPropBin("_rtlType", layoutBin.length(), layoutBin.toByteArray());
        fileDesc->queryProperties().setProp("ECL", recordDefinition);

        // NB: if file has copies on >1 cluster, they must share the same key
        StringBuffer keyPairName;
        unsigned port;
        bool secure;

        std::vector<std::string> groups;
        groups.push_back(groupName.str());
        getFileDafilesrvConfiguration(keyPairName, port, secure, normalizedFileName, groups);

        Owned<IPropertyTree> metaInfo = createDFUFileMetaInfo(tempFileName, fileDesc, requestId, "WRITE", expirySecs, userDesc, keyPairName, port, secure, maxFileAccessExpirySeconds);
        metaInfo->setProp("clusterName", clusterName);

        StringBuffer metaInfoBlob;
        encodeDFUFileMeta(metaInfoBlob, metaInfo, env);

        IEspDFUFileAccessInfo &accessInfo = resp.updateAccessInfo();
        accessInfo.setMetaInfoBlob(metaInfoBlob);

        getFilePartsInfo(context, nullptr, *fileDesc, true, accessInfo);
        getJsonTypeInfo(*fileDesc, accessInfo);

        accessInfo.setExpiryTime(metaInfo->queryProp("expiryTime"));
        accessInfo.setFileAccessPort(metaInfo->getPropInt("port"));
        accessInfo.setFileAccessSSL(metaInfo->getPropBool("secure"));
#endif
    }
    catch (IException *e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onDFUFileCreateV2(IEspContext &context, IEspDFUFileCreateV2Request &req, IEspDFUFileCreateResponse &resp)
{
    try
    {
        const char *fileName = req.getName();
        const char *clusterName = req.getCluster();
        const char *recordDefinition = req.getECLRecordDefinition();
        unsigned expirySecs = req.getExpirySeconds();
        StringBuffer requestId(req.getRequestId());
        bool returnTextResponse = req.getReturnTextResponse();

        if (isEmptyString(fileName))
            throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: No Name defined.");
        if (isEmptyString(recordDefinition))
            throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: No ECLRecordDefinition defined.");

        StringBuffer userId;
        context.getUserID(userId);
        Owned<IUserDescriptor> userDesc;
        if (!userId.isEmpty())
        {
            userDesc.setown(createUserDescriptor());
            userDesc->set(userId.str(), context.queryPassword(), context.querySignature());
        }

        CDfsLogicalFileName lfn;
        lfn.set(fileName);
        StringBuffer normalizedFileName(lfn.get());

        checkLogicalName(normalizedFileName, userDesc, false, true, false, nullptr);
        if (requestId.isEmpty())
            requestId.appendf("Create %s on %s", normalizedFileName.str(), clusterName);

        const char *fileType = nullptr;
        CDFUFileType kind = req.getType();
        switch (kind)
        {
            case CDFUFileType_Flat:
                fileType = "flat";
                break;
            case CDFUFileType_Csv:
                fileType = "csv";
                break;
            case CDFUFileType_Xml:
                fileType = "xml";
                break;
            case CDFUFileType_Json:
                fileType = "json";
                break;
            case CDFUFileType_Index:
                fileType = "key";
            default:
                throw makeStringExceptionV(ECLWATCH_MISSING_FILETYPE, "DFUFileCreateV2: File type not provided");
        }

        StringBuffer tempFileName(normalizedFileName);
        tempFileName.append(".").append(dfuCreateUniqId++); // avoid potential clash if >1 creating file. One will succeed at publish time.
        tempFileName.append(DFUFileCreate_FileNamePostfix);

        Owned<IFileDescriptor> fileDesc;
        StringBuffer keyPairName;
        unsigned port;
        bool secure;
        StringBuffer dafilesrvHost;
        StringBuffer fileId;

#ifdef _CONTAINERIZED
        keyPairName.set("signing");
        if (!hasIssuerTlsConfig(keyPairName))
            throw makeStringExceptionV(-1, "onDFUFileCreateV2: file signing certificate ('%s' ) not defined in configuration.", keyPairName.str());

        const char *planeName = clusterName;
        unsigned numParts = 0; // in future perhaps client can specify, for now default is = to plane default (defaultSprayParts)
        fileDesc.setown(createFileDescriptor(tempFileName, planeName, numParts));
        numParts = fileDesc->numParts();

        auto externalService = k8s::getDafileServiceFromConfig("stream", true, true);
        dafilesrvHost.set(externalService.first.c_str());
        port = externalService.second;
        secure = true;

        //create FileId (NB: used when this file is published)
        fileId.set(tempFileName).append(DFUFileIdSeparator).append(planeName).append(DFUFileIdSeparator).append(numParts);
        fileId.append(DFUFileIdSeparator).append(boolToStr(req.getCompressed()));
        fileId.append(DFUFileIdSeparator).append(fileType);
#else
        if (isEmptyString(clusterName))
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: No Cluster defined trying to create file '%s'.", fileName);
        ClusterType clusterType = getClusterTypeByClusterName(clusterName);
        if (clusterType == NoCluster)
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: Cluster %s not found.", clusterName);

        StringBuffer groupName;
        getClusterGroupName(groupName, clusterName);
        if (isEmptyString(groupName))
             throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: Group for cluster %s not found.", clusterName);
        Owned<IGroup> group = queryNamedGroupStore().lookup(groupName.str());
        if (!group)
            throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFileCreateV2: Failed to get Group %s.", groupName.str());
        fileDesc.setown(createFileDescriptor(tempFileName, clusterTypeString(clusterType, false), groupName, group));
        // NB: if file has copies on >1 cluster, they must share the same key
        std::vector<std::string> groups;
        groups.push_back(groupName.str());
        getFileDafilesrvConfiguration(keyPairName, port, secure, normalizedFileName, groups);

        //create FileId (NB: used when this file is published)
        fileId.set(tempFileName).append(DFUFileIdSeparator).append(clusterName).append(DFUFileIdSeparator).append(groupName);
        fileId.append(DFUFileIdSeparator).append(boolToStr(req.getCompressed()));
        fileId.append(DFUFileIdSeparator).append(fileType);
#endif
        resp.setFileId(fileId.str());

        fileDesc->queryProperties().setProp("@job", requestId);
        if (!userId.isEmpty())
            fileDesc->queryProperties().setProp("@owner", userId);
        fileDesc->queryProperties().setProp("ECL", recordDefinition);
        fileDesc->queryProperties().setProp("@kind", fileType);

        MemoryBuffer layoutBin;
        exportRecordDefinitionBinaryType(recordDefinition, layoutBin);
        if (0 == layoutBin.length())
            throw makeStringExceptionV(ECLWATCH_INVALID_ECLRECDEF, "DFUFileCreateV2: Failed to parse ECL record definition: %s.", recordDefinition);
        fileDesc->queryProperties().setPropBin("_rtlType", layoutBin.length(), layoutBin.toByteArray());
        if (req.getCompressed())
            fileDesc->queryProperties().setPropBool("@blockCompressed", true);

        Owned<IPropertyTree> metaInfo = createDFUFileMetaInfo(tempFileName, fileDesc, requestId, "WRITE", expirySecs, userDesc, keyPairName, port, secure, maxFileAccessExpirySeconds);
        metaInfo->setProp("clusterName", clusterName);

        StringBuffer metaInfoBlob;
        encodeDFUFileMeta(metaInfoBlob, metaInfo, env);

        IEspDFUFileAccessInfo &accessInfo = resp.updateAccessInfo();
        accessInfo.setMetaInfoBlob(metaInfoBlob);
        if (returnTextResponse)
        {
            getFilePartsInfo(context, nullIfEmptyString(dafilesrvHost), *fileDesc, true, accessInfo);
            getJsonTypeInfo(*fileDesc, accessInfo);

            accessInfo.setExpiryTime(metaInfo->queryProp("expiryTime"));
            accessInfo.setFileAccessPort(metaInfo->getPropInt("port"));
            accessInfo.setFileAccessSSL(metaInfo->getPropBool("secure"));
        }
    }
    catch (IException *e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsDfuEx::onDFUFilePublish(IEspContext &context, IEspDFUFilePublishRequest &req, IEspDFUFilePublishResponse &resp)
{
    Owned<IException> exception;
    Owned<IDistributedFile> newFile;
    Owned<IFileDescriptor> fileDesc;
    StringBuffer normalizeTempFileName;
    bool newFileAttached = false;
    try
    {
        const char *fileId = req.getFileId();
        if (isEmptyString(fileId))
             throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: No FileId defined.");

        const char *recordDefinition = req.getECLRecordDefinition();

        StringArray fileIdItems;
        fileIdItems.appendList(fileId, DFUFileIdSeparator);
        const char *tempFileName = fileIdItems.item(0);
        if (isEmptyString(tempFileName))
             throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid FileId: empty FileName.");

        CDfsLogicalFileName lfn;
        lfn.set(tempFileName);
        normalizeTempFileName.set(lfn.get());

        size32_t postFixLen = strlen(DFUFileCreate_FileNamePostfix);
        if (normalizeTempFileName.length() <= postFixLen)
            throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid temporary filename: %s.", fileId);

        const char *groupName = nullptr;
        const MemoryBuffer &fDescBlob = req.getFileDescriptorBlob();
        if (fDescBlob.length())
        {
            MemoryBuffer mb;
            mb.setBuffer(fDescBlob.length(), (void *)fDescBlob.toByteArray());
            fileDesc.setown(deserializeFileDescriptor(mb));
        }
        else
        {
            if (isEmptyString(recordDefinition))
                 throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: No ECLRecordDefinition defined.");

#ifdef _CONTAINERIZED
            const char *planeName = fileIdItems.item(1);
            groupName = planeName;
            unsigned numParts = atoi(fileIdItems.item(2));
            if (isEmptyString(planeName))
                throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid FileId: empty planeName.");
            if (0 == numParts)
                throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid FileId: numParts not specified.");
            fileDesc.setown(createFileDescriptor(tempFileName, planeName, numParts));
#else
            const char *clusterName = fileIdItems.item(1);
            groupName = fileIdItems.item(2);
            if (isEmptyString(groupName))
                throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid FileId: empty groupName.");
            if (isEmptyString(clusterName))
                throw makeStringException(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid FileId: empty clusterName.");

            ClusterType clusterType = getClusterTypeByClusterName(clusterName);
            const char *clusterTypeEx = clusterTypeString(clusterType, false);
            GroupType groupType;
            StringBuffer basedir;
            Owned<IGroup> group = queryNamedGroupStore().lookup(groupName, basedir, groupType);
            if (!group)
                throw makeStringExceptionV(ECLWATCH_FILE_NOT_EXIST, "DFUFilePublish: Failed to find group %s.", groupName);

            fileDesc.setown(createFileDescriptor(normalizeTempFileName, clusterTypeEx, groupName, group));
#endif
            if (fileIdItems.ordinality()>3) // compressed
            {
                bool compressed = strToBool(fileIdItems.item(3));
                if (compressed)
                    fileDesc->queryProperties().setPropBool("@blockCompressed", true);
                if (fileIdItems.ordinality()>4) // fileType
                    fileDesc->queryProperties().setProp("@kind", fileIdItems.item(4));
            }
        }

        StringBuffer newFileName(normalizeTempFileName);
        const char *start = newFileName;
        const char *pos = start + (newFileName.length() - postFixLen);
        const char *startPos = pos;
        if (!streq(pos, DFUFileCreate_FileNamePostfix))
            throw makeStringExceptionV(ECLWATCH_INVALID_INPUT, "DFUFilePublish: Invalid temporary filename: %s.", fileId);
        while (true)
        {
            --pos;
            if (pos == start)
            {
                // old style ( <= 1.39 ), without unique id
                newFileName.setLength(startPos-start);
                break;
            }
            else if ('.' == *pos)
            {
                newFileName.setLength(pos-start);
                break;
            }
        }
        newFileName.setLength(pos-start);

        if (!isEmptyString(recordDefinition))
        {
            MemoryBuffer layoutBin;
            exportRecordDefinitionBinaryType(recordDefinition, layoutBin);
            fileDesc->queryProperties().setPropBin("_rtlType", layoutBin.length(), layoutBin.toByteArray());
            fileDesc->queryProperties().setProp("ECL", recordDefinition);
        }
        if (!req.getRecordCount_isNull())
            fileDesc->queryProperties().setPropInt64("@recordCount", req.getRecordCount());
        if (!req.getFileSize_isNull())
            fileDesc->queryProperties().setPropInt64("@size", req.getFileSize());

        StringBuffer userId;
        Owned<IUserDescriptor> userDesc;
        context.getUserID(userId);
        if (!userId.isEmpty())
        {
            userDesc.setown(createUserDescriptor());
            userDesc->set(userId.str(), context.queryPassword(), context.querySignature());
            fileDesc->queryProperties().setProp("@owner", userId);
        }
        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(newFileName, userDesc, AccessMode::tbdRead, false, true, nullptr, defaultPrivilegedUser, req.getLockTimeoutMs());
        if (df)
        {
            if (!req.getOverwrite())
                throw makeStringExceptionV(ECLWATCH_FILE_ALREADY_EXISTS, "DFUFilePublish: File already exists (%s) and overwrite not specified.", newFileName.str());
            df->detach(req.getLockTimeoutMs());
        }

        setPublishFileSize(newFileName, fileDesc);
        newFile.setown(queryDistributedFileDirectory().createNew(fileDesc));
        newFile->validate();
        // JCSMORE attach() should have a timeout, then req.getLockTimeoutMs() should be used here.
        newFile->attach(normalizeTempFileName, userDesc);
        newFileAttached = true;

        if (!newFile->renamePhysicalPartFiles(newFileName, nullptr, nullptr, fileDesc->queryDefaultDir()))
            throw makeStringExceptionV(ECLWATCH_FILE_NOT_EXIST, "DFUFilePublish: Failed in renamePhysicalPartFiles %s.", newFileName.str());

        newFile->rename(newFileName, userDesc);

        LOG(MCauditInfo,",FileAccess,EspProcess,CREATED,%s,%s,%s", groupName, userId.str(), newFileName.str());
    }
    catch (IException *e)
    {
        exception.setown(e);
    }

    if (exception)
    {
        try
        {
            if (fileDesc)
            {
                Owned<IMultiException> exceptions = MakeMultiException("CWsDfuEx::onDFUFilePublish");
                queryDistributedFileDirectory().removePhysicalPartFiles(normalizeTempFileName, fileDesc, exceptions);
                if (exceptions->ordinality())
                {
                    StringBuffer errMsg("Error whilst clearing up temporary file: ");
                    EXCLOG(exceptions, errMsg.append(normalizeTempFileName).str());
                }
            }
            if (newFileAttached)
                newFile->detach(30000);
        }
        catch (IException *e)
        {
            // follow-on exception.
            // Ensure original exception takes precedence, log follow-on exception only.
            EXCLOG(e);
            e->Release();
        }
        FORWARDEXCEPTION(context, exception.getClear(), ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

void CWsDfuEx::setPublishFileSize(const char *lfn, IFileDescriptor *fileDesc)
{
    auto funcFilePartSize = [lfn, fileDesc](unsigned partNum)
    {
        bool compressed = fileDesc->isCompressed();
        IPartDescriptor *partDesc = fileDesc->queryPart(partNum);
        IPropertyTree &props = partDesc->queryProperties();
        unsigned numCopies = partDesc->numCopies();
        for (unsigned c = 0; c < numCopies; c++)
        {
            RemoteFilename rfn;
            partDesc->getFilename(c, rfn);
            try
            {
                Owned<IFile> file = createIFile(rfn);
                if (compressed)
                {
                    props.setPropInt64("@compressedSize", file->size());
                    Owned<IFileIO> io = createCompressedFileReader(file, nullptr, useDefaultIoBufferSize, false, IFEnone);
                    if (io)
                        props.setPropInt64("@size", io->size());
                }
                else
                {
                    props.setPropInt64("@size", file->size());
                }
                break;
            }
            catch (IException *e)
            {
                VStringBuffer tmp("%s: ", lfn);
                rfn.getPath(tmp);
                EXCLOG(e, tmp.str());
                e->Release();
            }
        }
    };

    //collect and set the sizes for file parts
    CAsyncForFunc<decltype(funcFilePartSize)> async(funcFilePartSize);
    async.For(fileDesc->numParts(), 100);
}

//////////////////////HPCC Browser//////////////////////////
