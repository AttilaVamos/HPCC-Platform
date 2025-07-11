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

#include "platform.h"

#include "jfile.hpp"
#include "jiface.hpp"
#include "jprop.hpp"
#include "jutil.hpp"


#include "thgraphmaster.ipp"
#include "thorport.hpp"
#include "thormisc.hpp"

#include "daaudit.hpp"
#include "dadfs.hpp"
#include "dalienv.hpp"
#include "dasess.hpp"
#include "dasds.hpp"
#include "dautils.hpp"
#include "thmfilemanager.hpp"
#include "thexception.hpp"

#include "workunit.hpp"

#include "ws_dfsclient.hpp"

#define CHECKPOINTSCOPE "checkpoints"
#define TMPSCOPE "temporary"

//#define TRACE_RESOLVE

static IThorFileManager *fileManager = NULL;

typedef OwningStringHTMapping<IDistributedFile> CIDistributeFileMapping;
class CFileManager : public CSimpleInterface, implements IThorFileManager
{
    OwningStringSuperHashTableOf<CIDistributeFileMapping> fileMap;
    bool replicateOutputs;


    StringBuffer &_getPublishPhysicalName(CJobBase &job, const char *logicalName, unsigned partno, const char *groupName, IGroup *group, StringBuffer &res)
    {
        // JCSMORE - this is daft functionality!
        // I think used by pipe progs create a logical entry at run time and writing to physicals
        // Problems:
        // i) it writes non replicated entry.
        // ii) It publishes immediately not when done!
        // iii) it is not removing existing physicals first
        // If we really want this, it should replicate when done somehow, and only publish at end.
        Owned<IDistributedFile> file = lookup(job, logicalName, AccessMode::writeSequential, false, true, false, defaultPrivilegedUser);
        StringBuffer scopedName;
        addScope(job, logicalName, scopedName);
        if (group) // publishing
        {
            if (partno >= group->ordinality())
                throw MakeThorException(TE_NoSuchPartForLogicalFile , "No such part number (%d) for logical file : %s", partno, scopedName.str());
            if (file)
            {
                file.clear();
                // JCSMORE delete orphan file parts here..
                // i.e. shouldn't it call
                // dfd.removeEntry(scopedName.str(), userDesc);
                // And/Or queryDistributedFileDirectory().removePhysical(scopedName.str(), 0, NULL, NULL, userDesc);
            }
            // overwrite assumed allowed here
            StringArray clusters;
            clusters.append(groupName);
            IArrayOf<IGroup> groups;
            groups.append(*LINK(group));
            Owned<IFileDescriptor> fileDesc = create(job, scopedName, clusters, groups, true, TDWnoreplicate);
            publish(job, scopedName, *fileDesc, &file);
        }
        else
        {
            if (!file)
                throw MakeThorException(TE_LogicalFileNotFound, "getPhysicalName: Logical file doesn't exist (%s)", scopedName.str());
            if (partno >= file->numParts())
                throw MakeThorException(TE_NoSuchPartForLogicalFile , "No such part number (%d) for logical file : %s", partno, logicalName);
        }
        Owned<IDistributedFilePart> part = file->getPart(partno);
        RemoteFilename rfn;
        part->getFilename(rfn).getRemotePath(res);
        return res;
    }

    bool scanDFS(const char *pattern, const char *scope, StringArray &results)
    {
        // TBD
        return false;
    }

    unsigned fixTotal(CJobBase &job, IArrayOf<IGroup> &groups, unsigned &offset)
    {
        offset = 0;
        unsigned max = 0;

        ForEachItemIn(g, groups)
        {
            IGroup *group = &groups.item(g);
            if (group->ordinality() <= queryDfsGroup().ordinality()) // i.e. cluster will make >= parts than target group (if more wraps)
            {
                if (queryDfsGroup().ordinality() > max) max = queryDfsGroup().ordinality();
            }
            else
            {
                // NB: it is very unlikely this code has ever been used.
                // The whole of fixTotal should be deleted at some point (see HPCC-33150)

                // Check options to shrink 'large' tgtfile to this cluster size
                const char *wideDestOptStr = globals->queryProp("@wideDestOpt");
                if (wideDestOptStr)
                {
                    if (0 == stricmp("smallMiddle", wideDestOptStr))
                    {
                        if (groups.ordinality() > 1)
                            throwUnexpected();
                        GroupRelation relation = queryDfsGroup().compare(group);
                        if (GRbasesubset == relation)
                        {
                            offset = group->rank(&queryDfsGroup().queryNode(0));
                            if (offset)
                                throwUnexpected(); // would require desc->setPartOffset(n); type functionality.
                        }
                        return queryDfsGroup().ordinality();
                    }
                    else if (0 == stricmp("smallStart", wideDestOptStr))
                    {
                        if (queryDfsGroup().ordinality() > max) max = queryDfsGroup().ordinality();
                    }
                }
                // else leave wide and try to target subset - if want smaller file on bigger remote clusters should use "smallStart" option
                if (group->ordinality() > max) max = group->ordinality();
            }
        }
        return max;
    }

    template<typename T>
    T blockReportFunc(CJobBase &job, std::function<T(unsigned timeout)> func, unsigned timeout, const char *msg)
    {
        // every minute timeout, and set workunit state to blocked for NN minutes
        WUState preBlockedState = WUStateUnknown; // set to current state if times out and blocks
        CTimeMon tm(timeout);
        if (timeout >= 60000)
            timeout = 60000;
        while (true)
        {
            try
            {
                T ret = func(timeout);
                if (WUStateUnknown != preBlockedState)
                {
                    Owned<IWorkUnit> workunit = &job.queryWorkUnit().lock();
                    WUState currentState = workunit->getState();
                    if (WUStateBlocked != currentState) // could happen if user aborted whilst blocked. Bail out.
                        throw MakeStringException(0, "WorkUnit state has changed to '%s' whilst blocked trying to '%s'", getWorkunitStateStr(currentState), msg);
                    workunit->setState(preBlockedState); // i.e. restore original state
                }
                return ret;
            }
            catch (ISDSException *e)
            {
                if ((SDSExcpt_LockTimeout != e->errorCode()) || tm.timedout())
                    throw;
                e->Release();
            }

            // NB: will reach here only if timeout occurred.
            Owned<IWorkUnit> workunit = &job.queryWorkUnit().lock();
            WUState currentState = workunit->getState();
            if (WUStateUnknown == preBlockedState) // i.e. 1st time
            {
                if (WUStateUnknown == currentState) // should never happen, but guard against it JIC
                    throw MakeStringException(0, "WorkUnit in unknown state");
                preBlockedState = currentState;
                workunit->setState(WUStateBlocked);
            }
            else // NB: implies >= 2nd time around
            {
                if (WUStateBlocked != currentState) // could happen if user aborted whilst blocked. Bail out.
                    throw MakeStringException(0, "WorkUnit state has changed to '%s' whilst blocked trying to '%s'", getWorkunitStateStr(currentState), msg);
            }
            VStringBuffer blockedMsg("Blocked for %u minutes trying to %s", tm.elapsed()/60000, msg);
            workunit->setStateEx(blockedMsg);
        }
    }

    void remove(CJobBase &job, IDistributedFile &file, unsigned timeout=INFINITE)
    {
        StringBuffer lfn;
        file.getLogicalName(lfn);
        CDfsLogicalFileName dlfn;
        dlfn.set(lfn.str());
        if (dlfn.isExternal())
        {
            RemoteFilename rfn;
            dlfn.getExternalFilename(rfn);
            StringBuffer path;
            rfn.getPath(path);
            OwnedIFile iFile = createIFile(path.str());
            if (iFile->exists() && !iFile->remove())
                throw MakeThorException(0, "Failed to remove external file: %s", lfn.str());
        }
        else
        {
            VStringBuffer blockedMsg("delete file '%s'", file.queryLogicalName());
            auto func = [&file](unsigned timeout){ file.detach(timeout); return true; };
            blockReportFunc<bool>(job, func, timeout, blockedMsg);
        }
    }

public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CFileManager()
    {
        replicateOutputs = globals->getPropBool("@replicateOutputs");
    }
    StringBuffer &mangleLFN(CJobBase &job, const char *lfn, StringBuffer &out)
    {
        out.append(lfn).append("__").append(job.queryWuid());
        return out;
    }
    StringBuffer &addScope(CJobBase &job, const char *logicalname, StringBuffer &ret, bool temporary=false, bool paused=false)
    {
        StringBuffer tmp;
        bool root=false;
        while (' ' == *logicalname)
            ++logicalname;
        if ('~' == *logicalname)
        {
            root=true;
            ++logicalname;
        }
        if (temporary)
        {
            StringBuffer tail;
            tmp.append(queryDfsXmlBranchName(DXB_Internal)).append("::");
            const char *user = job.queryUser();
            if (user && *user)
                tmp.append(user).append("::");
            if (paused)
            {
                tmp.append(PAUSETMPSCOPE);
                CDfsLogicalFileName dfslfn;
                dfslfn.set(logicalname);
                dfslfn.getTail(tail);
                logicalname = tail.str();
            }
            else
                tmp.append(job.queryUseCheckpoints() ? CHECKPOINTSCOPE : TMPSCOPE);
            tmp.append("::");

            mangleLFN(job, logicalname, tmp);
        }
        else
        {
            if (!root)
            {
                tmp.append(job.queryScope()).trim();
                if (tmp.length())
                {
                    tmp.toLowerCase();
                    tmp.append("::");
                }
            }
            tmp.append(logicalname).clip();
        }
        normalizeLFN(tmp.str(), ret);
        return ret;
    }

// IThorFileManager impl.
    void clearCacheEntry(const char *name)
    {
        fileMap.remove(name);
    }

    void noteFileRead(CJobBase &job, IDistributedFile *file, bool extended)
    {
        Owned<IWorkUnit> wu = &job.queryWorkUnit().lock();
        wu->noteFileRead(file);

        StringArray clusters;
        file->getClusterNames(clusters);
        StringBuffer outs;
        outs.appendf(",FileAccess,Thor,%s,%s,%s,%s,%s,%s,%" I64F "d,%d",
                        extended?"EXTEND":"READ",
                        globals->queryProp("@nodeGroup"),
                        job.queryUser(),
                        file->queryLogicalName(),
                        job.queryWuid(),
                        job.queryGraphName(),
                        file->getFileSize(false, false),clusters.ordinality());
        ForEachItemIn(i,clusters) {
            outs.append(',').append(clusters.item(i));
        }
        LOG(MCauditInfo,"%s",outs.str());
    }

    IDistributedFile *timedLookup(CJobBase &job, CDfsLogicalFileName &lfn, AccessMode accessMode, bool privilegedUser=false, unsigned timeout=INFINITE)
    {
        auto func = [&job, &lfn, accessMode, privilegedUser](unsigned timeout)
        {
            return wsdfs::lookup(lfn, job.queryUserDescriptor(), accessMode, false, false, nullptr, privilegedUser, timeout);
        };

        VStringBuffer blockedMsg("lock file '%s' for %s access", lfn.get(), isWrite(accessMode) ? "WRITE" : "READ");
        return blockReportFunc<IDistributedFile *>(job, func, timeout, blockedMsg);
    }
    
    IDistributedFile *timedLookup(CJobBase &job, const char *logicalName, AccessMode accessMode, bool privilegedUser=false, unsigned timeout=INFINITE)
    {
        CDfsLogicalFileName lfn;
        lfn.set(logicalName);
        return timedLookup(job, lfn, accessMode, privilegedUser, timeout);
    }
    IDistributedFile *lookup(CJobBase &job, const char *logicalName, AccessMode mode, bool temporary, bool optional, bool reportOptional, bool privilegedUser, bool updateAccessed=true)
    {
        StringBuffer scopedName;
        bool paused = false;
        if (temporary && job.queryResumed())
        { // need to check if previously published.
            StringBuffer pausedName;
            addScope(job, logicalName, pausedName, temporary, true); // temporary ignore if paused==true
            if (job.queryWorkUnit().queryFileUsage(pausedName.str())) // 0 == doesn't exist
            {
                paused = true;
                scopedName.set(pausedName.str());
            }
        }
        if (!paused)
            addScope(job, logicalName, scopedName, temporary);
        CIDistributeFileMapping *fileMapping = fileMap.find(scopedName.str());
        if (fileMapping)
            return &fileMapping->get();

        Owned<IDistributedFile> file = timedLookup(job, scopedName.str(), mode, privilegedUser, job.queryMaxLfnBlockTimeMins() * 60000);
        if (file && 0 == file->numParts())
        {
            if (file->querySuperFile())
            {
                if (optional)
                    file.clear();
                else
                    throw MakeStringException(TE_MachineOrderNotFound, "Superfile %s has no content\n", scopedName.str());
            }
            else
                throw MakeStringException(-1, "Unexpected, standard file %s contains no parts", scopedName.str());
        }
        if (!file)
        {
            if (!optional)
                throw MakeStringException(TE_MachineOrderNotFound, "Missing logical file %s\n", scopedName.str());
            if (reportOptional)
            {
                Owned<IThorException> e = MakeThorException(ENGINEERR_MISSING_OPTIONAL_FILE, "Input file '%s' was missing but declared optional", scopedName.str());
                e->setAction(tea_warning);
                e->setSeverity(SeverityWarning);
                reportExceptionToWorkunitCheckIgnore(job.queryWorkUnit(), e);
            }
            return NULL;
        }
        if (updateAccessed)
            file->setAccessed();
        return LINK(file);
    }

    IFileDescriptor *create(CJobBase &job, const char *logicalName, StringArray &groupNames, IArrayOf<IGroup> &groups, bool overwriteok, unsigned helperFlags=0, bool nonLocalIndex=false, unsigned restrictedWidth=0)
    {
        bool temporary = 0 != (helperFlags&TDXtemporary);
        bool jobReplicate = 0 != job.getWorkUnitValueInt("replicateOutputs", replicateOutputs);
        bool replicate = 0 != jobReplicate && !temporary && 0==(helperFlags&TDWnoreplicate);
        bool persistent = 0 != (helperFlags&TDWpersist);
        bool extend = 0 != (helperFlags&TDWextend);
        bool jobTemp = 0 != (helperFlags&TDXjobtemp);

        LOG(MCdebugInfo, "createLogicalFile ( %s )", logicalName);

        Owned<IDistributedFile> efile;
        CDfsLogicalFileName dlfn;
        if (!temporary)
        {
            if (!dlfn.setValidate(logicalName))
                throw MakeStringException(99, "Cannot publish %s, invalid logical name", logicalName);
            if (dlfn.isForeign())
                throw MakeStringException(99, "Cannot publish to a foreign Dali: %s", logicalName);
            efile.setown(timedLookup(job, dlfn, AccessMode::tbdWrite, true, job.queryMaxLfnBlockTimeMins() * 60000));
            if (efile)
            {
                if (!extend && !overwriteok)
                    throw MakeStringException(TE_OverwriteNotSpecified, "Cannot write %s, file already exists (missing OVERWRITE attribute?)", logicalName);
            }
        }

        StringAttr wuidStr(job.queryWorkUnit().queryWuid());
        StringAttr userStr(job.queryWorkUnit().queryUser());
        StringAttr jobStr(job.queryWorkUnit().queryJobName());
        if (overwriteok && (!temporary || job.queryUseCheckpoints()))
        {
            if (!temporary)
            {
                // removing dfs entry, factor out space for each file part used if previously considered by this wuid.
                Owned<IWorkUnit> workunit = &job.queryWorkUnit().lock();
                Owned<IPropertyTreeIterator> fileIter = &workunit->getFileIterator();
                bool found=false;
                ForEach (*fileIter)
                {
                    if (0 == stricmp(logicalName, fileIter->query().queryProp("@name")))
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                    workunit->releaseFile(logicalName);
            }
            if (efile.get())
            {
                __int64 fs = efile->getFileSize(false,false);
                StringArray clusters;
                unsigned c=0;
                for (; c<efile->numClusters(); c++)
                {
                    StringBuffer clusterName;
                    efile->getClusterName(c, clusterName);
                    clusters.append(clusterName);
                }
                remove(job, *efile, job.queryMaxLfnBlockTimeMins() * 60000);
                efile.clear();
                efile.setown(timedLookup(job, dlfn, AccessMode::tbdWrite, true, job.queryMaxLfnBlockTimeMins() * 60000));
                if (!efile.get())
                {
                    ForEachItemIn(c, clusters)
                    {
                        LOG(MCauditInfo,",FileAccess,Thor,DELETED,%s,%s,%s,%s,%s,%" I64F "d,%s",
                                        globals->queryProp("@name"),
                                        userStr.str(),
                                        logicalName,
                                        wuidStr.str(),
                                        job.queryGraphName(),fs,clusters.item(c));
                    }
                }
            }
        }
        Owned<IFileDescriptor> desc;
        if (!temporary && dlfn.isExternal())
            desc.setown(createExternalFileDescriptor(dlfn.get()));
        else
        {
            desc.setown(createFileDescriptor());
            if (temporary)
                desc->queryProperties().setPropBool("@temporary", temporary);
            else
                desc->setTraceName(logicalName);
            if (persistent)
                desc->queryProperties().setPropBool("@persistent", persistent);
            desc->queryProperties().setProp("@workunit", wuidStr.str());
            desc->queryProperties().setProp("@job", jobStr.str());
            desc->queryProperties().setProp("@owner", userStr.str());

            unsigned total;
            if (restrictedWidth)
                total = restrictedWidth;
            else if (job.getOptBool("legacyLFNTargetWidth"))
            {
                unsigned offset; // set by fixTotal (always 0), but never used.
                total = fixTotal(job, groups, offset);
            }
            else
                total = job.querySlaves();
            if (nonLocalIndex)
                ++total;
            StringBuffer dir;
            bool dirPerPart = false;
            if (temporary && !job.queryUseCheckpoints()) 
                dir.append(queryTempDir());
            else
            {
                StringBuffer planeDir;
                // NB: always >= 1 groupNames
                ForEachItemIn(gn, groupNames)
                {
                    Owned<const IStoragePlane> plane = getDataStoragePlane(groupNames.item(gn), true);
                    StringBuffer thisPlaneDir(plane->queryPrefix());
                    bool thisDirPerPart = plane->queryDirPerPart();
                    if (!planeDir.length()) // 1st output plane
                    {
                        planeDir.swapWith(thisPlaneDir);
                        dirPerPart = thisDirPerPart;
                    }
                    else
                    {
                        // 2nd+ output plane
                        if (!streq(thisPlaneDir, planeDir))
                            throw makeStringException(0, "When targeting multiple clusters on a write, the clusters must have the same target directory");
                        if (thisDirPerPart != dirPerPart)
                            throw makeStringException(0, "When targeting multiple clusters on a write, all clusters must have the same subDirPerFilePart value");
                    }
                }
                // places logical filename directory in 'dir'
                getLFNDirectoryUsingBaseDir(dir, logicalName, planeDir.str());
            }
            desc->setDefaultDir(dir.str());

            if (job.getOptBool("subDirPerFilePart", dirPerPart) && total>1)
                desc->setFlags(FileDescriptorFlags::dirperpart);

            StringBuffer partmask;
            getPartMask(partmask,logicalName,total);
            desc->setNumParts(total);
            desc->setPartMask(partmask);
            // desc->setPartOffset(offset); // possible future requirement

            ForEachItemIn(g, groups)
            {
                ClusterPartDiskMapSpec mspec;
                mspec.defaultCopies = replicate?DFD_DefaultCopies:DFD_NoCopies; // may be changed on publish to reflect always backed up on thor cluster
                const char *groupname = groupNames.item(g);
                if (groupname && *groupname)
                    desc->addCluster(groupname, &groups.item(g), mspec);
                else
                    desc->addCluster(&groups.item(g), mspec);
            }
        }
        if (!temporary && !jobTemp)
            job.addCreatedFile(logicalName);
        return LINK(desc);
    }

    void publish(CJobBase &job, const char *logicalName, IFileDescriptor &fileDesc, Owned<IDistributedFile> *publishedFile=NULL)
    {
        IPropertyTree &props = fileDesc.queryProperties();
        bool temporary = props.getPropBool("@temporary");
        if (!temporary || job.queryUseCheckpoints())
            queryDistributedFileDirectory().removeEntry(logicalName, job.queryUserDescriptor());
        // thor clusters are backed up so if replicateOutputs set *always* assume a replicate
        if (replicateOutputs && (!temporary || job.queryUseCheckpoints()))
        {
            // this potentially modifies fileDesc but I think OK at this point
           fileDesc.ensureReplicate();
        }
        Owned<IDistributedFile> file = queryDistributedFileDirectory().createNew(&fileDesc);
        if (temporary && !job.queryUseCheckpoints())
        {
            fileMap.replace(*new CIDistributeFileMapping(logicalName, *LINK(file))); // cache takes ownership
            return;
        }
        offset_t fs = file->getDiskSize(false, false);
        if (publishedFile)
            publishedFile->set(file);
        file->attach(logicalName, job.queryUserDescriptor());
        unsigned c=0;
        for (; c<fileDesc.numClusters(); c++)
        {
            StringBuffer clusterName;
            fileDesc.getClusterGroupName(c, clusterName, &queryNamedGroupStore());
            LOG(MCauditInfo,",FileAccess,Thor,CREATED,%s,%s,%s,%s,%s,%" I64F "d,%s",
                            globals->queryProp("@nodeGroup"),
                            job.queryUser(),
                            file->queryLogicalName(),
                            job.queryWuid(),
                            job.queryGraphName(),
                            fs,clusterName.str());
        }
    }

    StringBuffer &getPublishPhysicalName(CJobBase &job, const char *logicalName, unsigned partno, StringBuffer &res)
    {
        return _getPublishPhysicalName(job, logicalName, partno, globals->queryProp("@nodeGroup"), &queryDfsGroup(), res);
    }

    StringBuffer &getPhysicalName(CJobBase &job, const char *logicalName, unsigned partno, StringBuffer &res)
    {
        return _getPublishPhysicalName(job, logicalName, partno, NULL, NULL, res);
    }

    unsigned __int64 getFileOffset(CJobBase &job, const char *logicalName, unsigned partno)
    {
        Owned<IDistributedFile> file = lookup(job, logicalName, AccessMode::readMeta, false, false, false, defaultPrivilegedUser);
        StringBuffer scopedName;
        addScope(job, logicalName, scopedName);
        if (!file)
            throw MakeThorException(TE_LogicalFileNotFound, "getFileOffset: Logical file doesn't exist (%s)", scopedName.str());

        if (partno >= file->numParts())
            throw MakeThorException(TE_NoSuchPartForLogicalFile , "No such part number (%d) for logical file : %s", partno, scopedName.str());
        Owned<IDistributedFilePart> part = file->getPart(partno);
        return part->queryAttributes().getPropInt64("@offset");;
    }
    virtual bool scanLogicalFiles(CJobBase &job, const char *_pattern, StringArray &results)
    {
        if (strcspn(_pattern, "*?") == strlen(_pattern))
        {
            results.append(_pattern);
            return true;
        }
        StringBuffer pattern;
        addScope(job,_pattern,pattern,false);
        return scanDFS(pattern.str(),job.queryScope(),results);
    }
};

void initFileManager()
{
    assertex(!fileManager);
    fileManager = new CFileManager;
}

IThorFileManager &queryThorFileManager()
{
    return *fileManager;
}

void configureFileDescriptor(const char *logicalName, IFileDescriptor &fileDesc)
{
    unsigned __int64 offset = 0;
    Owned<IPartDescriptorIterator> iter = fileDesc.getIterator();
    bool noSize = false;
    ForEach (*iter)
    {
        IPartDescriptor &part = iter->query();
        IPropertyTree &props = part.queryProperties();
        if (!noSize) // don't want to set if failed do we?
            props.setPropInt64("@offset", offset);
        if (props.hasProp("@size"))
        {
            if (noSize)
                IWARNLOG("Some parts of logical file \"%s\" have sizes others do not!", logicalName);
            else
                offset += props.getPropInt64("@size");
        }
        else
            noSize = true;
    }
}

IFileDescriptor *getConfiguredFileDescriptor(IDistributedFile &file)
{
    Owned<IFileDescriptor> fileDesc = file.getFileDescriptor();
    Owned<IDistributedFilePartIterator> iter = file.getIterator();
    unsigned partn = 0;
    // ensure @size's present as some activities rely upon e.g. @offset's
    // NH->JCS queries won't have a size here - does this matter?
    ForEach (*iter)
    {
        IDistributedFilePart &part = iter->query();
        IPartDescriptor *partDesc = fileDesc->queryPart(partn);
        try {
            offset_t sz = part.getFileSize(true, false);             
            partDesc->queryProperties().setPropInt64("@size", sz);
        }
        catch (IDFS_Exception *e) {
            if (e->errorCode()!=DFSERR_CannotFindPartFileSize)
                throw;
            e->Release();
        }
        partn++;
    }
    configureFileDescriptor(file.queryLogicalName(), *fileDesc);
    return LINK(fileDesc);
}

unsigned getGroupOffset(IGroup &fileGroup, IGroup &group)
{
    unsigned fileWidth = fileGroup.ordinality();

    unsigned offset = 0;
    if (fileWidth > group.ordinality()) // file wider than the cluster, fileWidth-clusterWidth parts will be blank
    {
        const char *wideDestOptStr = globals->queryProp("@wideDestOpt");
        // default to "wideMiddle"
        if (NULL == wideDestOptStr || 0 == stricmp("wideMiddle", wideDestOptStr)) // try to fill file parts that overlap this cluster
        {
            GroupRelation relation = group.compare(&fileGroup);
            if (GRbasesubset == relation)
            {
                offset = fileGroup.rank(&group.queryNode(0));
                assertex(RANK_NULL != offset);
            }
        }
    }
    return offset;
}

void fillClusterArray(CJobBase &job, const char *filename, StringArray &clusters, IArrayOf<IGroup> &groups)
{
    if (!clusters.ordinality())
    {
        groups.append(*LINK(&queryDfsGroup()));
        clusters.append(globals->queryProp("@nodeGroup"));
    }
    else
    {
        const char *cluster = clusters.item(0);
        Owned<IGroup> group = queryNamedGroupStore().lookup(cluster);
        if (!group)
            throw MakeStringException(0, "Could not find cluster group %s for file: %s", cluster, filename);
#ifndef _CONTAINERIZED
        EnvMachineOS os = queryOS(group->queryNode(0).endpoint());
#endif
        unsigned clusterIdx = 1;
        for (;;)
        {
            groups.append(*LINK(group));
            if (clusterIdx>=clusters.ordinality())
                break;
            cluster = clusters.item(clusterIdx++);
            group.setown(queryNamedGroupStore().lookup(cluster));
            if (!group)
                throw MakeStringException(0, "Could not find cluster group %s for file: %s", cluster, filename);
#ifndef _CONTAINERIZED
            if (MachineOsUnknown != os)
            {
                EnvMachineOS thisOs = queryOS(group->queryNode(0).endpoint());
                if (MachineOsUnknown != thisOs && thisOs != os)
                    throw MakeStringException(0, "UNSUPPORTED: multiple clusters with different target OS's. File: %s", filename);
            }
            // check for overlap
            ForEachItemIn(g,groups)
            {
                IGroup &agrp = groups.item(g);
                if (GRdisjoint != agrp.compare(group))
                    throw MakeStringException(0, "Target cluster '%s', overlaps with target cluster '%s'", clusters.item(clusterIdx-1), clusters.item(g));
            }
#endif
        }
    }
}

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    return true;
}

MODULE_EXIT()
{
    ::Release(fileManager);
}

