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

#include "jliball.hpp"
#include "jhtree.hpp"
#include "ctfile.hpp"
#include "keybuild.hpp"
#include "rtlrecord.hpp"
#include "rtlformat.hpp"
#include "rtldynfield.hpp"
#include "eclhelper_dyn.hpp"
#include "eclhelper_base.hpp"
#include "hqlexpr.hpp"
#include "hqlutil.hpp"

void fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));
void fatal(const char *format, ...)
{
    va_list      args;

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);
    releaseAtoms();
    ExitModuleObjects();
    _exit(2);
}

bool optHex = false;
bool optRaw = false;
bool optFullHeader = false;
bool optHeader = false;
bool optOverwrite = false;
bool optNoSeek = false;
StringArray files;

void usage()
{
    fprintf(stderr, "Usage: dumpkey [options] dataset [dataset...]\n"
        "Options:\n"
        "  node=[n]            - dump node n (0 = just header)\n"
        "  fpos=[n]            - dump node at offset fpos\n"
        "  recs=[n]            - output n rows\n"
        "  recode=[compression-options]\n"
        "  fields=[fieldnames] - output specified fields only\n"
        "  filter=[filter]     - filter rows\n"
        "  -H                  - hex display\n"
        "  -R                  - raw output\n"
        "  -fullheader         - output full header info for each file\n"
        "  -header             - output minimal header info for each file\n"
        "\n"
        "Options when recode specified:\n"
        "  -noseek\n"
        "  -overwrite\n"
        "  outfile=filename\n"
                    );
    fflush(stderr);
    releaseAtoms();
    ExitModuleObjects();
    _exit(2);
}

void doOption(const char *opt)
{
    if (streq(opt, "-H"))
        optHex = true;
    else if (streq(opt, "-R"))
        optRaw = true;
    else if (streq(opt, "-header"))
        optHeader = true;
    else if (streq(opt, "-fullheader"))
        optFullHeader = true;
    else if (streq(opt, "-overwrite"))
        optOverwrite = true;
    else if (streq(opt, "-noseek"))
        optNoSeek = true;
    else
        usage();
}


class MyIndexWriteArg : public CThorIndexWriteArg
{
public:
    MyIndexWriteArg(const char * _filename, const char * _compression, IOutputMetaData * _meta)
     : filename(_filename), compression(_compression), meta(_meta)
    {
    }

    virtual const char * getFileName() { return filename; }
    virtual int getSequence() { return 0; }
    virtual IOutputMetaData * queryDiskRecordSize() { return meta; }
    virtual const char * queryRecordECL() { return nullptr; }
    virtual unsigned getFlags() { return compression ? TIWcompressdefined : 0; }
    virtual size32_t transform(ARowBuilder & rowBuilder, const void * src, IBlobCreator * blobs, unsigned __int64 & filepos)
    {
        throwUnexpected();
    }
    virtual unsigned getKeyedSize() { throwUnexpected(); }
    virtual unsigned getMaxKeySize() { throwUnexpected(); }
    virtual unsigned getFormatCrc() { throwUnexpected(); }
    virtual const char * queryCompression() { return compression; }

public:
    const char * filename = nullptr;
    const char * compression = nullptr;
    IOutputMetaData * meta = nullptr;
};

class MyIndexVirtualFieldCallback : public CInterfaceOf<IVirtualFieldCallback>
{
public:
    MyIndexVirtualFieldCallback(IKeyManager *_manager) : manager(_manager)
    {
    }
    virtual const char * queryLogicalFilename(const void * row) override
    {
        UNIMPLEMENTED;
    }
    virtual unsigned __int64 getFilePosition(const void * row) override
    {
        UNIMPLEMENTED;
    }
    virtual unsigned __int64 getLocalFilePosition(const void * row) override
    {
        UNIMPLEMENTED;
    }
    virtual const byte * lookupBlob(unsigned __int64 id) override
    {
        size32_t blobSize;
        return manager->loadBlob(id, blobSize, nullptr);
    }
private:
    Linked<IKeyManager> manager;
};

class DummyFileIOStream : public CInterfaceOf<IFileIOStream>
{
public:
    virtual size32_t read(size32_t max_len, void * data) override { throwUnexpected(); }
    virtual void flush() override { }
    virtual size32_t write(size32_t len, const void * data) override 
    {
        stats.ioWriteBytes.fetch_add(len);
        ++stats.ioWrites;
        if (len+offset > hwm) 
            hwm = offset+len;
        offset += len;
        return len; 
    }
    virtual void seek(offset_t pos, IFSmode origin) override 
    { 
        switch (origin)
        {
        case IFScurrent:
            offset += pos;
            break;
        case IFSend:
            offset = hwm + pos;
            break;
        case IFSbegin:
            offset = pos;
            break;
        }
        if (offset > hwm) 
            hwm = offset;
    }
    virtual offset_t size() override { return hwm; }
    virtual offset_t tell() override { return offset; }
    virtual unsigned __int64 getStatistic(StatisticKind kind) { return stats.getStatistic(kind); }
    virtual void close() override { }
private:
    offset_t offset = 0;
    offset_t hwm = 0;
    FileIOStats         stats;
};

int main(int argc, const char **argv)
{
    InitModuleObjects();
#ifdef _WIN32
    _setmode( _fileno( stdout ), _O_BINARY );
    _setmode( _fileno( stdin ), _O_BINARY );
#endif
    Owned<IProperties> globals = createProperties("dumpkey.ini", true);
    StringArray filters;
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-')
            doOption(argv[i]);
        else if (strncmp(argv[i], "filter=", 7)==0)
            filters.append(argv[i]+7);
        else if (strchr(argv[i], '='))
            globals->loadProp(argv[i]);
        else
            files.append(argv[i]);
    }
    try
    {
        StringBuffer logname("dumpkey.");
        logname.append(GetCachedHostName()).append(".");
        StringBuffer lf;
        openLogFile(lf, logname.append("log").str());
    }
    catch (IException *E)
    {
        // Silently ignore failure to open logfile.
        E->Release();
    }
    ForEachItemIn(idx, files)
    {
        try
        {
            const char * keyName = files.item(idx);
            if (!isIndexFile(keyName))
            {
                printf("%s does not appear to be an index file\n", keyName);
                continue;
            }
            Owned<IFile> in = createIFile(keyName);
            Owned<IFileIO> io = in->open(IFOread);
            if (!io)
                throw MakeStringException(999, "Failed to open file %s", keyName);

            //read with a buffer size of 4MB - for optimal speed, and minimize azure read costs
            Owned <IKeyIndex> index(createKeyIndex(keyName, 0, *io, 1, false, 0x400000));
            size32_t key_size = index->keySize();  // NOTE - in variable size case, this may be 32767 + sizeof(offset_t)
            size32_t keyedSize = index->keyedSize();
            unsigned nodeSize = index->getNodeSize();
            bool isTLK = index->isTopLevelKey();
            if (optFullHeader)
            {
                Owned<CKeyHdr> header = new CKeyHdr(index->queryId());
                MemoryAttr block(sizeof(KeyHdr));
                io->read(0, sizeof(KeyHdr), (void *)block.get());
                header->load(*(KeyHdr*)block.get());
                if (header->getKeyType() & USE_TRAILING_HEADER)
                {
                    printf("Reading trailing header at position %" I64F "d\n", in->size() - header->getNodeSize());
                    if (io->read(in->size() - header->getNodeSize(), sizeof(KeyHdr), (void *)block.get()) != sizeof(KeyHdr))
                        throw MakeStringException(4, "Invalid key %s: failed to read trailing key header", keyName);
                    header->load(*(KeyHdr*)block.get());
                }

                printf("Key '%s'\nkeySize=%d keyedSize = %d NumParts=%x, Top=%d\n", keyName, key_size, keyedSize, index->numParts(), index->isTopLevelKey());
                printf("File size = %" I64F "d, nodes = %" I64F "d\n", in->size(), in->size() / nodeSize - 1);
                printf("rootoffset=%" I64F "d[%" I64F "d]\n", header->getRootFPos(), header->getRootFPos()/nodeSize);
                printf("bloomoffset=%" I64F "d[%" I64F "d]\n", header->queryBloomHead(), header->queryBloomHead()/nodeSize);
                Owned<IPropertyTree> metadata = index->getMetadata();
                if (metadata)
                {
                    StringBuffer xml;
                    toXML(metadata, xml);
                    printf("MetaData:\n%s\n", xml.str());
                }
            }
            else if (optHeader)
            {
                if (idx)
                    printf("\n");
                printf("%s:\n\n", keyName);
            }

            if (globals->hasProp("node"))
            {
                if (stricmp(globals->queryProp("node"), "all")==0)
                {
                }
                else
                {
                    offset_t node = globals->getPropInt64("node");
                    if (node != 0)
                        index->dumpNode(stdout, node * nodeSize, globals->getPropInt("recs", 0), optRaw);
                }
            }
            else if (globals->hasProp("fpos"))
            {
                index->dumpNode(stdout, globals->getPropInt64("fpos"), globals->getPropInt("recs", 0), optRaw);
            }
            else
            {
                Owned<IKeyManager> manager;
                Owned<IPropertyTree> metadata = index->getMetadata();
                Owned<IOutputMetaData> diskmeta;
                Owned<IOutputMetaData> translatedmeta;
                ArrayOf<const RtlFieldInfo *> deleteFields;
                ArrayOf<const RtlFieldInfo *> fields;  // Note - the lifetime of the array needs to extend beyond the lifetime of outmeta. The fields themselves are shared with diskmeta, and do not need to be released.
                Owned<IOutputMetaData> outmeta;
                Owned<IXmlWriterExt> writer;
                Owned<const IDynamicTransform> translator;
                RowFilter rowFilter;
                bool recode = globals->hasProp("recode");
                unsigned __int64 count = recode ?(unsigned __int64) -1 : 1;
                count = globals->getPropInt("recs", count);
                const RtlRecordTypeInfo *outRecType = nullptr;
                if (metadata && metadata->hasProp("_rtlType"))
                {
                    MemoryBuffer layoutBin;
                    metadata->getPropBin("_rtlType", layoutBin);
                    try
                    {
                        diskmeta.setown(createTypeInfoOutputMetaData(layoutBin, false));
                    }
                    catch (IException *E)
                    {
                        EXCLOG(E);
                        E->Release();
                    }
                }
                if (!diskmeta && metadata && metadata->hasProp("_record_ECL"))
                {
                    MultiErrorReceiver errs;
                    Owned<IHqlExpression> expr = parseQuery(metadata->queryProp("_record_ECL"), &errs);
                    if (errs.errCount() == 0)
                    {
                        MemoryBuffer layoutBin;
                        if (exportBinaryType(layoutBin, expr, true))
                            diskmeta.setown(createTypeInfoOutputMetaData(layoutBin, false));
                    }
                }
                if (diskmeta)
                {
                    writer.setown(new SimpleOutputWriter);
                    const RtlRecord &inrec = diskmeta->queryRecordAccessor(true);
                    manager.setown(createLocalKeyManager(inrec, index, nullptr, true, false));
                    size32_t minRecSize = 0;
                    if (globals->hasProp("fields"))
                    {
                        StringArray fieldNames;
                        fieldNames.appendList(globals->queryProp("fields"), ",");
                        ForEachItemIn(idx, fieldNames)
                        {
                            unsigned fieldNum = inrec.getFieldNum(fieldNames.item(idx));
                            if (fieldNum == (unsigned) -1)
                                throw MakeStringException(0, "Requested output field '%s' not found", fieldNames.item(idx));
                            const RtlFieldInfo *field = inrec.queryOriginalField(fieldNum);
                            if (field->type->getType() == type_blob)
                            {
                                // We can't just use the original source field in this case (as blobs are only supported in the input)
                                // So instead, create a field in the target with the original type.
                                field = new RtlFieldStrInfo(field->name, field->xpath, field->type->queryChildType());
                                deleteFields.append(field);
                            }
                            fields.append(field);
                            minRecSize += field->type->getMinSize();
                        }
                        fields.append(nullptr);
                        outRecType = new RtlRecordTypeInfo(type_record, minRecSize, fields.getArray(0));
                        outmeta.setown(new CDynamicOutputMetaData(*outRecType));
                        translator.setown(createRecordTranslator(outmeta->queryRecordAccessor(true), inrec));
                    }
                    else
                    {
                        // Copy all fields from the source record
                        unsigned numFields = inrec.getNumFields();
                        for (unsigned idx = 0; idx < numFields;idx++)
                        {
                            const RtlFieldInfo *field = inrec.queryOriginalField(idx);
                            if (field->type->getType() == type_blob)
                            {
                                if (index->isTopLevelKey())
                                    continue;  // blob IDs in TLK are not valid
                                // See above - blob field in source needs special treatment
                                field = new RtlFieldStrInfo(field->name, field->xpath, field->type->queryChildType());
                                deleteFields.append(field);
                            }
                            fields.append(field);
                            minRecSize += field->type->getMinSize();
                        }
                        fields.append(nullptr);
                        outmeta.set(diskmeta);
                    }
                    if (filters.ordinality())
                    {
                        ForEachItemIn(idx, filters)
                        {
                            const IFieldFilter &thisFilter = rowFilter.addFilter(diskmeta->queryRecordAccessor(true), filters.item(idx));
                            unsigned idx = thisFilter.queryFieldIndex();
                            const RtlFieldInfo *field = inrec.queryOriginalField(idx);
                            if (field->flags & RFTMispayloadfield)
                                throw MakeStringException(0, "Cannot filter on payload field '%s'", field->name);
                        }
                    }
                    rowFilter.createSegmentMonitors(manager);
                }
                else
                {
                    // We don't have record info - fake it? We could pretend it's a single field...
                    UNIMPLEMENTED;
                    // manager.setown(createLocalKeyManager(fake, index, nullptr));
                }
                manager->finishSegmentMonitors();
                manager->reset();
                Owned<IFile> outFile;
                Owned<IFileIO> outFileIO;
                Owned<IFileIOStream> outFileStream;
                Owned<IKeyBuilder> keyBuilder;
                if (recode)
                {
                    const char *filename = globals->queryProp("outfile");
                    if (filename)
                    {
                        outFile.setown(createIFile(filename));
                        if(outFile->isFile() != fileBool::notFound && !optOverwrite)
                            throw MakeStringException(0, "Found preexisting index file %s (overwrite not selected)", filename);
                        
                        outFileIO.setown(outFile->openShared(IFOcreate, IFSHfull));
                        if(!outFileIO)
                            throw MakeStringException(0, "Could not write index file %s", filename);
                        outFileStream.setown(createBufferedIOStream(outFileIO, 0x200000));
                    }
                    else
                        outFileStream.setown(new DummyFileIOStream);
                    if (optNoSeek)
                        outFileStream.setown(createNoSeekIOStream(outFileStream));

                    unsigned flags = COL_PREFIX | HTREE_FULLSORT_KEY | HTREE_COMPRESSED_KEY | USE_TRAILING_HEADER;
                    if (optNoSeek)
                        flags |= TRAILING_HEADER_ONLY;
                    if (!outmeta->isFixedSize())
                        flags |= HTREE_VARSIZE;
                    //if (quickCompressed)
                    //    flags |= HTREE_QUICK_COMPRESSED_KEY;
                    // MORE - other global options
                    bool isVariable = outmeta->isVariableSize();
                    size32_t fileposSize = hasTrailingFileposition(outmeta->queryTypeInfo()) ? sizeof(offset_t) : 0;
                    size32_t maxDiskRecordSize;
                    if (isTLK)
                        maxDiskRecordSize = keyedSize;
                    else if (isVariable)
                        maxDiskRecordSize = KEYBUILD_MAXLENGTH;
                    else
                        maxDiskRecordSize = outmeta->getFixedSize()-fileposSize;
                    const RtlRecord &indexRecord = outmeta->queryRecordAccessor(true);
                    size32_t keyedSize = indexRecord.getFixedOffset(indexRecord.getNumKeyedFields());
                    MyIndexWriteArg helper(filename, globals->queryProp("recode"), outmeta);  // MORE - is lifetime ok? Bloom support? May need longer lifetime once we add bloom support...
                    keyBuilder.setown(createKeyBuilder(outFileStream, flags, maxDiskRecordSize, nodeSize, keyedSize, 0, &helper, nullptr, false, index->isTopLevelKey()));
                }
                MyIndexVirtualFieldCallback callback(manager);
                size32_t maxSizeSeen = 0;
                while (manager->lookup(true) && count--)
                {
                    byte const * buffer = manager->queryKeyBuffer();
                    size32_t size = manager->queryRowSize();
                    unsigned __int64 seq = manager->querySequence();
                    if (recode)
                    {
                        if (translator)
                        {
                            MemoryBuffer buf;
                            MemoryBufferBuilder aBuilder(buf, 0);
                            size = translator->translate(aBuilder, callback, buffer);
                            if (size)
                            {
                                // MORE - think about fpos
                                keyBuilder->processKeyData((const char *) aBuilder.getSelf(), 0, size);
                            }
                            else
                                count++;  // Row was postfiltered
                        }
                        else
                        {
                            if (hasTrailingFileposition(outmeta->queryTypeInfo()))
                                size -= sizeof(offset_t);
                            keyBuilder->processKeyData((const char *) buffer, manager->queryFPos(), size);
                            if (size > maxSizeSeen)
                                maxSizeSeen = size;
                        }
                    }
                    else if (optRaw)
                    {
                        fwrite(buffer, 1, size, stdout);
                    }
                    else if (optHex)
                    {
                        for (unsigned i = 0; i < size; i++)
                            printf("%02x", ((unsigned char) buffer[i]) & 0xff);
                        printf("  :%" I64F "u\n", seq);
                    }
                    else if (translator)
                    {
                        MemoryBuffer buf;
                        MemoryBufferBuilder aBuilder(buf, 0);
                        if (translator->translate(aBuilder, callback, buffer))
                        {
                            outmeta->toXML(aBuilder.getSelf(), *writer.get());
                            printf("%s\n", writer->str());
                            writer->clear();
                        }
                        else
                            count++;  // Row was postfiltered
                    }
                    else
                        printf("%.*s  :%" I64F "u\n", size, buffer, seq);
                    manager->releaseBlobs();
                }
                if (keyBuilder)
                {
                    keyBuilder->finish(metadata, nullptr, maxSizeSeen, nullptr);
                    printf("New key has %" I64F "u leaves, %" I64F "u branches, %" I64F "u duplicates\n", keyBuilder->getStatistic(StNumLeafCacheAdds), keyBuilder->getStatistic(StNumNodeCacheAdds), keyBuilder->getStatistic(StNumDuplicateKeys));
                    printf("Original key size: %" I64F "u bytes\n", const_cast<IFileIO *>(index->queryFileIO())->size());
                    printf("New key size: %" I64F "u bytes (%" I64F "u bytes written in %" I64F "u writes)\n", outFileStream->size(), outFileStream->getStatistic(StSizeDiskWrite), outFileStream->getStatistic(StNumDiskWrites));
                    keyBuilder.clear();
                }
                if (outRecType)
                    outRecType->doDelete();
                ForEachItemIn(idx, deleteFields)
                {
                    delete deleteFields.item(idx);
                }
            }
        }
        catch (IException *E)
        {
            StringBuffer msg;
            E->errorMessage(msg);
            printf("%s\n", msg.str());
            E->Release();
        }
    }
    releaseAtoms();
    ExitModuleObjects();
    return 0;
}


