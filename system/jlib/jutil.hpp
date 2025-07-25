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


#ifndef JUTIL_HPP
#define JUTIL_HPP

#include "jlib.hpp"
#include "jstring.hpp"
#include "jarray.hpp"
#include "jbuff.hpp"

#include <algorithm> 
#include <iterator>
#include <functional>
#include <vector>
#include <string>

#if defined (__APPLE__)
#include <mach/mach_time.h>
extern mach_timebase_info_data_t timebase_info;   // Calibration for nanosecond timer
#endif

//#define NAMEDCOUNTS

bool jlib_decl getEnvVar(const char * varName, StringBuffer & varValue);

interface IPropertyTree;
interface IProperties;

extern jlib_decl void MilliSleep(unsigned milli);
extern jlib_decl void NanoSleep(__uint64 ns);

long jlib_decl atolong_l(const char * s,int l);
int  jlib_decl atoi_l(const char * s,int l);
__int64 jlib_decl atoi64_l(const char * s,int l);
inline __int64 atoi64(const char* s) { return atoi64_l(s, (int)strlen(s)); }
#ifndef _WIN32
extern jlib_decl char * itoa(int n, char *str, int b);
extern jlib_decl char * ltoa(long n, char *str, int b);
extern jlib_decl char * ultoa(unsigned long n, char *str, int b);
#define Sleep(milli) MilliSleep(milli)
#endif
bool jlib_decl j_isnan(double x);
bool jlib_decl j_isinf(double x);

void jlib_decl packNumber(char * target, const char * source, unsigned slen);
void jlib_decl unpackNumber(char * target, const char * source, unsigned tlen);

int jlib_decl numtostr(char *dst, char _value);
int jlib_decl numtostr(char *dst, short _value);
int jlib_decl numtostr(char *dst, int _value);
int jlib_decl numtostr(char *dst, long _value);
int jlib_decl numtostr(char *dst, __int64 _value);
int jlib_decl numtostr(char *dst, unsigned char value);
int jlib_decl numtostr(char *dst, unsigned short value);
int jlib_decl numtostr(char *dst, unsigned int value);
int jlib_decl numtostr(char *dst, unsigned long value);
int jlib_decl numtostr(char *dst, unsigned __int64 _value);

// Translate "human readable" size strings like 4G to numbers
extern jlib_decl offset_t friendlyStringToSize(const char *in);

// Translate "human readable" cpu amounts that can either be a decimal (e.g. 2.5), or a number of milli-cores (e.g. 1500m)
extern jlib_decl double friendlyCPUToDecimal(const char *in);

extern jlib_decl double getResourcedCpus(const char *resourceName);

// Write a string as file contents, atomically
extern void jlib_decl atomicWriteFile(const char *fileName, const char *output);

#ifndef _WIN32
/**
 * Return full path name of a currently loaded dll that matches the supplied tail
 *
 * @param ret    StringBuffer to receive full path name
 * @param match  Partial name to be located
 * @return       True if a matching loaded dll was found
 */
extern jlib_decl bool findLoadedModule(StringBuffer &ret,  const char *match);
#endif

extern jlib_decl HINSTANCE LoadSharedObject(const char *name, bool isGlobal, bool raiseOnError);
extern jlib_decl void FreeSharedObject(HINSTANCE h);

class jlib_decl SharedObject : public CInterfaceOf<IInterface>
{
public:
    SharedObject()      { h = 0; bRefCounted = false; }
    ~SharedObject()     { unload(); }
    
    bool load(const char * dllName, bool isGlobal, bool raiseOnError=false);
    bool loadCurrentExecutable();
    bool loadResources(const char * dllName);
    bool loaded() const { return h != 0; }
    void unload();
    HINSTANCE getInstanceHandle() const { return h; }
    void *getEntry(const char * name) const;

public:
    HINSTANCE       h;
    bool bRefCounted;
};

// Interface for dynamically-loadable plugins

interface IPluggableFactory : extends IInterface
{
    virtual bool initializeStore() = 0;
};

typedef IPluggableFactory * (* IPluggableFactoryFactory)(const SharedObject *dll, const IPropertyTree *);

extern jlib_decl IPluggableFactory *loadPlugin(const IPropertyTree* pluginInfo);


//---------------------------------------------------------------------------

//functions for generating unique identifiers consisting of 0..9,A..V
typedef unsigned __int64  unique_id_t;

extern jlib_decl StringBuffer & appendUniqueId(StringBuffer & target, unique_id_t value);
extern jlib_decl unique_id_t    getUniqueId();
extern jlib_decl StringBuffer & getUniqueId(StringBuffer & target);
extern jlib_decl void           resetUniqueId();

extern jlib_decl unsigned getRandom();              // global 
extern jlib_decl void seedRandom(unsigned seed);

interface IRandomNumberGenerator: public IInterface
{
    virtual void seed(unsigned seedval)=0;
    virtual unsigned next()=0;
};

extern jlib_decl IRandomNumberGenerator *createRandomNumberGenerator();

// functions to populate buffers with randomly generated data
extern jlib_decl void fillRandomData(size32_t writeSz, void *writePtr);
extern jlib_decl void fillRandomData(size32_t writeSz, MemoryBuffer &mb);


#ifdef WIN32

// Reentrant version of the rand() function for use with multithreaded applications.
// rand_r return value between 0 and RAND_R_MAX (exclusive). Not that RAND_MAX is
// implementation dependent: SHORT_MAX on Windows, INT_MAX on Linux.
jlib_decl int rand_r(unsigned int *seed);
#define RAND_R_MAX  INT_MAX

#else 

#define RAND_R_MAX  RAND_MAX

#endif

inline int fastRand()
{
    // rand() causes Coverity can issue a 'WEAK_CRYPTO' warning, but we only use fastRand() where deemed safe to do so.

    // coverity[DC.WEAK_CRYPTO]
    return rand();
}


interface IShuffledIterator: extends IInterface
{
    virtual void seed(unsigned seedval)=0;  // ony required for repeatability
    virtual bool first()=0;
    virtual bool isValid() = 0;
    virtual bool next() = 0;
    virtual unsigned get() = 0;
    virtual unsigned lookup(unsigned idx) = 0;  // looks up idx'th entry
};

extern jlib_decl IShuffledIterator *createShuffledIterator(unsigned n); // returns iterator that returns 0..n-1 in shuffled order

/* misc */
extern jlib_decl bool isCIdentifier(const char* id);

/* base64 encoder/decoder */
extern jlib_decl void JBASE64_Encode(const void *data, long length, StringBuffer &out, bool addLineBreaks);
extern jlib_decl void JBASE64_Encode(const void *data, long length, IIOStream &out, bool addLineBreaks);
extern jlib_decl StringBuffer &JBASE64_Decode(const char *in, StringBuffer &out);
extern jlib_decl MemoryBuffer &JBASE64_Decode(const char *in, MemoryBuffer &out);
extern jlib_decl StringBuffer &JBASE64_Decode(ISimpleReadStream &in, StringBuffer &out);
extern jlib_decl MemoryBuffer &JBASE64_Decode(ISimpleReadStream &in, MemoryBuffer &out);

/**
 * Decode base 64 encoded string.
 * It handles forbidden printable and non-printable chars. Space(s) inserted among the valid chars,
 * missing pad chars and invalid length.
 *
 * @param length        Length of the input string.
 * @param in            Pointer to base64 encoded string
 * @param out           Decoded string if the input is valid
 * @return              True when success
 */
extern jlib_decl bool JBASE64_Decode(size32_t length, const char *in, StringBuffer &out);

extern jlib_decl void JBASE32_Encode(const char *in,StringBuffer &out);  // result all lower
extern jlib_decl void JBASE32_Decode(const char *in,StringBuffer &out);  

/* URL: http://user:passwd@host:port/path */
extern jlib_decl StringBuffer& encodeUrlUseridPassword(StringBuffer& out, const char* in);
extern jlib_decl StringBuffer& decodeUrlUseridPassword(StringBuffer& out, const char* in);
extern jlib_decl StringBuffer& encodeURL(StringBuffer& out, const char* in);
//--------------------------------------------------------------------------------------------------------------------

class StringPointerArrayMapper : public SimpleArrayMapper<const char *>
{
    typedef const char * MEMBER;
public:
    static void construct(const char * & member, const char * newValue)
    {
        member = newValue ? strdup(newValue) : nullptr;
    }
    static void destruct(MEMBER & member)
    {
        free(const_cast<char *>(member));
    }
    static inline bool matches(MEMBER const & member, const char * param)
    {
        return strcmp(member, param) == 0;
    }
};


class jlib_decl StringArray : public ArrayOf<const char *, const char *, StringPointerArrayMapper>
{
    struct CCmp
    {
        static int compare(char const * const *l, char const * const *r) { return strcmp(*l, *r); }
        static int compareNC(char const * const *l, char const * const *r) { return stricmp(*l, *r); }
        static int revCompare(char const * const *l, char const * const *r) { return strcmp(*r, *l); }
        static int revCompareNC(char const * const *l, char const * const *r) { return stricmp(*r, *l); }
    };
    typedef ArrayOf<const char *, const char *, StringPointerArrayMapper> PARENT;
public:
    void appendArray(const StringArray & other);
    // Appends a list in a string delimited by 'delim'
    void appendList(const char *list, const char *delim, bool trimSpaces = true);
    // Appends a list in a string delimited by 'delim' without duplicates
    void appendListUniq(const char *list, const char *delim, bool trimSpaces = true);
    StringBuffer &getString(StringBuffer &ret, const char *delim) const; // get CSV string of array contents
    void sortAscii(bool nocase=false);
    void sortAsciiReverse(bool nocase=false);
    void sortCompare(int (*compare)(const char * const * l, const char * const * r));
    void clear(bool) = delete;
private:
    using PARENT::sort; // prevent access to this function - to avoid ambiguity
};
class CIStringArray : public StringArray, public CInterface
{
};

interface IScmIterator : extends IInterface
{
    virtual bool first() = 0;
    virtual bool next() = 0;
    virtual bool isValid() = 0;
};

interface IStringIterator : extends IScmIterator
{
    virtual IStringVal & str(IStringVal & str) = 0;
};


class jlib_decl CStringArrayIterator : implements CInterfaceOf<IStringIterator>
{
    unsigned idx = 0;
    StringArray strings;
public:
    void append(const char *str) { strings.append(str); }
    void append_unique(const char *str) { strings.appendUniq(str); }
    virtual bool first() { idx = 0; return strings.isItem(idx); }
    virtual bool next() { idx ++; return strings.isItem(idx); }
    virtual bool isValid() { return strings.isItem(idx); }
    virtual IStringVal & str(IStringVal &s) { s.set(strings.item(idx)); return s; }
};

class jlib_decl CEmptyStringIterator : implements CInterfaceOf<IStringIterator>
{
public:
    virtual bool first() { return false; }
    virtual bool next() { return false; }
    virtual bool isValid() { return false; }
    virtual IStringVal & str(IStringVal &s) { s.clear(); return s; }
};


extern jlib_decl unsigned msTick();
extern jlib_decl unsigned usTick();
extern jlib_decl unsigned __int64 nsTick();
extern jlib_decl int write_pidfile(const char * instance);
extern jlib_decl void doStackProbe();

//isContainerized() allows a constant-folded check for defined(_CONTAINERIZED) in a general expression
#ifdef _CONTAINERIZED
inline constexpr bool isContainerized() { return true; }
#else
inline constexpr bool isContainerized() { return false; }
#endif

//Same for isDebugBuild() rather than requiring #ifdef _DEBUG
#ifdef _DEBUG
inline constexpr bool isDebugBuild() { return true; }
#else
inline constexpr bool isDebugBuild() { return false; }
#endif

#ifndef arraysize
#define arraysize(T) (sizeof(T)/sizeof(*T))
#endif

using EnvironmentVector = std::vector<std::pair<std::string, std::string>>;
extern jlib_decl unsigned runExternalCommand(StringBuffer &output, StringBuffer &error, const char *cmd, const char *input);
extern jlib_decl unsigned runExternalCommand(const char *title, StringBuffer &output, StringBuffer &error, const char *cmd, const char *input, const char * cwd, const EnvironmentVector * optEnvironment);

extern jlib_decl unsigned __int64 greatestCommonDivisor(unsigned __int64 left, unsigned __int64 right);

inline unsigned hex2num(char next)
{
    if ((next >= '0') && (next <= '9'))
        return next - '0';
    if ((next >= 'a') && (next <= 'f'))
        return next - 'a' + 10;
    if ((next >= 'A') && (next <= 'F'))
        return next - 'A' + 10;
    return 0;
}



extern jlib_decl bool matchesMask(const char *fn, const char *mask, unsigned p, unsigned n);
extern jlib_decl StringBuffer &expandMask(StringBuffer &buf, const char *mask, unsigned p, unsigned n);
extern jlib_decl bool constructMask(StringAttr &attr, const char *fn, unsigned p, unsigned n);
extern jlib_decl bool deduceMask(const char *fn, bool expandN, StringAttr &mask, unsigned &p, unsigned &n, unsigned &filenameLen); // p is 0 based in these routines

class HashKeyElement;
class jlib_decl NamedCount
{
    HashKeyElement *ht;
public:
    NamedCount();
    ~NamedCount();
    void set(const char *name);
};

#ifdef NAMEDCOUNTS
#define DECL_NAMEDCOUNT NamedCount namedCount
#define INIT_NAMEDCOUNT namedCount.set(typeid(*this).name())
#else
#define DECL_NAMEDCOUNT
#define INIT_NAMEDCOUNT {}
#endif

extern jlib_decl StringBuffer &dumpNamedCounts(StringBuffer &str);

interface IAtom;
extern jlib_decl void serializeAtom(MemoryBuffer & target, IAtom * name);
extern jlib_decl IAtom * deserializeAtom(MemoryBuffer & source);

template <class KEY, class VALUE, class COMPARE>
VALUE * binsearch(KEY key, VALUE * * values, unsigned num, COMPARE * cmp)
{
    unsigned l = 0;
    unsigned u = num;
    while(l<u)
    {
        unsigned i = l+(u-l)/2;
        int c = cmp->compare(key, values[i]);
        if(c == 0)
        {
            return values[i];
        }
        else if(c < 0)
        {
            u = i;
        }
        else
        {
            l = i+1;
        }
    }
    return NULL;
}

extern jlib_decl StringBuffer &genUUID(StringBuffer &in, bool nocase=false);

// Convert offset_t to string
// Note that offset_t can be 32, 64 bit integers or a structure
class jlib_decl OffsetToString
{
    StringBuffer m_buffer;
public:
    OffsetToString(offset_t offset);
    const char* str() { return m_buffer.str(); }
};

extern jlib_decl StringBuffer & passwordInput(const char* prompt, StringBuffer& passwd);

/**
 * Return a reference to a shared IProperties object representing the environment.conf settings.
 * The object is loaded when first needed, and freed at program termination. This function is threadsafe.
 *
 * @return    The environment.conf properties
 *
 */
extern jlib_decl const IProperties &queryEnvironmentConf();

/**
 * Return an owned copy of the local environment.xml file
 *
 * @return    The environment.xml property tree
 *
 */
extern jlib_decl IPropertyTree *getHPCCEnvironment();
extern jlib_decl bool getConfigurationDirectory(const IPropertyTree *dirtree, // NULL to use HPCC config
                                                const char *category, 
                                                const char *component,
                                                const char *instance, 
                                                StringBuffer &dirout);

extern jlib_decl bool querySecuritySettings(DAFSConnectCfg *_connectMethod,
                                            unsigned short *_port,
                                            const char * *  _certificate,
                                            const char * *  _privateKey,
                                            const char * *  _passPhrase);

extern jlib_decl bool queryDafsSecSettings(DAFSConnectCfg *_connectMethod,
                                           unsigned short *_port,
                                           unsigned short *_sslport,
                                           const char * *  _certificate,
                                           const char * *  _privateKey,
                                           const char * *  _passPhrase);

//Queries environment.conf file
extern jlib_decl bool queryHPCCPKIKeyFiles(const char * *  _certificate,//HPCCCertificateFile
                                           const char * *  _publicKey,  //HPCCPublicKeyFile
                                           const char * *  _privateKey, //HPCCPrivateKeyFile
                                           const char * *  _passPhrase);//HPCCPassPhrase, encrypted

#ifndef _CONTAINERIZED
extern jlib_decl bool queryMtlsBareMetalConfig();
#endif

extern jlib_decl const char * matchConfigurationDirectoryEntry(const char *path,const char *mask,StringBuffer &name, StringBuffer &component, StringBuffer &instance);
extern jlib_decl bool replaceConfigurationDirectoryEntry(const char *path,const char *frommask,const char *tomask,StringBuffer &out);
extern jlib_decl bool validateConfigurationDirectory(const IPropertyTree* useTree, const char* category, const char* component, const char* instance, const char* dirToValidate);

extern jlib_decl const char *queryCurrentProcessPath();

extern jlib_decl StringBuffer &getFileAccessUrl(StringBuffer &out);

/**
 * Locate the 'package home' directory - normally /opt/HPCCSystems - by detecting the current executable's location
 *
 * @param path     Returns the package home location
 * @return         True if the home directory was located
 */
extern jlib_decl bool getPackageFolder(StringBuffer & path);

extern jlib_decl int parseCommandLine(const char * cmdline, MemoryBuffer &mb, const char** &argvout); // parses cmdline into argvout returning arg count (mb used as buffer)

extern jlib_decl bool safe_ecvt(size_t len, char * buffer, double value, int numDigits, int * decimal, int * sign);
extern jlib_decl bool safe_fcvt(size_t len, char * buffer, double value, int numPlaces, int * decimal, int * sign);
extern jlib_decl StringBuffer &getTempFilePath(StringBuffer & target, const char * component, IPropertyTree * pTree);
extern jlib_decl StringBuffer &getSpillFilePath(StringBuffer & target, const char * component, IPropertyTree * pTree);
extern jlib_decl StringBuffer &createUniqueTempDirectoryName(StringBuffer & ret);
extern jlib_decl IFile *createUniqueTempDirectory();
extern jlib_decl StringBuffer &getSystemTempDir(StringBuffer &ret);

interface jlib_thrown_decl ICorruptDllException: extends IException
{
};

struct EnumMapping { int val; const char *str; };

extern jlib_decl const char *getEnumText(int value, const EnumMapping *map); // fails if no match
extern jlib_decl int getEnum(const char *v, const EnumMapping *map); //fails if no match
extern jlib_decl const char *getEnumText(int value, const EnumMapping *map, const char * defval);
extern jlib_decl int getEnum(const char *v, const EnumMapping *map, int defval);


class jlib_decl QuantilePositionIterator
{
public:
    QuantilePositionIterator(size_t _numRows, unsigned _numDivisions, bool roundUp)
    : numRows(_numRows), numDivisions(_numDivisions)
    {
        assertex(numDivisions);
        step = numRows / numDivisions;
        stepDelta = (unsigned)(numRows % numDivisions);
        initialDelta = roundUp ? (numDivisions)/2 : (numDivisions-1)/2;
        first();
    }
    bool first()
    {
        curRow = 0;
        curDelta = initialDelta;
        curQuantile = 0;
        return true;
    }
    bool next()
    {
        if (curQuantile >= numDivisions)
            return false;
        curQuantile++;
        curRow += step;
        curDelta += stepDelta;
        if (curDelta >= numDivisions)
        {
            curRow++;
            curDelta -= numDivisions;
        }
        assertex(curRow <= numRows);
        return true;
    }
    size_t get() { return curRow; }

protected:
    size_t numRows;
    size_t curRow;
    size_t step;
    unsigned numDivisions;
    unsigned stepDelta;
    unsigned curQuantile;
    unsigned curDelta;
    unsigned initialDelta;
};


class jlib_decl QuantileFilterIterator
{
public:
    QuantileFilterIterator(size_t _numRows, unsigned _numDivisions, bool roundUp)
    : numRows(_numRows), numDivisions(_numDivisions)
    {
        assertex(numDivisions);
        initialDelta = roundUp ? (numDivisions-1)/2 : (numDivisions)/2;
        first();
    }
    bool first()
    {
        curRow = 0;
        curDelta = initialDelta;
        curQuantile = 0;
        isQuantile = true;
        return true;
    }
    bool next()
    {
        if (curRow > numRows)
            return false;
        curRow++;
        curDelta += numDivisions;
        isQuantile = false;
        if (curDelta >= numRows)
        {
            curDelta -= numRows;
            isQuantile = true;
        }
        return true;
    }
    size_t get() { return isQuantile; }

protected:
    size_t numRows;
    size_t curRow;
    size_t step;
    size_t curDelta;
    unsigned numDivisions;
    unsigned curQuantile;
    unsigned initialDelta;
    bool isQuantile;
};

template <typename Container, typename Value>
inline bool stdContains(Container&& container, Value &&v)
{
    return container.end() != std::find(container.begin(), container.end(), std::forward<Value>(v));
}


class jlib_decl COnScopeExit
{
    const std::function<void()> exitFunc;
public:
    inline COnScopeExit(const std::function<void()> &_exitFunc) : exitFunc(_exitFunc) { }
    inline ~COnScopeExit()
    {
        exitFunc();
    }
};

struct HPCCBuildInfo
{
    const char *buildTag;

    const char *dirName;
    const char *prefix;
    const char *execPrefix;
    const char *configPrefix;
    const char *installDir;
    const char *libDir;
    const char *execDir;
    const char *componentDir;
    const char *configDir;
    const char *configSourceDir;
    const char *adminDir;
    const char *pluginsDir;
    const char *runtimeDir;
    const char *lockDir;
    const char *pidDir;
    const char *logDir;

    const char *envXmlFile;
    const char *envConfFile;

    unsigned buildVersionMajor;
    unsigned buildVersionMinor;
    unsigned buildVersionPoint;
    const char *buildVersion;
    const char *buildMaturity;
    const char *buildTagTimestamp;
};

extern jlib_decl HPCCBuildInfo hpccBuildInfo;
extern jlib_decl bool checkCreateDaemon(unsigned argc, const char * * argv);

extern jlib_decl unsigned readDigits(char const * & str, unsigned numDigits, bool throwOnFailure = true);

//Createpassword of specified length, containing UpperCaseAlphas, LowercaseAlphas, numerics and symbols
extern jlib_decl const char * generatePassword(StringBuffer &pwd, int pwdLen);

extern jlib_decl bool getDefaultPlane(StringBuffer &ret, const char * componentOption, const char * category);

extern jlib_decl void getResourceFromJfrog(StringBuffer &localPath, IPropertyTree &item);

extern jlib_decl void hold(const char *msg);

#endif

