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

#include "jiface.hpp"
#include "dadfs.hpp"

#include "thorfile.hpp"

#include "eclhelper.hpp"
#include "thexception.hpp"
#include "thkeydiff.ipp"

class CKeyDiffMaster : public CMasterActivity
{
    IHThorKeyDiffArg *helper;
    Owned<IDistributedFile> originalIndexFile, newIndexFile;
    bool local, copyTlk;
    unsigned width;
    Owned<IFileDescriptor> patchDesc, originalDesc, newIndexDesc;
    StringArray clusters;
    StringAttr originalName, updatedName, outputName;

public:
    CKeyDiffMaster(CMasterGraphElement *info) : CMasterActivity(info)
    {
        helper = (IHThorKeyDiffArg *)queryHelper();
        local = false;
        width = 0;
        copyTlk = globals->getPropBool("@diffCopyTlk", true); // because tlk can have meta data and diff/patch does not support
    }
    virtual void init()
    {
        CMasterActivity::init();
        OwnedRoxieString originalHelperName(helper->getOriginalName());
        OwnedRoxieString updatedHelperName(helper->getUpdatedName());
        OwnedRoxieString outputHelperName(helper->getOutputName());
        StringBuffer expandedFileName;
        queryThorFileManager().addScope(container.queryJob(), originalHelperName, expandedFileName, false);
        originalName.set(expandedFileName);
        queryThorFileManager().addScope(container.queryJob(), updatedHelperName, expandedFileName.clear(), false);
        updatedName.set(expandedFileName);
        queryThorFileManager().addScope(container.queryJob(), outputHelperName, expandedFileName.clear(), false);
        outputName.set(expandedFileName);

        originalIndexFile.setown(lookupReadFile(originalHelperName, AccessMode::readRandom, false, false, false));
        newIndexFile.setown(lookupReadFile(updatedHelperName, AccessMode::readRandom, false, false, false));
        if (!isFileKey(originalIndexFile))
            throw MakeActivityException(this, ENGINEERR_FILE_TYPE_MISMATCH, "Attempting to read flat file as an index: %s", originalHelperName.get());
        if (!isFileKey(newIndexFile))
            throw MakeActivityException(this, ENGINEERR_FILE_TYPE_MISMATCH, "Attempting to read flat file as an index: %s", updatedHelperName.get());
        if (originalIndexFile->numParts() != newIndexFile->numParts())
            throw MakeActivityException(this, TE_KeyDiffIndexSizeMismatch, "Index %s and %s differ in width", originalName.get(), updatedName.get());
        if (originalIndexFile->querySuperFile() || newIndexFile->querySuperFile())
            throw MakeActivityException(this, 0, "Diffing super files not supported");  

        width = originalIndexFile->numParts();

        originalDesc.setown(originalIndexFile->getFileDescriptor());
        newIndexDesc.setown(newIndexFile->getFileDescriptor());
        local = !hasTLK(*originalIndexFile, this);
        if (!local)
            width--; // 1 part == No n distributed / Monolithic key
        if (width > container.queryJob().querySlaves())
            throw MakeActivityException(this, 0, "Unsupported: keydiff(%s, %s) - Cannot diff a key that's wider(%d) than the target cluster size(%d)", originalIndexFile->queryLogicalName(), newIndexFile->queryLogicalName(), width, container.queryJob().querySlaves());

        StringBuffer defaultCluster;
        if (getDefaultIndexBuildStoragePlane(defaultCluster))
            clusters.append(defaultCluster);
        IArrayOf<IGroup> groups;
        fillClusterArray(container.queryJob(), outputName, clusters, groups);
        patchDesc.setown(queryThorFileManager().create(container.queryJob(), outputName, clusters, groups, 0 != (KDPoverwrite & helper->getFlags()), 0, !local, width));
        patchDesc->queryProperties().setProp("@kind", "keydiff");
    }
    virtual void serializeSlaveData(MemoryBuffer &dst, unsigned slave)
    {
        if (slave < width) // if false - due to mismatch width fitting - fill in with a blank entry
        {
            dst.append(true);
            Owned<IPartDescriptor> originalPartDesc = originalDesc->getPart(slave);
            originalPartDesc->serialize(dst);
            Owned<IPartDescriptor> newIndexPartDesc = newIndexDesc->getPart(slave);
            newIndexPartDesc->serialize(dst);
            patchDesc->queryPart(slave)->serialize(dst);
            
            if (0 == slave)
            {
                if (!local)
                {
                    dst.append(true);
                    Owned<IPartDescriptor> originalTlkPartDesc = originalDesc->getPart(originalDesc->numParts()-1);
                    originalTlkPartDesc->serialize(dst);                                        
                    Owned<IPartDescriptor> newIndexTlkPartDesc = newIndexDesc->getPart(newIndexDesc->numParts()-1);
                    newIndexTlkPartDesc->serialize(dst);
                    patchDesc->queryPart(patchDesc->numParts()-1)->serialize(dst);
                }
                else
                    dst.append(false);
            }
        }
        else
            dst.append(false); // no part
    }
    virtual void slaveDone(size32_t slaveIdx, MemoryBuffer &mb)
    {
        if (mb.length()) // if 0 implies aborted out from this slave.
        {
            offset_t size;
            mb.read(size);
            CDateTime modifiedTime(mb);

            IPartDescriptor *partDesc = patchDesc->queryPart(slaveIdx);
            IPropertyTree &props = partDesc->queryProperties();         
            StringBuffer timeStr;
            modifiedTime.getString(timeStr);
            props.setProp("@modified", timeStr.str());
            unsigned crc;
            mb.read(crc);
            props.setPropInt64("@fileCrc", crc);
            if (!local && 0 == slaveIdx)
            {
                IPartDescriptor *partDesc = patchDesc->queryPart(patchDesc->numParts()-1);
                IPropertyTree &props = partDesc->queryProperties();
                mb.read(size);
                props.setPropInt64("@size", size);
                CDateTime modifiedTime(mb);
                StringBuffer timeStr;
                modifiedTime.getString(timeStr);
                props.setProp("@modified", timeStr.str());
                if (copyTlk)
                {
                    Owned<IPartDescriptor> tlkDesc = newIndexDesc->getPart(newIndexDesc->numParts()-1);
                    if (partDesc->getCrc(crc))
                        props.setPropInt64("@fileCrc", crc);
                    props.setProp("@diffFormat", "copy");
                }
                else
                {
                    mb.read(crc);
                    props.setPropInt64("@fileCrc", crc);
                    props.setProp("@diffFormat", "diffV1");
                }
            }
        }
    }
    virtual void done()
    {
        Owned<IWorkUnit> wu = &container.queryJob().queryWorkUnit().lock();
        Owned<IWUResult> r = wu->updateResultBySequence(helper->getSequence());
        //Do not mark the result as calculated - because the patch file isn't a valid result
        r->setResultLogicalName(outputName);
        r.clear();
        wu.clear();

        IPropertyTree &patchProps = patchDesc->queryProperties();
        if (0 != (helper->getFlags() & KDPexpires))
            setExpiryTime(patchProps, helper->getExpiryDays());
        IPropertyTree &originalProps = originalDesc->queryProperties();;
        if (originalProps.queryProp("ECL"))
            patchProps.setProp("ECL", originalProps.queryProp("ECL"));
        if (originalProps.getPropBool("@local"))
            patchProps.setPropBool("@local", true);
        container.queryTempHandler()->registerFile(outputName, container.queryOwner().queryGraphId(), 0, false, WUFileStandard, &clusters);
        Owned<IDistributedFile> patchFile;
        // set part sizes etc
        queryThorFileManager().publish(container.queryJob(), outputName, *patchDesc, &patchFile);
        try // set file size
        {
            if (patchFile)
            {
                __int64 fs = patchFile->getFileSize(true,false);
                if (fs!=-1)
                    patchFile->queryAttributes().setPropInt64("@size",fs);
            }
        }
        catch (IException *e)
        {
            IERRLOG(e, "keydiff setting file size");
            e->Release();
        }
        // Add a new 'Patch' description to the secondary key.
        DistributedFilePropertyLock lock(newIndexFile);
        IPropertyTree &fileProps = lock.queryAttributes();
        StringBuffer path("Patch[@name=\"");
        path.append(outputName).append("\"]");
        IPropertyTree *patch = fileProps.queryPropTree(path.str());
        if (!patch) patch = fileProps.addPropTree("Patch", createPTree());
        patch->setProp("@name", outputName);
        unsigned checkSum;
        if (patchFile->getFileCheckSum(checkSum))
            patch->setPropInt64("@checkSum", checkSum);

        IPropertyTree *index = patch->setPropTree("Index", createPTree());
        index->setProp("@name", originalIndexFile->queryLogicalName());
        if (originalIndexFile->getFileCheckSum(checkSum))
            index->setPropInt64("@checkSum", checkSum);
        CMasterActivity::done();
    }
    virtual void preStart(size32_t parentExtractSz, const byte *parentExtract)
    {
        CMasterActivity::preStart(parentExtractSz, parentExtract);
        IHThorKeyDiffArg *helper = (IHThorKeyDiffArg *) queryHelper();
        if (0==(KDPoverwrite & helper->getFlags()))
        {
            if (KDPvaroutputname & helper->getFlags()) return;
            OwnedRoxieString outputHelperName(helper->getOutputName());
            Owned<IDistributedFile> file = queryThorFileManager().lookup(container.queryJob(), outputHelperName, AccessMode::readMeta, false, true, false, container.activityIsCodeSigned());
            if (file)
                throw MakeActivityException(this, TE_OverwriteNotSpecified, "Cannot write %s, file already exists (missing OVERWRITE attribute?)", file->queryLogicalName());
        }
    }
    virtual void kill()
    {
        CMasterActivity::kill();
        originalIndexFile.clear();
        newIndexFile.clear();
    }
};

CActivityBase *createKeyDiffActivityMaster(CMasterGraphElement *container)
{
    return new CKeyDiffMaster(container);
}
