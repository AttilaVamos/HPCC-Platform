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

#ifndef WORKUNIT_INCL
#define WORKUNIT_INCL

#ifdef WORKUNIT_EXPORTS
    #define WORKUNIT_API DECL_EXPORT
#else
    #define WORKUNIT_API DECL_IMPORT
#endif

#define MINIMUM_SCHEDULE_PRIORITY 0
#define DEFAULT_SCHEDULE_PRIORITY 50
#define MAXIMUM_SCHEDULE_PRIORITY 100

#include "jiface.hpp"
#include "errorlist.h"
#include "jtime.hpp"
#include "jsocket.hpp"
#include "jstats.h"
#include "jutil.hpp"
#include "jprop.hpp"
#include "jmisc.hpp"
#include "jtrace.hpp"
#include "wuattr.hpp"
#include <vector>
#include <list>
#include <utility>
#include <map>
#include <string>

#define LEGACY_GLOBAL_SCOPE "workunit"
#define GLOBAL_SCOPE ""

#define CHEAP_UCHAR_DEF
#ifdef _WIN32
typedef char16_t UChar;
#else //_WIN32
typedef unsigned short UChar;
#endif //_WIN32


enum : unsigned
{
    WUERR_ModifyFilterAfterFinalize = WORKUNIT_ERROR_START,
    WUERR_FinalizeAfterFinalize,
    WUERR_InvalidDebugValueName,
};

// error codes
#define QUERRREG_ADD_NAMEDQUERY     QUERYREGISTRY_ERROR_START
#define QUERRREG_REMOVE_NAMEDQUERY  QUERYREGISTRY_ERROR_START+1
#define QUERRREG_WUID               QUERYREGISTRY_ERROR_START+2
#define QUERRREG_DLL                QUERYREGISTRY_ERROR_START+3
#define QUERRREG_SETALIAS           QUERYREGISTRY_ERROR_START+4
#define QUERRREG_RESOLVEALIAS       QUERYREGISTRY_ERROR_START+5
#define QUERRREG_REMOVEALIAS        QUERYREGISTRY_ERROR_START+6
#define QUERRREG_QUERY_REGISTRY     QUERYREGISTRY_ERROR_START+7
#define QUERRREG_SUSPEND            QUERYREGISTRY_ERROR_START+8
#define QUERRREG_UNSUSPEND          QUERYREGISTRY_ERROR_START+9
#define QUERRREG_COMMENT            QUERYREGISTRY_ERROR_START+10

class CDateTime;
interface ISetToXmlTransformer;
interface ISecManager;
interface ISecUser;
class StringArray;
class StringBuffer;

typedef unsigned __int64 __uint64;

interface IQueueSwitcher : extends IInterface
{
    virtual void * getQ(const char * qname, const char * wuid) = 0;
    virtual void putQ(const char * qname, void * qitem) = 0;
    virtual bool isAuto() = 0;
};


//!  PriorityClass
//! Not sure what the real current class values are -- TBD

enum WUPriorityClass
{
    PriorityClassUnknown = 0,
    PriorityClassLow = 1,
    PriorityClassNormal = 2,
    PriorityClassHigh = 3,
    PriorityClassSize = 4
};



enum WUQueryType
{
    QueryTypeUnknown = 0,
    QueryTypeEcl = 1,
    QueryTypeSql = 2,
    QueryTypeXml = 3,
    QueryTypeAttribute = 4,
    QueryTypeSize = 5
};



enum WUState
{
    WUStateUnknown = 0,
    WUStateCompiled = 1,
    WUStateRunning = 2,
    WUStateCompleted = 3,
    WUStateFailed = 4,
    WUStateArchived = 5,
    WUStateAborting = 6,
    WUStateAborted = 7,
    WUStateBlocked = 8,
    WUStateSubmitted = 9,
    WUStateScheduled = 10,
    WUStateCompiling = 11,
    WUStateWait = 12,
    WUStateUploadingFiles = 13,
    WUStateDebugPaused = 14,
    WUStateDebugRunning = 15,
    WUStatePaused = 16,
    WUStateSize = 17
};



enum WUAction
{
    WUActionUnknown = 0,
    WUActionCompile = 1,
    WUActionCheck = 2,
    WUActionRun = 3,
    WUActionExecuteExisting = 4,
    WUActionPause = 5,
    WUActionPauseNow = 6,
    WUActionResume = 7,
    WUActionGenerateDebugInfo = 8,
    WUActionSize = 9, // NB: must be last
};


enum WUResultStatus
{
    ResultStatusUndefined = 0,
    ResultStatusCalculated = 1,
    ResultStatusSupplied = 2,
    ResultStatusFailed = 3,
    ResultStatusPartial = 4,
    ResultStatusSize = 5
};



//! IConstWUGraph

enum WUGraphType
{
    GraphTypeAny = 0,
    GraphTypeProgress = 1,
    GraphTypeEcl = 2,
    GraphTypeActivities = 3,
    GraphTypeSubProgress = 4,
    GraphTypeSize = 5
};


interface IConstWUGraphIterator;
interface ICsvToRawTransformer;
interface IXmlToRawTransformer;
interface IPropertyTree;
interface IPropertyTreeIterator;


enum WUGraphState
{
    WUGraphUnknown = 0,
    WUGraphComplete = 1,
    WUGraphRunning = 2,
    WUGraphFailed = 3,
    WUGraphPaused = 4
};

interface IConstWUGraphMeta : extends IInterface
{
    virtual IStringVal & getName(IStringVal & ret) const = 0;
    virtual IStringVal & getLabel(IStringVal & ret) const = 0;
    virtual IStringVal & getTypeName(IStringVal & ret) const = 0;
    virtual WUGraphType getType() const = 0;
    virtual WUGraphState getState() const = 0;
    virtual unsigned getWfid() const = 0;
};

interface IConstWUGraph : extends IConstWUGraphMeta
{
    virtual IStringVal & getXGMML(IStringVal & ret, bool mergeProgress, bool doFormatStats) const = 0;
    virtual IPropertyTree * getXGMMLTree(bool mergeProgress, bool doFormatStats) const = 0;
    virtual IPropertyTree * getXGMMLTreeRaw() const = 0;
};

interface IConstWUGraphIterator : extends IScmIterator
{
    virtual IConstWUGraph & query() = 0;
};

interface IConstWUTimer : extends IInterface
{
    virtual IStringVal & getName(IStringVal & ret) const = 0;
    virtual unsigned getCount() const = 0;
    virtual unsigned getDuration() const = 0;
};

interface IWUTimer : extends IConstWUTimer
{
    virtual void setName(const char * str) = 0;
    virtual void setCount(unsigned c) = 0;
    virtual void setDuration(unsigned d) = 0;
};

interface IConstWUTimerIterator : extends IScmIterator
{
    virtual IConstWUTimer & query() = 0;
};

interface IConstWUGraphMetaIterator : extends IScmIterator
{
    virtual IConstWUGraphMeta & query() = 0;
};


constexpr int LibraryBaseSequence = 1000000000;
//! IWUResult
enum
{
    ResultSequenceStored = -1,
    ResultSequencePersist = -2,
    ResultSequenceInternal = -3,
    ResultSequenceOnce = -4,
};

extern WORKUNIT_API bool isSpecialResultSequence(unsigned sequence);

enum WUResultFormat
{
    ResultFormatRaw = 0,
    ResultFormatXml = 1,
    ResultFormatXmlSet = 2,
    ResultFormatCsv = 3,
    ResultFormatSize = 4
};


interface ITypeInfo;

interface IConstWUResult : extends IInterface
{
    virtual WUResultStatus getResultStatus() const = 0;
    virtual IStringVal & getResultName(IStringVal & str) const = 0;
    virtual int getResultSequence() const = 0;
    virtual bool isResultScalar() const = 0;
    virtual IStringVal & getResultXml(IStringVal & str, bool hidePasswords) const = 0;
    virtual unsigned getResultFetchSize() const = 0;
    virtual __int64 getResultTotalRowCount() const = 0;
    virtual bool hasTotalRowCount() const = 0;
    virtual __int64 getResultRowCount() const = 0;
    virtual void getResultDataset(IStringVal & ecl, IStringVal & defs) const = 0;
    virtual IStringVal & getResultLogicalName(IStringVal & ecl) const = 0;
    virtual IStringVal & getResultKeyField(IStringVal & ecl) const = 0;
    virtual unsigned getResultRequestedRows() const = 0;
    virtual __int64 getResultInt() const = 0;
    virtual bool getResultBool() const = 0;
    virtual double getResultReal() const = 0;
    virtual IStringVal & getResultString(IStringVal & str, bool hidePasswords) const = 0;
    virtual IDataVal & getResultRaw(IDataVal & data, IXmlToRawTransformer * xmlTransformer, ICsvToRawTransformer * csvTransformer) const = 0;
    virtual IDataVal & getResultUnicode(IDataVal & data) const = 0;
    virtual IStringVal & getResultEclSchema(IStringVal & str) const = 0;
    virtual __int64 getResultRawSize(IXmlToRawTransformer * xmlTransformer, ICsvToRawTransformer * csvTransformer) const = 0;
    virtual IDataVal & getResultRaw(IDataVal & data, __int64 from, __int64 length, IXmlToRawTransformer * xmlTransformer, ICsvToRawTransformer * csvTransformer) const = 0;
    virtual IStringVal & getResultRecordSizeEntry(IStringVal & str) const = 0;
    virtual IStringVal & getResultTransformerEntry(IStringVal & str) const = 0;
    virtual __int64 getResultRowLimit() const = 0;
    virtual IStringVal & getResultFilename(IStringVal & str) const = 0;
    virtual WUResultFormat getResultFormat() const = 0;
    virtual unsigned getResultHash() const = 0;
    virtual void getResultDecimal(void * val, unsigned length, unsigned precision, bool isSigned) const = 0;
    virtual bool getResultIsAll() const = 0;
    virtual const IProperties *queryResultXmlns() = 0;
    virtual IStringVal &getResultFieldOpt(const char *name, IStringVal &str) const = 0;
    virtual void getSchema(IArrayOf<ITypeInfo> &types, StringAttrArray &names, IStringVal * eclText) const = 0;
    virtual void getResultWriteLocation(IStringVal & _graph, unsigned & _activityId) const = 0;
};


interface IWUResult : extends IConstWUResult
{
    virtual void setResultStatus(WUResultStatus status) = 0;
    virtual void setResultName(const char * name) = 0;
    virtual void setResultSequence(unsigned seq) = 0;
    virtual void setResultSchemaRaw(unsigned len, const void * schema) = 0;
    virtual void setResultScalar(bool isScalar) = 0;
    virtual void setResultRaw(unsigned len, const void * data, WUResultFormat format) = 0;
    virtual void setResultFetchSize(unsigned rows) = 0;
    virtual void setResultTotalRowCount(__int64 rows) = 0;
    virtual void setResultRowCount(__int64 rows) = 0;
    virtual void setResultDataset(const char * ecl, const char * defs) = 0;
    virtual void setResultLogicalName(const char * logicalName) = 0;
    virtual void setResultKeyField(const char * name) = 0;
    virtual void setResultRequestedRows(unsigned rowcount) = 0;
    virtual void setResultInt(__int64 val) = 0;
    virtual void setResultBool(bool val) = 0;
    virtual void setResultReal(double val) = 0;
    virtual void setResultString(const char * val, unsigned length) = 0;
    virtual void setResultData(const void * val, unsigned length) = 0;
    virtual void setResultDecimal(const void * val, unsigned length) = 0;
    virtual void addResultRaw(unsigned len, const void * data, WUResultFormat format) = 0;
    virtual void setResultRecordSizeEntry(const char * val) = 0;
    virtual void setResultTransformerEntry(const char * val) = 0;
    virtual void setResultRowLimit(__int64 value) = 0;
    virtual void setResultFilename(const char * name) = 0;
    virtual void setResultUnicode(const void * val, unsigned length) = 0;
    virtual void setResultUInt(__uint64 val) = 0;
    virtual void setResultIsAll(bool value) = 0;
    virtual void setResultFormat(WUResultFormat format) = 0;
    virtual void setResultXML(const char * xml) = 0;
    virtual void setResultRow(unsigned len, const void * data) = 0;
    virtual void setResultXmlns(const char *prefix, const char *uri) = 0;
    virtual void setResultFieldOpt(const char *name, const char *value)=0;
    virtual void setResultWriteLocation(const char * _graph, unsigned _activityId) = 0;

    virtual IPropertyTree *queryPTree() = 0;
};


interface IConstWUResultIterator : extends IScmIterator
{
    virtual IConstWUResult & query() = 0;
};


//! IWUQuery

enum WUFileType
{
    FileTypeCpp = 0,
    FileTypeDll = 1,
    FileTypeResText = 2,
    FileTypeHintXml = 3,
    FileTypeXml = 4,
    FileTypeLog = 5,
    FileTypePostMortem = 6,
    FileTypeSize = 7
};

extern WORKUNIT_API EnumMapping queryFileTypes[];


interface IConstWUAssociatedFile : extends IInterface
{
    virtual WUFileType getType() const = 0;
    virtual IStringVal & getDescription(IStringVal & ret) const = 0;
    virtual IStringVal & getIp(IStringVal & ret) const = 0;
    virtual IStringVal & getName(IStringVal & ret) const = 0;
    virtual IStringVal & getNameTail(IStringVal & ret) const = 0;
    virtual unsigned getCrc() const = 0;
    virtual unsigned getMinActivityId() const = 0;
    virtual unsigned getMaxActivityId() const = 0;
};



interface IConstWUAssociatedFileIterator : extends IScmIterator
{
    virtual IConstWUAssociatedFile & query() = 0;
};


interface IConstWUFieldUsage : extends IInterface // Defines a file (dataset or index) that contains used fields from queries
{
    virtual const char * queryName() const = 0;
};

interface IConstWUFieldUsageIterator : extends IScmIterator // Iterates over files that contains used fields
{
    virtual IConstWUFieldUsage * get() const = 0;
};

interface IConstWUFileUsage : extends IInterface // Defines a file (dataset or index) that contains used fields from queries
{
    virtual const char * queryName() const = 0;
    virtual const char * queryType() const = 0; // used file type: "dataset" or "index"
    virtual unsigned getNumFields() const = 0;
    virtual unsigned getNumFieldsUsed() const = 0;
    virtual IConstWUFieldUsageIterator * getFields() const = 0;
};

interface IConstWUFileUsageIterator : extends IScmIterator // Iterates over files that contains used fields
{
    virtual IConstWUFileUsage * get() const = 0;
};


interface IConstWUQuery : extends IInterface
{
    virtual WUQueryType getQueryType() const = 0;
    virtual IStringVal & getQueryText(IStringVal & str) const = 0;
    virtual IStringVal & getQueryName(IStringVal & str) const = 0;
    virtual IStringVal & getQueryDllName(IStringVal & str) const = 0;
    virtual unsigned getQueryDllCrc() const = 0;
    virtual IStringVal & getQueryCppName(IStringVal & str) const = 0;
    virtual IStringVal & getQueryResTxtName(IStringVal & str) const = 0;
    virtual IConstWUAssociatedFile * getAssociatedFile(WUFileType type, unsigned index) const = 0;
    virtual IConstWUAssociatedFileIterator & getAssociatedFiles() const = 0;
    virtual IStringVal & getQueryShortText(IStringVal & str) const = 0;
    virtual IStringVal & getQueryMainDefinition(IStringVal & str) const = 0;
    virtual bool isArchive() const = 0;
    virtual bool hasArchive() const = 0;
};


interface IWUQuery : extends IConstWUQuery
{
    virtual void setQueryType(WUQueryType qt) = 0;
    virtual void setQueryText(const char * pstr) = 0;
    virtual void setQueryName(const char * pstr) = 0;
    virtual void addAssociatedFile(WUFileType type, const char * name, const char * ip, const char * desc, unsigned crc, unsigned minActivity, unsigned maxActivity) = 0;
    virtual void removeAssociatedFiles() = 0;
    virtual void setQueryMainDefinition(const char * str) = 0;
    virtual void removeAssociatedFile(WUFileType type, const char * name, const char * desc) = 0;
};


interface IConstWUWebServicesInfo : extends IInterface
{
    virtual IStringVal & getModuleName(IStringVal & str) const = 0;
    virtual IStringVal & getAttributeName(IStringVal & str) const = 0;
    virtual IStringVal & getDefaultName(IStringVal & str) const = 0;
    virtual IStringVal & getInfo(const char * name, IStringVal & str) const = 0;
    virtual unsigned getWebServicesCRC() const = 0;
    virtual IStringVal & getText(const char * name, IStringVal & str) const = 0;
};


interface IWUWebServicesInfo : extends IConstWUWebServicesInfo
{
    virtual void setModuleName(const char * pstr) = 0;
    virtual void setAttributeName(const char * pstr) = 0;
    virtual void setDefaultName(const char * pstr) = 0;
    virtual void setInfo(const char * name, const char * info) = 0;
    virtual void setWebServicesCRC(unsigned crc) = 0;
    virtual void setText(const char * name, const char * text) = 0;
};


//! IWUPlugin

interface IConstWUPlugin : extends IInterface
{
    virtual IStringVal & getPluginName(IStringVal & str) const = 0;
    virtual IStringVal & getPluginVersion(IStringVal & str) const = 0;
};


interface IWUPlugin : extends IConstWUPlugin
{
    virtual void setPluginName(const char * str) = 0;
    virtual void setPluginVersion(const char * str) = 0;
};


interface IConstWUPluginIterator : extends IScmIterator
{
    virtual IConstWUPlugin & query() = 0;
};

interface IConstWULibrary : extends IInterface
{
    virtual IStringVal & getName(IStringVal & str) const = 0;
};


interface IWULibrary : extends IConstWULibrary
{
    virtual void setName(const char * str) = 0;
};


interface IConstWULibraryIterator : extends IScmIterator
{
    virtual IConstWULibrary & query() = 0;
};


//! IWUException

interface IConstWUException : extends IInterface
{
    virtual IStringVal & getExceptionSource(IStringVal & str) const = 0;
    virtual IStringVal & getExceptionMessage(IStringVal & str) const = 0;
    virtual unsigned getExceptionCode() const = 0;
    virtual ErrorSeverity getSeverity() const = 0;
    virtual IStringVal & getTimeStamp(IStringVal & dt) const = 0;
    virtual IStringVal & getExceptionFileName(IStringVal & str) const = 0;
    virtual unsigned getExceptionLineNo() const = 0;
    virtual unsigned getExceptionColumn() const = 0;
    virtual unsigned getSequence() const = 0;
    virtual unsigned getActivityId() const = 0;
    virtual const char * queryScope() const = 0;
    virtual unsigned getPriority() const = 0;  // For ordering within a severity - e.g. warnings about inefficiency
    virtual double getCost() const = 0; // cost optimizer cost saving estimate.
};


interface IWUException : extends IConstWUException
{
    virtual void setExceptionSource(const char * str) = 0;
    virtual void setExceptionMessage(const char * str) = 0;
    virtual void setExceptionCode(unsigned code) = 0;
    virtual void setSeverity(ErrorSeverity level) = 0;
    virtual void setTimeStamp(const char * dt) = 0;
    virtual void setExceptionFileName(const char * str) = 0;
    virtual void setExceptionLineNo(unsigned r) = 0;
    virtual void setExceptionColumn(unsigned c) = 0;
    virtual void setActivityId(unsigned _id) = 0;
    virtual void setScope(const char * _scope) = 0;
    virtual void setPriority(unsigned _priority) = 0;
    virtual void setCost(double cost) = 0; // cost optimizer cost saving estimate.
};


interface IConstWUExceptionIterator : extends IScmIterator
{
    virtual IConstWUException & query() = 0;
};

// This enumeration is currently duplicated in workunit.hpp and environment.hpp.  They must stay in sync.
#ifndef ENGINE_CLUSTER_TYPE
#define ENGINE_CLUSTER_TYPE
enum ClusterType { NoCluster, HThorCluster, RoxieCluster, ThorLCRCluster };
#endif

extern WORKUNIT_API ClusterType getClusterType(const char * platform, ClusterType dft = NoCluster);
extern WORKUNIT_API const char *clusterTypeString(ClusterType clusterType, bool lcrSensitive);
inline bool isThorCluster(ClusterType type) { return (type == ThorLCRCluster); }

//! IWorkflowItem
enum WFType
{
    WFTypeNormal = 0,
    WFTypeSuccess = 1,
    WFTypeFailure = 2,
    WFTypeRecovery = 3,
    WFTypeWait = 4,
    WFTypeSize = 5
};

enum WFMode
{
    WFModeNormal = 0,
    WFModeCondition = 1,
    WFModeSequential = 2,
    WFModeParallel = 3,
    WFModePersist = 4,
    WFModeBeginWait = 5,
    WFModeWait = 6,
    WFModeOnce = 7,
    WFModeUnused = 8,
    WFModeCritical = 9,
    WFModeOrdered = 10,
    //for parallel workflow at runtime
    WFModeConditionExpression = 11,
    WFModePersistActivator = 12,
    //Size needs to be the last mode
    WFModeSize = 13
};

enum WFState
{
    WFStateNull = 0,
    WFStateReqd = 1,
    WFStateDone = 2,
    WFStateFail = 3,
    WFStateSkip = 4,
    WFStateWait = 5,
    WFStateBlocked = 6,
    WFStateSize = 7
};



interface IWorkflowDependencyIterator : extends IScmIterator
{
    virtual unsigned query() const = 0;
};


interface IWorkflowEvent : extends IInterface
{
    virtual const char * queryName() const = 0;
    virtual const char * queryText() const = 0;
    virtual bool matches(const char * name, const char * text) const = 0;
};


interface IConstWorkflowItem : extends IInterface
{
    virtual unsigned queryWfid() const = 0;
    virtual bool isScheduled() const = 0;
    virtual bool isScheduledNow() const = 0;
    virtual IWorkflowEvent * getScheduleEvent() const = 0;
    virtual unsigned querySchedulePriority() const = 0;
    virtual bool hasScheduleCount() const = 0;
    virtual unsigned queryScheduleCount() const = 0;
    virtual IWorkflowDependencyIterator * getDependencies() const = 0;
    virtual WFType queryType() const = 0;
    virtual WFMode queryMode() const = 0;
    virtual unsigned querySuccess() const = 0;
    virtual unsigned queryFailure() const = 0;
    virtual unsigned queryRecovery() const = 0;
    virtual unsigned queryRetriesAllowed() const = 0;
    virtual unsigned queryContingencyFor() const = 0;
    virtual IStringVal & getPersistName(IStringVal & val) const = 0;
    virtual unsigned queryPersistWfid() const = 0;
    virtual int queryPersistCopies() const = 0;  // 0 - unmangled name,  < 0 - use default, > 0 - max number
    virtual bool queryPersistRefresh() const = 0;
    virtual IStringVal &getCriticalName(IStringVal & val) const = 0;
    virtual unsigned queryScheduleCountRemaining() const = 0;
    virtual WFState queryState() const = 0;
    virtual unsigned queryRetriesRemaining() const = 0;
    virtual int queryFailCode() const = 0;
    virtual const char * queryFailMessage() const = 0;
    virtual const char * queryEventName() const = 0;
    virtual const char * queryEventExtra() const = 0;
    virtual unsigned queryScheduledWfid() const = 0;
    virtual IStringVal & queryCluster(IStringVal & val) const = 0;
    virtual IStringVal & getLabel(IStringVal & val) const = 0;
};
inline bool isPersist(const IConstWorkflowItem & item) { return item.queryMode() == WFModePersist; }
inline bool isCritical(const IConstWorkflowItem & item) { return item.queryMode() == WFModeCritical; }


interface IRuntimeWorkflowItem : extends IConstWorkflowItem
{
    virtual void setState(WFState state) = 0;
    virtual bool testAndDecRetries() = 0;
    virtual bool decAndTestScheduleCountRemaining() = 0;
    virtual void setFailInfo(int code, const char * message) = 0;
    virtual void reset() = 0;
    virtual void setEvent(const char * name, const char * extra) = 0;
    virtual void incScheduleCount() = 0;
};


interface IWorkflowItem : extends IRuntimeWorkflowItem
{
    virtual void setScheduledNow() = 0;
    virtual void setScheduledOn(const char * name, const char * text) = 0;
    virtual void setSchedulePriority(unsigned priority) = 0;
    virtual void setScheduleCount(unsigned count) = 0;
    virtual void addDependency(unsigned wfid) = 0;
    virtual void setPersistInfo(const char * name, unsigned wfid, int maxCopies, bool refresh) = 0;
    virtual void setCriticalInfo(char const * name) = 0;
    virtual void syncRuntimeData(const IConstWorkflowItem & other) = 0;
    virtual void setScheduledWfid(unsigned wfid) = 0;
    virtual void setCluster(const char * cluster) = 0;
    virtual void setLabel(const char * label) = 0;
};


interface IConstWorkflowItemIterator : extends IScmIterator
{
    virtual IConstWorkflowItem * query() const = 0;
};


interface IRuntimeWorkflowItemIterator : extends IConstWorkflowItemIterator
{
    virtual IRuntimeWorkflowItem * get() const = 0;
};


interface IWorkflowItemIterator : extends IConstWorkflowItemIterator
{
    virtual IWorkflowItem * get() const = 0;
};


interface IWorkflowItemArray : extends IInterface
{
    virtual IRuntimeWorkflowItem & queryWfid(unsigned wfid) = 0;
    virtual unsigned count() const = 0;
    virtual IRuntimeWorkflowItemIterator * getSequenceIterator() = 0;
    virtual void addClone(const IConstWorkflowItem * other) = 0;
    virtual bool hasScheduling() const = 0;
};


enum LocalFileUploadType
{
    UploadTypeFileSpray = 0,
    UploadTypeWUResult = 1,
    UploadTypeWUResultCsv = 2,
    UploadTypeWUResultXml = 3,
    UploadTypeSize = 4
};



interface IConstLocalFileUpload : extends IInterface
{
    virtual unsigned queryID() const = 0;
    virtual LocalFileUploadType queryType() const = 0;
    virtual IStringVal & getSource(IStringVal & ret) const = 0;
    virtual IStringVal & getDestination(IStringVal & ret) const = 0;
    virtual IStringVal & getEventTag(IStringVal & ret) const = 0;
};


interface IConstLocalFileUploadIterator : extends IScmIterator
{
    virtual IConstLocalFileUpload * get() = 0;
};


enum WUSubscribeOptions
{
    SubscribeOptionState = 1,
    SubscribeOptionAbort = 2,
    SubscribeOptionAction = 4
};

interface IWorkUnitSubscriber
{
    virtual void notify(WUSubscribeOptions flags, unsigned valueLen, const void *valueData) = 0;
};

interface IWorkUnitWatcher : extends IInterface
{
    virtual void unsubscribe() = 0;
};

interface IWUGraphProgress;
interface IPropertyTree;

enum WUFileKind
{
    WUFileStandard = 0,
    WUFileTemporary = 1,
    WUFileOwned = 2,
    WUFileJobOwned = 3
};



typedef unsigned __int64 WUGraphIDType;
typedef unsigned __int64 WUNodeIDType;

interface IWUGraphProgress;
interface IWUGraphStats;
interface IPropertyTree;
interface IConstWUGraphProgress : extends IInterface
{
    virtual IPropertyTree * getProgressTree(bool doFormat) = 0;
    virtual unsigned queryFormatVersion() = 0;
};


interface IWUGraphStats : public IInterface
{
    virtual IStatisticGatherer & queryStatsBuilder() = 0;
};


interface IConstWUTimeStamp : extends IInterface
{
    virtual IStringVal & getApplication(IStringVal & str) const = 0;
    virtual IStringVal & getEvent(IStringVal & str) const = 0;
    virtual IStringVal & getDate(IStringVal & dt) const = 0;
};


interface IConstWUTimeStampIterator : extends IScmIterator
{
    virtual IConstWUTimeStamp & query() = 0;
};


interface IConstWUAppValue : extends IInterface
{
    virtual const char *queryApplication() const = 0;
    virtual const char *queryName() const = 0;
    virtual const char *queryValue() const = 0;
};


interface IConstWUAppValueIterator : extends IScmIterator
{
    virtual IConstWUAppValue & query() = 0;
};

//More: Counts on files? optional target?
/*
 * Statistics are used to store timestamps, time periods, counts memory usage and any other interesting statistic
 * which is collected as the query is built or executed.
 *
 * Each statistic has the following details:
 *
 * Creator      - Which component created the statistic.  This should be the name of the component instance i.e., "mythor_x_y" rather than the type ("thor").
 *              - It can also be used to represent a subcomponent e.g., mythor:0 the master, mythor:10 means the 10th slave.
 *              ?? Is the sub component always numeric ??
 *
 * Kind         - The specific kind of the statistic - uses a global enumeration.  (Engines can locally use different ranges of numbers and map them to the global enumeration).
 *
 * Measure      - What kind of statistic is it?  It can always be derived from the kind.  The following values are supported:
 *                      time - elapsed time in nanoseconds
 *                      timestamp/when - a point in time (?to the nanosecond?)
 *                      count - a count of the number of occurrences
 *                      memory/size - a quantity of memory (or disk) measured in kb
 *                      load - measure of cpu activity (stored as 1/1000000 core)
 *                      skew - a measure of skew. 10000 = perfectly balanced, range [0..infinity]
 *
 *Optional:
 *
 * Description  - Purely for display, calculated if not explicitly supplied.
 * Scope        - Where in the execution of the task is statistic gathered?  It can have multiple levels (separated by colons), and statistics for
 *                a given level can be retrieved independently.  The following scopes are supported:
 *                "global" - the default if not specified.  Globally/within a workunit.
 *                "wfid<n>" - within workflow item <n> (is this at all useful?)
 *                "graphn[:sg<n>[:ac<n>"]"
 *                Possibly additional levels to allow multiple instances of an activity when used in a graph etc.
 *
 * Target       - The target of the thing being monitored.  E.g., a filename.  ?? Is this needed?  Should this be combined with scope??
 *
 * Examples:
 * creator(mythor),scope(),kind(TimeWall)            total time spend processing in thor            search ct(thor),scope(),kind(TimeWall)
 * creator(mythor),scope(graph1),kind(TimeWall)    - total time spent processing a graph
 * creator(mythor),scope(graph1:sg<subid>),kind(TimeElapsed)    - total time spent processing a subgraph
 * creator(mythor),scope(graph1:sg<n>:ac<id>),kind(TimeElapsed) - time for activity from start to stop
 * creator(mythor),scope(graph1:sg<n>:ac<id>),kind(TimeLocal)   - time spent locally processing
 * creator(mythor),scope(graph1:sg<n>:ac<id>),kind(TimeWallRowRange) - time from first row to last row
 * creator(mythor),scope(graph1:sg<n>:ac<id>),kind(WhenFirstRow) - timestamp for first row
 * creator(myeclccserver@myip),scope(compile),kind(TimeWall)
 * creator(myeclccserver@myip),scope(compile:transform),kind(TimeWall)
 * creator(myeclccserver@myip),scope(compile:transform:fold),kind(TimeWall)
 *
 * Other possibilities
 * creator(myesp),scope(filefile::abc::def),kind(NumAccesses)
 *
 * Configuring statistic collection:
 * - Each engine allows the statistics being collected to be specified.  You need to configure the area (time/memory/disk/), the level of detail by component and location.
 *
 * Some background notes:
 * - Start time and end time (time processing first and last record) is useful for detecting time skew/serial activities.
 * - Information is lost if you only show final skew, rather than skew over time, but storing time series data is
 *   prohibitive so we may need to create some derived metrics.
 * - The engines need options to control what information is gathered.
 * - Need to ensure clocks are synchronized for the timestamps to be useful.
 *
 * Some typical analysis we want to perform:
 * - Activities that show significant skew between first (or last) record times between nodes.
 * - Activities where the majority of the time is being spent.
 *
 * Filtering statistics - with control over who is creating it, what is being recorded, and
 * [in order of importance]
 * - which level of creator you are interested in [summary or individual nodes, or both]    (*;*:*)?
 * - which level of scope (interested in activities, or just by graph, or both)
 * - a particular kind of statistic
 * - A particular creator (including fixed/wildcarded sub-component)
 *
 * => Provide a class for representing a filter, which can be used to filter when recording and retrieving.  Start simple and then extend.
 * Text representation creator(*,*:*),creatordepth(n),creatorkind(x),scopedepth(n),scopekind(xxx,yyy),scope(*:23),kind(x).
 *
 * Examples
 * kind(TimeElapsed),scopetype(subgraph)   - subgraph timings
 * kind(Time*),scopedepth(1)&kind(TimeElapsed),scopedepth(2),scopetype(subgraph) - all legacy global timings.
 * creatortype(thor),kind(TimeElapsed),scope("")   - how much time has been spent on thor? (Need to sum?)
 * creator(mythor),kind(TimeElapsed),scope("")     - how much time has been spent on *this* thor.
 * kind(TimeElapsed),scope("compiled")     - how much time has been spent on *this* thor.
 *
 * Need to efficiently
 * - Get all (simple) stats for a graph/activities (creator(*),kind(*),scope(x:*)) - display in graph, finding hotspots
 * - Get all stats for an activity (creator(*:*),measure(*:*),scope(x:y)) - providing details in a graph
 * - Merge stats from multiple components
 * - Merge stats from multiple runs?
 *
 * Bulk updates will tend to be for a given component and should only need minor processing (e.g. patch ids) or no processing to update/combine.
 * - You need to be able to filter only a certain level of statistic - e.g., times for transforms, but not details of those transforms.
 *
 * => suggest store as
 * stats[creatorDepth,scopeDepth][creator] { kind, scope, value, target }.  sorted by (scope, target, kind)
 * - allows high level filtering by level
 * - allows combining with minor updates.
 * - possibly extra structure within each creator - maybe depending on the level of the scope
 * - need to be sub-sorted to allow efficient merging between creators (e.g. for calculating skew)
 * - possibly different structure when collecting [e.g., indexed by stat, or using a local stat mapping ] and storing.
 *
 * Use (local) tables to map scope->uid.  Possibly implicitly defined on first occurrence, or zip the entire structure.
 *
 * The progress information should be stored compressed, with min,max child ids to avoid decompressing
 */

// Should the statistics classes be able to be stored globally e.g., for esp and other non workunit contexts?

/*
 * Work out how to represent all of the existing statistics
 *
 * Counts of number of skips on an index:  kind(CountIndexSkips),measure(count),scope(workunit | filename | graph:activity#)
 * Activity start time                     kind(WhenStart),measure(timestamp),scope(graph:activity#),creator(mythor)
 *                                         kind(WhenFirstRow),measure(timestamp),scope(graph:activity#),creator(mythor:slave#)
 * Number of times files accessed by esp:  kind(CountFileAccess),measure(count),scope(),target(filename);
 * Elapsed/remaining time for sprays:
 */

/*
 * Statistics and their kinds - prefixed indicates their type.  Note generally the same type won't be reused for two different things.
 *
 * TimeStamps:
 *      StWhenGraphStart           - When a graph starts
 *      StWhenFirstRow             - When the first row is processed by slave activity
 *
 * Time
 *      StTimeParseQuery
 *      StTimeTransformQuery
 *      StTimeTransformQuery_Fold         - transformquery:fold?  effectively an extra level of detail on the kind.
 *      StTimeTransformQuery_Normalize
 *      StTimeElapsedExecuting      - Elapsed wall time between first row and last row.
 *      StTimeExecuting             - Cpu time spent executing
 *
 *
 * Memory
 *      StSizeGeneratedCpp
 *      StSizePeakMemory
 *
 * Count
 *      StCountIndexSeeks
 *      StCountIndexScans
 *
 * Load
 *      StLoadWhileSorting          - Average load while processing a sort?
 *
 * Skew
 *      StSkewRecordDistribution    - Skew on the records across the different nodes
 *      StSkewExecutionTime         - Skew in the execution time between activities.
 *
 */


interface IConstWUScope : extends IInterface
{
    virtual IStringVal & getScope(IStringVal & str) const = 0;          // what scope is the statistic gathered over? e.g., workunit, wfid:n, graphn, graphn:m
    virtual StatisticScopeType getScopeType() const = 0;
};

interface IConstStatistic : extends IInterface
{
    virtual IStringVal & getDescription(IStringVal & str, bool createDefault) const = 0;    // Description of the statistic suitable for displaying to the user
    virtual IStringVal & getCreator(IStringVal & str) const = 0;        // what component gathered the statistic e.g., myroxie/eclserver_12/mythor:100
    virtual IStringVal & getFormattedValue(IStringVal & str) const = 0; // The formatted value for display
    virtual StatisticMeasure getMeasure() const = 0;
    virtual StatisticKind getKind() const = 0;
    virtual StatisticCreatorType getCreatorType() const = 0;
    virtual unsigned __int64 getValue() const = 0;
    virtual unsigned __int64 getCount() const = 0;
    virtual unsigned __int64 getMax() const = 0;
};

interface IConstWUStatistic : extends IConstStatistic
{
    virtual const char * queryScope() const = 0;          // what scope is the statistic gathered over? e.g., workunit, wfid:n, graphn, graphn:m
    virtual StatisticScopeType getScopeType() const = 0;
    virtual unsigned __int64 getTimestamp() const = 0;  // time the statistic was created
};

//---------------------------------------------------------------------------------------------------------------------

/*
 * An interface that is provided as a callback to a scope iterator to report properties when iterating scopes
 */
interface IWuScopeVisitor
{
    virtual void noteStatistic(StatisticKind kind, unsigned __int64 value, IConstWUStatistic & extra) = 0;
    virtual void noteAttribute(WuAttr attr, const char * value) = 0;
    virtual void noteHint(const char * kind, const char * value) = 0;
    virtual void noteException(IConstWUException & exception) = 0;
};

class WORKUNIT_API WuScopeVisitorBase : implements IWuScopeVisitor
{
    virtual void noteStatistic(StatisticKind kind, unsigned __int64 value, IConstWUStatistic & extra) override {}
    virtual void noteAttribute(WuAttr attr, const char * value) override {}
    virtual void noteHint(const char * kind, const char * value) override {}
    virtual void noteException(IConstWUException & exception) override {}
};

/*
 * Interface for an iterator that walks through the different logical elements (scopes) within a workunit
 */
enum WuPropertyTypes : unsigned
{
    PTnone                  = 0x00,
    PTstatistics            = 0x01,
    PTattributes            = 0x02,
    PThints                 = 0x04,
    PTscope                 = 0x08, // Just the existence of the scope is interesting
    PTnotes                 = 0x10,
    PTall                   = 0xFF,
    PTunknown               = 0x80000000,
};
BITMASK_ENUM(WuPropertyTypes);

enum WuScopeSourceFlags : unsigned
{
    SSFsearchDefault        = 0x0000,
    SSFsearchGlobalStats    = 0x0001,
    SSFsearchGraphStats     = 0x0002,
    SSFsearchGraph          = 0x0004,
    SSFsearchExceptions     = 0x0008,
    SSFsearchWorkflow       = 0x0010,
    SSFsearchAll            = 0x7fffffff,
    SSFunknown              = 0x80000000,
};
BITMASK_ENUM(WuScopeSourceFlags);

class WORKUNIT_API AttributeValueFilter
{
public:
    AttributeValueFilter(WuAttr _attr, const char * _value) : attr(_attr), value(_value)
    {
    }

    bool matches(const char * curValue) const
    {
        return !value || strsame(curValue, value);
    }

    WuAttr queryKind() const { return attr; }
    StringBuffer & describe(StringBuffer & out) const;

protected:
    WuAttr attr;
    StringAttr value;
};


/* WuScopeFilter syntax:
 * initial match:   scope[<scope-id>] | stype[<scope-type>] | id[<scope-id>] | depth[<value>| <min>,<max>]
 *                  source[global|stats|graph|exception]
 * stats filter:    where[<stat> | <stat>(=|!=|<|>|<=|>=)value | <stat>=<min>..<max>]
 *
 * returned scopes: matched[true|false] | nested[<depth>] | include[<scope-type>]
 * returned information:
 *                  props[stat|attr|hint|scope]
 *                  stat[<stat-name>] | attr[<attr-name>] | hint[<hint-name>] | measure[<measure-name>]
 */
class WORKUNIT_API WuScopeFilter
{
    friend class CompoundStatisticsScopeIterator;
public:
    WuScopeFilter() = default;
    WuScopeFilter(const char * filter);

    WuScopeFilter & addFilter(const char * filter);
    WuScopeFilter & addScope(const char * scope);
    WuScopeFilter & addScopeType(StatisticScopeType scopeType);
    WuScopeFilter & addScopeType(const char * scopeType);
    WuScopeFilter & addId(const char * id);
    WuScopeFilter & setDepth(unsigned low, unsigned high);
    WuScopeFilter & addSource(const char * source);
    WuScopeFilter & addSource(WuScopeSourceFlags source);
    WuScopeFilter & setSources(WuScopeSourceFlags sources);

    WuScopeFilter & setIncludeMatch(bool value);
    WuScopeFilter & setIncludeNesting(unsigned depth);
    WuScopeFilter & setIncludeScopeType(const char * scopeType);
    WuScopeFilter & setMeasure(const char * measure);

    WuScopeFilter & addOutput(const char * prop);              // Which statistics/properties/hints are required.
    WuScopeFilter & addOutputProperties(WuPropertyTypes prop); // stat/attr/hint/scope etc.
    WuScopeFilter & addOutputStatistic(StatisticKind stat);
    WuScopeFilter & addOutputStatistic(const char * prop);
    WuScopeFilter & addOutputAttribute(WuAttr attr);
    WuScopeFilter & addOutputAttribute(const char * prop);
    WuScopeFilter & addOutputHint(const char * prop);

    WuScopeFilter & addRequiredStat(StatisticKind statKind);
    WuScopeFilter & addRequiredStat(StatisticKind statKind, stat_type lowValue, stat_type highValue);
    WuScopeFilter & addRequiredAttr(WuAttr attr, const char * value = nullptr);

    void finishedFilter(); // Call once filter has been completely set up
    StringBuffer & describe(StringBuffer & out) const; // describe the filter - each option is preceded by a comma

    bool includeStatistic(StatisticKind kind) const;
    bool includeAttribute(WuAttr attr) const;
    bool includeHint(const char * kind) const;
    bool includeScope(const char * scope) const;

    ScopeCompare compareMatchScopes(const char * scope) const;
    const ScopeFilter & queryIterFilter() const;
    bool isOptimized() const { return optimized; }
    bool onlyIncludeScopes() const { return (properties & ~PTscope) == 0; }
    WuScopeSourceFlags querySources() const { return sourceFlags; }
    unsigned queryMinVersion() const { return minVersion; }
    bool outputDefined() const { return properties != PTnone; }

protected:
    void addRequiredStat(const char * filter);
    void checkModifiable() { if (unlikely(optimized)) reportModifyTooLate(); }
    bool matchOnly(StatisticScopeType scopeType) const;
    void reportModifyTooLate();

protected:
//The following members control which scopes are matched by the iterator
    ScopeFilter scopeFilter;                            // Filter that must be matched by a scope
    std::vector<StatisticValueFilter> requiredStats;    // The attributes that must be present for a particular scope
    std::vector<AttributeValueFilter> requiredAttrs;
    WuScopeSourceFlags sourceFlags = SSFsearchDefault;  // Which sources within the workunit should be included.  Default is to calculate from the properties.

// Once a match has been found which scopes are returned?
    struct
    {
        bool matchedScope = true;
        unsigned nestedDepth = 0;
        UnsignedArray scopeTypes;
    } include;

// For all scopes that are returned, what information is required?
    WuPropertyTypes properties = PTnone;  // What kind of information is desired (can be used to optimize the scopes). Default is scopes (for selected sources)
    UnsignedArray desiredStats;
    UnsignedArray desiredAttrs;
    StringArray desiredHints;
    StatisticMeasure desiredMeasure = SMeasureAll;

    __uint64 minVersion = 0;
    bool preFilterScope = false;
    bool optimized = false;
    //NB: Optimize scopeFilter.hasSingleMatch() + bail out early
};

interface IConstWUScopeIterator : extends IScmIterator
{
    //Allow iteration of the tree without walking through all the nodes.
    virtual bool nextSibling() = 0;
    virtual bool nextParent() = 0;

    //These return values are invalid after a call to next() or another call to the same function
    virtual const char * queryScope() const = 0;
    virtual StatisticScopeType getScopeType() const = 0;

    //Provide information about all stats, attributes and hints
    //whichProperties can be used to further restrict the output as a subset of the scope filter.
    virtual void playProperties(IWuScopeVisitor & visitor, WuPropertyTypes whichProperties = PTall) = 0;

    //Return true if the stat is present, if found and update the value - queryStat() wrapper is generally easier to use.
    virtual bool getStat(StatisticKind kind, unsigned __int64 & value) const = 0;
    virtual const char * queryAttribute(WuAttr attr, StringBuffer & scratchpad) const = 0; // Multiple values can be processed via the playStatistics() function
    virtual const char * queryHint(const char * kind) const = 0;

    inline unsigned __int64 queryStat(StatisticKind kind, unsigned __int64 defaultValue = 0) const
    {
        unsigned __int64 value = defaultValue;
        getStat(kind, value);
        return value;
    }
};

//---------------------------------------------------------------------------------------------------------------------
//! IWorkUnit
//! Provides high level access to WorkUnit "header" data.

// Be sure to update summaryTypeName in workunit.cpp if adding anything here
enum class SummaryType
{
    First,
    ReadFile = First,
    ReadIndex,
    WriteFile,
    WriteIndex,
    PersistFile,
    SpillFile,
    JobTemp,
    Service,
    // Keep these at the end
    NumItems,
    None = NumItems
};

enum SummaryFlags : byte
{
    None = 0,
    IsOpt = 0x01,
    IsSigned = 0x02,
};
BITMASK_ENUM(SummaryFlags);

struct ncasecomp { 
    bool operator() (const std::string& lhs, const std::string& rhs) const {
        return stricmp(lhs.c_str(), rhs.c_str()) < 0;
    }
};

typedef std::map<std::string, SummaryFlags, ncasecomp> SummaryMap;

interface IWorkUnit;
interface IUserDescriptor;

interface IConstWorkUnitInfo : extends IInterface
{
    virtual const char *queryWuid() const = 0;
    virtual const char *queryUser() const = 0;
    virtual const char *queryJobName() const = 0;
    virtual const char *queryWuScope() const = 0;
    virtual const char *queryClusterName() const = 0;
    virtual WUState getState() const = 0;
    virtual const char *queryStateDesc() const = 0;
    virtual WUAction getAction() const = 0;
    virtual const char *queryActionDesc() const = 0;
    virtual WUPriorityClass getPriority() const = 0;
    virtual const char *queryPriorityDesc() const = 0;
    virtual int getPriorityLevel() const = 0;
    virtual bool isProtected() const = 0;
    virtual cost_type getExecuteCost() const = 0;
    virtual cost_type getFileAccessCost() const = 0;
    virtual cost_type getCompileCost() const = 0;
    virtual IJlibDateTime & getTimeScheduled(IJlibDateTime & val) const = 0;

    virtual unsigned getTotalThorTime() const = 0;
    virtual IConstWUAppValueIterator & getApplicationValues() const = 0;
};

interface IConstWorkUnit : extends IConstWorkUnitInfo
{
    virtual bool aborting() const = 0;
    virtual void forceReload() = 0;
    virtual WUAction getAction() const = 0;
    virtual IStringVal & getApplicationValue(const char * application, const char * propname, IStringVal & str) const = 0;
    virtual int getApplicationValueInt(const char * application, const char * propname, int defVal) const = 0;
    virtual bool hasWorkflow() const = 0;
    virtual unsigned queryEventScheduledCount() const = 0;
    virtual IPropertyTree * queryWorkflowTree() const = 0;
    virtual IConstWorkflowItemIterator * getWorkflowItems() const = 0;
    virtual IWorkflowItemArray * getWorkflowClone() const = 0;
    virtual IConstLocalFileUploadIterator * getLocalFileUploads() const = 0;
    virtual bool requiresLocalFileUpload() const = 0;
    virtual bool getIsQueryService() const = 0;
    virtual bool hasDebugValue(const char * propname) const = 0;
    virtual IStringVal & getDebugValue(const char * propname, IStringVal & str) const = 0;
    virtual int getDebugValueInt(const char * propname, int defVal) const = 0;
    virtual __int64 getDebugValueInt64(const char * propname, __int64 defVal) const = 0;
    virtual double getDebugValueReal(const char * propname,  double defVal) const = 0;
    virtual bool getDebugValueBool(const char * propname, bool defVal) const = 0;
    virtual IStringIterator & getDebugValues() const = 0;
    virtual IStringIterator & getDebugValues(const char * prop) const = 0;
    virtual unsigned getExceptionCount() const = 0;
    virtual IConstWUExceptionIterator & getExceptions() const = 0;
    virtual IConstWUResult * getGlobalByName(const char * name) const = 0;
    virtual IConstWUGraphMetaIterator & getGraphsMeta(WUGraphType type) const = 0;
    virtual IConstWUGraphIterator & getGraphs(WUGraphType type) const = 0;
    virtual IConstWUGraph * getGraph(const char * name) const = 0;
    virtual IConstWUGraphProgress * getGraphProgress(const char * name) const = 0;
    virtual IConstWUPlugin * getPluginByName(const char * name) const = 0;
    virtual IConstWUPluginIterator & getPlugins() const = 0;
    virtual IConstWULibraryIterator & getLibraries() const = 0;
    virtual IConstWUQuery * getQuery() const = 0;
    virtual bool getRescheduleFlag() const = 0;
    virtual IConstWUResult * getResultByName(const char * name) const = 0;
    virtual IConstWUResult * getResultBySequence(unsigned seq) const = 0;
    // Like getResultByName, but ignores "special" results or results from libraries
    virtual IConstWUResult * getQueryResultByName(const char * name) const = 0;
    virtual unsigned getResultLimit() const = 0;
    virtual IConstWUResultIterator & getResults() const = 0;
    virtual IStringVal & getScope(IStringVal & str) const = 0;
    virtual IStringVal & getWorkunitDistributedAccessToken(IStringVal & datoken) const = 0;
    virtual IStringVal & getStateEx(IStringVal & str) const = 0;
    virtual __int64 getAgentSession() const = 0;
    virtual __int64 getEngineSession() const = 0;
    virtual unsigned getAgentPID() const = 0;
    virtual IConstWUResult * getTemporaryByName(const char * name) const = 0;
    virtual IConstWUResultIterator & getTemporaries() const = 0;
    virtual bool getRunningGraph(IStringVal & graphName, WUGraphIDType & subId) const = 0;
    virtual IConstWUWebServicesInfo * getWebServicesInfo() const = 0;
    virtual bool getStatistic(stat_type & value, const char * scope, StatisticKind kind) const = 0;
    virtual IConstWUScopeIterator & getScopeIterator(const WuScopeFilter & filter) const = 0; // filter must currently stay alive while the iterator does.
    virtual IConstWUResult * getVariableByName(const char * name) const = 0;
    virtual IConstWUResultIterator & getVariables() const = 0;
    virtual bool isPausing() const = 0;
    virtual IWorkUnit & lock() = 0;
    virtual void requestAbort() = 0;
    virtual void subscribe(WUSubscribeOptions options) = 0;
    virtual unsigned queryFileUsage(const char * filename) const = 0;
    virtual IConstWUFileUsageIterator * getFieldUsage() const = 0;
    virtual bool getFieldUsageArray(StringArray & filenames, StringArray & columnnames, const char * clusterName) const = 0;
    virtual bool getSummary(SummaryType type, SummaryMap &result) const = 0;

    virtual unsigned getCodeVersion() const = 0;
    virtual unsigned getWuidVersion() const  = 0;
    virtual void getBuildVersion(IStringVal & buildVersion, IStringVal & eclVersion) const = 0;
    virtual IPropertyTreeIterator & getFileIterator() const = 0;
    virtual bool getCloneable() const = 0;
    virtual IUserDescriptor * queryUserDescriptor() const = 0;
    virtual IStringVal & getSnapshot(IStringVal & str) const = 0;
    virtual ErrorSeverity getWarningSeverity(unsigned code, ErrorSeverity defaultSeverity) const = 0;
    virtual IPropertyTreeIterator & getFilesReadIterator() const = 0;
    virtual void protect(bool protectMode) = 0;
    virtual IStringVal & getAllowedClusters(IStringVal & str) const = 0;
    virtual int getPriorityValue() const = 0;
    virtual void remoteCheckAccess(IUserDescriptor * user, bool writeaccess) const = 0;
    virtual bool getAllowAutoQueueSwitch() const = 0;
    virtual IConstWULibrary * getLibraryByName(const char * name) const = 0;
    virtual unsigned getGraphCount() const = 0;
    virtual unsigned getSourceFileCount() const = 0;
    virtual unsigned getResultCount() const = 0;
    virtual unsigned getVariableCount() const = 0;
    virtual unsigned getApplicationValueCount() const = 0;
    virtual unsigned getDebugAgentListenerPort() const = 0;
    virtual IStringVal & getDebugAgentListenerIP(IStringVal & ip) const = 0;
    virtual IStringVal & getXmlParams(IStringVal & params, bool hidePasswords) const = 0;
    virtual const IPropertyTree * getXmlParams() const = 0;
    virtual unsigned __int64 getHash() const = 0;
    virtual IStringIterator *getLogs(const char *type, const char *instance=NULL) const = 0;
    virtual IPropertyTreeIterator *getProcessTypes() const = 0;
    virtual IStringIterator *getProcesses(const char *type) const = 0;
    virtual IPropertyTreeIterator* getProcesses(const char *type, const char *instance) const = 0;

    // Note that these don't read/modify the workunit itself, but rather the associated progress info.
    // As such they can be called without locking the workunit, and are 'const' as far as the WU is concerned.

    virtual WUGraphState queryGraphState(const char *graphName) const = 0;
    virtual WUGraphState queryNodeState(const char *graphName, WUGraphIDType nodeId) const = 0;
    virtual void setGraphState(const char *graphName, unsigned wfid, WUGraphState state) const = 0;
    virtual void setNodeState(const char *graphName, WUGraphIDType nodeId, WUGraphState state) const = 0;
    virtual IWUGraphStats *updateStats(const char *graphName, StatisticCreatorType creatorType, const char * creator, unsigned _wfid, unsigned subgraph, bool merge) const = 0;
    virtual void clearGraphProgress() const = 0;
    virtual IStringVal & getAbortBy(IStringVal & str) const = 0;
    virtual unsigned __int64 getAbortTimeStamp() const = 0;
    virtual cost_type getExecuteCost() const = 0;
    virtual cost_type getFileAccessCost() const = 0;
    virtual cost_type getCompileCost() const = 0;
};


interface IDistributedFile;

interface IWorkUnit : extends IConstWorkUnit
{
    virtual void clearExceptions(const char *source=nullptr) = 0;
    virtual void commit() = 0;
    virtual IWUException * createException() = 0;
    virtual void addProcess(const char *type, const char *instance, unsigned pid, unsigned max, const char *pattern, bool singleLog, const char *log=nullptr) = 0;
    virtual bool setContainerizedProcessInfo(const char *type, const char *instance, const char *podName, const char *containerName, const char *graphName, const char *sequence) = 0;
    virtual void setAction(WUAction action) = 0;
    virtual void setApplicationValue(const char * application, const char * propname, const char * value, bool overwrite) = 0;
    virtual void setApplicationValueInt(const char * application, const char * propname, int value, bool overwrite) = 0;
    virtual void incEventScheduledCount() = 0;
    virtual void setIsQueryService(bool cached) = 0;
    virtual void setClusterName(const char * value) = 0;
    virtual void setDebugValue(const char * propname, const char * value, bool overwrite) = 0;
    virtual void setDebugValueInt(const char * propname, int value, bool overwrite) = 0;
    virtual void setJobName(const char * value) = 0;
    virtual void setPriority(WUPriorityClass cls) = 0;
    virtual void setPriorityLevel(int level) = 0;
    virtual void setRescheduleFlag(bool value) = 0;
    virtual void setResultLimit(unsigned value) = 0;
    virtual void setState(WUState state) = 0;
    virtual void setStateEx(const char * text) = 0;  // Indicates why blocked
    virtual void setAgentSession(__int64 sessionId) = 0;
    virtual void setEngineSession(__int64 sessionId) = 0;
    virtual void setStatistic(StatisticCreatorType creatorType, const char * creator, StatisticScopeType scopeType, const char * scope, StatisticKind kind, const char * optDescription, unsigned __int64 value, unsigned __int64 count, unsigned __int64 maxValue, StatsMergeAction mergeAction) = 0;
    virtual void setTracingValue(const char * propname, const char * value) = 0;
    virtual void setTracingValueInt(const char * propname, int value) = 0;
    virtual void setTracingValueInt64(const char * propname, __int64 value) = 0;
    virtual void setUser(const char * value) = 0;
    virtual void setWuScope(const char * value) = 0;
    virtual void setSnapshot(const char * value) = 0;
    virtual void setWarningSeverity(unsigned code, ErrorSeverity severity) = 0;
    virtual IWorkflowItemIterator * updateWorkflowItems() = 0;
    virtual void syncRuntimeWorkflow(IWorkflowItemArray * array) = 0;
    virtual IWorkflowItem * addWorkflowItem(unsigned wfid, WFType type, WFMode mode, unsigned success, unsigned failure, unsigned recovery, unsigned retriesAllowed, unsigned contingencyFor) = 0;
    virtual void resetWorkflow() = 0;
    virtual void schedule() = 0;
    virtual void deschedule() = 0;
    virtual unsigned addLocalFileUpload(LocalFileUploadType type, const char * source, const char * destination, const char * eventTag) = 0;
    virtual IWUResult * updateGlobalByName(const char * name) = 0;
    virtual void createGraph(const char * name, const char *label, WUGraphType type, IPropertyTree *xgmml, unsigned wfid) = 0;
    virtual IWUQuery * updateQuery() = 0;
    virtual IWUWebServicesInfo * updateWebServicesInfo(bool create) = 0;
    virtual IWUPlugin * updatePluginByName(const char * name) = 0;
    virtual IWULibrary * updateLibraryByName(const char * name) = 0;
    virtual IWUResult * updateResultByName(const char * name) = 0;
    virtual IWUResult * updateResultBySequence(unsigned seq) = 0;
    virtual IWUResult * updateTemporaryByName(const char * name) = 0;
    virtual IWUResult * updateVariableByName(const char * name) = 0;
    virtual void addFile(const char * fileName, StringArray * clusters, unsigned usageCount, WUFileKind fileKind, const char * graphOwner) = 0;
    virtual void releaseFile(const char * fileName) = 0;
    virtual void setCodeVersion(unsigned version, const char * buildVersion, const char * eclVersion) = 0;
    virtual void deleteTempFiles(const char * graph, bool deleteOwned, bool deleteJobOwned) = 0;
    virtual void deleteTemporaries() = 0;
    virtual void setCloneable(bool value) = 0;
    virtual void setIsClone(bool value) = 0;
    virtual void setTimeScheduled(const IJlibDateTime & val) = 0;
    virtual void noteFileRead(IDistributedFile * file) = 0;
    virtual void noteFieldUsage(IPropertyTree * file) = 0;
    virtual void resetBeforeGeneration() = 0;
    virtual bool switchThorQueue(const char * newcluster, IQueueSwitcher * qs, const char *item) = 0;
    virtual void setAllowedClusters(const char * value) = 0;
    virtual void setAllowAutoQueueSwitch(bool val) = 0;
    virtual void setLibraryInformation(const char * name, unsigned interfaceHash, unsigned definitionHash) = 0;
    virtual void setDebugAgentListenerPort(unsigned port) = 0;
    virtual void setDebugAgentListenerIP(const char * ip) = 0;
    virtual void setXmlParams(const char *xml) = 0;
    virtual void setXmlParams(IPropertyTree *tree) = 0;
    virtual void setHash(unsigned __int64 hash) = 0;

    virtual void setResultInt(const char * name, unsigned sequence, __int64 val) = 0;
    virtual void setResultUInt(const char * name, unsigned sequence, unsigned __int64 val) = 0;
    virtual void setResultReal(const char *name, unsigned sequence, double val) = 0;
    virtual void setResultVarString(const char * stepname, unsigned sequence, const char *val) = 0;
    virtual void setResultVarUnicode(const char * stepname, unsigned sequence, UChar const *val) = 0;
    virtual void setResultString(const char * stepname, unsigned sequence, int len, const char *val) = 0;
    virtual void setResultData(const char * stepname, unsigned sequence, int len, const void *val) = 0;
    virtual void setResultRaw(const char * name, unsigned sequence, int len, const void *val) = 0;
    virtual void setResultSet(const char * name, unsigned sequence, bool isAll, size32_t len, const void *val, ISetToXmlTransformer *) = 0;
    virtual void setResultUnicode(const char * name, unsigned sequence, int len, UChar const * val) = 0;
    virtual void setResultBool(const char *name, unsigned sequence, bool val) = 0;
    virtual void setResultDecimal(const char *name, unsigned sequence, int len, int precision, bool isSigned, const void *val) = 0;
    virtual void setResultDataset(const char * name, unsigned sequence, size32_t len, const void *val, unsigned numRows, bool extend) = 0;
    virtual void import(IPropertyTree *wuTree, IPropertyTree *graphProgressTree = nullptr) = 0;
    virtual void setSummary(SummaryType type, const SummaryMap &map) = 0;
    virtual IConstWorkUnit * unlock() = 0;
};


interface IConstWorkUnitIterator : extends IScmIterator
{
    virtual IConstWorkUnitInfo & query() = 0;
};

//! IWUTimers

interface IWUTimers : extends IInterface
{
    virtual void setTrigger(const IJlibDateTime & dt) = 0;
    virtual IJlibDateTime & getTrigger(IJlibDateTime & dt) const = 0;
    virtual void setExpiration(const IJlibDateTime & dt) = 0;
    virtual IJlibDateTime & getExpiration(IJlibDateTime & dt) const = 0;
    virtual void setSubmission(const IJlibDateTime & dt) = 0;
    virtual IJlibDateTime & getSubmission(IJlibDateTime & dt) const = 0;
};



//! IWUFactory
//! Used to instantiate WorkUnit components.

class MemoryBuffer;

interface ILocalWorkUnit : extends IWorkUnit
{
    virtual void serialize(MemoryBuffer & tgt) = 0;
    virtual void deserialize(MemoryBuffer & src) = 0;
};


enum WUSortField
{
    WUSFuser = 1,
    WUSFcluster = 2,
    WUSFjob = 3,
    WUSFstate = 4,
    WUSFpriority = 5,
    WUSFwuid = 6,
    WUSFwuidhigh = 7,
    WUSFfileread = 8,
    // WUSFroxiecluster = 9, obsolete
    WUSFprotected = 10,
    WUSFtotalthortime = 11,
    WUSFwildwuid = 12,
    WUSFecl = 13,
    // WUSFcustom = 14, obsolete
    WUSFappvalue = 15,
    WUSFfilewritten = 16,
    WUSFcostexecute = 17,
    WUSFcostcompile = 18,
    WUSFcostfileaccess = 19,
    WUSFterm = 0,
    WUSFreverse = 256,
    WUSFnocase = 512,
    WUSFnumeric = 1024,
    WUSFwild = 2048
};

extern WORKUNIT_API const char *queryFilterXPath(WUSortField field);

enum WUQueryFilterBoolean
{
    WUQFSNo = 0,
    WUQFSYes = 1,
    WUQFSAll = 2
};

enum WUQueryFilterSuspended
{
    WUQFAllQueries = 0,//all queries including Suspended and not suspended
    WUQFSUSPDNo = 1,
    WUQFSUSPDYes = 2,
    WUQFSUSPDByUser = 3,
    WUQFSUSPDByFirstNode = 4,
    WUQFSUSPDByAnyNode = 5
};

enum WUQuerySortField
{
    WUQSFId = 1,
    WUQSFname = 2,
    WUQSFwuid = 3,
    WUQSFdll = 4,
    WUQSFmemoryLimit = 5,
    WUQSFmemoryLimitHi = 6,
    WUQSFtimeLimit = 7,
    WUQSFtimeLimitHi = 8,
    WUQSFwarnTimeLimit = 9,
    WUQSFwarnTimeLimitHi = 10,
    WUQSFpriority = 11,
    WUQSFpriorityHi = 12,
    WUQSFQuerySet = 13,
    WUQSFActivited = 14,
    WUQSFSuspendedByUser = 15,
    WUQSFLibrary = 16,
    WUQSFPublishedBy = 17,
    WUQSFSuspendedFilter = 18,
    WUQSFterm = 0,
    WUQSFreverse = 256,
    WUQSFnocase = 512,
    WUQSFnumeric = 1024,
    WUQSFwild = 2048
};

typedef IIteratorOf<IPropertyTree> IConstQuerySetQueryIterator;


interface IWorkUnitFactory : extends IPluggableFactory
{
    virtual IWorkUnit *createWorkUnit(const char *app, const char *scope, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual void importWorkUnit(const char *zapReportFileName, const char *zapReportPassword,
        const char *importDir, const char *app, const char *user, ISecManager *secMgr, ISecUser *secUser) = 0;
    virtual bool deleteWorkUnit(const char *wuid, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual bool deleteWorkUnitEx(const char *wuid, bool throwException, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual IConstWorkUnit * openWorkUnit(const char *wuid, ISecManager *secmgr = NULL, ISecUser *secuser = NULL, bool expected = true) = 0;
    virtual IConstWorkUnitIterator * getWorkUnitsByOwner(const char * owner, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual IWorkUnit * updateWorkUnit(const char * wuid, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual bool restoreWorkUnit(const char *base, const char *wuid, bool restoreAssociatedFiles) = 0;
    virtual int setTracingLevel(int newlevel) = 0;
    virtual IWorkUnit * createNamedWorkUnit(const char * wuid, const char * app, const char * scope, ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual IWorkUnit * getGlobalWorkUnit(ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual IConstWorkUnitIterator * getWorkUnitsSorted(WUSortField sortorder, WUSortField * filters, const void * filterbuf,
                                                        unsigned startoffset, unsigned maxnum, __int64 * cachehint, unsigned *total,
                                                        ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual unsigned numWorkUnits() = 0;
    virtual IConstWorkUnitIterator *getScheduledWorkUnits(ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual void descheduleAllWorkUnits(ISecManager *secmgr = NULL, ISecUser *secuser = NULL) = 0;
    virtual IConstQuerySetQueryIterator * getQuerySetQueriesSorted(WUQuerySortField *sortorder, WUQuerySortField *filters, const void *filterbuf,
        unsigned startoffset, unsigned maxnum, __int64 *cachehint, unsigned *total, const MapStringTo<bool> *subset, const MapStringTo<bool> *suspendedByCluster) = 0;
    virtual bool isAborting(const char *wuid) const = 0;
    virtual void clearAborting(const char *wuid) = 0;
    virtual WUState waitForWorkUnit(const char * wuid, unsigned timeout, bool compiled, std::list<WUState> expectedStates) = 0;
    virtual WUAction waitForWorkUnitAction(const char * wuid, WUAction original) = 0;

    virtual unsigned validateRepository(bool fixErrors) = 0;
    virtual void deleteRepository(bool recreate) = 0;
    virtual void createRepository() = 0;  // If not already there...
    virtual const char *queryStoreType() const = 0; // Returns "Dali" or "Cassandra"

    virtual StringArray &getUniqueValues(WUSortField field, const char *prefix, StringArray &result) const = 0;
    virtual IWorkUnitWatcher *getWatcher(IWorkUnitSubscriber *subscriber, WUSubscribeOptions options, const char *wuid) const = 0;
};

interface IWorkflowScheduleConnection : extends IInterface
{
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual void setActive() = 0;
    virtual void resetActive() = 0;
    virtual bool queryActive() = 0;
    virtual bool pull(IWorkflowItemArray * workflow) = 0;
    virtual void push(const char * name, const char * text) = 0;
    virtual void remove() = 0;
};


interface IExtendedWUInterface
{
    virtual unsigned calculateHash(unsigned prevHash) = 0;
    virtual void copyWorkUnit(IConstWorkUnit *cached, bool copyStats, bool all) = 0;
    virtual bool archiveWorkUnit(const char *base,bool del,bool ignoredllerrors,bool deleteOwned,bool exportAssociatedFiles) = 0;
    virtual IPropertyTree *getUnpackedTree(bool includeProgress) const = 0;
    virtual IPropertyTree *queryPTree() const = 0;

};

//Do not mark this as WORKUNIT_API - all functions are inline, and it causes windows link errors
struct WorkunitUpdate : public Owned<IWorkUnit>
{
public:
    inline WorkunitUpdate(IWorkUnit *wu) : Owned<IWorkUnit>(wu) { }
    inline ~WorkunitUpdate() { if (get()) get()->commit(); }
};

class WORKUNIT_API WuStatisticTarget : implements IStatisticTarget
{
public:
    WuStatisticTarget(IWorkUnit * _wu, const char * _defaultWho) : wu(_wu), defaultWho(_defaultWho) {}

    virtual void addStatistic(StatisticScopeType scopeType, const char * scope, StatisticKind kind, char * description, unsigned __int64 value, unsigned __int64 count, unsigned __int64 maxValue, StatsMergeAction mergeAction)
    {
        wu->setStatistic(queryStatisticsComponentType(), queryStatisticsComponentName(), scopeType, scope, kind, description, value, count, maxValue, mergeAction);
    }

protected:
    Linked<IWorkUnit> wu;
    const char * defaultWho;
};

interface ILoadedDllEntry;

typedef IWorkUnitFactory * (* WorkUnitFactoryFactory)(const IPropertyTree *);

extern WORKUNIT_API IStatisticGatherer * createGlobalStatisticGatherer(IWorkUnit * wu);
extern WORKUNIT_API WUGraphType getGraphTypeFromString(const char* type);

extern WORKUNIT_API bool getWorkUnitCreateTime(const char *wuid,CDateTime &time); // based on WUID
extern WORKUNIT_API void clientShutdownWorkUnit();
extern WORKUNIT_API IExtendedWUInterface * queryExtendedWU(IConstWorkUnit * wu);
extern WORKUNIT_API const IExtendedWUInterface * queryExtendedWU(const IConstWorkUnit * wu);
extern WORKUNIT_API StringBuffer &formatGraphTimerLabel(StringBuffer &str, const char *graphName, unsigned subGraphNum=0, unsigned __int64 subId=0);
extern WORKUNIT_API StringBuffer &formatGraphTimerScope(StringBuffer &str, unsigned wfid, const char *graphName, unsigned subGraphNum, unsigned __int64 subId);
extern WORKUNIT_API bool parseGraphTimerLabel(const char *label, StringAttr &graphName, unsigned & graphNum, unsigned &subGraphNum, unsigned &subId);
extern WORKUNIT_API bool parseGraphScope(const char *scope, StringAttr &graphName, unsigned & graphNum, unsigned &subGraphId);
extern WORKUNIT_API void addExceptionToWorkunit(IWorkUnit * wu, ErrorSeverity severity, const char * source, unsigned code, const char * text, const char * filename, unsigned lineno, unsigned column, unsigned activity);
extern WORKUNIT_API void setWorkUnitFactory(IWorkUnitFactory *_factory);
extern WORKUNIT_API IWorkUnitFactory * getWorkUnitFactory();
extern WORKUNIT_API IWorkUnitFactory * getWorkUnitFactory(ISecManager *secmgr, ISecUser *secuser);
extern WORKUNIT_API ILocalWorkUnit * createLocalWorkUnit();
extern WORKUNIT_API ILocalWorkUnit * createLocalWorkUnitFromPTree(IPropertyTree *ptree);  // takes ownership of tree
extern WORKUNIT_API ILocalWorkUnit * createLocalWorkUnitFromXml(const char *XML);
extern WORKUNIT_API ILocalWorkUnit * createLocalWorkUnit(ILoadedDllEntry * dll);
extern WORKUNIT_API ILocalWorkUnit * createLocalWorkUnitFromFile(const char * filename);
extern WORKUNIT_API IConstWorkUnitInfo *createConstWorkUnitInfo(IPropertyTree &p);
extern WORKUNIT_API IWorkUnitFactory * createUnexpectedWorkUnitFactory();
extern WORKUNIT_API StringBuffer &exportWorkUnitToXML(const IConstWorkUnit *wu, StringBuffer &str, bool unpack, bool includeProgress, bool hidePasswords);
extern WORKUNIT_API void exportWorkUnitToBinary(const IConstWorkUnit *wu, MemoryBuffer & serialized);
extern WORKUNIT_API void exportWorkUnitToXMLFile(const IConstWorkUnit *wu, const char * filename, unsigned extraXmlFlags, bool unpack, bool includeProgress, bool hidePasswords, bool splitStats);
extern WORKUNIT_API void submitWorkUnit(const char *wuid, const char *username, const char *password);
extern WORKUNIT_API void abortWorkUnit(const char *wuid);
extern WORKUNIT_API void submitWorkUnit(const char *wuid, ISecManager *secmgr, ISecUser *secuser);
extern WORKUNIT_API void abortWorkUnit(const char *wuid, ISecManager *secmgr, ISecUser *secuser);
extern WORKUNIT_API void secSubmitWorkUnit(const char *wuid, ISecManager &secmgr, ISecUser &secuser);
extern WORKUNIT_API void secAbortWorkUnit(const char *wuid, ISecManager &secmgr, ISecUser &secuser);
extern WORKUNIT_API IWUResult * updateWorkUnitResult(IWorkUnit * w, const char *name, unsigned sequence);
extern WORKUNIT_API IConstWUResult * getWorkUnitResult(IConstWorkUnit * w, const char *name, unsigned sequence);
extern WORKUNIT_API void updateSuppliedXmlParams(IWorkUnit * w);

//workunit distributed access token support
enum wuTokenStates { wuTokenValid=0, wuTokenInvalid, wuTokenWorkunitInactive };
extern WORKUNIT_API wuTokenStates verifyWorkunitDAToken(const char * ctxUser, const char * daToken);
extern WORKUNIT_API bool extractFromWorkunitDAToken(const char * token, StringBuffer * wuid, StringBuffer * user, StringBuffer * privKey);
inline bool isWorkunitDAToken(const char * distributedAccessToken)
{
	//Does given token appear to be in the right format. KeyFile and signature are optional
	//    HPCC[u=user,w=wuid]keyFile;signature
    const char * finger = distributedAccessToken;
    if (finger && 0 == strncmp(finger,"HPCC[u=",7))
        if ((finger = strstr(finger+7, ",w=")))
            if ((finger = strchr(finger+3, ']')))
                if ((finger = strchr(finger+1, ';')))
                    return true;
    return false;
}

//returns a state code.  WUStateUnknown == timeout
extern WORKUNIT_API WUState waitForWorkUnitToComplete(const char * wuid, int timeout = -1, std::list<WUState> expectedStates = {});
extern WORKUNIT_API bool waitForWorkUnitToCompile(const char * wuid, int timeout = -1);
extern WORKUNIT_API WUState secWaitForWorkUnitToComplete(const char * wuid, ISecManager *secmgr, ISecUser *secuser, int timeout = -1, std::list<WUState> expectedStates = {});
extern WORKUNIT_API bool secWaitForWorkUnitToCompile(const char *wuid, ISecManager *secmgr, ISecUser *secuser, int timeout = -1);
extern WORKUNIT_API bool secDebugWorkunit(const char *wuid, ISecManager *secmgr, ISecUser *secuser, const char *command, StringBuffer &response);
extern WORKUNIT_API WUState getWorkUnitState(const char* state);
extern WORKUNIT_API IWorkflowScheduleConnection * getWorkflowScheduleConnection(char const * wuid);
extern WORKUNIT_API const char *skipLeadingXml(const char *text);
extern WORKUNIT_API bool isArchiveQuery(const char * text);
extern WORKUNIT_API bool isQueryManifest(const char * text);
extern WORKUNIT_API IPropertyTree * resolveDefinitionInArchive(IPropertyTree * archive, const char * path);

inline bool isLibrary(IConstWorkUnit * wu) { return wu->getApplicationValueInt("LibraryModule", "interfaceHash", 0) != 0; }
extern WORKUNIT_API bool looksLikeAWuid(const char * wuid, const char firstChar);

extern WORKUNIT_API IConstWorkUnitIterator *createSecureConstWUIterator(IPropertyTreeIterator *iter, ISecManager *secmgr, ISecUser *secuser);
extern WORKUNIT_API IConstWorkUnitIterator *createSecureConstWUIterator(IConstWorkUnitIterator *iter, ISecManager *secmgr, ISecUser *secuser);

enum WUQueryActivationOptions
{
    DO_NOT_ACTIVATE = 0,
    MAKE_ACTIVATE= 1,
    ACTIVATE_SUSPEND_PREVIOUS = 2,
    ACTIVATE_DELETE_PREVIOUS = 3,
    DO_NOT_ACTIVATE_LOAD_DATA_ONLY = 4,
    MAKE_ACTIVATE_LOAD_DATA_ONLY = 5
};

extern WORKUNIT_API int calcPriorityValue(const IPropertyTree * p);  // Calls to this should really go through the workunit interface.

extern WORKUNIT_API IPropertyTree * addNamedQuery(IPropertyTree * queryRegistry, const char * name, const char * wuid, const char * dll, bool library, const char *userid, const char *snapshot);       // result not linked
extern WORKUNIT_API void removeNamedQuery(IPropertyTree * queryRegistry, const char * id);
extern WORKUNIT_API void removeWuidFromNamedQueries(IPropertyTree * queryRegistry, const char * wuid);
extern WORKUNIT_API void removeDllFromNamedQueries(IPropertyTree * queryRegistry, const char * dll);
extern WORKUNIT_API void removeAliasesFromNamedQuery(IPropertyTree * queryRegistry, const char * id);
extern WORKUNIT_API void setQueryAlias(IPropertyTree * queryRegistry, const char * name, const char * value);

extern WORKUNIT_API IPropertyTree * getQueryById(IPropertyTree * queryRegistry, const char *queryid);
extern WORKUNIT_API IPropertyTree * getQueryById(const char *queryset, const char *queryid, bool readonly);
extern WORKUNIT_API IPropertyTree * resolveQueryAlias(IPropertyTree * queryRegistry, const char * alias);
extern WORKUNIT_API IPropertyTree * resolveQueryAlias(const char *queryset, const char *alias, bool readonly);
extern WORKUNIT_API IPropertyTree * getQueryRegistry(const char * wsEclId, bool readonly);
extern WORKUNIT_API IPropertyTree * getQueryRegistryRoot();

extern WORKUNIT_API void checkAddLibrariesToQueryEntry(IPropertyTree *queryTree, IConstWULibraryIterator *libraries);
extern WORKUNIT_API void checkAddLibrariesToQueryEntry(IPropertyTree *queryTree, IConstWorkUnit *cw);

extern WORKUNIT_API void setQueryCommentForNamedQuery(IPropertyTree * queryRegistry, const char *id, const char *queryComment);

extern WORKUNIT_API void setQuerySuspendedState(IPropertyTree * queryRegistry, const char * name, bool suspend, const char *userid);

extern WORKUNIT_API IPropertyTree * addNamedPackageSet(IPropertyTree * packageRegistry, const char * name, IPropertyTree *packageInfo, bool overWrite);     // result not linked
extern WORKUNIT_API void removeNamedPackage(IPropertyTree * packageRegistry, const char * id);
extern WORKUNIT_API IPropertyTree * getPackageSetRegistry(const char * wsEclId, bool readonly);

extern WORKUNIT_API void addQueryToQuerySet(IWorkUnit *workunit, IPropertyTree *queryRegistry, const char *queryName, WUQueryActivationOptions activateOption, StringBuffer &newQueryId, const char *userid);
extern WORKUNIT_API void addQueryToQuerySet(IWorkUnit *workunit, const char *querySetName, const char *queryName, WUQueryActivationOptions activateOption, StringBuffer &newQueryId, const char *userid);
extern WORKUNIT_API void activateQuery(IPropertyTree *queryRegistry, WUQueryActivationOptions activateOption, const char *queryName, const char *queryId, const char *userid);
extern WORKUNIT_API bool removeQuerySetAlias(const char *querySetName, const char *alias);
extern WORKUNIT_API void addQuerySetAlias(const char *querySetName, const char *alias, const char *id);
extern WORKUNIT_API void setSuspendQuerySetQuery(const char *querySetName, const char *id, bool suspend, const char *userid);
extern WORKUNIT_API void deleteQuerySetQuery(const char *querySetName, const char *id);
extern WORKUNIT_API const char *queryIdFromQuerySetWuid(IPropertyTree *queryRegistry, const char *wuid, const char *queryName, IStringVal &id);
extern WORKUNIT_API const char *queryIdFromQuerySetWuid(const char *querySetName, const char *wuid, const char *queryName, IStringVal &id);
extern WORKUNIT_API void removeQuerySetAliasesFromNamedQuery(const char *querySetName, const char * id);
extern WORKUNIT_API void setQueryCommentForNamedQuery(const char *querySetName, const char *id, const char *comment);
extern WORKUNIT_API void gatherLibraryNames(StringArray &names, StringArray &unresolved, IWorkUnitFactory &workunitFactory, IConstWorkUnit &cw, IPropertyTree *queryset);

//If we add any more parameters we should consider returning an object that can be updated
extern WORKUNIT_API void associateLocalFile(IWUQuery * query, WUFileType type, const char * name, const char * description, unsigned crc, unsigned minActivity=0, unsigned maxActivity=0);

extern WORKUNIT_API void addWorkunitSummary(IWorkUnit * wu, SummaryType summaryType, SummaryMap &map);

interface ITimeReporter;
extern WORKUNIT_API void updateWorkunitStat(IWorkUnit * wu, StatisticScopeType scopeType, const char * scope, StatisticKind kind, const char * description, unsigned __int64 value, unsigned wfid=0);
extern WORKUNIT_API void updateWorkunitTimings(IWorkUnit * wu, ITimeReporter *timer);
extern WORKUNIT_API void updateWorkunitTimings(IWorkUnit * wu, StatisticScopeType scopeType, StatisticKind kind, ITimeReporter *timer);
extern WORKUNIT_API void aggregateStatistic(StatsAggregation & result, IConstWorkUnit * wu, const WuScopeFilter & filter, StatisticKind search);
extern WORKUNIT_API cost_type aggregateCost(const IConstWorkUnit * wu, const char *scope=nullptr, bool excludehThor=false);
extern WORKUNIT_API const char *getTargetClusterComponentName(const char *clustname, const char *processType, StringBuffer &name);
extern WORKUNIT_API void descheduleWorkunit(char const * wuid);
#if 0
void WORKUNIT_API testWorkflow();
#endif

extern WORKUNIT_API const char * getWorkunitStateStr(WUState state);
extern WORKUNIT_API const char * getWorkunitActionStr(WUAction action);
extern WORKUNIT_API WUAction getWorkunitAction(const char * actionStr);

extern WORKUNIT_API void addTimeStamp(IWorkUnit * wu, StatisticScopeType scopeType, const char * scope, StatisticKind kind, unsigned wfid=0);
extern WORKUNIT_API void addTimeStamp(IWorkUnit * wu, unsigned wfid, const char * scope, StatisticKind kind);
extern WORKUNIT_API double getMachineCostRate();
extern WORKUNIT_API double getThorManagerRate();
extern WORKUNIT_API double getThorWorkerRate();
extern WORKUNIT_API double getThorRate(unsigned numberOfWorkers);
extern WORKUNIT_API double calculateThorCost(unsigned __int64 ms, unsigned numberOfWorkers);
inline double calculateThorCostPerHour(unsigned numberOfWorkers)
{
    return calculateThorCost(1000*60*60, numberOfWorkers);
}

extern WORKUNIT_API IPropertyTree * getWUGraphProgress(const char * wuid, bool readonly);

class WORKUNIT_API WorkUnitErrorReceiver : implements IErrorReceiver, public CInterface
{
public:
    WorkUnitErrorReceiver(IWorkUnit * _wu, const char * _component, bool _removeTimeStamp) { wu.set(_wu); component.set(_component); removeTimeStamp = _removeTimeStamp; }
    IMPLEMENT_IINTERFACE;

    virtual IError * mapError(IError * error);
    virtual void exportMappings(IWorkUnit * wu) const { }
    virtual void report(IError*);
    virtual size32_t errCount();
    virtual size32_t warnCount();

private:
    Owned<IWorkUnit> wu;
    StringAttr component;
    bool removeTimeStamp;
};

extern WORKUNIT_API IProperties * extractTraceDebugOptions(IConstWorkUnit * source);
extern WORKUNIT_API IProperties * deserializeTraceDebugOptions(const IPropertyTree * debugOptions);
extern WORKUNIT_API void recordTraceDebugOptions(IWorkUnit * target, const IProperties * source);

extern WORKUNIT_API void addWorkunitException(IWorkUnit * wu, IError * error, bool removeTimeStamp);

inline bool isGlobalScope(const char * scope) { return scope && (streq(scope, GLOBAL_SCOPE) || streq(scope, LEGACY_GLOBAL_SCOPE)); }

extern WORKUNIT_API bool isValidPriorityValue(const char * priority);
extern WORKUNIT_API bool isValidMemoryValue(const char * memoryUnit);

constexpr bool defaultThorMultiJobLinger = true;
constexpr unsigned defaultThorLingerPeriod = 60;

constexpr bool defaultAnalyzeWhenComplete = isContainerized() ? false : true;
// Non-containerized: the analyzer is executed in the eclagent (legacy behavior)
// Containerized: the analyzer is executed in Thor by default because the eclagent cannot
//                calculate the cost of issues as it doesn't have access to thor cost values
// (Note: it is not presently possible to analyze with defaultAnalyzeWhenComplete option and in thor)
constexpr bool defaultAnalyzeInEclAgent = isContainerized() ? false : true;

extern WORKUNIT_API void executeThorGraph(const char * graphName, IConstWorkUnit &workunit, const IPropertyTree &config);

extern WORKUNIT_API TraceFlags wuLoadTraceFlags(IConstWorkUnit * wu, const std::initializer_list<TraceOption> & optNames, TraceFlags dft);
// useful when workunit info is serialize to a property tree (e.g. in Thor workers)
extern WORKUNIT_API TraceFlags wuLoadTraceFlags(const IPropertyTree * wuInfo, const std::initializer_list<TraceOption> & optNames, TraceFlags dft);

extern WORKUNIT_API bool executeGraphOnLingeringThor(IConstWorkUnit &workunit, unsigned wfid, const char *graphName);
extern WORKUNIT_API bool workunitGraphCacheEnabled;


class WORKUNIT_API StatisticsAggregator : public CInterface
{
public:
    StatisticsAggregator(const StatisticsMapping & _mapping) : mapping(_mapping) {}
    void loadExistingAggregates(const IConstWorkUnit &workunit);
    void recordStats(IStatisticCollection * sourceStats, unsigned wfid, const char * graphName, unsigned graphId);
    void updateAggregates(IWorkUnit *wu);
private:
    Owned<IStatisticCollection> statsCollection;
    const StatisticsMapping & mapping;
};

class WORKUNIT_API WuidPattern
{
private:
    StringBuffer pattern;
public:
    WuidPattern(const char* _pattern);
    inline bool isEmpty() const { return pattern.isEmpty(); }
    inline const char* str() const { return pattern.str(); }
    inline operator const char* () const { return pattern.str(); }
    inline operator const StringBuffer& () const { return pattern; }
    inline operator StringBuffer& () { return pattern; }
};

inline cost_type getGuillotineCost(IConstWorkUnit *wu)
{
    cost_type wuMaxCost = money2cost_type(wu->getDebugValueReal("maxCost", 0));
    cost_type guillotineCost = wuMaxCost ? wuMaxCost : money2cost_type(getConfigReal("cost/@limit"));
    cost_type hardCostLimit = money2cost_type(getConfigReal("cost/@hardlimit"));
    if (hardCostLimit && ((guillotineCost == 0) || (guillotineCost > hardCostLimit)))
        guillotineCost = hardCostLimit;
    return guillotineCost;
}

#endif
