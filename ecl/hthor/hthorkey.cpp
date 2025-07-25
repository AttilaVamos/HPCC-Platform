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
#include "hthor.ipp"
#include "rtlkey.hpp"
#include "jhtree.hpp"
#include "eclhelper.hpp"
#include "jthread.hpp"
#include "jqueue.tpp"
#include "dasess.hpp"
#include "thorxmlwrite.hpp"
#include "thorstep.ipp"
#include "roxiedebug.hpp"
#include "thorcommon.hpp"
#include "rtldynfield.hpp"
#include "thorfile.hpp"
#include "jstats.h"

#define MAX_FETCH_LOOKAHEAD 1000

using roxiemem::IRowManager;
using roxiemem::OwnedRoxieRow;
using roxiemem::OwnedConstRoxieRow;
using roxiemem::OwnedRoxieString;

class TransformCallback : public CInterface, implements IThorIndexCallback 
{
public:
    TransformCallback() { keyManager = NULL; };
    IMPLEMENT_IINTERFACE_O

//IThorIndexCallback
    virtual const byte * lookupBlob(unsigned __int64 id) override
    { 
        size32_t dummy; 
        return (byte *) keyManager->loadBlob(id, dummy, ctx); 
    }

public:
    void setManager(IKeyManager * _manager, IContextLogger * _ctx)
    {
        finishedRow();
        keyManager = _manager;
        ctx = _ctx;
    }

    void finishedRow()
    {
        if (keyManager)
            keyManager->releaseBlobs(); 
    }

protected:
    IKeyManager * keyManager = nullptr;
    IContextLogger * ctx = nullptr;
};


//-------------------------------------------------------------------------------------------------------------

static ILocalOrDistributedFile *resolveLFNIndex(IAgentContext &agent, const char *logicalName, const char *errorTxt, bool optional, bool noteRead, AccessMode accessMode, bool isPrivilegedUser)
{
    Owned<ILocalOrDistributedFile> ldFile = agent.resolveLFN(logicalName, errorTxt, optional, noteRead, accessMode, nullptr, isPrivilegedUser);
    if (!ldFile)
        return nullptr;
    IDistributedFile *dFile = ldFile->queryDistributedFile();
    if (dFile && !isFileKey(dFile))
        throw MakeStringException(0, "Attempting to read flat file as an index: %s", logicalName);
    return ldFile.getClear();
}


void enterSingletonSuperfiles(Shared<IDistributedFile> & file)
{
    IDistributedSuperFile * super = file->querySuperFile();
    while(super && (super->numSubFiles() == 1))
    {
        file.setown(super->getSubFile(0));
        super = file->querySuperFile();
    }
}

//-------------------------------------------------------------------------------------------------------------

class CHThorNullAggregateActivity : public CHThorNullActivity
{
public:
    CHThorNullAggregateActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorArg & _arg, IHThorCompoundAggregateExtra &_extra, ThorActivityKind _kind, EclGraph & _graph) : CHThorNullActivity(agent, _activityId, _subgraphId, _arg, _kind, _graph), helper(_extra) {}

    //interface IHThorInput
    virtual void ready();
    virtual const void *nextRow();
    virtual bool needsAllocator() const { return true; }

protected:
    IHThorCompoundAggregateExtra &helper;
    bool finished;
};


void CHThorNullAggregateActivity::ready()
{
    CHThorNullActivity::ready();
    finished = false;
}

const void *CHThorNullAggregateActivity::nextRow()
{
    if (finished) return NULL;

    processed++;
    finished = true;
    RtlDynamicRowBuilder rowBuilder(rowAllocator);
    try
    {
        size32_t newSize = helper.clearAggregate(rowBuilder);
        return rowBuilder.finalizeRowClear(newSize);
    }
    catch(IException * e)
    {
        throw makeWrappedException(e);
    }
}

//-------------------------------------------------------------------------------------------------------------

class CHThorNullCountActivity : public CHThorNullActivity
{
public:
    CHThorNullCountActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorArg & _arg, ThorActivityKind _kind, EclGraph & _graph)
        : CHThorNullActivity(agent, _activityId, _subgraphId, _arg, _kind, _graph), finished(false) {}

    //interface IHThorInput
    virtual void ready();
    virtual const void *nextRow();
    virtual bool needsAllocator() const { return true; }

protected:
    bool finished;
};


void CHThorNullCountActivity::ready()
{
    CHThorNullActivity::ready();
    finished = false;
}

const void *CHThorNullCountActivity::nextRow()
{
    if (finished) return NULL;

    processed++;
    finished = true;

    size32_t outSize = outputMeta.getFixedSize();
    void * ret = rowAllocator->createRow(); //meta: outputMeta
    if (outSize == 1)
        *(byte *)ret = 0;
    else
        *(unsigned __int64 *)ret = 0;
    return rowAllocator->finalizeRow(outSize, ret, outSize);
}


//-------------------------------------------------------------------------------------------------------------


class CHThorIndexReadActivityBase : public CHThorActivityBase
{

public:
    CHThorIndexReadActivityBase(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexReadBaseArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);
    ~CHThorIndexReadActivityBase();

    virtual void ready();
    virtual void stop();
    IHThorInput *queryOutput(unsigned index) { return this; }
    virtual bool needsAllocator() const { return true; }

    //interface IHThorInput
    virtual bool isGrouped()                { return false; }
    virtual const char *getFileName()       { return NULL; }
    virtual bool outputToFile(const char *) { return false; } 
    virtual IOutputMetaData * queryOutputMeta() const { return outputMeta; }

    virtual void updateProgress(IStatisticGatherer &progress) const
    {
        CHThorActivityBase::updateProgress(progress);
        StatsActivityScope scope(progress, activityId);
        contextLogger.recordStatistics(progress);
        progress.addStatistic(StNumPostFiltered, queryPostFiltered());
    }

    virtual unsigned queryPostFiltered() const
    {
        return postFiltered;
    }

    virtual void fail(char const * msg)
    {
        throw MakeStringExceptionDirect(0, msg);
    }

protected:
    bool doPreopenLimit(unsigned __int64 limit);
    bool doPreopenLimitFile(unsigned __int64 & count, unsigned __int64 limit);
    IKeyIndex * doPreopenLimitPart(unsigned __int64 & count, unsigned __int64 limit, unsigned part);
    const void * createKeyedLimitOnFailRow();
    void getLayoutTranslators();
    const IDynamicTransform * getLayoutTranslator(IDistributedFile * f);
    void verifyIndex(IKeyIndex * idx);
    void initManager(IKeyManager *manager, bool isTlk);
    bool firstPart();
    virtual bool nextPart();
    virtual void initPart();
    void resolveIndexFilename();
    void killPart();

private:
    bool firstMultiPart();
    bool nextMultiPart();
    bool setCurrentPart(unsigned whichPart);
    void clearTlk()                                         { tlk.clear(); tlManager.clear(); }
    void openTlk();
    bool doNextSuper();

protected:
    IHThorIndexReadBaseArg &helper;
    IHThorSourceLimitTransformExtra * limitTransformExtra;
    CachedOutputMetaData eclKeySize;
    size32_t keySize= 0;

// current part
    Owned<IDistributedFilePart> curPart;
    Owned<IKeyManager> klManager;
    Owned<IKeyIndex> keyIndex;
    unsigned nextPartNumber = 0;

//multi files
    Owned<IDistributedFile> df;
    Owned<IKeyIndex> tlk;
    Owned<IKeyManager> tlManager;

//super files:
    Owned<IDistributedFileIterator> superIterator;
    unsigned superIndex = 0;
    unsigned superCount = 1;
    StringBuffer superName;

    TransformCallback callback;

//for preopening (when need counts for keyed skip limit):
    Owned<IKeyIndexSet> keyIndexCache;
    UnsignedArray superIndexCache;
    unsigned keyIndexCacheIdx = 0;

    unsigned postFiltered;
    CStatsContextLogger contextLogger;
    bool singlePart = false;                // a single part index, not part of a super file - optimize so never reload the part.
    bool localSortKey = false;
    bool initializedFileInfo = false;

//for layout translation
    Owned<const IDynamicTransform> layoutTrans;
    IConstPointerArrayOf<IDynamicTransform> layoutTransArray;
    IPointerArrayOf<IOutputMetaData> actualLayouts;
    RecordTranslationMode recordTranslationModeHint = RecordTranslationMode::Unspecified;

    RecordTranslationMode getLayoutTranslationMode()
    {
        if (recordTranslationModeHint != RecordTranslationMode::Unspecified)
            return recordTranslationModeHint;
        return agent.getLayoutTranslationMode();
    }


};

CHThorIndexReadActivityBase::CHThorIndexReadActivityBase(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexReadBaseArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
    : CHThorActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph), helper(_arg), contextLogger(jhtreeCacheStatistics)
{
    nextPartNumber = 0;

    eclKeySize.set(helper.queryDiskRecordSize());

    postFiltered = 0;
    helper.setCallback(&callback);
    limitTransformExtra = nullptr;
    if (_node)
    {
        const char *recordTranslationModeHintText = _node->queryProp("hint[@name='layouttranslation']/@value");
        if (recordTranslationModeHintText)
            recordTranslationModeHint = getTranslationMode(recordTranslationModeHintText, true);
    }
}

CHThorIndexReadActivityBase::~CHThorIndexReadActivityBase()
{
//  ReleaseRoxieRow(recBuffer);
}

void CHThorIndexReadActivityBase::ready()
{
    CHThorActivityBase::ready();
    if(!initializedFileInfo || (helper.getFlags() & TIRvarfilename))
    {
        resolveIndexFilename();
        layoutTransArray.kill();
        getLayoutTranslators();
        initializedFileInfo = true;
    }
}

void CHThorIndexReadActivityBase::resolveIndexFilename()
{
    // A logical filename for the key should refer to a single physical file - either the TLK or a monolithic key
    OwnedRoxieString lfn(helper.getFileName());
    Owned<ILocalOrDistributedFile> ldFile = resolveLFNIndex(agent, lfn, "IndexRead", 0 != (helper.getFlags() & TIRoptional),true, AccessMode::tbdRead, defaultPrivilegedUser);
    df.set(ldFile ? ldFile->queryDistributedFile() : NULL);
    if (!df)
    {
        StringBuffer buff;
        buff.append("Skipping OPT index read of nonexistent file ").append(lfn);
        agent.addWuExceptionEx(buff.str(), WRN_SkipMissingOptIndex, SeverityInformation, MSGAUD_user, "hthor");
    }
    else
    {
        agent.logFileAccess(df, "HThor", "READ", graph);
        enterSingletonSuperfiles(df);

        singlePart = false;
        localSortKey = (df->queryAttributes().hasProp("@local"));
        IDistributedSuperFile *super = df->querySuperFile();
        superCount = 1;
        superIndex = 0;
        nextPartNumber = 0;
        if (super)
        {
            superIterator.setown(super->getSubFileIterator(true));
            superCount = super->numSubFiles(true);
            if (helper.getFlags() & TIRsorted)
                throw MakeStringException(1000, "SORTED attribute is not supported when reading from superkey");
            superName.append(df->queryLogicalName());
            df.clear();
        }
        else if (df->numParts() == 1)
        {
            singlePart = true;
        }
    }
    killPart();
}

void CHThorIndexReadActivityBase::stop()
{ 
    killPart(); 
    CHThorActivityBase::stop(); 
}

bool CHThorIndexReadActivityBase::doPreopenLimit(unsigned __int64 limit)
{
    keyIndexCache.clear();
    superIndexCache.kill();
    if(!helper.canMatchAny())
        return false;
    keyIndexCache.setown(createKeyIndexSet());
    unsigned __int64 count = 0;
    if(superIterator)
    {
        superIterator->first();
        superIndex = 0;
        do
        {
            df.set(&superIterator->query());
            if(doPreopenLimitFile(count, limit))
                return true;
            ++superIndex;
        } while(superIterator->next());
        return false;
    }
    else
    {
        return doPreopenLimitFile(count, limit);
    }
}

bool CHThorIndexReadActivityBase::doPreopenLimitFile(unsigned __int64 & count, unsigned __int64 limit)
{
    unsigned num = df->numParts()-1;
    if(num)
    {
        if(localSortKey)
        {
            // MORE - partition support goes here
            Owned<IKeyIndex> tlk = openKeyFile(df->queryPart(num));
            verifyIndex(tlk);
            for(unsigned idx = 0; idx < num; ++idx)
            {
                keyIndexCache->addIndex(doPreopenLimitPart(count, limit, idx));
                if(superIterator)
                    superIndexCache.append(superIndex);
            }
        }
        else
        {
            Owned<IKeyIndex> tlk = openKeyFile(df->queryPart(num));
            verifyIndex(tlk);
            Owned<IKeyManager> tlman = createLocalKeyManager(eclKeySize.queryRecordAccessor(true), tlk, &contextLogger, helper.hasNewSegmentMonitors(), false);
            initManager(tlman, true);
            while(tlman->lookup(false) && (count<=limit))
            {
                unsigned slavePart = (unsigned)extractFpos(tlman);
                if (slavePart)
                {
                    keyIndexCache->addIndex(doPreopenLimitPart(count, limit, slavePart-1));
                    if(superIterator)
                        superIndexCache.append(superIndex);
                }
            }
            if (count>limit)
            {
                if ( agent.queryCodeContext()->queryDebugContext())
                    agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
            }
        }
    }
    else
    {
        keyIndexCache->addIndex(doPreopenLimitPart(count, limit, 0));
        if(superIterator)
            superIndexCache.append(superIndex);
    }
    return (count>limit);
}

IKeyIndex * CHThorIndexReadActivityBase::doPreopenLimitPart(unsigned __int64 & result, unsigned __int64 limit, unsigned part)
{
    Owned<IKeyIndex> kidx;
    kidx.setown(openKeyFile(df->queryPart(part)));
    if(df->numParts() == 1)
        verifyIndex(kidx);
    if (limit != (unsigned) -1)
    {
        Owned<IKeyManager> kman = createLocalKeyManager(eclKeySize.queryRecordAccessor(true), kidx, &contextLogger, helper.hasNewSegmentMonitors(), false);
        initManager(kman, false);
        result += kman->checkCount(limit-result);
    }
    return kidx.getClear();
}

void CHThorIndexReadActivityBase::openTlk()
{
    tlk.setown(openKeyFile(df->queryPart(df->numParts()-1)));
}


const void * CHThorIndexReadActivityBase::createKeyedLimitOnFailRow()
{
    RtlDynamicRowBuilder rowBuilder(rowAllocator);
    size32_t newSize = limitTransformExtra->transformOnKeyedLimitExceeded(rowBuilder);
    if (newSize)
        return rowBuilder.finalizeRowClear(newSize);
    return NULL;
}

bool CHThorIndexReadActivityBase::firstPart()
{
    killPart();
    if ((df || superIterator) && helper.canMatchAny())
    {
        if(keyIndexCache)
        {
            keyIndexCacheIdx = 0;
            return nextPart();
        }

        if (singlePart)
        {
            //part is cached and not reloaded - for efficiency in subqueries.
            if (!keyIndex)
                return setCurrentPart(0);
            initPart();
            return true;
        }

        if (superIterator)
        {
            superIterator->first();
            superIndex = 0;
            return doNextSuper();
        }
        else
            return firstMultiPart();
    }
    return false;
}


bool CHThorIndexReadActivityBase::nextPart()
{
    killPart();
    if(keyIndexCache)
    {
        if(keyIndexCacheIdx >= keyIndexCache->numParts())
            return false;
        keyIndex.set(keyIndexCache->queryPart(keyIndexCacheIdx));
        if(superIterator)
        {
            superIndex = superIndexCache.item(keyIndexCacheIdx);
            layoutTrans.set(layoutTransArray.item(superIndex));
            keySize = keyIndex->keySize();
        }
        ++keyIndexCacheIdx;
        initPart();
        return true;
    }

    if (singlePart)
        return false;

    if (nextMultiPart())
        return true;

    if (superIterator && superIterator->next())
    {
        ++superIndex;
        return doNextSuper();
    }

    return false;
}


void CHThorIndexReadActivityBase::initManager(IKeyManager *manager, bool isTlk)
{
    if(layoutTrans && !isTlk)
        manager->setLayoutTranslator(layoutTrans);
    helper.createSegmentMonitors(manager);
    manager->finishSegmentMonitors();
    manager->reset();
}

void CHThorIndexReadActivityBase::initPart()
{
    assertex(!keyIndex->isTopLevelKey());
    klManager.setown(createLocalKeyManager(eclKeySize.queryRecordAccessor(true), keyIndex, &contextLogger, helper.hasNewSegmentMonitors(), false));
    initManager(klManager, false);
    callback.setManager(klManager, nullptr);
}

void CHThorIndexReadActivityBase::killPart()
{
    callback.setManager(nullptr, nullptr);
    if (klManager)
        klManager.clear();
}

bool CHThorIndexReadActivityBase::setCurrentPart(unsigned whichPart)
{
    IDistributedFilePart &part = df->queryPart(whichPart);
    size32_t blockedSize = 0;
    if (!helper.hasSegmentMonitors()) // unfiltered
    {
        StringBuffer planeName;
        df->getClusterName(part.copyClusterNum(0), planeName);
        blockedSize = getBlockedFileIOSize(planeName);
    }
    keyIndex.setown(openKeyFile(part, blockedSize));
    if(df->numParts() == 1)
        verifyIndex(keyIndex);
    initPart();
    return true;
}

bool CHThorIndexReadActivityBase::firstMultiPart()
{
    if(!tlk)
        openTlk();
    verifyIndex(tlk);
    tlManager.setown(createLocalKeyManager(eclKeySize.queryRecordAccessor(true), tlk, &contextLogger, helper.hasNewSegmentMonitors(), false));
    initManager(tlManager, true);
    nextPartNumber = 0;
    return nextMultiPart();
}

bool CHThorIndexReadActivityBase::nextMultiPart()
{
    //tlManager may be null for a single part index within a superfile.
    if (tlManager)
    {
        if (localSortKey)
        {
            // MORE - partition key support should go here?
            if (nextPartNumber<(df->numParts()-1))
                return setCurrentPart(nextPartNumber++);
        }
        else
        {
            while (tlManager->lookup(false))
            {
                offset_t node = extractFpos(tlManager);
                if (node)
                    return setCurrentPart((unsigned)node-1);
            }
        }
    }
    return false;
}

bool CHThorIndexReadActivityBase::doNextSuper()
{
    do
    {
        clearTlk(); 
        df.set(&superIterator->query());
        unsigned numParts = df->numParts();
        if (numParts==1)
            return setCurrentPart(0);

        if (firstMultiPart())
            return true;
        ++superIndex;
    } while (superIterator->next());
    return false;
}

void CHThorIndexReadActivityBase::getLayoutTranslators()
{
    if(superIterator)
    {
        superIterator->first();
        do
        {
            IDistributedFile & f = superIterator->query();
            layoutTrans.setown(getLayoutTranslator(&f));
            layoutTransArray.append(layoutTrans.getClear());
        } while(superIterator->next());
    }
    else if (df)
    {
        layoutTrans.setown(getLayoutTranslator(df));
    }
    else
        layoutTrans.clear();
}

const IDynamicTransform * CHThorIndexReadActivityBase::getLayoutTranslator(IDistributedFile * f)
{
    IOutputMetaData * expectedFormat = helper.queryDiskRecordSize();
    Linked<IOutputMetaData> actualFormat = expectedFormat;

    switch (getLayoutTranslationMode())
    {
    case RecordTranslationMode::AlwaysECL:
        verifyFormatCrc(helper.getDiskFormatCrc(), f, (superIterator ? superName.str() : NULL) , true, false);
        break;
    case RecordTranslationMode::None:
        verifyFormatCrc(helper.getDiskFormatCrc(), f, (superIterator ? superName.str() : NULL) , true, true);
        break;
    default:
        if(!verifyFormatCrc(helper.getDiskFormatCrc(), f, (superIterator ? superName.str() : NULL) , true, false))
        {
            IPropertyTree &props = f->queryAttributes();
            actualFormat.setown(getDaliLayoutInfo(props));
            if (!actualFormat)
                throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s - key layout information not found", f->queryLogicalName());

            //MORE: We could introduce a more efficient way of checking this that does not create a translator
            Owned<const IDynamicTransform> actualTranslator = createRecordTranslator(expectedFormat->queryRecordAccessor(true), actualFormat->queryRecordAccessor(true));
            DBGLOG("Record layout translator created for %s", f->queryLogicalName());
            actualTranslator->describe();
            if (actualTranslator->keyedTranslated())
                throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s - keyed fields do not match", f->queryLogicalName());
            VStringBuffer msg("Record layout translation required for %s", f->queryLogicalName());
            agent.addWuExceptionEx(msg.str(), WRN_UseLayoutTranslation, SeverityInformation, MSGAUD_user, "hthor");

            actualLayouts.append(actualFormat.getLink());  // ensure adequate lifespan
        }
        break;
    }

    IOutputMetaData * projectedFormat = helper.queryProjectedDiskRecordSize();
    if (projectedFormat == actualFormat)
        return nullptr;

    Owned<const IDynamicTransform> payloadTranslator =  createRecordTranslator(projectedFormat->queryRecordAccessor(true), actualFormat->queryRecordAccessor(true));
    if (!payloadTranslator->canTranslate())
        throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s", f->queryLogicalName());
    if (getLayoutTranslationMode() == RecordTranslationMode::PayloadRemoveOnly && payloadTranslator->hasNewFields())
        throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", f->queryLogicalName());
    if (payloadTranslator->needsTranslate())
        return payloadTranslator.getClear();
    return nullptr;
}

void CHThorIndexReadActivityBase::verifyIndex(IKeyIndex * idx)
{
    if(superIterator)
        layoutTrans.set(layoutTransArray.item(superIndex));
    if (eclKeySize.isFixedSize())
    {
        if(layoutTrans)
        {
            if (!layoutTrans->canTranslate())
                throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s", df->queryLogicalName());
            if (getLayoutTranslationMode() == RecordTranslationMode::PayloadRemoveOnly && layoutTrans->hasNewFields())
                throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", df->queryLogicalName());
        }
        else
        {
            keySize = idx->keySize();
            //The index rows always have the filepositions appended, but the ecl may not include a field
            unsigned fileposSize = idx->hasSpecialFileposition() && !hasTrailingFileposition(eclKeySize.queryTypeInfo()) ? sizeof(offset_t) : 0;
            if (keySize != eclKeySize.getFixedSize() + fileposSize && !idx->isTopLevelKey())
                throw MakeStringException(0, "Key size mismatch reading index %s: index indicates size %u, ECL indicates size %u", df->queryLogicalName(), keySize, eclKeySize.getFixedSize() + fileposSize);
        }
    }
}

//-------------------------------------------------------------------------------------------------------------

class CHThorIndexReadActivity : public CHThorIndexReadActivityBase
{

public:
    CHThorIndexReadActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexReadArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);
    ~CHThorIndexReadActivity();

    //interface IHThorInput
    virtual void ready();
    virtual const void *nextRow();
    virtual const void *nextRowGE(const void * seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra);

    virtual IInputSteppingMeta * querySteppingMeta();

protected:
    virtual bool nextPart();
    virtual void initPart();

protected:
    IHThorIndexReadArg &helper;
    IHThorSteppedSourceExtra * steppedExtra;
    unsigned __int64 keyedProcessed;
    unsigned __int64 keyedLimit;
    unsigned __int64 rowLimit;
    unsigned __int64 stopAfter;
    ISteppingMeta * rawMeta;
    ISteppingMeta * projectedMeta;
    size32_t seekGEOffset;
    unsigned * seekSizes;
    CSteppingMeta steppingMeta;
    bool needTransform;
    bool keyedLimitReached;
    bool keyedLimitSkips;
    bool keyedLimitCreates;
    bool keyedLimitRowCreated;
};

CHThorIndexReadActivity::CHThorIndexReadActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexReadArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
    : CHThorIndexReadActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph, _node), helper(_arg)
{
    limitTransformExtra = &helper;
    steppedExtra = helper.querySteppingExtra();
    needTransform = helper.needTransform();
    keyedLimit = (unsigned __int64)-1;
    rowLimit = (unsigned __int64)-1;
    stopAfter = (unsigned __int64)-1;
    keyedLimitReached = false;
    keyedLimitSkips = ((helper.getFlags() & TIRkeyedlimitskips) != 0);
    keyedLimitCreates = ((helper.getFlags() & TIRkeyedlimitcreates) != 0);
    keyedLimitRowCreated = false;
    keyedProcessed = 0;
    rawMeta = helper.queryRawSteppingMeta();
    projectedMeta = helper.queryProjectedSteppingMeta();
    seekGEOffset = 0;
    seekSizes = 0;
    if (rawMeta)
    {
        //should check that no translation, also should check all keys in maxFields list can actually be keyed.
        const CFieldOffsetSize * fields = rawMeta->queryFields();
        unsigned maxFields = rawMeta->getNumFields();
        seekGEOffset = fields[0].offset;
        seekSizes = new unsigned[maxFields];
        seekSizes[0] = fields[0].size;
        for (unsigned i=1; i < maxFields; i++)
            seekSizes[i] = seekSizes[i-1] + fields[i].size;
        if (projectedMeta)
            steppingMeta.init(projectedMeta, false);
        else
            steppingMeta.init(rawMeta, false);
    }
}

CHThorIndexReadActivity::~CHThorIndexReadActivity()
{
    delete [] seekSizes;
}

void CHThorIndexReadActivity::ready()
{
    CHThorIndexReadActivityBase::ready();
    keyedLimitReached = false;
    keyedLimitRowCreated = false;
    keyedLimit = helper.getKeyedLimit();
    rowLimit = helper.getRowLimit();
    if (helper.getFlags() & TIRlimitskips)
        rowLimit = (unsigned __int64) -1;
    stopAfter = helper.getChooseNLimit();
    keyedProcessed = 0;
    if (seekGEOffset || localSortKey || ((keyedLimit != (unsigned __int64) -1) && ((helper.getFlags() & TIRcountkeyedlimit) != 0) && !singlePart))
        keyedLimitReached = doPreopenLimit(keyedLimit);
    if (steppedExtra)
        steppingMeta.setExtra(steppedExtra);

    firstPart();

    if(klManager && (keyedLimit != (unsigned __int64) -1) && ((helper.getFlags() & TIRcountkeyedlimit) != 0) && singlePart && !seekGEOffset)
    {
        unsigned __int64 result = klManager->checkCount(keyedLimit);
        keyedLimitReached = (result > keyedLimit);
        klManager->reset();
    }
}

bool CHThorIndexReadActivity::nextPart()
{
    if(keyIndexCache && (seekGEOffset || localSortKey))
    {
        killPart();
        klManager.setown(createKeyMerger(eclKeySize.queryRecordAccessor(true), keyIndexCache, seekGEOffset, NULL, helper.hasNewSegmentMonitors(), false));
        keyIndexCache.clear();
        initManager(klManager, false);
        callback.setManager(klManager, nullptr);
        return true;
    }
    else if (seekGEOffset || localSortKey)
        return false;
    else
        return CHThorIndexReadActivityBase::nextPart();
}

void CHThorIndexReadActivity::initPart()
{ 
    CHThorIndexReadActivityBase::initPart();
}

const void *CHThorIndexReadActivity::nextRow()
{
    if(keyedLimitReached)
    {
        if (keyedLimitSkips)
            return NULL;
        if (keyedLimitCreates)
        {
            if (!keyedLimitRowCreated)
            {
                keyedLimitRowCreated = true;
                const void * ret = createKeyedLimitOnFailRow();
                if (ret)
                    processed++;
                return ret;
            }
            return NULL;
        }
        helper.onKeyedLimitExceeded(); // should throw exception
    }

    if((stopAfter && (processed-initialProcessed)==stopAfter) || !klManager)
        return NULL;

    for (;;)
    {
        agent.reportProgress(NULL);

        if (klManager->lookup(true))
        {
            keyedProcessed++;
            if ((keyedLimit != (unsigned __int64) -1) && keyedProcessed > keyedLimit)
                helper.onKeyedLimitExceeded();
            byte const * keyRow = klManager->queryKeyBuffer();
            if (likely(helper.canMatch(keyRow)))
            {
                if (needTransform)
                {
                    try
                    {
                        size32_t recSize;
                        RtlDynamicRowBuilder rowBuilder(rowAllocator);
                        recSize = helper.transform(rowBuilder, keyRow);
                        callback.finishedRow();
                        if (recSize)
                        {
                            processed++;
                            if ((processed-initialProcessed) > rowLimit)
                            {
                                helper.onLimitExceeded();
                                if ( agent.queryCodeContext()->queryDebugContext())
                                    agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
                            }
                            return rowBuilder.finalizeRowClear(recSize);
                        }
                        else
                        {
                            postFiltered++;
                        }
                    }
                    catch(IException * e)
                    {
                        throw makeWrappedException(e);
                    }
                }
                else
                {
                    callback.finishedRow(); // since filter might have accessed a blob
                    processed++;
                    if ((processed-initialProcessed) > rowLimit)
                    {
                        helper.onLimitExceeded();
                        if ( agent.queryCodeContext()->queryDebugContext())
                            agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
                    }
                    try
                    {
                        RtlDynamicRowBuilder rowBuilder(rowAllocator);
                        size32_t finalSize = cloneRow(rowBuilder, keyRow, outputMeta);
                        return rowBuilder.finalizeRowClear(finalSize);
                    }
                    catch(IException * e)
                    {
                        throw makeWrappedException(e);
                    }
                }
            }
            else
            {
                callback.finishedRow(); // since filter might have accessed a blob
                postFiltered++;
            }
        }
        else if (!nextPart())
            return NULL;
    }
}


const void *CHThorIndexReadActivity::nextRowGE(const void * seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
{
    // MORE - should set wasCompleteMatch
    if(keyedLimitReached && !keyedLimitSkips)
        helper.onKeyedLimitExceeded(); // should throw exception

    if(keyedLimitReached || (stopAfter && (processed-initialProcessed)==stopAfter) || !klManager)
        return NULL;

    const byte * rawSeek = (const byte *)seek + seekGEOffset;
    unsigned seekSize = seekSizes[numFields-1];
    if (projectedMeta)
    {
        byte *temp = (byte *) alloca(seekSize);
        RtlStaticRowBuilder tempBuilder(temp - seekGEOffset, seekSize + seekGEOffset);
        helper.mapOutputToInput(tempBuilder, seek, numFields); // NOTE - weird interface to mapOutputToInput means that it STARTS writing at seekGEOffset...
        rawSeek = (byte *)temp;
    }
    for (;;)
    {
        agent.reportProgress(NULL);

        if (klManager->lookupSkip(rawSeek, seekGEOffset, seekSize))
        {
            const byte * row = klManager->queryKeyBuffer();
#ifdef _DEBUG
            if (memcmp(row + seekGEOffset, rawSeek, seekSize) < 0)
                assertex("smart seek failure");
#endif

            keyedProcessed++;
            if ((keyedLimit != (unsigned __int64) -1) && keyedProcessed > keyedLimit)
                helper.onKeyedLimitExceeded();
            if (likely(helper.canMatch(row)))
            {
                if (needTransform)
                {
                    try
                    {
                        size32_t recSize;
                        RtlDynamicRowBuilder rowBuilder(rowAllocator);
                        recSize = helper.transform(rowBuilder, row);
                        callback.finishedRow();
                        if (recSize)
                        {
                            processed++;
                            if ((processed-initialProcessed) > rowLimit)
                            {
                                helper.onLimitExceeded();
                                if ( agent.queryCodeContext()->queryDebugContext())
                                    agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
                            }
                            return rowBuilder.finalizeRowClear(recSize);
                        }
                        else
                        {
                            postFiltered++;
                        }
                    }
                    catch(IException * e)
                    {
                        throw makeWrappedException(e);
                    }
                }
                else
                {
                    callback.finishedRow(); // since filter might have accessed a blob
                    processed++;
                    if ((processed-initialProcessed) > rowLimit)
                    {
                        helper.onLimitExceeded();
                        if ( agent.queryCodeContext()->queryDebugContext())
                            agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
                    }
                    try
                    {
                        RtlDynamicRowBuilder rowBuilder(rowAllocator);
                        size32_t finalSize = cloneRow(rowBuilder, row, outputMeta);
                        return rowBuilder.finalizeRowClear(finalSize);
                    }
                    catch(IException * e)
                    {
                        throw makeWrappedException(e);
                    }
                }
            }
            else
            {
                callback.finishedRow(); // since filter might have accessed a blob
                postFiltered++;
            }
        }
        else if (!nextPart())
            return NULL;
    }
}


IInputSteppingMeta * CHThorIndexReadActivity::querySteppingMeta()
{
    if (rawMeta)
        return &steppingMeta;
    return NULL;
}


extern HTHOR_API IHThorActivity *createIndexReadActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexReadArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorIndexReadActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//-------------------------------------------------------------------------------------------------------------


class CHThorIndexNormalizeActivity : public CHThorIndexReadActivityBase
{

public:
    CHThorIndexNormalizeActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexNormalizeArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);
    ~CHThorIndexNormalizeActivity();

    virtual void ready();
    virtual void stop();
    virtual const void *nextRow();
    virtual bool needsAllocator() const { return true; }

protected:
    const void * createNextRow();

protected:
    IHThorIndexNormalizeArg &helper;
    unsigned __int64 rowLimit;
    unsigned __int64 stopAfter;
    RtlDynamicRowBuilder outBuilder;
    unsigned __int64 keyedProcessed;
    unsigned __int64 keyedLimit;
    bool skipLimitReached;
    bool expanding;
};


CHThorIndexNormalizeActivity::CHThorIndexNormalizeActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexNormalizeArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node) : CHThorIndexReadActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph, _node), helper(_arg), outBuilder(NULL)
{
    limitTransformExtra = &helper;
    keyedLimit = (unsigned __int64)-1;
    skipLimitReached = false;
    keyedProcessed = 0;
    rowLimit = (unsigned __int64)-1;
    stopAfter = (unsigned __int64)-1;
    expanding = false;
}

CHThorIndexNormalizeActivity::~CHThorIndexNormalizeActivity()
{
}

void CHThorIndexNormalizeActivity::ready()
{
    CHThorIndexReadActivityBase::ready();
    keyedLimit = helper.getKeyedLimit();
    skipLimitReached = false;
    keyedProcessed = 0;
    rowLimit = helper.getRowLimit();
    if (helper.getFlags() & TIRlimitskips)
        rowLimit = (unsigned __int64) -1;
    stopAfter = helper.getChooseNLimit();
    expanding = false;
    outBuilder.setAllocator(rowAllocator);

    firstPart();
}

void CHThorIndexNormalizeActivity::stop()
{
    outBuilder.clear();
    CHThorIndexReadActivityBase::stop();
}

const void *CHThorIndexNormalizeActivity::nextRow()
{
    if ((stopAfter && (processed-initialProcessed)==stopAfter) || !klManager)
        return NULL;

    if (skipLimitReached || (stopAfter && (processed-initialProcessed)==stopAfter) || !klManager)
        return NULL;

    if ((keyedLimit != (unsigned __int64) -1) && (helper.getFlags() & TIRcountkeyedlimit) != 0)
    {
        unsigned __int64 result = klManager->checkCount(keyedLimit);
        if (result > keyedLimit)
        {
            if((helper.getFlags() & TIRkeyedlimitskips) != 0)
                skipLimitReached = true;
            else if((helper.getFlags() & TIRkeyedlimitcreates) != 0)
            {
                skipLimitReached = true;
                const void * ret = createKeyedLimitOnFailRow();
                if (ret)
                    processed++;
                return ret;
            }
            else
                helper.onKeyedLimitExceeded(); // should throw exception
            return NULL;
        }
        klManager->reset();
        keyedLimit = (unsigned __int64) -1; // to avoid checking it again
    }
    assertex(!((keyedLimit != (unsigned __int64) -1) && ((helper.getFlags() & TIRkeyedlimitskips) != 0)));

    for (;;)
    {
        for (;;)
        {
            if (expanding)
            {
                for (;;)
                {
                    expanding = helper.next();
                    if (!expanding)
                        break;

                    const void * ret = createNextRow();
                    if (ret)
                        return ret;
                }
            }

            while (!klManager->lookup(true))
            {
                keyedProcessed++;
                if ((keyedLimit != (unsigned __int64) -1) && keyedProcessed > keyedLimit)
                    helper.onKeyedLimitExceeded();

                if (!nextPart())
                    return NULL;
            }

            agent.reportProgress(NULL);
            expanding = helper.first(klManager->queryKeyBuffer());
            callback.finishedRow(); // first() can lookup blobs
            if (expanding)
            {
                const void * ret = createNextRow();
                if (ret)
                    return ret;
            }
        }
    }
}

const void * CHThorIndexNormalizeActivity::createNextRow()
{
    try
    {
        outBuilder.ensureRow();
        size32_t thisSize = helper.transform(outBuilder);
        if (thisSize == 0)
        {
            return NULL;
        }

        OwnedConstRoxieRow ret = outBuilder.finalizeRowClear(thisSize);
        if ((processed - initialProcessed) >=rowLimit)
        {
            helper.onLimitExceeded();
            if ( agent.queryCodeContext()->queryDebugContext())
                agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
            return NULL;
        }
        processed++;
        return ret.getClear();
    }
    catch(IException * e)
    {
        throw makeWrappedException(e);
    }

}

extern HTHOR_API IHThorActivity *createIndexNormalizeActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexNormalizeArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorIndexNormalizeActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//-------------------------------------------------------------------------------------------------------------


class CHThorIndexAggregateActivity : public CHThorIndexReadActivityBase
{

public:
    CHThorIndexAggregateActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexAggregateArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);
    ~CHThorIndexAggregateActivity();

    //interface IHThorInput
    virtual void stop();
    virtual void ready();
    virtual const void *nextRow();
    virtual bool needsAllocator() const { return true; }

protected:
    void * createNextRow();
    void gather();

protected:
    IHThorIndexAggregateArg &helper;
    RtlDynamicRowBuilder outBuilder;
    bool finished;
};


CHThorIndexAggregateActivity::CHThorIndexAggregateActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexAggregateArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
    : CHThorIndexReadActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph, _node), helper(_arg), outBuilder(NULL)
{
}

CHThorIndexAggregateActivity::~CHThorIndexAggregateActivity()
{
}

void CHThorIndexAggregateActivity::ready()
{
    CHThorIndexReadActivityBase::ready();
    outBuilder.setAllocator(rowAllocator);
    finished = false;

    firstPart();
}

void CHThorIndexAggregateActivity::stop()
{
    outBuilder.clear();
    CHThorIndexReadActivityBase::stop();
}



void CHThorIndexAggregateActivity::gather()
{
    outBuilder.ensureRow();

    try
    {
        helper.clearAggregate(outBuilder);
    }
    catch(IException * e)
    {
        throw makeWrappedException(e);
    }
    if(!klManager)
        return;

    for (;;)
    {
        while (!klManager->lookup(true))
        {
            if (!nextPart())
                return;
        }

        agent.reportProgress(NULL);
        try
        {
            helper.processRow(outBuilder, klManager->queryKeyBuffer());
        }
        catch(IException * e)
        {
            throw makeWrappedException(e);
        }
        callback.finishedRow();
    }
}

const void *CHThorIndexAggregateActivity::nextRow()
{
    if (finished) return NULL;
    gather();

    processed++;
    finished = true;
    size32_t size = outputMeta.getRecordSize(outBuilder.getSelf());
    return outBuilder.finalizeRowClear(size);
}


extern HTHOR_API IHThorActivity *createIndexAggregateActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexAggregateArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorIndexAggregateActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//-------------------------------------------------------------------------------------------------------------

class CHThorIndexCountActivity : public CHThorIndexReadActivityBase
{
    bool keyedLimitReached = false;
    bool keyedLimitSkips = false;
    unsigned __int64 keyedLimit = (unsigned __int64)-1;
    unsigned __int64 rowLimit = (unsigned __int64)-1;

public:
    CHThorIndexCountActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexCountArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);

    //interface IHThorInput
    virtual void ready();
    virtual const void *nextRow();

protected:
    void * createNextRow();

protected:
    IHThorIndexCountArg &helper;
    unsigned __int64 choosenLimit;
    bool finished;
};


CHThorIndexCountActivity::CHThorIndexCountActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexCountArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
    : CHThorIndexReadActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph, _node), helper(_arg)
{
    choosenLimit = (unsigned __int64)-1;
    finished = false;
    keyedLimitSkips = ((helper.getFlags() & TIRkeyedlimitskips) != 0);
}

void CHThorIndexCountActivity::ready()
{
    CHThorIndexReadActivityBase::ready();

    keyedLimitReached = false;
    keyedLimit = helper.getKeyedLimit();
    rowLimit = helper.getRowLimit();

    finished = false;
    choosenLimit = helper.getChooseNLimit();

    firstPart();

    if ((keyedLimit != (unsigned __int64) -1) && ((helper.getFlags() & TIRcountkeyedlimit) != 0))
    {
        if (singlePart)
        {
            if (klManager) // NB: opened by firstPart()
            {
                unsigned __int64 result = klManager->checkCount(keyedLimit);
                keyedLimitReached = (result > keyedLimit);
                klManager->reset();
            }
        }
        else
            keyedLimitReached = doPreopenLimit(keyedLimit);
    }
}

const void *CHThorIndexCountActivity::nextRow()
{
    if (finished) return NULL;

    unsigned __int64 totalCount = 0;

    if (keyedLimitReached)
    {
        if (!keyedLimitSkips)
            helper.onKeyedLimitExceeded(); // should throw exception
    }
    else if (klManager)
    {
        unsigned __int64 keyedProcessed = 0;
        unsigned __int64 rowsProcessed = 0;
        bool limitSkipped = false;
        for (;;)
        {
            if (helper.hasFilter())
            {
                for (;;)
                {
                    agent.reportProgress(NULL);
                    if (!klManager->lookup(true))
                        break;
                    ++keyedProcessed;
                    if ((keyedLimit != (unsigned __int64) -1) && keyedProcessed > keyedLimit)
                        helper.onKeyedLimitExceeded();
                    totalCount += helper.numValid(klManager->queryKeyBuffer());
                    callback.finishedRow();

                    rowsProcessed++;
                    if (rowsProcessed > rowLimit)
                    {
                        if (0 != (helper.getFlags() & TIRlimitskips))
                        {
                            totalCount = 0;
                            limitSkipped = true;
                            break;
                        }
                        else
                            helper.onLimitExceeded();
                    }

                    if ((totalCount > choosenLimit))
                        break;
                }
            }
            else
                totalCount += klManager->getCount();

            if (limitSkipped || (totalCount > choosenLimit) || !nextPart())
                break;
        }
    }

    finished = true;
    processed++;
    if (totalCount > choosenLimit)
        totalCount = choosenLimit;

    size32_t outSize = outputMeta.getFixedSize();
    void * ret = rowAllocator->createRow(); //meta: outputMeta
    if (outSize == 1)
    {
        assertex(choosenLimit == 1);
        *(byte *)ret = (byte)totalCount;
    }
    else
    {
        assertex(outSize == sizeof(unsigned __int64));
        *(unsigned __int64 *)ret = totalCount;
    }
    return ret = rowAllocator->finalizeRow(outSize, ret, outSize);
}


extern HTHOR_API IHThorActivity *createIndexCountActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexCountArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorIndexCountActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//-------------------------------------------------------------------------------------------------------------

class CHThorIndexGroupAggregateActivity : public CHThorIndexReadActivityBase, implements IHThorGroupAggregateCallback
{

public:
    CHThorIndexGroupAggregateActivity(IAgentContext &agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexGroupAggregateArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node);
    IMPLEMENT_IINTERFACE

    //interface IHThorInput
    virtual void ready();
    virtual const void *nextRow();
    virtual bool needsAllocator() const { return true; }        
    virtual void processRow(const void * next);

protected:
    void * createNextRow();
    void gather();

protected:
    IHThorIndexGroupAggregateArg &helper;
    RowAggregator aggregated;
    bool eof;
    bool gathered;
};


CHThorIndexGroupAggregateActivity::CHThorIndexGroupAggregateActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexGroupAggregateArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node) : CHThorIndexReadActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph, _node), helper(_arg), aggregated(_arg, _arg)
{
    eof = false;
    gathered = false;
}

void CHThorIndexGroupAggregateActivity::ready()
{
    CHThorIndexReadActivityBase::ready();
    eof = false;
    gathered = false;
    aggregated.reset();
    aggregated.start(rowAllocator, agent.queryCodeContext(), activityId);

    firstPart();
}

void CHThorIndexGroupAggregateActivity::processRow(const void * next)
{
    aggregated.addRow(next);
}


void CHThorIndexGroupAggregateActivity::gather()
{
    gathered = true;
    if(!klManager)
        return;
    for (;;)
    {
        while (!klManager->lookup(true))
        {
            if (!nextPart())
                return;
        }
                
        agent.reportProgress(NULL);
        try
        {
            helper.processRow(klManager->queryKeyBuffer(), this);
        }
        catch(IException * e)
        {
            throw makeWrappedException(e);
        }
        callback.finishedRow();
    }
}

const void *CHThorIndexGroupAggregateActivity::nextRow()
{
    if (eof)
        return NULL;

    if (!gathered)
        gather();

    Owned<AggregateRowBuilder> next = aggregated.nextResult();
    if (next)
    {
        processed++;
        return next->finalizeRowClear();
    }
    eof = true;
    return NULL;
}


extern HTHOR_API IHThorActivity *createIndexGroupAggregateActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorIndexGroupAggregateArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorIndexGroupAggregateActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//-------------------------------------------------------------------------------------------------------------

interface IThreadedExceptionHandler
{
    virtual void noteException(IException *E) = 0;
};

template <class ROW, class OWNER>
class PartHandlerThread : public CInterface, implements IPooledThread
{
public:
    typedef PartHandlerThread<ROW, OWNER> SELF;
    IMPLEMENT_IINTERFACE;
    PartHandlerThread() : owner(0)
    {
    }
    virtual void init(void * _owner) override { owner = (OWNER *)_owner; }
    virtual void threadmain() override
    {
        try
        {
            owner->openPart();
            for (;;)
            {
                ROW * row = owner->getRow();
                if (!row)
                    break;
                owner->doRequest(row);
            }
        }
        catch (IException *E)
        {
            owner->noteException(E);
        }
    }

    virtual bool stop() override
    {
        owner->stopThread();
        return true;
    }

    virtual bool canReuse() const override { return true; }
private:
    OWNER * owner;
};

template <class ROW>
class ThreadedPartHandler : public CInterface
{
protected:
    Linked<IThreadPool> threadPool;
    PooledThreadHandle threadHandle = 0;
    QueueOf<ROW, true> pending;
    CriticalSection crit;
    Semaphore limit;
    bool started = false;
    Owned<IDistributedFilePart> part;
    IThreadedExceptionHandler *handler;

public:
    typedef ThreadedPartHandler<ROW> SELF;
    ThreadedPartHandler(IDistributedFilePart *_part, IThreadedExceptionHandler *_handler, IThreadPool * _threadPool)
        : threadPool(_threadPool), limit(MAX_FETCH_LOOKAHEAD), part(_part), handler(_handler)
    {
    }

    ~ThreadedPartHandler()
    {
        //is it the responsibility of the derived class to clean up the list on destruction --- can do nothing but assert here, since implementations different and VMTs gone by now
        assertex(pending.ordinality() == 0);
    }

    void addRow(ROW * row)
    {
        limit.wait();
        CriticalBlock procedure(crit);
        pending.enqueue(row);
        if (!started)
        {
            started = true;
            start();
        }
    }

    void stopThread()
    {
    }

    void start()
    {
        threadHandle = threadPool->start(this);
    }

    void join()
    {
        threadPool->join(threadHandle);
        started = false;
    }

    ROW * getRow()
    {
        CriticalBlock procedure(crit);
        if(pending.ordinality())
        {
            limit.signal();
            return pending.dequeue();
        }
        else
        {
            started = false; //because returning NULL will cause thread to terminate (has to be within this CriticalBlock to avoid race cond.)
            return NULL;
        }
    }

    void noteException(IException * e)
    {
        handler->noteException(e);
    }

private:
    friend class PartHandlerThread<ROW, SELF>;
    virtual void doRequest(ROW * row) = 0; // Must be implemented by derived class
    virtual void openPart() = 0;         // Must be implemented by derived class
};

template <class ROW>
class PartHandlerThreadFactory : public CInterface, implements IThreadFactory
{
    IMPLEMENT_IINTERFACE;
    typedef ThreadedPartHandler<ROW> OWNER;
    IPooledThread * createNew() { return new PartHandlerThread<ROW, OWNER>(); }
};

class FetchRequest : public CInterface
{
public:
    const void * left;
    offset_t pos;
    offset_t seq;
    FetchRequest(const void * _left, offset_t _pos, offset_t _seq) : left(_left), pos(_pos), seq(_seq) {}
    ~FetchRequest() { ReleaseRoxieRow(left); }
};

class IFlatFetchHandlerCallback
{
public:
    virtual void processFetch(FetchRequest const * fetch, offset_t pos, IBufferedSerialInputStream *rawStream) = 0;
};

class IXmlFetchHandlerCallback
{
public:
    virtual void processFetched(FetchRequest const * fetch, IColumnProvider * lastMatch) = 0;
    virtual IException * makeWrappedException(IException * e, char const * extra) const = 0;
};

// this class base for all three fetch activities and keyed join
class FetchPartHandlerBase
{
protected:
    Owned<IFileIO> rawFile;
    Owned<IBufferedSerialInputStream> rawStream;
    offset_t base;
    offset_t top;
    bool blockcompressed;
    MemoryAttr encryptionkey;
    unsigned activityId;
    CachedOutputMetaData const & outputMeta;
    IEngineRowAllocator * rowAllocator;
    ISourceRowPrefetcher * prefetcher;
public:
    FetchPartHandlerBase(offset_t _base, offset_t _size, bool _blockcompressed, MemoryAttr &_encryptionkey, unsigned _activityId, CachedOutputMetaData const & _outputMeta, ISourceRowPrefetcher * _prefetcher, IEngineRowAllocator *_rowAllocator)
        : blockcompressed(_blockcompressed), 
          encryptionkey(_encryptionkey), 
          activityId(_activityId), 
          outputMeta(_outputMeta),
          rowAllocator(_rowAllocator),
          prefetcher(_prefetcher)
    {
        base = _base;
        top = _base + _size;
    }

    int compare(offset_t offset)
    {
        if (offset < base)
            return -1;
        else if (offset >= top)
            return 1;
        else
            return 0;
    }

    offset_t translateFPos(offset_t rp)
    {
        if(isLocalFpos(rp))
            return getLocalFposOffset(rp);
        else
            return rp-base;
    }

    virtual void openPart()
    {
        // MORE - cached file handles?
        if(rawFile)
            return;
        IDistributedFilePart * part = queryPart();
        unsigned numCopies = part->numCopies();
        for (unsigned copy=0; copy < numCopies; copy++)
        {
            RemoteFilename rfn;
            try
            {
                OwnedIFile ifile = createIFile(part->getFilename(rfn,copy));
                offset_t thissize = ifile->size();
                if (thissize != (offset_t)-1)
                {
                    IPropertyTree & props = part->queryAttributes();
                    unsigned __int64 expectedSize;
                    Owned<IExpander> eexp;
                    if (encryptionkey.length()!=0) {
                        eexp.setown(createAESExpander256((size32_t)encryptionkey.length(),encryptionkey.get()));
                        blockcompressed = true;
                    }
                    if(blockcompressed)
                        expectedSize = props.getPropInt64("@compressedSize", -1);
                    else
                        expectedSize = props.getPropInt64("@size", -1);
                    if(thissize != expectedSize && expectedSize != (unsigned __int64)-1)
                        throw MakeStringException(0, "File size mismatch: file %s was supposed to be %" I64F "d bytes but appears to be %" I64F "d bytes", ifile->queryFilename(), expectedSize, thissize); 
                    if(blockcompressed)
                        rawFile.setown(createCompressedFileReader(ifile, eexp, useDefaultIoBufferSize, false, IFEnone));
                    else
                        rawFile.setown(ifile->open(IFOread));
                    break;
                }
            }
            catch (IException *E)
            {
                EXCLOG(E, "Opening key part");
                E->Release();
            }
        }
        if(!rawFile)
        {
            RemoteFilename rfn;
            StringBuffer rmtPath;
            part->getFilename(rfn).getRemotePath(rmtPath);
            throw MakeStringException(1001, "Could not open file part at %s%s", rmtPath.str(), (numCopies > 1) ? " or any alternate location." : ".");
        }
        rawStream.setown(createFileSerialStream(rawFile, 0, -1, 0));
    }

    virtual IDistributedFilePart * queryPart() = 0;
};

// this class base for all three fetch activities, but not keyed join
class SimpleFetchPartHandlerBase : public FetchPartHandlerBase, public ThreadedPartHandler<FetchRequest>
{
public:
    SimpleFetchPartHandlerBase(IDistributedFilePart *_part, offset_t _base, offset_t _size, IThreadedExceptionHandler *_handler, IThreadPool * _threadPool, bool _blockcompressed, MemoryAttr &_encryptionkey, unsigned _activityId, CachedOutputMetaData const & _outputMeta, ISourceRowPrefetcher * _prefetcher, IEngineRowAllocator *_rowAllocator)
        : FetchPartHandlerBase(_base, _size, _blockcompressed, _encryptionkey, _activityId, _outputMeta, _prefetcher, _rowAllocator),
          ThreadedPartHandler<FetchRequest>(_part, _handler, _threadPool)
    {
    }

    ~SimpleFetchPartHandlerBase()
    {
        while(FetchRequest * fetch = pending.dequeue())
            fetch->Release();
    }

    IMPLEMENT_IINTERFACE;

    virtual IDistributedFilePart * queryPart() { return part; }

private:
    virtual void openPart() { FetchPartHandlerBase::openPart(); }
};

// this class used for flat and CSV fetch activities, but not XML fetch or keyed join
class FlatFetchPartHandler : public SimpleFetchPartHandlerBase
{
public:
    FlatFetchPartHandler(IFlatFetchHandlerCallback & _owner, IDistributedFilePart * _part, offset_t _base, offset_t _size, IThreadedExceptionHandler *_handler, IThreadPool * _threadPool, bool _blockcompressed, MemoryAttr &_encryptionkey, unsigned _activityId, CachedOutputMetaData const & _outputMeta, ISourceRowPrefetcher * _prefetcher, IEngineRowAllocator *_rowAllocator)
        : SimpleFetchPartHandlerBase(_part, _base, _size, _handler, _threadPool, _blockcompressed, _encryptionkey, _activityId, _outputMeta, _prefetcher, _rowAllocator),
          owner(_owner)
    {
    }

    virtual void doRequest(FetchRequest * _fetch)
    {
        Owned<FetchRequest> fetch(_fetch);
        offset_t pos = translateFPos(fetch->pos);
        if(pos >= rawFile->size())
            throw MakeStringException(0, "Attempted to fetch at invalid filepos");
        owner.processFetch(fetch, pos, rawStream);
    }

private:
    IFlatFetchHandlerCallback & owner;
};

class DistributedFileFetchHandlerBase : public CInterface, implements IInterface, implements IThreadedExceptionHandler
{
public:
    IMPLEMENT_IINTERFACE;
    DistributedFileFetchHandlerBase() {}
    virtual ~DistributedFileFetchHandlerBase() {}

    virtual void noteException(IException *E)
    {
        CriticalBlock procedure(exceptionCrit);
        if (exception)
            E->Release();
        else
            exception = E;
    }

protected:
    static offset_t getPartSize(IDistributedFilePart *part)
    {
        offset_t partsize = part->queryAttributes().getPropInt64("@size", -1);
        if (partsize == (offset_t)-1)
        {
            MTIME_SECTION(queryActiveTimer(), "Fetch remote file size");
            unsigned numCopies = part->numCopies();
            for (unsigned copy=0; copy < numCopies; copy++)
            {
                RemoteFilename rfn;
                try
                {
                    OwnedIFile ifile = createIFile(part->getFilename(rfn,copy));
                    partsize = ifile->size();
                    if (partsize != (offset_t)-1)
                    {
                        // TODO: Create DistributedFilePropertyLock for parts
                        part->lockProperties();
                        part->queryAttributes().setPropInt64("@size", partsize);
                        part->unlockProperties();
                        break;
                    }
                }
                catch(IException *E)
                {
                    EXCLOG(E, "Open remote file");
                    E->Release();
                }
            }
        }
        if (partsize == (offset_t)-1)
            throw MakeStringException(0, "Unable to determine size of filepart"); 
        return partsize;
    }

protected:
    CriticalSection exceptionCrit;
    IException * exception;
};

template <class PARTHANDLER>
class IFetchHandlerFactory
{
public:
    virtual PARTHANDLER * createFetchPartHandler(IDistributedFilePart * part, offset_t base, offset_t size, IThreadedExceptionHandler * handler, bool blockcompressed, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator) = 0;
};

template <class PARTHANDLER, class LEFTPTR, class REQUEST>
class DistributedFileFetchHandler : public DistributedFileFetchHandlerBase
{
public:
    typedef DistributedFileFetchHandler<PARTHANDLER, LEFTPTR, REQUEST> SELF;

    DistributedFileFetchHandler(IDistributedFile * f, IFetchHandlerFactory<PARTHANDLER> & factory, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator) : file(f)
    {
        numParts = f->numParts();
        parts = new PARTHANDLER *[numParts];
        Owned<IFileDescriptor> fdesc = f->getFileDescriptor();
        bool blockcompressed = fdesc->isCompressed(); //assume new compression, old compression was never handled on fetch
        offset_t base = 0;
        unsigned idx;
        for (idx = 0; idx < numParts; idx++)
        {
            IDistributedFilePart *part = f->getPart(idx);
            offset_t size = getPartSize(part);
            parts[idx] = factory.createFetchPartHandler(part, base, size, this, blockcompressed, encryptionkey, prefetcher, rowAllocator);
            base += size;
        }
        exception = NULL;
    }

    ~DistributedFileFetchHandler()
    {
        unsigned idx;
        for (idx = 0; idx < numParts; idx++)
        {
            delete parts[idx];
        }
        delete [] parts;
    }

    int compare(offset_t l, PARTHANDLER * r)
    {
        return r->compare(l);
    }

    void addRow(LEFTPTR left, offset_t rp, offset_t seq)
    {
        PARTHANDLER * part = binsearch(rp, parts, numParts, this);
        if(!part)
            throw MakeStringException(1002, "FETCH: file position %" I64F "d out of range", rp);
        part->addRow(new REQUEST(left, rp, seq));
    }

    void stopThread()
    {
        unsigned idx;
        for (idx = 0; idx < numParts; idx++)
        {
            parts[idx]->stopThread();
            parts[idx]->join();
        }
        if (exception)
            throw (exception);
    }

private:
    Linked<IDistributedFile> file;
    unsigned numParts;
    PARTHANDLER * * parts;
};

//-------------------------------------------------------------------------------------------------------------

class CHThorThreadedActivityBase : public CHThorActivityBase, implements IThreadedExceptionHandler
{
    class InputHandler : extends Thread
    {
        CHThorThreadedActivityBase *parent;

    public:
        InputHandler(CHThorThreadedActivityBase  *_parent) : parent(_parent)
        {
        }

        virtual int run()
        {
            try
            {
                parent->fetchAll();
            }
            catch (IException *E)
            {
                parent->noteException(E);
            }
            catch (...)
            {
                parent->noteException(MakeStringException(0, "Unknown exception caught in Fetch::InputHandler"));
            }
            return 0;
        }

    };

public:
    CHThorThreadedActivityBase (IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorArg &_arg, IHThorFetchContext &_fetch, ThorActivityKind _kind, EclGraph & _graph, IRecordSize *diskSize, IPropertyTree *_node)
        : CHThorActivityBase(_agent, _activityId, _subgraphId, _arg, _kind, _graph), fetch(_fetch)
    {
        exception = NULL;
        rowLimit = 0;
        if (_node)
            isCodeSigned = isActivityCodeSigned(*_node);
    }

    virtual ~CHThorThreadedActivityBase ()
    {
    }

    virtual void waitForThreads()
    {
        aborting = true;
        if (inputThread)
            inputThread->join();
        inputThread.clear();
        threadPool.clear();
    }

    virtual void fetchAll() = 0;

    virtual void ready()        
    { 
        CHThorActivityBase::ready(); 
        started = false;
        stopped = false;
        aborting = false;
        initializeThreadPool();
    }

    virtual void initializeThreadPool() = 0;

    virtual void stop()
    {
        aborting = true;
        stopThread();
        if (inputThread)
            inputThread->join();

        while (!stopped)
        {
            const void * row = getRow();
            ReleaseRoxieRow(row);
        }
        clearQueue();
        waitForThreads();
        avail.reinit(0);
        CHThorActivityBase::stop(); 
    }

    virtual const void * getRow() = 0;
    virtual void clearQueue() = 0;

    IHThorInput *queryOutput(unsigned index) { return this; }

    //interface IHThorInput
    virtual bool isGrouped()                { return false; }
    virtual const char *getFileName()       { return NULL; }
    virtual bool outputToFile(const char *) { return false; } 
    virtual IOutputMetaData * queryOutputMeta() const { return CHThorActivityBase::outputMeta; }

protected:
    Semaphore avail;
    bool stopped;
    bool started;
    bool aborting;
    IHThorFetchContext &fetch;
    Owned<InputHandler> inputThread;
    unsigned numParts;
    unsigned __int64 rowLimit;
    bool isCodeSigned = false;
    Owned<IThreadPool> threadPool;
    CriticalSection pendingCrit;
    IException *exception;

public:
    virtual void noteException(IException *E)
    {
        CriticalBlock procedure(pendingCrit);
        if (exception)
            E->Release();
        else
            exception = E;
        avail.signal();
    }

    void stopThread()
    {
        avail.signal();
    }

    virtual const void *nextRow()
    {
        if (!started)
        {
            started = true;
            start();
        }
        try
        {
            const void *ret = getRow();
            if (ret)
            {
                processed++;
                if ((processed-initialProcessed) > rowLimit)
                {
                    onLimitExceeded();
                    if ( agent.queryCodeContext()->queryDebugContext())
                        agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
                }
            }
            return ret;
        }
        catch(...)
        {
            stopParts();
            throw;
        }
    }

    virtual void initParts(IDistributedFile * f) = 0;
    
    virtual void stopParts() = 0;

    virtual void onLimitExceeded() = 0;

    virtual void start()
    {
        OwnedRoxieString lfn(fetch.getFileName());
        if (lfn.get())
        {
            Owned<ILocalOrDistributedFile> ldFile = resolveLFNFlat(agent, lfn, "Fetch", 0 != (fetch.getFetchFlags() & FFdatafileoptional), isCodeSigned);
            IDistributedFile * dFile = ldFile ? ldFile->queryDistributedFile() : NULL;
            if(dFile)
            {
                verifyFetchFormatCrc(dFile);
                agent.logFileAccess(dFile, "HThor", "READ", graph);
                initParts(dFile);
            }
            else
            {
                StringBuffer buff;
                buff.append("Skipping OPT fetch of nonexistent file ").append(lfn);
                agent.addWuExceptionEx(buff.str(), WRN_SkipMissingOptFile, SeverityInformation, MSGAUD_user, "hthor");
            }
        }
        inputThread.setown(new InputHandler(this));
        inputThread->start(true);
    }

protected:
    virtual void verifyFetchFormatCrc(IDistributedFile * f) {} // do nothing here as (currently, and probably by design) not available for CSV and XML, so only implement for binary
};

class CHThorFetchActivityBase : public CHThorThreadedActivityBase, public IFetchHandlerFactory<SimpleFetchPartHandlerBase>
{
public:
    CHThorFetchActivityBase(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorArg &_arg, IHThorFetchContext &_fetch, ThorActivityKind _kind, EclGraph & _graph, IRecordSize *diskSize, IPropertyTree *_node)
      : CHThorThreadedActivityBase (_agent, _activityId, _subgraphId, _arg, _fetch, _kind, _graph, diskSize, _node)
    {
        pendingSeq = 0;
        signalSeq = 0;
        dequeuedSeq = 0;
        if (_node)
        {
            const char *recordTranslationModeHintText = _node->queryProp("hint[@name='layouttranslation']/@value");
            if (recordTranslationModeHintText)
                recordTranslationModeHint = getTranslationMode(recordTranslationModeHintText, true);
        }
    }

    ~CHThorFetchActivityBase()
    {
        clearQueue();
    }

    virtual void initializeThreadPool()
    {
        unsigned maxThreads = 400; // NB: can exceed with throttling (up to 1s delay)
        threadPool.setown(createThreadPool("hthor fetch activity thread pool", &threadFactory, true, nullptr, maxThreads));
    }

    virtual void initParts(IDistributedFile * f)
    {
        size32_t kl;
        void *k;
        fetch.getFileEncryptKey(kl,k);
        MemoryAttr encryptionkey;
        encryptionkey.setOwn(kl,k);
        parts.setown(new DistributedFileFetchHandler<SimpleFetchPartHandlerBase, const void *, FetchRequest>(f, *this, encryptionkey, prefetcher, rowAllocator));
    }

    virtual void stopParts()
    {
        if(parts)
            parts->stopThread();
    }

    virtual void fetchAll()
    {
        if(parts)
        {
            for (;;)
            {
                if (aborting)
                    break;
                const void *row = input->nextRow();
                if (!row)
                {
                    row = input->nextRow();
                    if (!row)
                        break;
                }
                offset_t rp = fetch.extractPosition(row);
                offset_t seq = addRowPlaceholder();
                parts->addRow(row, rp, seq);
            }
            parts->stopThread();
        }
        stopThread();
    }

    // to preserve order, we enqueue NULLs onto the queue and issue sequence numbers, and we only signal avail when rows in correct sequence are available
    // pendingSeq gives the next sequence number to issue; signalSeq gives the next sequence number to signal for; and dequeuedSeq gives the number actually dequeued

    offset_t addRowPlaceholder()
    {
        CriticalBlock procedure(pendingCrit);
        pending.enqueue(NULL);
        return pendingSeq++;
    }

    void setRow(const void *row, offset_t seq)
    {
        CriticalBlock procedure(pendingCrit);
        //GH->?  Why does this append allocated nulls instead of having a queue of const void??
        pending.set((unsigned)(seq-dequeuedSeq), new const void*(row));
        if(seq!=signalSeq)
            return;
        do
        {
            avail.signal();
            ++signalSeq;
        } while((signalSeq < pendingSeq) && (pending.query((unsigned)(signalSeq-dequeuedSeq)) != NULL));
    }

    const void * getRow()
    {
        while(!stopped)
        {
            avail.wait();
            CriticalBlock procedure(pendingCrit);
            if (exception)
            {
                IException *E = exception;
                exception = NULL;
                throw E;
            }
            if(pending.ordinality() == 0)
            {
                stopped = true;
                break;
            }
            const void * * ptr = pending.dequeue();
            ++dequeuedSeq;
            const void * ret = *ptr;
            delete ptr;
            if(ret)
                return ret;
        }
        return NULL;
    }

    virtual void clearQueue()
    {
        while(pending.ordinality())
        {
            const void * * ptr = pending.dequeue();
            if(ptr)
            {
                ReleaseRoxieRow(*ptr);
                delete ptr;
            }
        }
        pendingSeq = 0;
        signalSeq = 0;
        dequeuedSeq = 0;
    }
protected:
    Owned<ISourceRowPrefetcher> prefetcher;
    Owned<IOutputMetaData> actualDiskMeta;
    Owned<const IDynamicTransform> translator;
private:
    PartHandlerThreadFactory<FetchRequest> threadFactory;   
    Owned<DistributedFileFetchHandler<SimpleFetchPartHandlerBase, const void *, FetchRequest> > parts;
    offset_t pendingSeq, signalSeq, dequeuedSeq;
    QueueOf<const void *, true> pending;
    RecordTranslationMode recordTranslationModeHint = RecordTranslationMode::Unspecified;

protected:
    RecordTranslationMode getLayoutTranslationMode()
    {
        if (recordTranslationModeHint != RecordTranslationMode::Unspecified)
            return recordTranslationModeHint;
        return agent.getLayoutTranslationMode();
    }
};

class CHThorFlatFetchActivity : public CHThorFetchActivityBase, public IFlatFetchHandlerCallback
{
public:
    CHThorFlatFetchActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorFetchArg &_arg, IHThorFetchContext &_fetch, ThorActivityKind _kind, EclGraph & _graph, IRecordSize *diskSize, IPropertyTree *_node, MemoryAttr &encryptionkey)
        : CHThorFetchActivityBase (_agent, _activityId, _subgraphId, _arg, _fetch, _kind, _graph, diskSize, _node), helper(_arg)
    {}

    ~CHThorFlatFetchActivity()
    {
        waitForThreads();
    }

    virtual void ready()
    {
        CHThorFetchActivityBase::ready();
        rowLimit = helper.getRowLimit();
    }

    virtual void initParts(IDistributedFile * f) override
    {
        CHThorFetchActivityBase::initParts(f);
        prefetcher.setown(actualDiskMeta->createDiskPrefetcher());
    }

    virtual bool needsAllocator() const { return true; }

    virtual void processFetch(FetchRequest const * fetch, offset_t pos, IBufferedSerialInputStream *rawStream)
    {
        CThorContiguousRowBuffer prefetchSource;
        prefetchSource.setStream(rawStream);
        prefetchSource.reset(pos);
        prefetcher->readAhead(prefetchSource);
        const byte *rawBuffer = prefetchSource.queryRow();

        MemoryBuffer buf;
        if (translator)
        {
            MemoryBufferBuilder aBuilder(buf, 0);
            FetchVirtualFieldCallback fieldCallback(fetch->pos);
            translator->translate(aBuilder, fieldCallback, rawBuffer);
            rawBuffer = aBuilder.getSelf();
        }

        CriticalBlock procedure(transformCrit);
        size32_t thisSize;
        try
        {
            RtlDynamicRowBuilder rowBuilder(rowAllocator);
            thisSize = helper.transform(rowBuilder, rawBuffer, fetch->left, fetch->pos);
            if(thisSize)
            {
                setRow(rowBuilder.finalizeRowClear(thisSize), fetch->seq);
            }
            else
            {
                setRow(NULL, fetch->seq);
            }
        }
        catch(IException * e)
        {
            throw makeWrappedException(e);
        }
    }

    virtual void onLimitExceeded()
    {
        helper.onLimitExceeded();
    }

    virtual SimpleFetchPartHandlerBase * createFetchPartHandler(IDistributedFilePart * part, offset_t base, offset_t size, IThreadedExceptionHandler * handler, bool blockcompressed, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator)
    {
        return new FlatFetchPartHandler(*this, part, base, size, handler, threadPool, blockcompressed, encryptionkey, activityId, outputMeta, prefetcher, rowAllocator);
    }

protected:
    virtual void verifyFetchFormatCrc(IDistributedFile * f)
    {
        actualDiskMeta.set(helper.queryDiskRecordSize());
        translator.clear();
        if (getLayoutTranslationMode()==RecordTranslationMode::None)
        {
            ::verifyFormatCrcSuper(helper.getDiskFormatCrc(), f, false, true);
        }
        else
        {
            bool crcMatched = ::verifyFormatCrcSuper(helper.getDiskFormatCrc(), f, false, false);  // MORE - fetch requires all to match.
            if (!crcMatched)
            {
                IPropertyTree &props = f->queryAttributes();
                actualDiskMeta.setown(getDaliLayoutInfo(props));
                if (actualDiskMeta)
                {
                    translator.setown(createRecordTranslator(helper.queryProjectedDiskRecordSize()->queryRecordAccessor(true), actualDiskMeta->queryRecordAccessor(true)));
                    DBGLOG("Record layout translator created for %s", f->queryLogicalName());
                    translator->describe();
                    if (translator->canTranslate())
                    {
                        if (getLayoutTranslationMode()==RecordTranslationMode::PayloadRemoveOnly && translator->hasNewFields())
                            throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", f->queryLogicalName());
                        if (getLayoutTranslationMode()==RecordTranslationMode::None)
                            throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled", f->queryLogicalName());
                        VStringBuffer msg("Record layout translation required for %s", f->queryLogicalName());
                        agent.addWuExceptionEx(msg.str(), WRN_UseLayoutTranslation, SeverityInformation, MSGAUD_user, "hthor");
                    }
                    else
                        throw MakeStringException(0, "Untranslatable file layout mismatch reading file %s", f->queryLogicalName());
                }
                else
                    throw MakeStringException(0, "Untranslatable file layout mismatch reading file %s - key layout information not found", f->queryLogicalName());
            }
        }
    }

protected:
    CriticalSection transformCrit;
    IHThorFetchArg & helper;
};

extern HTHOR_API IHThorActivity *createFetchActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorFetchArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    size32_t kl;
    void *k;
    arg.getFileEncryptKey(kl,k);
    MemoryAttr encryptionkey;
    encryptionkey.setOwn(kl,k);
    return new CHThorFlatFetchActivity(_agent, _activityId, _subgraphId, arg, arg, _kind, _graph, arg.queryDiskRecordSize(), _node, encryptionkey);
}

//------------------------------------------------------------------------------------------

class CHThorCsvFetchActivity : public CHThorFetchActivityBase, public IFlatFetchHandlerCallback
{
public:
    CHThorCsvFetchActivity (IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorCsvFetchArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
        : CHThorFetchActivityBase(_agent, _activityId, _subgraphId, _arg, _arg, _kind, _graph, NULL, _node), helper(_arg)
    {
        //MORE: I have no idea what should be passed for recordSize in the line above, either something that reads a fixed size, or
        //reads a record based on the csv information
        ICsvParameters * csvInfo = _arg.queryCsvParameters();

        OwnedRoxieString lfn(fetch.getFileName());
        Owned<ILocalOrDistributedFile> ldFile = resolveLFNFlat(agent, lfn, "CsvFetch", 0 != (_arg.getFetchFlags() & FFdatafileoptional), isCodeSigned);
        IDistributedFile * dFile = ldFile ? ldFile->queryDistributedFile() : NULL;
        const char * quotes = NULL;
        const char * separators = NULL;
        const char * terminators = NULL;
        const char * escapes = NULL;
        if (dFile)
        {
            IPropertyTree & options = dFile->queryAttributes();
            quotes = options.queryProp("@csvQuote");
            separators = options.queryProp("@csvSeparate");
            terminators = options.queryProp("@csvTerminate");
            escapes = options.queryProp("@csvEscape");
            agent.logFileAccess(dFile, "HThor", "READ", graph);
        }
        else
        {
            StringBuffer buff;
            buff.append("Skipping OPT fetch of nonexistent file ").append(lfn);
            agent.addWuExceptionEx(buff.str(), WRN_SkipMissingOptFile, SeverityInformation, MSGAUD_user, "hthor");
        }
            
        csvSplitter.init(_arg.getMaxColumns(), csvInfo, quotes, separators, terminators, escapes);
    }

    ~CHThorCsvFetchActivity()
    {
        waitForThreads();
    }

    virtual bool needsAllocator() const { return true; }

    virtual void processFetch(FetchRequest const * fetch, offset_t pos, IBufferedSerialInputStream *rawStream)
    {
        rawStream->reset(pos, UnknownOffset);
        CriticalBlock procedure(transformCrit);
        size32_t maxRowSize = 10*1024*1024; // MORE - make configurable
        unsigned thisLineLength = csvSplitter.splitLine(rawStream, maxRowSize);
        if (!thisLineLength)
            return;
        try
        {
            RtlDynamicRowBuilder rowBuilder(rowAllocator);
            size32_t thisSize = helper.transform(rowBuilder, csvSplitter.queryLengths(), (const char * *)csvSplitter.queryData(), fetch->left, fetch->pos);
            if (thisSize)
            {
                setRow(rowBuilder.finalizeRowClear(thisSize), fetch->seq);
            }
            else
            {
                setRow(NULL, fetch->seq);
            }
        }
        catch(IException * e)
        {
            throw makeWrappedException(e);
        }
    }

    virtual void ready()
    {
        CHThorFetchActivityBase::ready();
        rowLimit = helper.getRowLimit();
    }

    virtual void onLimitExceeded()
    {
        helper.onLimitExceeded();
    }

    virtual SimpleFetchPartHandlerBase * createFetchPartHandler(IDistributedFilePart * part, offset_t base, offset_t size, IThreadedExceptionHandler * handler, bool blockcompressed, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator)
    {
        return new FlatFetchPartHandler(*this, part, base, size, handler, threadPool, blockcompressed, encryptionkey, activityId, outputMeta, prefetcher, rowAllocator);
    }

protected:
    CSVSplitter csvSplitter;    
    CriticalSection transformCrit;
    IHThorCsvFetchArg & helper;
};

extern HTHOR_API IHThorActivity *createCsvFetchActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorCsvFetchArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorCsvFetchActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//------------------------------------------------------------------------------------------

class XmlFetchPartHandler : public SimpleFetchPartHandlerBase, public IXMLSelect
{
public:
    IMPLEMENT_IINTERFACE;

    XmlFetchPartHandler(IXmlFetchHandlerCallback & _owner, IDistributedFilePart * _part, offset_t _base, offset_t _size, IThreadedExceptionHandler * _handler, unsigned _streamBufferSize, IThreadPool * _threadPool, bool _blockcompressed, MemoryAttr &_encryptionkey, unsigned _activityId, CachedOutputMetaData const & _outputMeta, bool _jsonFormat)
        : SimpleFetchPartHandlerBase(_part, _base, _size, _handler, _threadPool, _blockcompressed, _encryptionkey, _activityId, _outputMeta, NULL, NULL),
          owner(_owner),
          streamBufferSize(_streamBufferSize),
          jsonFormat(_jsonFormat)
    {
    }

    virtual void doRequest(FetchRequest * _fetch)
    {
        Owned<FetchRequest> fetch(_fetch);
        offset_t pos = translateFPos(fetch->pos);
        rawStream->seek(pos, IFSbegin);
        while(!lastMatch)
        {
            bool gotNext = false;
            try
            {
                gotNext = parser->next();
            }
            catch(IException * e)
            {
                StringBuffer fname;
                RemoteFilename rfn;
                part->getFilename(rfn).getPath(fname);
                throw owner.makeWrappedException(e, fname.str());
            }
            if(!gotNext)
            {
                StringBuffer fname;
                RemoteFilename rfn;
                part->getFilename(rfn).getPath(fname);
                throw MakeStringException(0, "Fetch fpos at EOF of %s", fname.str());
            }
        }
        owner.processFetched(fetch, lastMatch);
        lastMatch.clear();
        parser->reset();
    }

    virtual void openPart()
    {
        if(parser)
            return;
        FetchPartHandlerBase::openPart();
        rawStream.setown(createBufferedIOStream(rawFile, streamBufferSize));
        parser.setown(jsonFormat ? createJSONParse(*rawStream, "/", *this) : createXMLParse(*rawStream, "/", *this));
    }

    //iface IXMLSelect
    void match(IColumnProvider & entry, offset_t startOffset, offset_t endOffset)
    {
        lastMatch.set(&entry);
    }

protected:
    IXmlFetchHandlerCallback & owner;
    Owned<IFileIOStream> rawStream;
    Owned<IXMLParse> parser;
    Owned<IColumnProvider> lastMatch;
    unsigned streamBufferSize;
    bool jsonFormat;
};

class CHThorXmlFetchActivity : public CHThorFetchActivityBase, public IXmlFetchHandlerCallback
{
public:
    CHThorXmlFetchActivity(IAgentContext & _agent, unsigned _activityId, unsigned _subgraphId, IHThorXmlFetchArg & _arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
        : CHThorFetchActivityBase(_agent, _activityId, _subgraphId, _arg, _arg, _kind, _graph, NULL, _node), helper(_arg)
    {
    }

    ~CHThorXmlFetchActivity()
    {
        waitForThreads();
    }

    virtual bool needsAllocator() const { return true; }

    virtual void processFetched(FetchRequest const * fetch, IColumnProvider * lastMatch)
    {
        CriticalBlock procedure(transformCrit);
        size32_t thisSize;
        try
        {
            RtlDynamicRowBuilder rowBuilder(rowAllocator);
            thisSize = helper.transform(rowBuilder, lastMatch, fetch->left, fetch->pos);

            if(thisSize)
            {
                setRow(rowBuilder.finalizeRowClear(thisSize), fetch->seq);
            }
            else
            {   
                setRow(NULL, fetch->seq);
            }
        }
        catch(IException * e)
        {
            throw makeWrappedException(e);
        }
    }

    IException * makeWrappedException(IException * e) const { return CHThorActivityBase::makeWrappedException(e); }
    virtual IException * makeWrappedException(IException * e, char const * extra) const { return CHThorActivityBase::makeWrappedException(e, extra); }

    virtual void ready()
    {
        CHThorFetchActivityBase::ready();
        rowLimit = helper.getRowLimit();
    }

    virtual void onLimitExceeded()
    {
        helper.onLimitExceeded();
    }

    virtual SimpleFetchPartHandlerBase * createFetchPartHandler(IDistributedFilePart * part, offset_t base, offset_t size, IThreadedExceptionHandler * handler, bool blockcompressed, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator)
    {
        return new XmlFetchPartHandler(*this, part, base, size, handler, 4096, threadPool, blockcompressed, encryptionkey, activityId, outputMeta, kind==TAKjsonfetch); //MORE: need to put correct stream buffer size here, when Gavin provides it
    }

protected:
    CriticalSection transformCrit;
    IHThorXmlFetchArg & helper;
};

extern HTHOR_API IHThorActivity *createXmlFetchActivity(IAgentContext & _agent, unsigned _activityId, unsigned _subgraphId, IHThorXmlFetchArg & arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorXmlFetchActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}

//------------------------------------------------------------------------------------------

class CJoinGroup;

class MatchSet : public CInterface
{
public:
    MatchSet(CJoinGroup * _jg) : jg(_jg)
    {
    }

    ~MatchSet()
    {
        ForEachItemIn(idx, rows)
            ReleaseRoxieRow(rows.item(idx));
    }

    void addRightMatch(void * right);
    offset_t addRightPending();
    void setPendingRightMatch(offset_t seq, void * right);
    void incRightMatchCount();

    unsigned count() const { return rows.ordinality(); }
    CJoinGroup * queryJoinGroup() const { return jg; }
    void * queryRow(unsigned idx) const { return rows.item(idx); }

private:
    CJoinGroup * jg;
    PointerArray rows;
};

interface IJoinProcessor
{
    virtual CJoinGroup *createJoinGroup(const void *row) = 0;
    virtual void readyManager(IKeyManager * manager, const void * row) = 0;
    virtual void doneManager(IKeyManager * manager) = 0;
    virtual bool addMatch(MatchSet * ms, IKeyManager * manager) = 0;
    virtual void onComplete(CJoinGroup * jg) = 0;
    virtual bool leftCanMatch(const void *_left) = 0;
    virtual const IDynamicTransform * getLayoutTranslator(IDistributedFile * f) = 0;
    virtual const RtlRecord &queryIndexRecord() = 0;
    virtual void verifyIndex(IDistributedFile * f, IKeyIndex * idx, const IDynamicTransform * trans) = 0;
    virtual bool hasNewSegmentMonitors() = 0;
};

class CJoinGroup : implements IInterface, public CInterface
{
public:
    class MatchIterator
    {
    public:
        // Single threaded by now
        void const * queryRow() const { return owner.matchsets.item(ms).queryRow(idx); }
        bool start()
        {
            idx = 0;
            for(ms = 0; owner.matchsets.isItem(ms); ++ms)
                if(owner.matchsets.item(ms).count())
                    return true;
            return false;
        }
        bool next()
        {
            if(++idx < owner.matchsets.item(ms).count())
                return true;
            idx = 0;
            while(owner.matchsets.isItem(++ms))
                if(owner.matchsets.item(ms).count())
                    return true;
            return false;
        }

    private:
        friend class CJoinGroup;
        MatchIterator(CJoinGroup const & _owner) : owner(_owner) {}
        CJoinGroup const & owner;
        unsigned ms;
        unsigned idx;
    } matches;
    
    CJoinGroup *prev;  // Doubly-linked list to allow us to keep track of ones that are still in use
    CJoinGroup *next;

    CJoinGroup() : matches(*this)
    {
        // Used for head object only
        left = NULL;
        prev = NULL;
        next = NULL;
        endMarkersPending = 0;
        groupStart = NULL;
        matchcount = 0;
    }

    IMPLEMENT_IINTERFACE;

    CJoinGroup(const void *_left, IJoinProcessor *_join, CJoinGroup *_groupStart) : matches(*this),join(_join)
    {
        candidates = 0;
        left = _left;
        if (_groupStart)
        {
            groupStart = _groupStart;
            ++_groupStart->endMarkersPending;
        }
        else
        {
            groupStart = this;
            endMarkersPending = 1;
        }
        matchcount = 0;
    }

    ~CJoinGroup()
    {
        ReleaseRoxieRow(left);
        join = nullptr; // not required, but clear to highlight any race conditions
    }

    MatchSet * getMatchSet()
    {
        CriticalBlock b(crit);
        MatchSet * ms = new MatchSet(this);
        matchsets.append(*ms);
        return ms;
    }

    inline void notePending()
    {
//      assertex(!complete());
        ++groupStart->endMarkersPending;
    }

    inline bool complete() const
    {
        return groupStart->endMarkersPending == 0;
    }

    inline bool inGroup(CJoinGroup *leader) const
    {
        return groupStart==leader;
    }

    inline void noteEnd()
    {
        assertex(!complete());
        //Another completing group could cause this group to be processed once endMarkersPending is set to 0
        //So link this object to ensure it is not disposed of while this function is executing
        Linked<CJoinGroup> saveThis(this);
        if (--groupStart->endMarkersPending == 0)
        {
            join->onComplete(groupStart);
        }
    }

    inline unsigned noteCandidate()
    {
        CriticalBlock b(crit);
        return ++candidates;
    }

    inline const void *queryLeft() const
    {
        return left;
    }

    inline unsigned rowsSeen() const
    {
        CriticalBlock b(crit);
        return matchcount;
    }

    inline unsigned candidateCount() const
    {
        CriticalBlock b(crit);
        return candidates;
    }

protected:
    friend class MatchSet;
    friend class MatchIterator;
    const void *left;
    unsigned matchcount;
    CIArrayOf<MatchSet> matchsets;
    std::atomic<unsigned> endMarkersPending;
    IJoinProcessor *join = nullptr;
    mutable CriticalSection crit;
    CJoinGroup *groupStart;
    unsigned candidates;
};

void MatchSet::addRightMatch(void * right)
{
    assertex(!jg->complete());
    CriticalBlock b(jg->crit);
    rows.append(right);
    jg->matchcount++;
}

offset_t MatchSet::addRightPending()
{
    assertex(!jg->complete());
    CriticalBlock b(jg->crit);
    offset_t seq = rows.ordinality();
    rows.append(NULL);
    return seq;
}

void MatchSet::setPendingRightMatch(offset_t seq, void * right)
{
    assertex(!jg->complete());
    CriticalBlock b(jg->crit);
    rows.replace(right, (aindex_t)seq);
    jg->matchcount++;
}

void MatchSet::incRightMatchCount()
{
    assertex(!jg->complete());
    CriticalBlock b(jg->crit);
    jg->matchcount++;
}

class JoinGroupPool : public CInterface
{
    CJoinGroup *groupStart;
public:
    CJoinGroup head;
    CriticalSection crit;
    bool preserveGroups;

    JoinGroupPool(bool _preserveGroups)
    {
        head.next = &head;
        head.prev = &head;
        preserveGroups = _preserveGroups;
        groupStart = NULL;
    }

    ~JoinGroupPool()
    {
        CJoinGroup *finger = head.next;
        while (finger != &head)
        {
            CJoinGroup *next = finger->next;
            finger->Release();
            finger = next;
        }
    }

    CJoinGroup *createJoinGroup(const void *row, IJoinProcessor *join)
    {
        CJoinGroup *jg = new CJoinGroup(row, join, groupStart);
        if (preserveGroups && !groupStart)
        {
            jg->notePending(); // Make sure we wait for the group end
            groupStart = jg;
        }
        CriticalBlock c(crit);
        jg->next = &head;
        jg->prev = head.prev;
        head.prev->next = jg;
        head.prev = jg;
        return jg;
    }

    void endGroup()
    {
        if (groupStart)
            groupStart->noteEnd();
        groupStart = NULL;
    }

    void releaseJoinGroup(CJoinGroup *goer)
    {
        CriticalBlock c(crit);
        goer->next->prev = goer->prev;
        goer->prev->next = goer->next;
        goer->Release(); // MORE - could put onto another list to reuse....
    }
};

//=============================================================================================

class DistributedKeyLookupHandler;

class KeyedLookupPartHandler : extends ThreadedPartHandler<MatchSet>, implements IInterface
{
    IJoinProcessor &owner;
    Owned<IKeyManager> manager;
    IAgentContext &agent;
    DistributedKeyLookupHandler * tlk;
    IContextLogger &contextLogger;
public:
    IMPLEMENT_IINTERFACE;

    KeyedLookupPartHandler(IJoinProcessor &_owner, IDistributedFilePart *_part, DistributedKeyLookupHandler * _tlk, unsigned _subno, IThreadPool * _threadPool, IAgentContext &_agent, IContextLogger & _contextLogger);

    ~KeyedLookupPartHandler()
    {
        while(pending.dequeue())
            ;  //do nothing but dequeue as don't own MatchSets
    }

private:
    virtual void doRequest(MatchSet * ms)
    {
        agent.reportProgress(NULL);
        CJoinGroup * jg = ms->queryJoinGroup();
        owner.readyManager(manager, jg->queryLeft());
        while(manager->lookup(true))
        {
            if(owner.addMatch(ms, manager))
                break;
        }
        jg->noteEnd();
        owner.doneManager(manager);
    }

    virtual void openPart();
};

interface IKeyLookupHandler : extends IInterface
{
    virtual void addRow(const void *row) = 0;
    virtual void stopThread() = 0;
};

class DistributedKeyLookupHandler : public CInterface, implements IThreadedExceptionHandler, implements IKeyLookupHandler
{
    bool opened;
    IArrayOf<IKeyManager> managers;
    Owned<const IDynamicTransform> trans;
    UnsignedArray keyNumParts;

    IArrayOf<KeyedLookupPartHandler> parts;
    IArrayOf<IDistributedFile> keyFiles;
    IArrayOf<IDistributedFilePart> tlks;
    IJoinProcessor &owner;
    CriticalSection exceptionCrit;
    IException *exception;
    Linked<IDistributedFile> file;
    PartHandlerThreadFactory<MatchSet> threadFactory;
    Owned<IThreadPool> threadPool;
    IntArray subSizes;
    IAgentContext &agent;
    IContextLogger &contextLogger;

    void addFile(IDistributedFile &f)
    {
        if((f.numParts() == 1) || (f.queryAttributes().hasProp("@local")))
            throw MakeStringException(0, "Superfile %s contained mixed monolithic/local/noroot and regular distributed keys --- not supported", file->queryLogicalName());
        subSizes.append(parts.length());
        unsigned numParts = f.numParts()-1;
        for (unsigned idx = 0; idx < numParts; idx++)
        {
            IDistributedFilePart *part = f.getPart(idx);
            parts.append(*new KeyedLookupPartHandler(owner, part, this, tlks.ordinality(), threadPool, agent, contextLogger));
        }
        keyFiles.append(OLINK(f));
        tlks.append(*f.getPart(numParts));
        keyNumParts.append(numParts);
    }

public:
    IMPLEMENT_IINTERFACE;

    DistributedKeyLookupHandler(IDistributedFile *f, IJoinProcessor &_owner, IAgentContext &_agent, IContextLogger & _contextLogger)
        : owner(_owner), file(f), agent(_agent), contextLogger(_contextLogger)
    {
        unsigned maxThreads = 400; // NB: can exceed with throttling (up to 1s delay)
        threadPool.setown(createThreadPool("hthor keyed join lookup thread pool", &threadFactory, true, nullptr, maxThreads));
        IDistributedSuperFile *super = f->querySuperFile();
        if (super)
        {
            Owned<IDistributedFileIterator> it = super->getSubFileIterator(true);
            ForEach(*it)
                addFile(it->query());
        }
        else
            addFile(*f);

        opened = false;
        exception = NULL;
    }

    ~DistributedKeyLookupHandler()
    {
        threadPool.clear();
    }

    void addRow(const void *row)
    {
        if (owner.leftCanMatch(row))
        {
            if(!opened)
                openTLK();
            CJoinGroup *jg = owner.createJoinGroup(row);
            ForEachItemIn(subno, managers)
            {
                agent.reportProgress(NULL);
                unsigned subStart = subSizes.item(subno);
                IKeyManager & manager = managers.item(subno);
                owner.readyManager(&manager, row);
                while(manager.lookup(false))
                {
                    unsigned recptr = (unsigned)extractFpos(&manager);
                    if (recptr)
                    {
                        jg->notePending();
                        parts.item(recptr+subStart-1).addRow(jg->getMatchSet());
                    }
                }
                owner.doneManager(&manager);
            }
            jg->noteEnd();
        }
        else
        {
            CJoinGroup *jg = owner.createJoinGroup(row);
            jg->noteEnd();
        }
    }

    void openTLK()
    {
        ForEachItemIn(idx, tlks)
        {
            IDistributedFile & f = keyFiles.item(idx);
            IDistributedFilePart &tlk = tlks.item(idx);
            Owned<IKeyIndex> index = openKeyFile(tlk);
            //Owned<IRecordLayoutTranslator> 
            trans.setown(owner.getLayoutTranslator(&f));
            owner.verifyIndex(&f, index, trans);
            Owned<IKeyManager> manager = createLocalKeyManager(owner.queryIndexRecord(), index, &contextLogger, owner.hasNewSegmentMonitors(), false);
            managers.append(*manager.getLink());
        }
        opened = true;
    }

    void stopThread()
    {
        ForEachItemIn(idx, parts)
        {
            parts.item(idx).stopThread();
            parts.item(idx).join();
        }
        if (exception)
            throw exception;
    }

    virtual void noteException(IException *E)
    {
        CriticalBlock procedure(exceptionCrit);
        if (exception)
            E->Release();
        else
            exception = E;
    }

    const IDynamicTransform * queryRecordLayoutTranslator() const { return trans; }
};

KeyedLookupPartHandler::KeyedLookupPartHandler(IJoinProcessor &_owner, IDistributedFilePart *_part, DistributedKeyLookupHandler * _tlk, unsigned _subno, IThreadPool * _threadPool, IAgentContext &_agent, IContextLogger & _contextLogger)
    : ThreadedPartHandler<MatchSet>(_part, _tlk, _threadPool), owner(_owner), agent(_agent), tlk(_tlk), contextLogger(_contextLogger)
{
}

void KeyedLookupPartHandler::openPart()
{
    if(manager)
        return;
    Owned<IKeyIndex> index = openKeyFile(*part);
    manager.setown(createLocalKeyManager(owner.queryIndexRecord(), index, &contextLogger, owner.hasNewSegmentMonitors(), false));
    const IDynamicTransform * trans = tlk->queryRecordLayoutTranslator();
    if(trans && !index->isTopLevelKey())
        manager->setLayoutTranslator(trans);
}

class MonolithicKeyLookupHandler : public CInterface, implements IKeyLookupHandler
{
    IArrayOf<IKeyManager> managers;
    Linked<IDistributedFile> file;
    IDistributedSuperFile * super;
    IArrayOf<IDistributedFile> keyFiles;
    IJoinProcessor &owner;
    IAgentContext &agent;
    bool opened;
    IContextLogger &contextLogger;

public:
    IMPLEMENT_IINTERFACE;


    MonolithicKeyLookupHandler(IDistributedFile *f, IJoinProcessor &_owner, IAgentContext &_agent, IContextLogger & _contextLogger)
        : file(f), owner(_owner), agent(_agent), opened(false), contextLogger(_contextLogger)
    {
        super = f->querySuperFile();
        if (super)
        {
            Owned<IDistributedFileIterator> it = super->getSubFileIterator(true);
            ForEach(*it)
                addFile(it->query());
        }
        else
            addFile(*f);
    }

    void addFile(IDistributedFile &f)
    {
        if((f.numParts() != 1) && (!f.queryAttributes().hasProp("@local")))
            throw MakeStringException(0, "Superfile %s contained mixed monolithic/local/noroot and regular distributed keys --- not supported", file->queryLogicalName());
        keyFiles.append(OLINK(f));
    }

    void addRow(const void *row)
    {
        if (owner.leftCanMatch(row))
        {
            if(!opened)
                openKey();
            CJoinGroup *jg = owner.createJoinGroup(row);
            ForEachItemIn(idx, managers)
            {
                agent.reportProgress(NULL);
                IKeyManager & manager = managers.item(idx);
                owner.readyManager(&manager, row);
                while(manager.lookup(true))
                {
                    if(owner.addMatch(jg->getMatchSet(), &manager))
                        break;
                }
                owner.doneManager(&manager);
            }
            jg->noteEnd();
        }
        else
        {
            CJoinGroup *jg = owner.createJoinGroup(row);
            jg->noteEnd();
        }
    }

    void openKey()
    {
        ForEachItemIn(idx, keyFiles)
        {
            IDistributedFile & f = keyFiles.item(idx);
            Owned<const IDynamicTransform> trans = owner.getLayoutTranslator(&f);
            Owned<IKeyManager> manager;
            if(f.numParts() == 1)
            {
                Owned<IKeyIndex> index = openKeyFile(f.queryPart(0));
                owner.verifyIndex(&f, index, trans);
                manager.setown(createLocalKeyManager(owner.queryIndexRecord(), index, &contextLogger, owner.hasNewSegmentMonitors(), false));
            }
            else
            {
                unsigned num = f.numParts()-1;
                Owned<IKeyIndexSet> parts = createKeyIndexSet();
                Owned<IKeyIndex> index;
                for(unsigned i=0; i<num; ++i)
                {
                    index.setown(openKeyFile(f.queryPart(i)));
                    parts->addIndex(index.getLink());
                }
                owner.verifyIndex(&f, index, trans);
                manager.setown(createKeyMerger(owner.queryIndexRecord(), parts, 0, &contextLogger, owner.hasNewSegmentMonitors(), false));
            }
            if(trans)
                manager->setLayoutTranslator(trans);
            managers.append(*manager.getLink());
        }
        opened = true;
    }

    void stopThread()
    {
    }
};


//------------------------------------------------------------------------------------------

class KeyedJoinFetchRequest : public CInterface
{
public:
    MatchSet * ms;
    offset_t pos;
    offset_t seq;
    KeyedJoinFetchRequest(MatchSet * _ms, offset_t _pos, offset_t _seq) : ms(_ms), pos(_pos), seq(_seq) {}
};

class IKeyedJoinFetchHandlerCallback
{
public:
    virtual void processFetch(KeyedJoinFetchRequest const * fetch, offset_t pos, IBufferedSerialInputStream *rawStream) = 0;
};

class KeyedJoinFetchPartHandler : public FetchPartHandlerBase, public ThreadedPartHandler<KeyedJoinFetchRequest>
{
public:
    KeyedJoinFetchPartHandler(IKeyedJoinFetchHandlerCallback & _owner, IDistributedFilePart *_part, offset_t _base, offset_t _size, IThreadedExceptionHandler *_handler, IThreadPool * _threadPool, bool _blockcompressed, MemoryAttr &_encryptionkey, unsigned _activityId, CachedOutputMetaData const & _outputMeta, ISourceRowPrefetcher * _prefetcher, IEngineRowAllocator *_rowAllocator)
        : FetchPartHandlerBase(_base, _size, _blockcompressed, _encryptionkey, _activityId, _outputMeta, _prefetcher, _rowAllocator),
          ThreadedPartHandler<KeyedJoinFetchRequest>(_part, _handler, _threadPool),
          owner(_owner)
    {
    }

    virtual ~KeyedJoinFetchPartHandler()
    {
        while(KeyedJoinFetchRequest * fetch = pending.dequeue())
            fetch->Release();
    }

    IMPLEMENT_IINTERFACE;

    virtual IDistributedFilePart * queryPart() { return part; }

private:
    virtual void openPart() 
    { 
        FetchPartHandlerBase::openPart(); 
    }
    
    virtual void doRequest(KeyedJoinFetchRequest * _fetch)
    {
        Owned<KeyedJoinFetchRequest> fetch(_fetch);
        offset_t pos = translateFPos(fetch->pos);
        if(pos >= rawFile->size())
            throw MakeStringException(0, "Attempted to fetch at invalid filepos");
        owner.processFetch(fetch, pos, rawStream);
    }

private:
    IKeyedJoinFetchHandlerCallback & owner;
};

class CHThorKeyedJoinActivity  : public CHThorThreadedActivityBase, implements IJoinProcessor, public IKeyedJoinFetchHandlerCallback, public IFetchHandlerFactory<KeyedJoinFetchPartHandler>
{
    PartHandlerThreadFactory<FetchRequest> threadFactory;   
    Owned<DistributedFileFetchHandler<KeyedJoinFetchPartHandler, MatchSet *, KeyedJoinFetchRequest> > parts;
    IHThorKeyedJoinArg &helper;
    Owned<IKeyLookupHandler> lookup;
    Owned<IEngineRowAllocator> defaultRightAllocator;
    OwnedConstRoxieRow defaultRight;
    bool leftOuter;
    bool exclude;
    bool extractJoinFields;
    bool limitFail;
    bool limitOnFail;
    bool needsDiskRead;
    unsigned atMost;
    unsigned abortLimit;
    unsigned keepLimit;
    bool preserveOrder;
    bool preserveGroups;
    Owned<JoinGroupPool> pool;
    QueueOf<const void, true> pending;
    CriticalSection statsCrit, imatchCrit, fmatchCrit;
    RelaxedAtomic<unsigned> prefiltered;
    RelaxedAtomic<unsigned> postfiltered;
    RelaxedAtomic<unsigned> skips;
    OwnedRowArray extractedRows;
    Owned <ILocalOrDistributedFile> ldFile;
    IDistributedFile * dFile;
    IDistributedSuperFile * super;
    CachedOutputMetaData eclKeySize;
    Owned<ISourceRowPrefetcher> prefetcher;
    IPointerArrayOf<IOutputMetaData> actualLayouts;  // all the index layouts are saved in here to ensure their lifetime is adequate
    Owned<IOutputMetaData> actualDiskMeta;           // only one disk layout is permitted
    Owned<const IDynamicTransform> translator;
    RecordTranslationMode recordTranslationModeHint = RecordTranslationMode::Unspecified;
    bool isCodeSigned = false;
    CStatsContextLogger contextLogger;

public:
    CHThorKeyedJoinActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorKeyedJoinArg &_arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
        : CHThorThreadedActivityBase(_agent, _activityId, _subgraphId, _arg, _arg, _kind, _graph, _arg.queryDiskRecordSize(), _node), helper(_arg), contextLogger(jhtreeCacheStatistics)
    {
        prefiltered = 0;
        postfiltered = 0;
        skips = 0;
        eclKeySize.set(helper.queryIndexRecordSize());
        if (_node)
        {
            const char *recordTranslationModeHintText = _node->queryProp("hint[@name='layouttranslation']/@value");
            if (recordTranslationModeHintText)
                recordTranslationModeHint = getTranslationMode(recordTranslationModeHintText, true);
            isCodeSigned = isActivityCodeSigned(*_node);
        }
    }

    ~CHThorKeyedJoinActivity()
    {
        clearQueue();
        waitForThreads();
    }

    virtual bool needsAllocator() const { return true; }

    virtual bool hasNewSegmentMonitors() { return helper.hasNewSegmentMonitors(); }

    virtual void ready()        
    { 
        CHThorThreadedActivityBase::ready(); 

        preserveOrder = ((helper.getJoinFlags() & JFreorderable) == 0);
        preserveGroups = helper.queryOutputMeta()->isGrouped();
        needsDiskRead = helper.diskAccessRequired();
        extractJoinFields = ((helper.getJoinFlags() & JFextractjoinfields) != 0);
        atMost = helper.getJoinLimit();
        if (atMost == 0) atMost = (unsigned)-1;
        abortLimit = helper.getMatchAbortLimit();
        if (abortLimit == 0) abortLimit = (unsigned)-1;
        leftOuter = ((helper.getJoinFlags() & JFleftouter) != 0);
        exclude = ((helper.getJoinFlags() & JFexclude) != 0);
        keepLimit = helper.getKeepLimit();
        if (keepLimit == 0) keepLimit = (unsigned)-1;
        rowLimit = helper.getRowLimit();
        pool.setown(new JoinGroupPool(preserveGroups));
        limitOnFail = ((helper.getJoinFlags() & JFonfail) != 0);
        limitFail = !limitOnFail && ((helper.getJoinFlags() & JFmatchAbortLimitSkips) == 0);
        if(leftOuter || limitOnFail)
        {
            if (!defaultRight)
            {
                RtlDynamicRowBuilder rowBuilder(queryRightRowAllocator());
                size32_t thisSize = helper.createDefaultRight(rowBuilder);
                defaultRight.setown(rowBuilder.finalizeRowClear(thisSize));
            }
        }
    }

    virtual void stop()
    {
        ldFile.clear();
        CHThorThreadedActivityBase::stop();
    }

    virtual void initializeThreadPool()
    {
        unsigned maxThreads = 400; // NB: can exceed with throttling (up to 1s delay)
        threadPool.setown(createThreadPool("hthor keyed join fetch thread pool", &threadFactory, true, nullptr, maxThreads));
    }

    virtual void initParts(IDistributedFile * f)
    {
        size32_t kl;
        void *k;
        fetch.getFileEncryptKey(kl,k);
        MemoryAttr encryptionkey;
        encryptionkey.setOwn(kl,k);
        Owned<IEngineRowAllocator> inputRowAllocator;   
        if (needsDiskRead)
        {
            inputRowAllocator.setown(agent.queryCodeContext()->getRowAllocator(helper.queryDiskRecordSize(), activityId));
            parts.setown(new DistributedFileFetchHandler<KeyedJoinFetchPartHandler, MatchSet *, KeyedJoinFetchRequest>(f, *this, encryptionkey, prefetcher, inputRowAllocator));
            prefetcher.setown(actualDiskMeta->createDiskPrefetcher());
        }
    }

    virtual void stopParts()
    {
        if(parts)
            parts->stopThread();
    }

    virtual bool isGrouped() { return preserveGroups; } 

    virtual void waitForThreads()
    {
        aborting = true;
        if (inputThread)
            inputThread->join();
        lookup.clear();
        threadPool.clear();
    }

    virtual void clearQueue()
    {
        while (pending.ordinality())
            ReleaseRoxieRow(pending.dequeue());
    }

    void addRow(const void *row)
    {
        CriticalBlock procedure(pendingCrit);
        pending.enqueue(row);
        avail.signal();
    }

    const void * getRow()
    {
        if (stopped)
            return NULL;
        avail.wait();
        CriticalBlock procedure(pendingCrit);
        if (exception)
        {
            IException *E = exception;
            exception = NULL;
            throw E;
        }
        if (pending.ordinality())
            return pending.dequeue();
        else
        {
            stopped = true;
            return NULL;
        }
    }

    virtual void fetchAll()
    {
        bool eogSeen = false;  // arguably true makes more sense
        for (;;)
        {
            if (aborting)
                break;
            const void *row = input->nextRow();
            if (!row)
            {
                if (eogSeen)
                    break;
                else 
                    eogSeen = true;
                pool->endGroup();
            }
            else
            {
                eogSeen = false;
                if(lookup)
                {
                    lookup->addRow(row);
                }
                else
                {
                    CJoinGroup *jg = createJoinGroup(row);
                    jg->noteEnd();
                }
            }
        }
        if(lookup)
            lookup->stopThread();
        if (parts)
            parts->stopThread();
        stopThread();
    }

    virtual KeyedJoinFetchPartHandler * createFetchPartHandler(IDistributedFilePart * part, offset_t base, offset_t size, IThreadedExceptionHandler * handler, bool blockcompressed, MemoryAttr &encryptionkey, ISourceRowPrefetcher * prefetcher, IEngineRowAllocator *rowAllocator)
    {
        return new KeyedJoinFetchPartHandler(*this, part, base, size, handler, threadPool, blockcompressed, encryptionkey, activityId, outputMeta, prefetcher, rowAllocator);
    }

    virtual void processFetch(KeyedJoinFetchRequest const * fetch, offset_t pos, IBufferedSerialInputStream *rawStream)
    {
        CThorContiguousRowBuffer prefetchSource;
        prefetchSource.setStream(rawStream);
        prefetchSource.reset(pos);
        prefetcher->readAhead(prefetchSource);
        const byte *row = prefetchSource.queryRow();

        MemoryBuffer buf;
        if (translator)
        {
            MemoryBufferBuilder aBuilder(buf, 0);
            FetchVirtualFieldCallback fieldCallback(pos);
            translator->translate(aBuilder, fieldCallback, row);
            row = aBuilder.getSelf();
        }
        if(match(fetch->ms, row))
        {
            if(exclude)
            {
                fetch->ms->incRightMatchCount();
            }
            else
            {
                RtlDynamicRowBuilder extractBuilder(queryRightRowAllocator()); 
                size32_t size = helper.extractJoinFields(extractBuilder, row, NULL);
                void * ret = (void *) extractBuilder.finalizeRowClear(size);
                fetch->ms->setPendingRightMatch(fetch->seq, ret);
            }
        }
        fetch->ms->queryJoinGroup()->noteEnd();
    }

    bool match(MatchSet * ms, const void * right)
    {
        CriticalBlock proc(fmatchCrit);
        bool ret = helper.fetchMatch(ms->queryJoinGroup()->queryLeft(), right);
        if (!ret)
            ++postfiltered;
        return ret;
    }

    virtual bool leftCanMatch(const void * _left)
    {
        bool ret = helper.leftCanMatch(_left);
        if (!ret)
            ++prefiltered;
        return ret;
    }

    virtual CJoinGroup *createJoinGroup(const void *row)
    {
        // NOTE - single threaded
        return pool->createJoinGroup(row, this);
    }

    virtual void onComplete(CJoinGroup *jg)
    {
        CriticalBlock c(pool->crit);
        if (preserveOrder)
        {
            CJoinGroup *finger = pool->head.next;
            if(preserveGroups)
            {
                unsigned joinGroupSize = 0;
                Linked<CJoinGroup> firstInGroup = finger;
                while(finger != &pool->head)
                {
                    CJoinGroup *next = finger->next;
                    if(finger->complete())
                        joinGroupSize += doJoinGroup(finger);
                    else
                        break;
                    finger = next;
                    if(!finger->inGroup(firstInGroup))
                    {
                        if(joinGroupSize)
                            addRow(NULL);
                        joinGroupSize = 0;
                        firstInGroup.set(finger);
                    }
                }
                assertex(finger == firstInGroup.get());
            }
            else
            {
                while(finger != &pool->head)
                {
                    CJoinGroup *next = finger->next;
                    if(finger->complete())
                        doJoinGroup(finger);
                    else
                        break;
                    finger = next;
                }
            }
        }
        else if (preserveGroups)
        {
            Linked<CJoinGroup> head = jg;  // Must avoid releasing head until the end, or while loop can overrun if head is reused
            assertex(jg->inGroup(jg));
            CJoinGroup *finger = jg;
            unsigned joinGroupSize = 0;
            while (finger->inGroup(jg))
            {
                CJoinGroup *next = finger->next;
                joinGroupSize += doJoinGroup(finger);
                finger = next;
            }
            if (joinGroupSize)
                addRow(NULL);
        }
        else
            doJoinGroup(jg);
    }

    void failLimit(const void * left)
    {
        helper.onMatchAbortLimitExceeded();
        CommonXmlWriter xmlwrite(0);
        if (input && input->queryOutputMeta() && input->queryOutputMeta()->hasXML())
        {
            input->queryOutputMeta()->toXML((byte *) left, xmlwrite);
        }
        throw MakeStringException(0, "More than %d match candidates in keyed join for row %s", abortLimit, xmlwrite.str());
    }

    unsigned doJoinGroup(CJoinGroup *jg)
    {
        unsigned matched = jg->rowsSeen();
        unsigned added = 0;
        const void *left = jg->queryLeft();
        if (jg->candidateCount() > abortLimit)
        {
            if(limitFail)
                failLimit(left);
            if(limitOnFail)
            {
                Owned<IException> except;
                try
                {
                    failLimit(left);
                }
                catch(IException * e)
                {
                    except.setown(e);
                }
                assertex(except);
                size32_t transformedSize;
                RtlDynamicRowBuilder rowBuilder(rowAllocator);
                try
                {
                    transformedSize = helper.onFailTransform(rowBuilder, left, defaultRight, 0, except);
                }
                catch(IException * e)
                {
                    throw makeWrappedException(e);
                }
                if(transformedSize)
                {
                    const void * shrunk = rowBuilder.finalizeRowClear(transformedSize);
                    addRow(shrunk);
                    added++;
                }
                else
                {
                    ++skips;
                }
            }
            else
                return 0;
        }
        else if(!matched || jg->candidateCount() > atMost)
        {
            if(leftOuter)
            {
                switch(kind)
                {
                case TAKkeyedjoin:
                case TAKkeyeddenormalizegroup:
                    {
                        size32_t transformedSize = 0;
                        try
                        {
                            RtlDynamicRowBuilder rowBuilder(rowAllocator);
                            if (kind == TAKkeyedjoin)
                                transformedSize = helper.transform(rowBuilder, left, defaultRight, (__uint64)0, (unsigned)0);
                            else if (kind == TAKkeyeddenormalizegroup)
                                transformedSize = helper.transform(rowBuilder, left, defaultRight, 0, (const void * *)NULL);
                            if (transformedSize)
                            {
                                const void * shrunk = rowBuilder.finalizeRowClear(transformedSize);
                                addRow(shrunk);
                                added++;
                            }
                            else
                            {
                                ++skips;
                            }
                        }
                        catch(IException * e)
                        {
                            throw makeWrappedException(e);
                        }
                        break;
                    }
                case TAKkeyeddenormalize:
                    {
                        LinkRoxieRow(left);     
                        addRow((void *) left ); 
                        added++;
                        break;
                    }
                default:
                    throwUnexpected();
                }
            }
        }
        else if(!exclude)
        {
            switch(kind)
            {
            case TAKkeyedjoin:
                {
                    if(jg->matches.start())
                    {
                        unsigned counter = 0;
                        do
                        {
                            try
                            {
                                RtlDynamicRowBuilder rowBuilder(rowAllocator);
                                void const * row = jg->matches.queryRow();
                                if(!row) continue;
                                offset_t fpos = 0;
                                size32_t transformedSize;
                                transformedSize = helper.transform(rowBuilder, left, row, fpos, ++counter);
                                if (transformedSize)
                                {
                                    const void * shrunk = rowBuilder.finalizeRowClear(transformedSize);
                                    addRow(shrunk);
                                    added++;
                                    if (added==keepLimit)
                                        break;
                                }
                                else
                                {
                                    ++skips;
                                }
                            }
                            catch(IException * e)
                            {
                                throw makeWrappedException(e);
                            }

                        } while(jg->matches.next());
                    }
                    break;
                }
            case TAKkeyeddenormalize:
                {
                    OwnedConstRoxieRow newLeft;
                    newLeft.set(left);
                    unsigned rowSize = 0;
                    unsigned count = 0;
                    unsigned rightAdded = 0;
                    if(jg->matches.start())
                    {
                        do
                        {
                            void const * row = jg->matches.queryRow();
                            if(!row) continue;
                            ++count;
                            offset_t fpos = 0;
                            size32_t transformedSize;
                            try
                            {
                                RtlDynamicRowBuilder rowBuilder(rowAllocator);

                                transformedSize = helper.transform(rowBuilder, newLeft, row, fpos, count);
                                if (transformedSize)
                                {
                                    newLeft.setown(rowBuilder.finalizeRowClear(transformedSize));
                                    rowSize = transformedSize;
                                    rightAdded++;
                                    if (rightAdded==keepLimit)
                                        break;
                                }
                                else
                                {
                                    ++skips;
                                }
                            }
                            catch(IException * e)
                            {
                                throw makeWrappedException(e);
                            }

                        } while(jg->matches.next());
                    }
                    if (rowSize)
                    {
                        addRow(newLeft.getClear());
                        ReleaseRoxieRow(newLeft);
                        added++;
                    }
                    break;
                }
            case TAKkeyeddenormalizegroup:
                {
                    extractedRows.clear();
                    unsigned count = 0;
                    if(jg->matches.start())
                        do
                        {
                            const void * row = jg->matches.queryRow();
                            if(!row) continue;
                            if(++count > keepLimit)
                                break;
                            LinkRoxieRow(row);
                            extractedRows.append(row);
                        } while(jg->matches.next());
                    
                    size32_t transformedSize;
                    try
                    {
                        RtlDynamicRowBuilder rowBuilder(rowAllocator);
                        transformedSize = helper.transform(rowBuilder, left, extractedRows.item(0), extractedRows.ordinality(), (const void * *)extractedRows.getArray());
                        extractedRows.clear();
                        if (transformedSize)
                        {
                            const void * shrunk = rowBuilder.finalizeRowClear(transformedSize);
                            addRow(shrunk);
                            added++;
                        }
                        else
                        {
                            ++skips;
                        }
                    }
                    catch(IException * e)
                    {
                        throw makeWrappedException(e);
                    }

                    break;
                }
            default:
                throwUnexpected();
            }
        }
        pool->releaseJoinGroup(jg); // releases link to gotten row
        return added;
    }

    static bool useMonolithic(IDistributedFile & f)
    {
        return ((f.numParts() == 1) || (f.queryAttributes().hasProp("@local")));
    }

    virtual void start()
    {
        OwnedRoxieString lfn(helper.getIndexFileName());
        Owned<ILocalOrDistributedFile> ldFile = resolveLFNIndex(agent, lfn, "KeyedJoin", 0 != (helper.getJoinFlags() & JFindexoptional), true, AccessMode::tbdRead, isCodeSigned);
        dFile = ldFile ? ldFile->queryDistributedFile() : NULL;
        if (dFile)
        {
            Owned<IDistributedFile> odFile;
            odFile.setown(dFile);
            LINK(odFile);
            enterSingletonSuperfiles(odFile);
            bool mono;
            super = dFile->querySuperFile();
            if(super)
            {
                if(super->numSubFiles()==0)
                    throw MakeStringException(0, "Superkey %s empty", super->queryLogicalName());
                mono = useMonolithic(super->querySubFile(0));
            }
            else
            {
                mono = useMonolithic(*dFile);
            }
            if (mono)
                lookup.setown(new MonolithicKeyLookupHandler(dFile, *this, agent, contextLogger));
            else
                lookup.setown(new DistributedKeyLookupHandler(dFile, *this, agent, contextLogger));
            agent.logFileAccess(dFile, "HThor", "READ", graph);
        }
        else
        {
            StringBuffer buff;
            buff.append("Skipping OPT keyed join against nonexistent file ").append(lfn);
            agent.addWuExceptionEx(buff.str(), WRN_SkipMissingOptFile, SeverityInformation, MSGAUD_user, "hthor");
        }
        CHThorThreadedActivityBase::start();
    }

    virtual void readyManager(IKeyManager * manager, const void * row)
    {
        helper.createSegmentMonitors(manager, row);
        manager->finishSegmentMonitors();
        manager->reset();
    }

    virtual void doneManager(IKeyManager * manager)
    {
        manager->releaseSegmentMonitors();
    }

    virtual bool addMatch(MatchSet * ms, IKeyManager * manager)
    {
        CJoinGroup * jg = ms->queryJoinGroup();
        unsigned candTotal = jg->noteCandidate();
        if (candTotal > atMost || candTotal > abortLimit)
        {
            if ( agent.queryCodeContext()->queryDebugContext())
                agent.queryCodeContext()->queryDebugContext()->checkBreakpoint(DebugStateLimit, NULL, static_cast<IActivityBase *>(this));
            return true;
        }
        IContextLogger * ctx = nullptr;
        KLBlobProviderAdapter adapter(manager, ctx);
        byte const * rhs = manager->queryKeyBuffer();
        if(indexReadMatch(jg->queryLeft(), rhs, &adapter))
        {
            if(needsDiskRead)
            {
                size_t fposOffset = manager->queryRowSize() - sizeof(offset_t);
                offset_t fpos = rtlReadBigUInt8(rhs + fposOffset);
                jg->notePending();
                offset_t seq = ms->addRightPending();
                parts->addRow(ms, fpos, seq);
            }
            else
            {
                if(exclude)
                    ms->incRightMatchCount();
                else
                {
                    RtlDynamicRowBuilder rowBuilder(queryRightRowAllocator()); 
                    size32_t size = helper.extractJoinFields(rowBuilder, rhs, &adapter);
                    void * ret = (void *)rowBuilder.finalizeRowClear(size);
                    ms->addRightMatch(ret);
                }
            }
        }
        else
        {
            ++postfiltered;
        }
        return false;
    }

    bool indexReadMatch(const void * indexRow, const void * inputRow, IBlobProvider * blobs)
    {
        CriticalBlock proc(imatchCrit);
        return helper.indexReadMatch(indexRow, inputRow, blobs);
    }

    IEngineRowAllocator * queryRightRowAllocator()
    {
        if (!defaultRightAllocator)
            defaultRightAllocator.setown(agent.queryCodeContext()->getRowAllocator(helper.queryJoinFieldsRecordSize(), activityId));
        return defaultRightAllocator;
    }

    virtual void onLimitExceeded()
    {
        helper.onLimitExceeded();
    }

    virtual void updateProgress(IStatisticGatherer &progress) const
    {
        CHThorThreadedActivityBase::updateProgress(progress);
        StatsActivityScope scope(progress, activityId);
        progress.addStatistic(StNumPreFiltered, prefiltered);
        progress.addStatistic(StNumPostFiltered, postfiltered);
        progress.addStatistic(StNumIndexSkips, skips);
        contextLogger.recordStatistics(progress);
    }

protected:
    RecordTranslationMode getLayoutTranslationMode()
    {
        if (recordTranslationModeHint != RecordTranslationMode::Unspecified)
            return recordTranslationModeHint;
        return agent.getLayoutTranslationMode();
    }

    virtual const IDynamicTransform * getLayoutTranslator(IDistributedFile * f) override
    {
        if(getLayoutTranslationMode() == RecordTranslationMode::AlwaysECL)
        {
            verifyFormatCrc(helper.getIndexFormatCrc(), f, super ? super->queryLogicalName() : NULL, true, false);  // Traces if mismatch
            return NULL;
        }

        if(getLayoutTranslationMode() == RecordTranslationMode::None)
        {
            verifyFormatCrc(helper.getIndexFormatCrc(), f, super ? super->queryLogicalName() : NULL, true, true);
            return NULL;
        }

        if(verifyFormatCrc(helper.getIndexFormatCrc(), f, super ? super->queryLogicalName() : NULL, true, false))
        {
            return NULL;
        }

        IPropertyTree &props = f->queryAttributes();
        Owned<IOutputMetaData> actualFormat = getDaliLayoutInfo(props);
        if (actualFormat)
        {
            actualLayouts.append(actualFormat.getLink());  // ensure adequate lifespan
            Owned<const IDynamicTransform> payloadTranslator = createRecordTranslator(helper.queryProjectedIndexRecordSize()->queryRecordAccessor(true), actualFormat->queryRecordAccessor(true));
            DBGLOG("Record layout translator created for %s", f->queryLogicalName());
            payloadTranslator->describe();
            if (!payloadTranslator->canTranslate())
                throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s", f->queryLogicalName());
            if (payloadTranslator->keyedTranslated())
                throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s - keyed fields do not match", f->queryLogicalName());
            if (getLayoutTranslationMode()==RecordTranslationMode::PayloadRemoveOnly && payloadTranslator->hasNewFields())
                throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", f->queryLogicalName());
            if (getLayoutTranslationMode()==RecordTranslationMode::None)
                throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled", f->queryLogicalName());
            VStringBuffer msg("Record layout translation required for %s", f->queryLogicalName());
            agent.addWuExceptionEx(msg.str(), WRN_UseLayoutTranslation, SeverityInformation, MSGAUD_user, "hthor");
            return payloadTranslator.getClear();
        }
        throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s - key layout information not found", f->queryLogicalName());
    }

    virtual void verifyIndex(IDistributedFile * f, IKeyIndex * idx, const IDynamicTransform * trans)
    {
        if (eclKeySize.isFixedSize())
        {
            if(trans)
            {
                if (!trans->canTranslate())
                    throw MakeStringException(0, "Untranslatable key layout mismatch reading index %s", f->queryLogicalName());
                if (getLayoutTranslationMode() == RecordTranslationMode::PayloadRemoveOnly && trans->hasNewFields())
                    throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", f->queryLogicalName());
            }
            else
            {
                unsigned fileposSize = idx->hasSpecialFileposition() && !hasTrailingFileposition(eclKeySize.queryTypeInfo()) ? sizeof(offset_t) : 0;
                if(idx->keySize() != eclKeySize.getFixedSize() + fileposSize && !idx->isTopLevelKey())
                    throw MakeStringException(1002, "Key size mismatch on key %s: key file indicates record size should be %u, but ECL declaration was %u", f->queryLogicalName(), idx->keySize(), eclKeySize.getFixedSize() + fileposSize);
            }
        }
    }

    virtual void verifyFetchFormatCrc(IDistributedFile * f)
    {
        actualDiskMeta.set(helper.queryDiskRecordSize());
        translator.clear();
        if (getLayoutTranslationMode()==RecordTranslationMode::None)
        {
            ::verifyFormatCrcSuper(helper.getDiskFormatCrc(), f, false, true);
        }
        else
        {
            bool crcMatched = ::verifyFormatCrcSuper(helper.getDiskFormatCrc(), f, false, false);  // MORE - fetch requires all to match.
            if (!crcMatched)
            {
                IPropertyTree &props = f->queryAttributes();
                actualDiskMeta.setown(getDaliLayoutInfo(props));
                if (actualDiskMeta)
                {
                    translator.setown(createRecordTranslator(helper.queryProjectedDiskRecordSize()->queryRecordAccessor(true), actualDiskMeta->queryRecordAccessor(true)));
                    if (translator->canTranslate())
                    {
                        if (getLayoutTranslationMode()==RecordTranslationMode::PayloadRemoveOnly && translator->hasNewFields())
                            throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled when expected fields are missing from source.", f->queryLogicalName());
                        if (getLayoutTranslationMode()==RecordTranslationMode::None)
                            throw MakeStringException(0, "Translatable file layout mismatch reading file %s but translation disabled", f->queryLogicalName());
                        VStringBuffer msg("Record layout translation required for %s", f->queryLogicalName());
                        agent.addWuExceptionEx(msg.str(), WRN_UseLayoutTranslation, SeverityInformation, MSGAUD_user, "hthor");
                    }
                    else
                        throw MakeStringException(0, "Untranslatable file layout mismatch reading file %s", f->queryLogicalName());
                }
                else
                    throw MakeStringException(0, "Untranslatable file layout mismatch reading file %s - key layout information not found", f->queryLogicalName());
            }
        }
    }

    virtual const RtlRecord &queryIndexRecord()
    {
        return eclKeySize.queryRecordAccessor(true);
    }

    virtual void fail(char const * msg)
    {
        throw MakeStringExceptionDirect(0, msg);
    }
};

extern HTHOR_API IHThorActivity *createKeyedJoinActivity(IAgentContext &_agent, unsigned _activityId, unsigned _subgraphId, IHThorKeyedJoinArg &arg, ThorActivityKind _kind, EclGraph & _graph, IPropertyTree *_node)
{
    return new CHThorKeyedJoinActivity(_agent, _activityId, _subgraphId, arg, _kind, _graph, _node);
}
