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
#include "thormisc.hpp"
#include "thexception.hpp"
#include "thbufdef.hpp"
#include "tsorta.hpp"

#include "thorstep.ipp"

#include "thmergeslave.ipp"

#ifdef _DEBUG
//#define _FULL_TRACE
#endif

#define _STABLE_MERGE

class GlobalMergeSlaveActivity : public CSlaveActivity
{
    typedef CSlaveActivity PARENT;

public:
    IArrayOf<IRowStream> streams; 
    IHThorMergeArg *helper;
    Owned<IFile> tmpfile;
    Owned<IRowStream> out;
    mptag_t masterMpTag;
    mptag_t intertag;
    offset_t *partitionpos;
    size32_t chunkmaxsize;
    unsigned width;
    unsigned rwFlags = 0x0; // flags for streams (e.g. compression flags)

    class cRemoteStream : implements IRowStream, public CSimpleInterface
    {
        GlobalMergeSlaveActivity *parent;
        Linked<IOutputRowDeserializer> deserializer;
        ThorRowQueue rows;
        rank_t rank;
        mptag_t tag;
        bool eos;
        Linked<IEngineRowAllocator> allocator;
        offset_t bufpos;
        size32_t bufsize;

    public:
        IMPLEMENT_IINTERFACE_USING(CSimpleInterface);
        cRemoteStream(IThorRowInterfaces *rowif, unsigned i,mptag_t _tag, GlobalMergeSlaveActivity *_parent)
            : allocator(rowif->queryRowAllocator()), deserializer(rowif->queryRowDeserializer())
        {
            rank = (rank_t)(i+1);
            tag = _tag;
            parent = _parent;
            eos = false;
            bufpos = 0;
            bufsize = 0;
        }

        ~cRemoteStream()
        {
            while (rows.ordinality()) 
                ReleaseThorRow(rows.dequeue());
        }

        void load()
        {
            bufpos += bufsize;
            if (rank==parent->queryJobChannel().queryMyRank()) {
#ifdef _FULL_TRACE
                ::ActPrintLog(parent, "Merge cRemoteStream::load, get chunk from node %d (local) pos = %" I64F "d",rank,bufpos);
#endif
                bufsize = parent->getRows(rank-1,bufpos,rows);
            }
            else {
#ifdef _FULL_TRACE
                ::ActPrintLog(parent, "Merge cRemoteStream::load, get chunk from node %d tag %d (remote) pos = %" I64F "d",rank,tag,bufpos);
#endif
                CMessageBuffer mb;
                mb.append(bufpos);
                parent->queryContainer().queryJobChannel().queryJobComm().sendRecv(mb, rank, tag);
                bufsize = mb.length();
                CThorStreamDeserializerSource dsz(bufsize,mb.bufferBase());
                while (!dsz.eos()) {
                    RtlDynamicRowBuilder rowBuilder(allocator);
                    size32_t sz=deserializer->deserialize(rowBuilder,dsz);
                    rows.enqueue(rowBuilder.finalizeRowClear(sz));
                }
            }
#ifdef _FULL_TRACE
            ::ActPrintLog(parent, "Merge cRemoteStream::load, got chunk %d",bufsize);
#endif
        }

        const void *nextRow()
        {
            if (!eos) {
                if (rows.ordinality()==0) 
                    load();
                if (rows.ordinality()) 
                    return rows.dequeue();
                eos = true;
            }
            return NULL;
        }
        void stop()
        {
            // no action
        }
    };

    class cProvider: public Thread
    {
        mptag_t tag = TAG_NULL;
        GlobalMergeSlaveActivity *parent = nullptr;
        bool stopped = true;
        Linked<IOutputRowSerializer> serializer;
    public:
        void init(GlobalMergeSlaveActivity *_parent, IOutputRowSerializer *_serializer,mptag_t _tag)
        {
            serializer.set(_serializer);
            parent = _parent;
            stopped = false;
            tag = _tag;
        }
        int run()
        {
            while (!stopped) {
#ifdef _FULL_TRACE
                ::ActPrintLog(parent, "Merge cProvider Receiving request from tag %d",tag);
#endif
                CMessageBuffer mb;
                rank_t sender;
                if (parent->queryJobChannel().queryJobComm().recv(mb, RANK_ALL, tag, &sender)&&!stopped)  {
                    offset_t pos;
                    mb.read(pos);
#ifdef _FULL_TRACE
                    ::ActPrintLog(parent, "Merge cProvider Received request from %d pos = %" I64F "d",sender,pos);
#endif
                    ThorRowQueue rows;
                    size32_t sz = parent->getRows(sender-1, pos, rows);
                    mb.clear().ensureCapacity(sz);
                    CMemoryRowSerializer msz(mb);
                    for (;;) {                                          // there must be a better way than deserializing then serializing!!
                        OwnedConstThorRow row = rows.dequeue();
                        if (!row)
                            break;
                        serializer->serialize(msz,(const byte *)row.get());
                    }
                    if (sz!=mb.length()) {
                        static bool logged=false;
                        if (!logged) {
                            logged = true;
                            IWARNLOG("GlobalMergeSlaveActivity mismatch serialize, deserialize (%u,%u)",sz,mb.length());
                        }
                    }
#ifdef _FULL_TRACE
                    ::ActPrintLog(parent, "Merge cProvider replying size %d",mb.length());
#endif
                    if (!stopped)
                        parent->queryJobChannel().queryJobComm().reply(mb);
                }
            }
#ifdef _FULL_TRACE
            ::ActPrintLog(parent, "Merge cProvider exiting",tag);
#endif
            return 0;
        }
        void stop() 
        {
            if (!stopped)
            {
                stopped = true;
                parent->queryJobChannel().queryJobComm().cancel(RANK_ALL, tag);
                join();
            }
        }

    } provider;

    Owned<CThorRowLinkCounter> linkcounter;

    IRowStream * createPartitionMerger(CThorKeyArray &sample)
    {
        queryJobChannel().queryJobComm().verifyAll();
        CMessageBuffer mb;
        mptag_t replytag = queryMPServer().createReplyTag();
        mptag_t intertag = queryMPServer().createReplyTag();
        serializeMPtag(mb,replytag);
        serializeMPtag(mb,intertag);
        sample.serialize(mb);
        ActPrintLog("MERGE sending samples to master");
        if (!queryJobChannel().queryJobComm().send(mb, (rank_t)0, masterMpTag))
            return NULL;
        ActPrintLog("MERGE receiving partition from master");
        rank_t sender;
        if (!queryJobChannel().queryJobComm().recv(mb, 0, replytag, &sender)) 
            return NULL;
        assertex((unsigned)sender==0);
        ActPrintLog("MERGE received partition from master");
        mb.read(width);
        mptag_t *intertags = new mptag_t[width];
        mb.read(sizeof(mptag_t)*width,intertags);

        CThorKeyArray partition(*this, queryRowInterfaces(this),helper->querySerialize(),helper->queryCompare(),helper->queryCompareKey(),helper->queryCompareRowKey());
        partition.deserialize(mb,false);
        partition.calcPositions(tmpfile, sample, rwFlags);
        partitionpos = new offset_t[width];
        unsigned i;
        for (i=0;i<width;i++) {
            streams.append(*new cRemoteStream(queryRowInterfaces(this),i,intertags[i],this));
            partitionpos[i] = partition.getFilePos(i);
#ifdef _FULL_TRACE
            if (i<width-1) {
                partition.traceKey("MERGE partition key",i);
            }
#endif

            ::ActPrintLog(this, thorDetailedLogLevel, "Merge: partitionpos[%d] = %" I64F "d",i,partitionpos[i]);
        }
        delete [] intertags;
        provider.init(this,queryRowSerializer(),intertag);
        provider.start(true);
        if (!streams.ordinality())
            return NULL;
        if (streams.ordinality()==1)
            return &streams.popGet();
        return createRowStreamMerger(streams.ordinality(), streams.getArray(), helper->queryCompare(), helper->dedup(), linkcounter);
    }
public:
    GlobalMergeSlaveActivity(CGraphElementBase *_container) : CSlaveActivity(_container)
    {
        partitionpos = NULL;
        linkcounter.setown(new CThorRowLinkCounter);
        helper = (IHThorMergeArg *)queryHelper();
        appendOutputLinked(this);
    }

    ~GlobalMergeSlaveActivity()
    {
        try {
            if (tmpfile) 
                tmpfile->remove();
        }
        catch (IException *e) {
            ActPrintLog(e,"~GlobalMergeSlaveActivity");
            e->Release();
        }
        if (partitionpos)
            delete [] partitionpos;
    }

// IThorSlaveActivity overloaded methods
    void init(MemoryBuffer &data, MemoryBuffer &slaveData) override
    {
        masterMpTag = container.queryJobChannel().deserializeMPTag(data);
        rwFlags = DEFAULT_RWFLAGS;
        if (getOptBool(THOROPT_COMPRESS_SPILLS, true))
        {
            StringBuffer compType;
            getOpt(THOROPT_COMPRESS_SPILL_TYPE, compType);
            unsigned spillCompInfo = 0x0;
            setCompFlag(compType, spillCompInfo);
            if (spillCompInfo)
            {
                rwFlags |= rw_compress;
                rwFlags |= spillCompInfo;
            }
        }
    }

    void abort()
    {
        CSlaveActivity::abort();
        provider.stop();
    }


// IThorDataLink
    virtual void start() override
    {
        ActivityTimer s(slaveTimerStats, timeActivities);
        ForEachItemIn(i, inputs)
        {
            IThorDataLink * input = queryInput(i);
            try
            {
                startInput(i);
            }
            catch (CATCHALL)
            {
                ActPrintLog("MERGE(%" ACTPF "d): Error starting input %d", container.queryId(), i);
                ForEachItemIn(s, streams)
                    streams.item(s).stop();
                throw;
            }
            if (input->isGrouped())
                streams.append(*createUngroupStream(queryInputStream(i)));
            else
                streams.append(*LINK(queryInputStream(i)));
        }
#ifndef _STABLE_MERGE
        // shuffle streams otherwise will all be reading in order initially
        unsigned n=streams.ordinality();
        while (n>1) {
            unsigned i = getRandom()%n;
            n--;
            if (i!=n) 
                streams.swap(i,n);
        }
#endif
        if (partitionpos)
        {
            delete [] partitionpos;
            partitionpos = NULL;
        }
        chunkmaxsize = MERGE_TRANSFER_BUFFER_SIZE;
        Owned<IRowStream> merged = createRowStreamMerger(streams.ordinality(), streams.getArray(), helper->queryCompare(),helper->dedup(), linkcounter);
        StringBuffer tmpname;
        GetTempFilePath(tmpname,"merge");
        tmpfile.setown(createIFile(tmpname.str()));
        Owned<IRowWriter> writer =  createRowWriter(tmpfile, this, rwFlags);
        CThorKeyArray sample(*this, this, helper->querySerialize(), helper->queryCompare(), helper->queryCompareKey(), helper->queryCompareRowKey());
        sample.setSampling(MERGE_TRANSFER_BUFFER_SIZE);
        ActPrintLog("MERGE: start gather");
        for (;;) {
            OwnedConstThorRow row = merged->nextRow();
            if (!row)
                break;
            sample.add(row);
            writer->putRow(row.getClear());
        }
        merged->stop();
        merged.clear();
        streams.kill();
        ActPrintLog("MERGE: gather done");
        writer->flush();
        writer.clear();
        out.setown(createPartitionMerger(sample));

        dataLinkStart();
    }

    virtual void stop() override
    {
        if (out)
            out->stop();
        dataLinkStop();
    }

    void kill()
    {
        provider.stop();
        streams.kill();
        CSlaveActivity::kill();
    }

    size32_t getRows(unsigned idx, offset_t pos, ThorRowQueue &out)
    {  // always returns whole rows

        offset_t start = idx?partitionpos[idx-1]:0;
        pos += start;
        offset_t end = partitionpos[idx];
        if (pos>=end)
            return 0;
        Owned<IExtRowStream> rs = createRowStreamEx(tmpfile, queryRowInterfaces(this), pos, end, (unsigned __int64)-1, rwFlags); // this is not good
        offset_t so = rs->getOffset();
        size32_t len = 0;
        size32_t chunksize = chunkmaxsize;
        if (pos+chunksize>end) 
            chunksize = (size32_t)(end-pos);
        do {
            OwnedConstThorRow r = rs->nextRow();
            size32_t l = (size32_t)(rs->getOffset()-so);
            if (!r)
                break;
            if (pos+l>end) {
                ActPrintLogEx(&queryContainer(), thorlog_null, MCwarning, "overrun in GlobalMergeSlaveActivity::getRows(%u,%" I64F "d,%" I64F "d)",l,rs->getOffset(),end);
                break; // don't think should happen
            }
            len = l;
            out.enqueue(r.getClear());
        } while (len<chunksize);
        return len;
    }

    const void * nextRow() 
    {
        if (!abortSoon) {
            if (out) {
                OwnedConstThorRow row = out->nextRow();
                if (row) {
                    dataLinkIncrement();
                    return row.getClear();
                }
            }
        }
        return NULL;
    }

    virtual bool isGrouped() const override { return false; }
    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) const override
    {
        initMetaInfo(info);
        calcMetaInfoSize(info, inputs);
    }
};



class LocalMergeSlaveActivity : public CSlaveActivity
{
    IArrayOf<IRowStream> streams; 
    Owned<IRowStream> out;
    IHThorMergeArg *helper;
public:
    LocalMergeSlaveActivity(CGraphElementBase *_container) : CSlaveActivity(_container)
    {
        helper = (IHThorMergeArg *)queryHelper();
        setRequireInitData(false);
        appendOutputLinked(this);
    }

// IThorDataLink
    virtual void start() override
    {
        ActivityTimer s(slaveTimerStats, timeActivities);
        ForEachItemIn(i, inputs)
        {
            IThorDataLink *input = queryInput(i);
            try
            {
                startInput(i);
            }
            catch (CATCHALL) {
                ActPrintLog("MERGE(%" ACTPF "d): Error starting input %d", container.queryId(), i);
                ForEachItemIn(s, streams)
                    streams.item(s).stop();
                throw;
            }
            if (input->isGrouped())
                streams.append(*createUngroupStream(queryInputStream(i)));
            else
                streams.append(*LINK(queryInputStream(i)));
        }
        Owned<IRowLinkCounter> linkcounter = new CThorRowLinkCounter;
        out.setown(createRowStreamMerger(streams.ordinality(), streams.getArray(), helper->queryCompare(), helper->dedup(), linkcounter));
        dataLinkStart();
    }

    virtual void stop() override
    {
        if (out)
            out->stop();
        dataLinkStop();
    }

    void kill()
    {
        streams.kill();
        CSlaveActivity::kill();
    }

    CATCH_NEXTROW()
    {
        ActivityTimer t(slaveTimerStats, timeActivities);
        if (!abortSoon) {
            OwnedConstThorRow row = out->nextRow();
            if (row) {
                dataLinkIncrement();
                return row.getClear();
            }
        }
        return NULL;
    }

    virtual bool isGrouped() const override { return false; }
    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) const override
    {
        initMetaInfo(info);
        calcMetaInfoSize(info, inputs);
    }
};


class CThorStreamMerger : public CStreamMerger
{
    IEngineRowStream **inputArray;
public:
    CThorStreamMerger() : CStreamMerger(true) {}

    void initInputs(unsigned _numInputs, IEngineRowStream ** _inputArray)
    {
        CStreamMerger::initInputs(_numInputs);
        inputArray = _inputArray;
    }
    virtual bool pullInput(unsigned i, const void *seek, unsigned numFields, const SmartStepExtra *stepExtra)
    {
        const void *next;
        bool matches = true;
        if (seek)
            next = inputArray[i]->nextRowGE(seek, numFields, matches, *stepExtra);
        else
            next = inputArray[i]->ungroupedNextRow();
        pending[i] = (void *)next;
        pendingMatches[i] = matches;
        return (next != NULL);
    }
    virtual void releaseRow(const void *row)
    {
        ReleaseThorRow(row);
    }
};


class CNWayMergeActivity : public CThorNarySlaveActivity, public CThorSteppable
{
    typedef CThorNarySlaveActivity PARENT;

    IHThorNWayMergeArg *helper;
    CThorStreamMerger merger;
    CSteppingMeta meta;
    bool initializedMeta;

    PointerArrayOf<IEngineRowStream> expandedInputStreams;

public:
    IMPLEMENT_IINTERFACE_USING(PARENT);

    CNWayMergeActivity(CGraphElementBase *container) : CThorNarySlaveActivity(container), CThorSteppable(this)
    {
        helper = (IHThorNWayMergeArg *)queryHelper();
        merger.init(helper->queryCompare(), helper->dedup(), helper->querySteppingMeta()->queryCompare());
        initializedMeta = false;
        appendOutputLinked(this);
    }
    ~CNWayMergeActivity()
    {
        merger.cleanup();
    }
    virtual void start() override
    {
        CThorNarySlaveActivity::start();
        merger.initInputs(expandedStreams.length(), expandedStreams.getArray());
    }
    virtual void stop() override
    {
        merger.done();
        CThorNarySlaveActivity::stop();
    }
    virtual void reset() override
    {
        CThorNarySlaveActivity::reset();
        initializedMeta = false;
    }
    CATCH_NEXTROW()
    {
        ActivityTimer t(slaveTimerStats, timeActivities);
        OwnedConstThorRow ret = merger.nextRow();
        if (ret)
        {
            dataLinkIncrement();
            return ret.getClear();
        }
        return NULL;
    }
    virtual const void *nextRowGE(const void *seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
    {
        try { return nextRowGENoCatch(seek, numFields, wasCompleteMatch, stepExtra); }
        CATCH_NEXTROWX_CATCH;
    }
    virtual const void *nextRowGENoCatch(const void *seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
    {
        ActivityTimer t(slaveTimerStats, timeActivities);
        OwnedConstThorRow ret = merger.nextRowGE(seek, numFields, wasCompleteMatch, stepExtra);
        if (ret)
        {
            dataLinkIncrement();
            return ret.getClear();
        }
        return NULL;
    }
    virtual bool isGrouped() const override { return false; }
    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) const override
    {
        initMetaInfo(info);
        calcMetaInfoSize(info, inputs);
    }
    virtual void setInputStream(unsigned index, CThorInput &input, bool consumerOrdered) override
    {
        CThorNarySlaveActivity::setInputStream(index, input, consumerOrdered);
        CThorSteppable::setInputStream(index, input, consumerOrdered);
    }
    virtual IInputSteppingMeta *querySteppingMeta()
    {
        if (expandedInputs.ordinality() == 0)
            return NULL;
        if (!initializedMeta)
        {
            meta.init(helper->querySteppingMeta(), false);
            ForEachItemIn(i, expandedInputs)
            {
                if (meta.getNumFields() == 0)
                    break;
                IInputSteppingMeta *inputMeta = expandedInputs.item(i)->querySteppingMeta();
                meta.intersect(inputMeta);
            }
            initializedMeta = true;
        }
        if (meta.getNumFields() == 0)
            return NULL;
        return &meta;
    }
};


CActivityBase *createNWayMergeActivity(CGraphElementBase *container)
{
    return new CNWayMergeActivity(container);
}


CActivityBase *createLocalMergeSlave(CGraphElementBase *container)
{
    return new LocalMergeSlaveActivity(container);
}

CActivityBase *createGlobalMergeSlave(CGraphElementBase *container)
{
    return new GlobalMergeSlaveActivity(container);
}

