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

#include <atomic>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
#include <errno.h>
//#include <winsock.h>  // for TransmitFile
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <algorithm>

#if defined (__linux__) || defined (__APPLE__) || defined (EMSCRIPTEN)
#include <time.h>
#include <dirent.h>
#include <utime.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#endif

#if defined (__linux__)
#include <poll.h>
#include <sys/vfs.h>
#include <sys/sendfile.h>
#include <sys/inotify.h>
#endif
#if defined (__APPLE__)
#include <sys/mount.h>
#undef MIN
#endif

#include "time.h"

#include "jerror.hpp"
#include "jlib.hpp"
#include "jio.hpp"
#include "jmisc.hpp"
#include "jsort.hpp"

#include "jmutex.hpp"
#include "jfile.hpp"
#include "jfile.ipp"
#include "jplane.hpp"

#include <limits.h>
#include "jexcept.hpp"
#include "jsocket.hpp"      // for IpAddress
#include "jregexp.hpp"
#include "portlist.h"

#include "jprop.hpp"

// #define REMOTE_DISCONNECT_ON_DESTRUCTOR  // enable to disconnect on IFile destructor
                                            // this should not be enabled in WindowRemoteDirectory used
//#define CHECK_FILE_IO           // If enabled, reads and writes are checked for sensible parameters

#ifdef _DEBUG
//#define CHECK_FILE_CLOSED_BEFORE_DELETE
#endif

#ifdef _DEBUG
#define ASSERTEX(e) assertex(e); 
#else
#define ASSERTEX(e)
#endif

#ifdef __64BIT__
#define DEFAULT_STREAM_BUFFER_SIZE 0x100000
#else
// Restrict buffer sizes on 32-bit systems
#define DEFAULT_STREAM_BUFFER_SIZE 0x10000
#endif

#ifdef _WIN32
#define NULLFILE INVALID_HANDLE_VALUE
#else
#define NULLFILE -1
#endif

// #define CFILEIOTRACE 1

static IFile *createIFileByHook(const RemoteFilename & filename);
static IFile *createContainedIFileByHook(const char *filename);
static inline bool isPCFlushAllowed();

static char ShareChar='$';

// defaults
static IFEflags expertEnableIFileFlagsMask = IFEnone;
static IFEflags expertDisableIFileFlagsMask = IFEnone;

bool isShareChar(char c)
{
    return (c==ShareChar)||(c=='$');
}

char getShareChar()
{
    return ShareChar;
}

void setShareChar(char c)
{
    ShareChar = c;
}

StringBuffer &setPathDrive(StringBuffer &filename,unsigned drvnum)
{
    return swapPathDrive(filename,(unsigned)-1,drvnum);
}


unsigned getPathDrive(const char *s)
{
    char c = *s;
    if (isPathSepChar(c)&&(s[1]==c)) {
        s = strchr(s+2,c);
        if (!s)
            return 0;
    }
    if (isPathSepChar(c))
        s++;
    if (*s&&((s[1]==':')||(isShareChar(s[1]))))
        return *s-'c';
    return 0;
}

StringBuffer &swapPathDrive(StringBuffer &filename,unsigned fromdrvnum,unsigned todrvnum,const char *frommask,const char *tomask)
{
    const char *s = filename.str();
    char c = *s;
    if (isPathSepChar(c)&&(s[1]==c)) {
        s = strchr(s+2,c);
        if (!s)
            return filename;
    }
    if (isPathSepChar(c))
        s++;
    if (*s&&((fromdrvnum==(unsigned)-1)||(*s==(char) (fromdrvnum+'c')))&&((s[1]==':')||(isShareChar(s[1]))))
        filename.setCharAt((size32_t)(s-filename.str()),todrvnum+'c');
    else if (frommask&&*frommask) { // OSS
        StringBuffer tmp;
        if (replaceConfigurationDirectoryEntry(filename.str(),frommask,tomask, tmp))
            tmp.swapWith(filename);
    }
    return filename;
}

StringBuffer &getStandardPosixPath(StringBuffer &result, const char *path)
{
    result.set(path);
    const char *s = result.trim().replace('\\', '/').str();
    bool startWithPathSepChar = isPathSepChar(s[0]);
    if (startWithPathSepChar)
        s++;

    if (*s && ((s[1]==':') || isShareChar(s[1])))
    {
        char c = tolower(s[0]);
        if (!startWithPathSepChar)
            result.insert(0, '/');
        result.setCharAt(1, c);
        result.setCharAt(2, '$');
    }
    return result;
}

bool isRootDirectory(const char * path)
{
    char c = path[0];
    //Check for linux root directory
    if (isPathSepChar(c))
        return path[1] == '\0';
    //Also check for windows drive:
    if (c && (path[1] == ':') && isPathSepChar(path[2]))
        return path[3] == '\0';
    return false;
}

const char *pathTail(const char *path)
{
    if (!path)
        return NULL;
    const char *tail=path;
    const char *s = path;
    while (*s)
        if (isPathSepChar(*(s++)))
            tail = s;
    return tail;
}

const char * pathExtension(const char * path)
{
    const char * tail = pathTail(path);
    if (tail)
        return strrchr(tail, '.');
    return NULL;
}

enum class SafeStatBehaviour : unsigned {
    Standard      = 0, // Fire error on anything except ENOENT and ENOTDIR
    IgnoreEaccess = 1, // Ignore EACCES
    IgnoreAll     = 2, // Ignore all errors (legacy behaviour)
    Count              // count/end marker
};
static constexpr SafeStatBehaviour defaultSafeStatBehaviour{SafeStatBehaviour::Standard};
static std::atomic<SafeStatBehaviour> safeStatBehaviour{defaultSafeStatBehaviour};
static bool safeStat(const char *filename, struct stat &info)
{
    if (stat(filename, &info) == 0)
        return true;
    else if ((ENOENT == errno) || (ENOTDIR == errno)) // always also consider a missing directory as a missing file/dir
        return false;
    SafeStatBehaviour behaviour = safeStatBehaviour.load();
    switch (behaviour)
    {
        case SafeStatBehaviour::Standard:
            break; // fall through and fire error
        case SafeStatBehaviour::IgnoreEaccess:
            if (EACCES == errno)
                return false;
            break;
        case SafeStatBehaviour::IgnoreAll:
            return false;
        default:
            throwUnexpected();
    }
    throw makeErrnoExceptionV(errno, "CFile::checkFileExists %s", filename);
}

bool checkFileExists(const char * filename)
{
#ifdef _WIN32
    for (unsigned i=0;i<10;i++) {
        DWORD ret = (DWORD)GetFileAttributes(filename); 
        if (ret!=(DWORD)-1)
            return true;
        DWORD err = GetLastError();
        if (err!=ERROR_IO_PENDING)
            break;
        Sleep(100*i);
    }
    return false;
#else
    struct stat info;
    return safeStat(filename, info);
#endif
}

bool checkDirExists(const char * filename)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(filename);
    return (attr != (DWORD)-1)&&(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    if (!safeStat(filename, info))
        return false;
    return S_ISDIR(info.st_mode);
#endif
}

static StringBuffer &getLocalOrRemoteName(StringBuffer &name,const RemoteFilename & filename)
{
    if (filename.isLocal()&&!filename.isNull()) {
        filename.getLocalPath(name);
#ifdef _WIN32
        // kludge to allow local linux paths on windows
        const char *n=name.str();
        if (n[0]=='/') {
            StringBuffer tmp;
            if (isShareChar(n[2])) {
                tmp.append(n[1]).append(':');
                n+=3;
            }
            while (*n) {
                if (*n=='/')
                    tmp.append('\\');
                else
                    tmp.append(*n);
                n++;
            }
            name.clear().append(tmp);
        }
#endif
    }
    else  
        filename.getRemotePath(name);
    return name;
}


CFile::CFile(const char * _filename)
{
    filename.set(_filename);
    flags = ((unsigned)IFSHread)|((S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)<<16);
}

void CFile::setCreateFlags(unsigned short cflags)
{
    flags &= 0xffff;
    flags |= ((unsigned)cflags<<16);
}

void CFile::setShareMode(IFSHmode shmode)
{
    flags &= ~(IFSHfull|IFSHread);
    flags |= (unsigned)(shmode&(IFSHfull|IFSHread));
}


bool CFile::exists()
{
    if (stdIoHandle(filename)>=0)
        return true;
    return checkFileExists(filename);
}

#ifdef _WIN32

bool WindowsCreateDirectory(const char * path)
{
    unsigned retry = 0;
    for (;;) {
        if (CreateDirectory(path, NULL))
            return true;
        DWORD err = GetLastError();
        if ((err==ERROR_FILE_NOT_FOUND) || (err==ERROR_PATH_NOT_FOUND) || (err==ERROR_FILE_EXISTS) || (err==ERROR_CANNOT_MAKE) || (err==ERROR_ACCESS_DENIED))
            break;
        else if (err==ERROR_ALREADY_EXISTS) {
            DWORD attr = GetFileAttributes(path);
            if ((attr != -1)&&( attr & FILE_ATTRIBUTE_DIRECTORY))
                return true;
            return false;
        }
        if ((retry++==10)||  // some or all of the following can occur when the domain controller gets busy
                             // retrying improves chance of success
            ((err!=ERROR_NETNAME_DELETED)&&(err!=ERROR_DEV_NOT_EXIST)&&(err!=ERROR_GEN_FAILURE)&&(err!=ERROR_NETWORK_BUSY)&&(err!=ERROR_BAD_NET_NAME))) 
            return false;
        //PROGLOG("Retrying(%d) WindowsCreateDirectory %s, err=%d",retry,filename,err);
        Sleep(retry*100); 
    }
    return false;
}

#else


bool LinuxCreateDirectory(const char * path)
{
    if (!path)
        return false;
    if (CreateDirectory(path, NULL))
        return true;
    else
    {
        if (EEXIST == errno)
        {
            struct stat info;
            if (stat(path, &info) != 0) // unlikely to fail given EEXIST
                return false;
            return S_ISDIR(info.st_mode);
        }
    }
    return false;
}


#endif


bool localCreateDirectory(const char *name)
{
    if (!name)
        return false;
    size32_t l = (size32_t)strlen(name);
    if (l==0)
        return true;
    if (isPathSepChar(name[0])&&((l==1)||(isPathSepChar(name[1])&&!containsPathSepChar(name+2))))
        return true;
#ifdef _WIN32
    if (name[1]==':') {
        if ((l==2)||((l==3)&&isPathSepChar(name[2])))
            return true;
    }
#endif
    if (checkDirExists(name))
        return true;
#ifdef _WIN32
    if (WindowsCreateDirectory(name))
#else
    if (LinuxCreateDirectory(name))
#endif
        return true;
    if (isPathSepChar(name[l-1])) l--;
    while (l&&!isPathSepChar(name[l-1]))
        l--;
    if (l<=1)
        return true;
    StringAttr parent(name,l-1);
    if (!localCreateDirectory(parent.get()))
        return false;
#ifdef _WIN32
    return (WindowsCreateDirectory(name));
#else
    return (LinuxCreateDirectory(name));
#endif
}




bool CFile::createDirectory()
{
    return localCreateDirectory(filename);
}



#ifdef _WIN32
union TimeIntegerUnion
{
    FILETIME        ft;
    ULARGE_INTEGER  l;
};

void FILETIMEtoIDateTime(CDateTime * target, const FILETIME & ft)
{
    if (target)
    {
        TimeIntegerUnion u;
        memcpy(&u, &ft, sizeof(ft));
        unsigned __int64 hundredNanoseconds = (u.l.QuadPart % 10000000);
        u.l.QuadPart -= hundredNanoseconds;
        SYSTEMTIME systime;
        FileTimeToSystemTime(&u.ft, &systime);
        target->set(systime.wYear, systime.wMonth, systime.wDay, systime.wHour, systime.wMinute, systime.wSecond, (unsigned)(hundredNanoseconds*100));
    }
}

FILETIME * IDateTimetoFILETIME(FILETIME & ft, const CDateTime * dt)
{
    if (!dt)
        return NULL;

    SYSTEMTIME systime;
    TimeIntegerUnion u;
    unsigned wYear, wMonth, wDay, wHour, wMinute, wSecond, nanoSeconds;
    dt->getDate(wYear, wMonth, wDay);
    dt->getTime(wHour, wMinute, wSecond, nanoSeconds);
    systime.wYear = wYear;
    systime.wMonth = wMonth;
    systime.wDay = wDay;
    systime.wHour = wHour;
    systime.wMinute = wMinute;
    systime.wSecond = wSecond;
    systime.wMilliseconds = 0;
    SystemTimeToFileTime(&systime, &u.ft);
    //Adjust the fractions of a second ourselves because the function above is only accurate to milli seconds.
    u.l.QuadPart += (nanoSeconds / 100);
    memcpy(&ft, &u, sizeof(ft));
    return &ft;
}

#endif


bool CFile::getTime(CDateTime * createTime, CDateTime * modifiedTime, CDateTime * accessedTime)
{
#ifdef _WIN32
    //MORE could use GetFileAttributesEx() if we were allowed...
    FILETIME timeCreated, timeModified, timeAccessed;
    HANDLE handle = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle==(HANDLE)-1)
        return false;
    GetFileTime(handle, &timeCreated, &timeAccessed, &timeModified);
    CloseHandle(handle);
    FILETIMEtoIDateTime(createTime, timeCreated);
    FILETIMEtoIDateTime(modifiedTime, timeModified);
    FILETIMEtoIDateTime(accessedTime, timeAccessed);
#else
    struct stat info;
    if (!safeStat(filename, info))
        return false;
    timetToIDateTime(accessedTime,  info.st_atime);
    timetToIDateTime(createTime,    info.st_ctime);
    timetToIDateTime(modifiedTime,  info.st_mtime);
#endif
    return true;
}

#if defined (EMSCRIPTEN)
#define _utimbuf utimbuf
#endif

bool CFile::setTime(const CDateTime * createTime, const CDateTime * modifiedTime, const CDateTime * accessedTime)
{
#ifdef _WIN32
    FILETIME timeCreated, timeModified, timeAccessed;
    FILETIME *pTimeCreated, *pTimeModified, *pTimeAccessed;

    pTimeCreated = IDateTimetoFILETIME(timeCreated, createTime);
    pTimeModified = IDateTimetoFILETIME(timeModified, modifiedTime);
    pTimeAccessed = IDateTimetoFILETIME(timeAccessed, accessedTime);
    HANDLE handle = CreateFile(filename, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (!handle)
        return false;
    SetFileTime(handle, pTimeCreated, pTimeAccessed, pTimeModified);
    CloseHandle(handle);
    return true;

#else
    struct utimbuf am;
    if (!accessedTime||!modifiedTime) {
        struct stat info;
        if (!safeStat(filename, info))
            return false;
        am.actime = info.st_atime;
        am.modtime = info.st_mtime;
    }
    if (accessedTime)
        am.actime   = timetFromIDateTime (accessedTime);
    if (modifiedTime)
        am.modtime  = timetFromIDateTime (modifiedTime);
    return (utime(filename, &am)==0);
#endif
}



fileBool CFile::isDirectory()
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(filename);
    if (attr == -1)
        return fileBool::notFound;
    return ( attr & FILE_ATTRIBUTE_DIRECTORY) ? fileBool::foundYes : fileBool::foundNo;
#else
    struct stat info;
    if (!safeStat(filename, info))
        return fileBool::notFound;
    return S_ISDIR(info.st_mode) ? fileBool::foundYes : fileBool::foundNo;
#endif
}

fileBool CFile::isFile()
{
    if (stdIoHandle(filename)>=0)
        return fileBool::foundYes;
#ifdef _WIN32
    DWORD attr = GetFileAttributes(filename);
    if (attr == -1)
        return fileBool::notFound;
    return ( attr & FILE_ATTRIBUTE_DIRECTORY) ? fileBool::foundNo : fileBool::foundYes;
#else
    struct stat info;
    if (!safeStat(filename, info))
        return fileBool::notFound;
    return S_ISREG(info.st_mode) ? fileBool::foundYes : fileBool::foundNo;
#endif
}

fileBool CFile::isReadOnly()
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(filename);
    if (attr == -1)
        return fileBool::notFound;
    return ( attr & FILE_ATTRIBUTE_READONLY) ? fileBool::foundYes : fileBool::foundNo;
#else
    struct stat info;
    if (!safeStat(filename, info))
        return fileBool::notFound;
    //MORE: I think this is correct, but someone with better unix knowledge should check!
    return (info.st_mode & (S_IWUSR|S_IWGRP|S_IWOTH)) ? fileBool::foundNo : fileBool::foundYes;
#endif
}

#ifndef _WIN32

static bool setShareLock(int fd,IFSHmode share)
{
    struct flock fl;
    do {
        memset(&fl,0,sizeof(fl));
        if (share==IFSHnone)
            fl.l_type = F_WRLCK;
        else if (share==IFSHread)
            fl.l_type = F_RDLCK;
        else
            fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_pid    = getpid(); 
        if (fcntl(fd, F_SETLK, &fl) != -1)
            return true;
    } while (errno==EINTR);
    if ((errno!=EAGAIN)&&(errno!=EACCES))
        IERRLOG("setShareLock failure %d, mode %d", errno,(int)share);
    return (share==IFSHfull); // always return true for full
}


#endif

HANDLE CFile::openHandle(IFOmode mode, IFSHmode sharemode, bool async, int stdh)
{
    HANDLE handle = NULLFILE;
#ifdef _WIN32
    if (stdh>=0) {
        DWORD mode;
        switch (stdh) {
        case 0: mode = STD_INPUT_HANDLE; break;
        case 1: mode = STD_OUTPUT_HANDLE; break;
        case 2: mode = STD_ERROR_HANDLE; break;
        default:
            return handle;
        }
        DuplicateHandle(GetCurrentProcess(), GetStdHandle(mode), GetCurrentProcess(), &handle , 0, FALSE, DUPLICATE_SAME_ACCESS);
        return handle;
    }

    DWORD share = 0;
    switch (sharemode) {
    case (IFSHfull|IFSHread):
    case IFSHfull:
        share |= FILE_SHARE_WRITE; 
        // fall through
    case IFSHread:
        share |= FILE_SHARE_READ;
    }
    DWORD fflags = async?FILE_FLAG_OVERLAPPED:0;
    if (async&&(mode==IFOread))
        fflags |= FILE_FLAG_SEQUENTIAL_SCAN; 
    switch (mode) {
    case IFOcreate:
        handle = CreateFile(filename, GENERIC_WRITE, share, NULL, CREATE_ALWAYS, fflags, NULL);
        break;
    case IFOread:
        handle = CreateFile(filename, GENERIC_READ, share, NULL, OPEN_EXISTING, fflags, NULL);
        break;
    case IFOwrite:
        handle = CreateFile(filename, GENERIC_WRITE, share, NULL, OPEN_ALWAYS, fflags, NULL);
        break;
    case IFOcreaterw:
        handle = CreateFile(filename, GENERIC_WRITE|GENERIC_READ, share, NULL, CREATE_ALWAYS, fflags, NULL);
        break;
    case IFOreadwrite:
        handle = CreateFile(filename, GENERIC_WRITE|GENERIC_READ, share, NULL, OPEN_ALWAYS, fflags, NULL);
        break;
    }
    if (handle == NULLFILE)
    {   
        DWORD err = GetLastError();
        if ((IFOread!=mode) || ((err!=ERROR_FILE_NOT_FOUND) && (err!=ERROR_PATH_NOT_FOUND)))
            throw makeOsExceptionV(err,"CFile::open %s (%x, %x)", filename.get(), mode, share);
        return NULLFILE;
    }
#else
    if (stdh>=0)
        return (HANDLE)dup(stdh);

    unsigned fileflags = (flags>>16) &  (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_IROTH|S_IWOTH|S_IXOTH);
    unsigned openflags;
    switch (mode) {
    case IFOcreate:
        openflags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case IFOread:
        openflags = O_RDONLY;
        break;
    case IFOwrite:
        openflags = O_WRONLY | O_CREAT;
        break;
    case IFOcreaterw:
        openflags = O_RDWR | O_CREAT | O_TRUNC;
        break;
    case IFOreadwrite:
        openflags = O_RDWR | O_CREAT;
        break;
    default:
        return NULLFILE;
    }
    // MCK - if (extraFlags & IFEsync) should we add O_SYNC or O_DSYNC to openflags
    //       and then not have to fdatasync() at close(), flush(), write() ?
    //Prevent the file from being inherited by a child process;
    openflags |= O_CLOEXEC;
    handle = _lopen(filename.get(), openflags, fileflags);
    if (handle == -1)
    {
        if ((IFOread!=mode) || (errno != ENOENT))
            throw makeErrnoExceptionV(errno, "CFile::open %s", filename.get());
        return NULLFILE;
    }
    // check not a directory (compatible with windows)

    struct stat info;
    if (fstat(handle, &info) == -1) {
        int err = errno;
        close(handle);
        handle = NULLFILE;
        throw makeErrnoExceptionV(err, "CFile::open fstat %s", filename.get());
    }
    if (S_ISDIR(info.st_mode)) {
        close(handle);
        handle = NULLFILE;
        throw makeErrnoExceptionV(EISDIR, "CFile::open %s", filename.get());
    }

#ifdef CFILEIOTRACE
    DBGLOG("CFile::openHandle(%s,%d) returns %d", filename.get(), mode, handle);
#endif

#endif
    return handle;
}

IFileIO * CFile::open(IFOmode mode,IFEflags extraFlags)
{
    // we may want mode dependant defaults later 
    return openShared(mode,(IFSHmode)(flags&(IFSHfull|IFSHread)),extraFlags);
}


IFileAsyncIO * CFile::openAsync(IFOmode mode)
{
    HANDLE handle = openHandle(mode,IFSHread,true);     // I don't think we want shared write to an async file
    return new CFileAsyncIO(handle,IFSHread);
}

const char * CFile::queryFilename()
{
    return filename;
}


bool CFile::remove()
{
#ifdef _WIN32
    unsigned retry = 0;
    for (;;) {
        if (isDirectory()==fileBool::foundYes) {
            if (RemoveDirectory(filename) != 0)
                return true;
        }
        else {
            if (DeleteFile(filename) != 0)
                return true;
        }
        DWORD err = GetLastError();
        if ( (err==ERROR_FILE_NOT_FOUND) || (err==ERROR_PATH_NOT_FOUND) )
            break;
        if ((retry++==10)||  // some or all of the following can occur when the domain controller gets busy
                             // retrying improves chance of success
            ((err!=ERROR_NETNAME_DELETED)&&(err!=ERROR_DEV_NOT_EXIST)&&(err!=ERROR_GEN_FAILURE)&&(err!=ERROR_NETWORK_BUSY)&&(err!=ERROR_BAD_NET_NAME))) 
            throw makeOsExceptionV(err, "CFile::remove %s", filename.get());
        //PROGLOG("Retrying(%d) DeleteFile %s, err=%d",retry,filename,err);
        Sleep(retry*100); 
    }
    return false;
#else
    if (isDirectory()==fileBool::foundYes) {
        if (rmdir(filename) == 0)
            return true;
    }
    else {
        if (unlink(filename) == 0)
            return true;
    }
    if (ENOENT!=errno)
        throw makeErrnoExceptionV("CFile::remove %s", filename.get());
    return false;
#endif
}

static std::atomic<unsigned> renameRetries = (unsigned)-1; // initially uninitialized, set on 1st use (in rename)
static std::atomic<bool> manualRenameChk = false;
static constexpr bool defaultManualRenameChk = true;
static constexpr unsigned defaultNumRetries = 4;

void setRenameRetries(unsigned _renameRetries, bool _manualRenameChk)
{
    manualRenameChk = _manualRenameChk;
    renameRetries = _renameRetries;
}

static void initRenameRetrySettings()
{
    if ((unsigned)-1 != renameRetries)
        return;
    // NB: potentially could be >1 thread here, but that's ok.
    try
    {
        Owned<IPropertyTree> globalConfig = getGlobalConfigSP();
        Owned<IPropertyTree> config = getComponentConfigSP();
        manualRenameChk = config->getPropBool("expert/@manualRenameChk", globalConfig->getPropBool("expert/@manualRenameChk", defaultManualRenameChk));
        renameRetries = config->getPropInt("expert/@numRenameRetries", globalConfig->getPropInt("expert/@numRenameRetries", defaultNumRetries));
    }
    catch (IException *e) // handle cases where config. not available
    {
        EXCLOG(e, "initRenameRetrySettings");
        e->Release();
        renameRetries = 0;
        manualRenameChk = false;
    }
}

static void doRename(const char *src, const char *dst, const char *callerName)
{
    initRenameRetrySettings();

    unsigned retriesLeft = renameRetries;
    unsigned retry = 0;
    while (true)
    {
        // NB: if 'manualRenameChk' is on, since we don't explicitly check the existence of 'src' and 'dst' before rename
        // it's theoretically possible the rename failure is legitimate because src does not exist and dst does.
        // In this situation, we continue and return success below.
        if (-1 != ::rename(src, dst))
            return; // success

        int err = errno; // preserve to use after tracing if exhausted retry attempts

#ifndef _WIN32 // NB: this is primarily here to help track down the issues reported in HPCC-28454 and HPCC-29812
        /*
         * In testing 2 [rare] issues have been seen after rename returned -1:
         * 1) The file was renamed (dst exists and src does not).
         *    If 'manualRenameChk' is on, we check for this and return success.
         * 2) Neither src nor dst exist, however, after a short time, dst appears.
         *    Assuming 'numRenameRetries'is >0, this case is covered below by looping and trying again.
         */

        CDateTime modTime;
        struct stat statInfo;
        StringBuffer srcModTimeStr, dstModTimeStr;
        int srcStatErr = stat(src, &statInfo);
        if (0 == srcStatErr)
        {
            timetToIDateTime(&modTime, statInfo.st_mtime);
            modTime.getString(srcModTimeStr);
        }
        int dstStatErr = stat(dst, &statInfo);
        if (0 == dstStatErr)
        {
            timetToIDateTime(&modTime, statInfo.st_mtime);
            modTime.getString(dstModTimeStr);
        }

        if (0 != srcStatErr) // probably means src does now not exist
        {
            if (manualRenameChk) // manually validate if rename() actually succeeded
            {
                if (0 == dstStatErr) // dst now exists
                {
                    DBGLOG("%s-doRename(%s, %s) FAILED BUT DID RENAME (dst modtime=%s)", callerName, src, dst, dstModTimeStr.str());
                    return;
                }
            }
        }
        VStringBuffer errMsg("%s-doRename(%s, %s) FAILED [err=%d] ", callerName, src, dst, err);
        errMsg.appendf("src stat:[err=%d", srcStatErr);
        if (0 == srcStatErr)
            errMsg.appendf(",modTime=%s", srcModTimeStr.str());
        errMsg.append("]");
        errMsg.appendf(", dst stat:[err=%d", dstStatErr);
        if (0 == dstStatErr)
            errMsg.appendf(",modTime=%s", dstModTimeStr.str());
        errMsg.append("]");
        DBGLOG("%s", errMsg.str());
#endif

        if (0 == retriesLeft)
            throw makeErrnoExceptionV(err, "%s(%s, %s)", callerName, src, dst);

        MilliSleep(retry>5 ? 5000 : (100 << retry)); // start at 100ms and double each retry, but cap at 5 seconds

        --retriesLeft;
        ++retry;
    }
}

void CFile::rename(const char *newname)
{
    // now hopefully newname is just file tail 
    // however we do allow full paths and extract tail, warning if the directory appears to mismatch 
    StringBuffer path;
    splitDirTail(filename,path);
    StringBuffer newdir;
    const char *tail = splitDirTail(newname,newdir);
    if (path.length()&&newdir.length()) {
        if (strcmp(newdir.str(),path.str())!=0) 
            IWARNLOG("CFile::rename '%s' to '%s' : directory mismatch",filename.get(),newname);
    }
    const char *dst = path.append(tail);
    if (isPathSepChar(dst[0])&&(dst[1]==dst[0])) { // hmm is share - convert to local path
        RemoteFilename rfn;
        rfn.setRemotePath(dst);
        if (rfn.isLocal())
            dst = rfn.getLocalPath(path.clear()).str();
    }
    doRename(filename, dst, "CFile::rename");
    filename.set(path);
}

void CFile::move(const char *newname)
{
    if (!newname||!*newname)
        return;
    StringBuffer path;
    if (isPathSepChar(newname[0])&&(newname[1]==newname[0])) { // hmm is share - convert to local path
        RemoteFilename rfn;
        rfn.setRemotePath(newname);
        if (rfn.isLocal())
            newname = rfn.getLocalPath(path.clear()).str();
    }
#ifdef _WIN32
    unsigned retry = 0;
    for (;;) {
        if (MoveFileEx(filename.get(),newname, MOVEFILE_REPLACE_EXISTING))
            return;
        DWORD err = GetLastError();
        if ((retry++==10)||  // some or all of the following can occur when the domain controller gets busy
                             // retrying improves chance of success
            ((err!=ERROR_NETNAME_DELETED)&&(err!=ERROR_DEV_NOT_EXIST)&&(err!=ERROR_GEN_FAILURE)&&(err!=ERROR_NETWORK_BUSY)&&(err!=ERROR_BAD_NET_NAME))) 
            throw makeOsExceptionV(err, "CFile::move(%s, %s)", filename.get(), newname);
        Sleep(retry*100); 
    }
#else
    doRename(filename, newname, "CFile::move");
#endif
    filename.set(newname);
}

#ifdef _WIN32
DWORD CALLBACK fastCopyProgressRoutine(
  LARGE_INTEGER TotalFileSize,          
  LARGE_INTEGER TotalBytesTransferred,  
  LARGE_INTEGER StreamSize,             
  LARGE_INTEGER StreamBytesTransferred, 
  DWORD dwStreamNumber,                 
  DWORD dwCallbackReason,               
  HANDLE hSourceFile,                  
  HANDLE hDestinationFile,              
  LPVOID lpData                        
)
{
    if (TotalBytesTransferred.QuadPart<=TotalFileSize.QuadPart)
    {
        CFPmode status = ((ICopyFileProgress *)lpData)->onProgress(TotalBytesTransferred.QuadPart,TotalFileSize.QuadPart);
        switch (status)
        {
        case CFPcontinue:
            return PROGRESS_CONTINUE;
        case CFPstop:
            return PROGRESS_STOP;
        default:
            return PROGRESS_CANCEL;
        }
    }
    return PROGRESS_CONTINUE;
}
#endif

bool CFile::fastCopyFile(CFile &target, size32_t buffersize, ICopyFileProgress *progress)
{
    // only currently supported for windows
#ifdef _WIN32
    unsigned retry = 0;
    for (;;) {
        BOOL cancel=FALSE;
        if (CopyFileEx(queryFilename(),target.queryFilename(),progress?fastCopyProgressRoutine:NULL,progress?progress:NULL,&cancel,0))
            break;
        DWORD err = GetLastError();
        if ( (err==ERROR_FILE_NOT_FOUND) || (err==ERROR_PATH_NOT_FOUND) )
            return false;
        if ((retry++==10)||  // some or all of the following can occur when the domain controller gets busy
                             // retrying improves chance of success
            ((err!=ERROR_NETNAME_DELETED)&&(err!=ERROR_DEV_NOT_EXIST)&&(err!=ERROR_GEN_FAILURE)&&(err!=ERROR_NETWORK_BUSY)&&(err!=ERROR_BAD_NET_NAME))) 
            return false;
        Sleep(retry*100); 
    }
    return true;
#else
    return false;
#endif
}


void copyFileSection(IFile * src, IFile * target, offset_t toOfs, offset_t fromOfs, offset_t size, ICopyFileProgress *progress, CFflags copyFlags)
{
    // check to see if src and target are remote

    const size32_t buffersize = DEFAULT_COPY_BLKSIZE;
    IFOmode omode = IFOwrite;
    if (toOfs==(offset_t)-1) {
        if (fromOfs==0) {
            copyFile(target,src,buffersize,progress,copyFlags);
            return;
        }
        omode = IFOcreate;
        toOfs = 0;
    }
    IFEflags tgtFlags = IFEnone;
    if (copyFlags & CFflush_write)
        tgtFlags = IFEnocache;
    OwnedIFileIO targetIO = target->open(omode, tgtFlags);
    if (!targetIO)
        throw MakeStringException(-1, "copyFile: target path '%s' could not be created", target->queryFilename());
    MemoryAttr mb;
    void * buffer = mb.allocate(buffersize);
    IFEflags srcFlags = IFEnone;
    if (copyFlags & CFflush_read)
        srcFlags = IFEnocache;
    OwnedIFileIO sourceIO = src->open(IFOread, srcFlags);
    if (!sourceIO)
        throw MakeStringException(-1, "copySection: source '%s' not found", src->queryFilename());
    
    offset_t offset = 0;
    offset_t total;
    try
    {
        total = sourceIO->size();
        if (total<fromOfs)
            total = 0;
        else
            total -= fromOfs;
        if (total>size)
            total = size;
        while (offset<total)
        {
            size32_t got = sourceIO->read(fromOfs+offset, (buffersize>total-offset)?((size32_t)(total-offset)):buffersize, buffer);
            if (got == 0)
                break;
            targetIO->write(offset+toOfs, got, buffer);
            offset += got;
            if (progress && progress->onProgress(offset, total) != CFPcontinue)
                break;
        }
    }
    catch (IException *e)
    {
        StringBuffer s;
        s.append("copyFile target=").append(target->queryFilename()).append(" source=").append(src->queryFilename()).append("; read/write failure").append(": ");
        e->errorMessage(s);
        IException *e2 = makeOsExceptionV(e->errorCode(), "%s", s.str());
        e->Release();
        throw e2;
    }
}

void CFile::copySection(const RemoteFilename &dest, offset_t toOfs, offset_t fromOfs, offset_t size, ICopyFileProgress *progress, CFflags copyFlags)
{
    Owned<IFile> target = createIFile(dest);
    copyFileSection(this, target, toOfs, fromOfs, size, progress, copyFlags);
}

void CFile::copyTo(IFile *dest, size32_t buffersize, ICopyFileProgress *progress,bool usetmp,CFflags copyFlags)
{
    doCopyFile(dest,this,buffersize,progress,NULL,usetmp,copyFlags);
}



void CFile::setReadOnly(bool ro)
{
#ifdef _WIN32
    if (!SetFileAttributes(filename, ro ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL))
    {
        DWORD err = GetLastError();
        if ( (err!=ERROR_FILE_NOT_FOUND) && (err!=ERROR_PATH_NOT_FOUND) )
            throw makeOsExceptionV(err, "CFile::setReadOnly %s", filename.get());
    }
#else
    struct stat info;
    if (stat(filename, &info) != 0)
        throw makeErrnoExceptionV("CFile::setReadOnly() %s", filename.get());
    // not sure correct but consistant with isReadOnly
    if (ro)
        info.st_mode &= ~(S_IWUSR|S_IWGRP|S_IWOTH);
    else
        info.st_mode |= (S_IWUSR|S_IWGRP|S_IWOTH);
    chmod(filename, info.st_mode);
#endif
}

void CFile::setFilePermissions(unsigned fPerms)
{
#ifndef _WIN32
    struct stat info;
    if (stat(filename, &info) != 0)
        throw makeErrnoExceptionV("CFile::setFilePermissions() %s", filename.get());
    if (chmod(filename, fPerms&0777) != 0)
        throw makeErrnoExceptionV("CFile::setFilePermissions() %s", filename.get());
#endif
}


offset_t CFile::size()
{
    if (stdIoHandle(filename)>=0)
        return 0;   // dummy value
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesEx(filename, GetFileExInfoStandard, &info) != 0) {
        LARGE_INTEGER x;
        x.LowPart = info.nFileSizeLow;
        x.HighPart = info.nFileSizeHigh;
        return (offset_t)x.QuadPart;
    }
#else
    struct stat info;
    if (safeStat(filename, info))
        return info.st_size;
#endif
#if 0
    // we could try opening but I don't think needed
    Owned<IFileIO>io = openShared(IFOread,IFSHfull);
    if (io)
        return io->size();
#endif
    return (offset_t)-1;
}

bool CFile::setCompression(bool set)
{
#ifdef _WIN32
    DWORD attr=::GetFileAttributes(filename.get());
    if(attr==-1)
        throw makeOsExceptionV(::GetLastError(), "CFile::setCompression %s", filename.get());
    if (((attr & FILE_ATTRIBUTE_COMPRESSED) != 0) == set)
        return true;

    HANDLE handle=::CreateFile(filename.get(),GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(handle==INVALID_HANDLE_VALUE)
        throw makeOsExceptionV(::GetLastError(), "CFile::setCompression %s", filename.get());

    USHORT compression=set ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;
    DWORD bytes;
    if(::DeviceIoControl(handle, FSCTL_SET_COMPRESSION, &compression, sizeof(compression), NULL, 0, &bytes, NULL))
    {
        ::CloseHandle(handle);
        return true;
    }
    DWORD err=::GetLastError();
    ::CloseHandle(handle);
    throw makeOsExceptionV(err, "CFile::setCompression %s", filename.get());
#else
    return false;
#endif
}

offset_t CFile::compressedSize()
{
#ifdef _WIN32
    DWORD hi, lo=::GetCompressedFileSize(filename.get(),&hi), err;
    if(lo==INVALID_FILE_SIZE && (err=::GetLastError())!=NO_ERROR)
    {
        if ( (err!=ERROR_FILE_NOT_FOUND) && (err!=ERROR_PATH_NOT_FOUND) )
            throw makeOsExceptionV(err,"CFile::compressedSize %s", filename.get());
        return -1;
    }
    return makeint64(hi,lo);
#else
    return size();
#endif
}

class CDiscretionaryFileLock: implements IDiscretionaryLock, public CInterface
{
    bool locked;
    bool excllock;
    Linked<IFile> file;
    Linked<IFileIO> fileio;
    HANDLE handle;


public:
    CDiscretionaryFileLock(IFile *_file)
        : file(_file)
    {
        excllock = false;
        locked = false;
#ifdef _WIN32
        handle=::CreateFile(file->queryFilename(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(handle==INVALID_HANDLE_VALUE)
        {
            handle = NULLFILE;
            throw makeOsExceptionV(GetLastError(), "CDiscretionaryFileLock::openhandle %s", file->queryFilename());
        }
#else
        handle = _lopen(file->queryFilename(), O_RDONLY, 0);
        if (handle == -1)
        {
            handle = NULLFILE;
            throw makeErrnoExceptionV(errno, "CDiscretionaryFileLock::openhandle %s", file->queryFilename());
        }
#endif
    }
    CDiscretionaryFileLock(IFileIO *_fileio)
        : fileio(_fileio)
    {
        excllock = false;
        locked = false;
        handle = NULLFILE;
        CFileIO *cfileio = QUERYINTERFACE(_fileio,CFileIO);
        if (cfileio)
            handle = cfileio->queryHandle();
        if (handle==NULLFILE)
            throw makeStringException(-1, "CDiscretionaryFileLock - invalid parameter");
    }
    ~CDiscretionaryFileLock()
    {
        if (locked)
            unlock();
        if ((handle!=NULLFILE)&&!fileio.get())
#ifdef _WIN32
            CloseHandle(handle);
#else
            _lclose(handle);
#endif
    }
    IMPLEMENT_IINTERFACE;

    virtual bool isLocked() { return (handle!=NULLFILE); }
    virtual bool isExclusiveLocked() { return excllock; }

    bool lock(bool exclusive=true, unsigned timeout=INFINITE)
    {
        if (locked) {
            if (exclusive) {
                if (excllock)
                    return true;
                unlock(); // have to unlock - unfortunate as means no window-less change mode
            }
            else {
                if (!excllock)
                    return true;
                unlock(); // have to unlock - unfortunate as means no window-less change mode
            }
        }
        unsigned interval = 1;
        unsigned start = msTick();
        for (;;) {
#ifdef _WIN32
            OVERLAPPED overlapped;
            memset(&overlapped,0,sizeof(overlapped));
            if (LockFileEx(handle,
                (exclusive?LOCKFILE_EXCLUSIVE_LOCK:0)|((timeout==INFINITE)?0:LOCKFILE_FAIL_IMMEDIATELY),
                          0,1,0,&overlapped))
                break;
            
            DWORD err = ::GetLastError();
            if (err!=ERROR_LOCK_VIOLATION) 
                throw makeOsException(err, "CDiscretionaryFileLock::lock");
#else
            if (setShareLock(handle,exclusive?IFSHnone:IFSHread))
                break;
#endif

            unsigned now = msTick();
            if (now-start>=timeout) 
                return false; 
            if (interval>timeout-now+start)
                interval = timeout-now+start;
            Sleep(interval);
            interval *= 2;
            if (interval>1000)
                interval = 1000;
        }
        excllock = exclusive;
        locked = true;
        return true;
    }

    void unlock()
    {
        if (handle!=NULLFILE)
        {
#ifdef _WIN32
            OVERLAPPED overlapped;
            memset(&overlapped,0,sizeof(overlapped));
            if (!UnlockFileEx(handle,0,1,0,&overlapped)) 
                throw makeOsException(::GetLastError(), "CDiscretionaryFileLock::unlockhandle");
#else
            setShareLock(handle,IFSHfull);
#endif
        }
    }


};

IDiscretionaryLock *createDiscretionaryLock(IFile *file)
{
    return new CDiscretionaryFileLock(file);
}

IDiscretionaryLock *createDiscretionaryLock(IFileIO *fileio)
{
    return new CDiscretionaryFileLock(fileio);
}


unsigned CFile::getCRC()
{
    if (stdIoHandle(filename)>=0)
        return 0;   // dummy value
    unsigned crc=~0;
    MemoryAttr ma;
    void *buf = ma.allocate(DEFAULT_STREAM_BUFFER_SIZE);
    Owned<IFileIO> fileio = open(IFOread);
    if (fileio) {
        offset_t pos=0;
        for (;;) {
            size32_t rd = fileio->read(pos,DEFAULT_STREAM_BUFFER_SIZE,buf);
            if (!rd)
                break;
            crc=crc32((const char *)buf,rd,crc);
            pos += rd;
        }
    }
    return ~crc;
}



//---------------------------------------------------------------------------

#ifdef _WIN32
static bool parseShare(const char *filename,IpAddress &machine,StringBuffer &share)
{ // windows share parsing
    if (!filename||!isPathSepChar(filename[0])||(filename[0]!=filename[1]))
        return false;
    const char *start = filename+2;
    const char * end = start;
    while (*end && !isPathSepChar(*end))
        end++;
    if (!*end)
        return false;
    StringBuffer ipText(end-start, start);
    machine.ipset(ipText.str());
    end++;  
    while (*end && !isPathSepChar(*end))
        end++;
    start = filename;
    while (start!=end) {
        if (*start=='/')
            share.append('\\');
        else 
            share.append(*start);
        start++;
    }
    return true;
}

static CriticalSection connectcrit;

static bool connectToExternalDrive(const char * const filename)
{
#if 0
    //The following code used the passwordProvider.  Retained in case similar code is needed in the future.
    CriticalBlock block(connectcrit);
    if (!passwordProvider)
        return false;

    StringBuffer share, username, password;
    IpAddress ip;
    if (!parseShare(filename,ip,share))
        return false;
    if (!passwordProvider->getPassword(ip, username, password))
        return false;

    // first see if connected
    char buf[255];
    buf[0] = 0;
    DWORD len = sizeof(buf);
    DWORD err = WNetGetUser(share.str(),buf,&len);
    if ((err==0)&&(stricmp(username.str(),buf)==0)) {
        // check can access share as well
        for (unsigned i=0;i<10;i++) {
            DWORD ret = (DWORD)GetFileAttributes(share.str()); 
            DWORD err = (ret==(DWORD)-1)?GetLastError():0;
            if (err!=ERROR_IO_INCOMPLETE)
                break;
            Sleep(100*i);
        }
        if (err==0)
            return true;
    }
    for (unsigned retry=0;retry<5;retry++) {
        NETRESOURCE res;
        memset(&res, 0, sizeof(res));
        res.dwType = RESOURCETYPE_DISK;
        res.lpRemoteName = (char *)share.str();
        err = WNetAddConnection2(&res, password.str(), username.str(), 0);
        if (err) {
            if (err==ERROR_SESSION_CREDENTIAL_CONFLICT) {
                WNetCancelConnection2(res.lpRemoteName, 0, false);
                err = WNetAddConnection2(&res, password.str(), username.str(), 0);
            }
            if (err) 
                IERRLOG("WNetAddConnection2(%d): connecting to %s, User: %s",err,res.lpRemoteName,username.str());
        }
        if (err==0)
            return true;
        Sleep(retry*100);
    }
#endif
    return false;
}


static void disconnectFromExternalDrive(const char * const filename)
{
    CriticalBlock block(connectcrit);
    StringBuffer share;
    IpAddress ip;
    if (!parseShare(filename,ip,share))
        return;
    if (share.length()&&isShareChar(share.charAt(share.length()))) 
        WNetCancelConnection2((char *)share.str(), 0, 0);
}

class CWindowsRemoteFile : implements IFile, public CInterface
{
    IFile               *ifile;
    StringAttr          filename;
    bool                connected;

    inline bool connect()
    {
        if (!connected)
            connected = connectToExternalDrive(filename);
        return connected;
    }

public:


    CWindowsRemoteFile(const char * _filename)
        : filename(_filename)
    {
        connected = false;
        ifile = new CFile(_filename);
    }

    ~CWindowsRemoteFile()
    {
        ifile->Release();
#ifdef REMOTE_DISCONNECT_ON_DESTRUCTOR
        if (connected)
            disconnectFromExternalDrive(filename);
#endif

    }

    IMPLEMENT_IINTERFACE

    virtual bool exists()
    {
        connect();
        return ifile->exists();
    }
    virtual bool getTime(CDateTime * createTime, CDateTime * modifiedTime, CDateTime * accessedTime)
    {
        connect();
        return ifile->getTime(createTime, modifiedTime, accessedTime);
    }
    virtual bool setTime(const CDateTime * createTime, const CDateTime * modifiedTime, const CDateTime * accessedTime)
    {
        connect();
        if (ifile->setTime(createTime, modifiedTime, accessedTime))
            return true;
        if (connected||!connect())
            return false;
        return ifile->setTime(createTime, modifiedTime, accessedTime);
    }
    virtual fileBool isDirectory()
    {
        connect();
        fileBool ok = ifile->isDirectory();
        if (ok == fileBool::notFound && !connected && connect())
            ok = ifile->isDirectory();
        return ok;
    }
    virtual fileBool isFile()
    {
        connect();
        fileBool ok = ifile->isFile();
        if (ok == fileBool::notFound && !connected && connect())
            ok = ifile->isFile();
        return ok;
    }
    virtual fileBool isReadOnly()
    {
        connect();
        fileBool ok = ifile->isReadOnly();
        if (ok == fileBool::notFound && !connected && connect())
            ok = ifile->isFile();
        return ok;
    }
    virtual IFileIO * open(IFOmode mode,IFEflags extraFlags=IFEnone)
    {
        connect();
        return ifile->open(mode,extraFlags);
    }
    virtual IFileIO * openShared(IFOmode mode,IFSHmode shared,IFEflags extraFlags=IFEnone)
    {
        connect();
        return ifile->openShared(mode,shared,extraFlags);
    }
    virtual IFileAsyncIO * openAsync(IFOmode mode)
    {
        connect();
        return ifile->openAsync(mode);
    }
    virtual const char * queryFilename()
    {
        return ifile->queryFilename();
    }
    virtual bool remove()
    {
        connect();
        unsigned attempt=0;         \
        return ifile->remove();
    }
    virtual void rename(const char *newname)
    {
        connect();
        StringBuffer path;
        splitDirTail(filename,path);
        StringBuffer newdir;
        const char *tail = splitDirTail(newname,newdir);
        if ((newname[0]=='\\')&&(newname[1]=='\\')) { // rename to remote
            if (path.length()&&newdir.length()) {
                if (strcmp(newdir.str(),path.str())!=0) 
                    IWARNLOG("CWindowsRemoteFile '%s' to '%s' : directory mismatch",filename.get(),newname);
            }
            newname = tail; // just rename using tail
        }
        ifile->rename(newname);
        path.append(tail);
        filename.set(path);
    }
    virtual void move(const char *newName)
    {
        connect();
        ifile->move(newName);
        filename.set(ifile->queryFilename());
    }
    virtual void setReadOnly(bool ro)
    {
        connect();
        ifile->setReadOnly(ro);
    }
    virtual void setFilePermissions(unsigned fPerms)
    {
        connect();
        ifile->setFilePermissions(fPerms);
    }
    virtual offset_t size()
    {
        connect();
        offset_t ret=ifile->size();
        if ((ret==(offset_t)-1)&&!connected&&connect())
            ret=ifile->size();
        return ret;
    }
    virtual bool setCompression(bool set)
    {
        connect();
        return ifile->setCompression(set);
    }
    virtual offset_t compressedSize()
    {
        connect();
        return ifile->compressedSize();
    }

    bool fastCopyFile(CWindowsRemoteFile &target, size32_t buffersize, ICopyFileProgress *progress)
    {
#ifdef _WIN32
        CFile *src = QUERYINTERFACE(ifile,CFile);
        if (!src)
            return false;
        CFile *dst = QUERYINTERFACE(target.ifile,CFile);
        if (!dst)
            return false;
        target.isFile(); // encourage connect on target
        connect();
        return src->fastCopyFile(*dst, buffersize, progress);
#else
        return false;
#endif
    }


    bool fastCopyFile(CFile &target, size32_t buffersize, ICopyFileProgress *progress)
    {
#ifdef _WIN32
        CFile *src = QUERYINTERFACE(ifile,CFile);
        if (!src)
            return false;
        connect();
        return src->fastCopyFile(target, buffersize, progress);
#else
        return false;
#endif
    }

    bool fastCopyFileRev(CFile &src, size32_t buffersize, ICopyFileProgress *progress)
    {
#ifdef _WIN32
        CFile *dst = QUERYINTERFACE(ifile,CFile);
        if (!dst)
            return false;
        connect();
        return src.fastCopyFile(*dst, buffersize, progress);
#else
        return false;
#endif
    }

    void copyTo(IFile *dest, size32_t buffersize, ICopyFileProgress *progress, bool usetmp,CFflags copyFlags)
    {
        doCopyFile(dest,this,buffersize,progress,NULL,usetmp,copyFlags);
    }



    bool createDirectory()
    {
        connect();
        return localCreateDirectory(filename);
    }

    IDirectoryIterator *directoryFiles(const char *mask,bool sub,bool includedirs)
    {
        connect();
        return ifile->directoryFiles(mask,sub,includedirs);
    }

    IDirectoryDifferenceIterator *monitorDirectory(
                                  IDirectoryIterator *prev=NULL,    // in (NULL means use current as baseline)
                                  const char *mask=NULL,
                                  bool sub=false,
                                  bool includedirs=false,
                                  unsigned checkinterval=60*1000,
                                  unsigned timeout=(unsigned)-1,
                                  Semaphore *abortsem=NULL) // returns NULL if timed out or abortsem signalled
    {
        connect();
        return ifile->monitorDirectory(prev,mask,sub,includedirs,checkinterval,timeout,abortsem);
    }

    bool getInfo(bool &isdir,offset_t &size,CDateTime &modtime)
    {
        connect();
        return ifile->getInfo(isdir,size,modtime);
    }



    unsigned getCRC()
    {
        connect();
        return ifile->getCRC();
    }

    void setCreateFlags(unsigned short cflags)
    {
        ifile->setCreateFlags(cflags);
    }
    void setShareMode(IFSHmode shmode)
    {
        ifile->setShareMode(shmode);
    }

    void copySection(const RemoteFilename &dest, offset_t toOfs, offset_t fromOfs, offset_t size, ICopyFileProgress *progress, CFflags copyFlags)
    {
        connect();
        ifile->copySection(dest,toOfs,fromOfs,size,progress,copyFlags);
    }

    IMemoryMappedFile *openMemoryMapped(offset_t ofs, memsize_t len, bool write)
    {
        throw MakeStringException(-1,"Remote file cannot be memory mapped");
        return NULL;
    }
};
#endif



IFileIO *_createIFileIO(const void *buffer, unsigned sz, bool readOnly)
{
    class CMemoryBufferIO : implements IFileIO, public CInterface
    {
        MemoryBuffer mb;
        void *buffer;
        size32_t sz;
        bool readOnly;
    public:
        IMPLEMENT_IINTERFACE;
        CMemoryBufferIO(const void *_buffer, unsigned _sz, bool _readOnly) : readOnly(_readOnly)
        {
            // JCSMORE - should probably have copy as option
            mb.append(_sz, _buffer);
            buffer = (void *)mb.toByteArray();
            sz = mb.length();
        }
        virtual size32_t read(offset_t pos, size32_t len, void * data)
        {
            if (pos>sz)
                throw MakeStringException(-1, "CMemoryBufferIO: read beyond end of buffer pos=%" I64F "d, len=%d, buffer length=%d", pos, len, mb.length());
            if (pos+len > sz)
                len = (size32_t)(sz-pos);
            memcpy(data, (byte *)buffer+pos, len);
            return len;
        }

        virtual offset_t size() { return sz; }
        virtual size32_t write(offset_t pos, size32_t len, const void * data)
        {
            assertex(!readOnly);
            if (pos+len>sz)
                throw MakeStringException(-1, "CMemoryBufferIO: UNIMPLEMENTED, writing beyond buffer, pos=%" I64F "d, len=%d, buffer length=%d", pos, len, mb.length());
            memcpy((byte *)buffer+pos, data, len);
            return len;
        }
        virtual void flush() {}
        virtual void close() {}
        virtual unsigned __int64 getStatistic(StatisticKind kind)
        {
            return 0;
        }
        virtual void setSize(offset_t size)
        {
            if (size > mb.length())
                throw MakeStringException(-1, "CMemoryBufferIO: UNIMPLEMENTED, setting size %" I64F "d beyond end of buffer, buffer length=%d", size, mb.length());
            mb.setLength((size32_t)size);
        }
        virtual IFile * queryFile() const override
        {
            return nullptr;
        }

    };

    return new CMemoryBufferIO(buffer, sz, readOnly);
}

IFileIO * createIFileI(unsigned len, const void * buffer)
{
    return _createIFileIO((void *)buffer, len, true);
}

//---------------------------------------------------------------------------

class jlib_decl CSequentialFileIO : public CFileIO
{
    offset_t pos;
    
    void checkPos(const char *fn,offset_t _pos)
    {
        if (_pos!=pos)
            throw MakeStringException(-1, "CSequentialFileIO %s out of sequence (%" I64F "d,%" I64F "d)",fn,pos,_pos);
    }

public:
    CSequentialFileIO(IFile * _creator, HANDLE h,IFOmode _openmode,IFSHmode _sharemode,IFEflags _extraFlags)
        : CFileIO(_creator, h,_openmode,_sharemode,_extraFlags)
    {
        pos = 0;
    }
    ~CSequentialFileIO()
    {
    }

    size32_t read(offset_t _pos, size32_t len, void * data)
    {
        checkPos("read",_pos);
#ifdef _WIN32
        // Can't use checked_read because don't have the c fileno for it
        DWORD numRead;
        if (ReadFile(file,data,len,&numRead,NULL) == 0) {
            DWORD err = GetLastError();
            if (err==ERROR_BROKEN_PIPE)  // windows returns this at end of pipe
                return 0;
            throw makeOsException(GetLastError(),"CSequentialFileIO::read");
        }
        size32_t ret = (size32_t)numRead;
#else
        size32_t ret = checked_read(querySafeFilename(), file, data, len);
#endif
        pos += ret;
        return ret;
    }
    
    virtual size32_t write(offset_t _pos, size32_t len, const void * data)
    {
        checkPos("write",_pos);

        size32_t ret;
#ifdef _WIN32
        DWORD numWritten;
        if (!WriteFile(file, data, len, &numWritten, NULL))
            throw makeOsException(GetLastError(), "CSequentialFileIO::write");
        if (numWritten != len)
            throw makeOsException(DISK_FULL_EXCEPTION_CODE, "CSequentialFileIO::write");
        ret = (size32_t) numWritten;
#else
        ret = ::write(file,data,len);
        if (ret==(size32_t)-1)
        {
            PrintStackReport();
            IERRLOG("errno(%d): %" I64F "d %u",errno,pos,len);
            throw makeErrnoException(errno, "CFileIO::write");
        }
        if (ret<len)
            throw makeOsException(DISK_FULL_EXCEPTION_CODE, "CSequentialFileIO::write");
#endif
        pos += ret;
        return ret;

    }

};

IFileIO * CFile::openShared(IFOmode mode,IFSHmode share,IFEflags extraFlags)
{
    int stdh = stdIoHandle(filename);
    HANDLE handle = openHandle(mode,share,false, stdh);
    if (handle==NULLFILE)
        return NULL;
    // MCK - if (extraFlags & IFEnocache) and mode is not WRONLY perhaps turn off read-ahead ?
    //       No - while read-ahead can put more into page-cache, IFEnocache is a hint and
    //       disabling readahead might affect performance negatively too much ...
    if (stdh>=0)
        return new CSequentialFileIO(this, handle,mode,share,extraFlags);

    Owned<IFileIO> io = new CFileIO(this,handle,mode,share,extraFlags);
#ifdef CHECK_FILE_IO
    return new CCheckingFileIO(filename, io);
#else
    return io.getClear();
#endif
}



//---------------------------------------------------------------------------

static std::atomic<bool> defaultFileSyncWriteCloseEnabled = false; // NB: set/updated by config updateFunc

extern jlib_decl IFileIO *createIFileIO(IFile * creator,HANDLE handle,IFOmode openmode,IFEflags extraFlags)
{
    return new CFileIO(creator, handle,openmode,IFSHfull,extraFlags);
}

offset_t appendFile(IFileIO * target, IFile *file,offset_t pos,offset_t len)
{
    if (!file)
        return 0;
    MemoryAttr mb;
    const size32_t buffsize = DEFAULT_STREAM_BUFFER_SIZE;
    void * buffer = mb.allocate(buffsize);
    Owned<IFileIO> fileio = file->open(IFOread);
    offset_t ret=0;
    offset_t outp = target->size();
    while (len)
    {
        size32_t toread = (len>=buffsize) ? buffsize : (size32_t)len;
        size32_t read = fileio->read(pos,toread,buffer);
        if (read==0)
            break;
        size32_t wr = target->write(outp,read,buffer);
        pos += read;
        outp += wr;
        len -= read;
        ret += wr;
        if (wr!=read)
            break;
    }
    return ret;
}


unsigned __int64 CFileIO::getStatistic(StatisticKind kind)
{
    return stats.getStatistic(kind);
}

#ifdef _WIN32

//-- Windows implementation -------------------------------------------------

CFileIO::CFileIO(IFile * _creator, HANDLE handle, IFOmode _openmode, IFSHmode _sharemode, IFEflags _extraFlags)
    : creator(_creator), unflushedReadBytes(0), unflushedWriteBytes(0)
{
    assertex(handle != NULLFILE);
    throwOnError = false;
    file = handle;
    sharemode = _sharemode;
    openmode = _openmode;
    extraFlags = _extraFlags; // page cache flush option silently ignored on Windows for now
    if (extraFlags & IFEnocache)
        if (!isPCFlushAllowed())
            extraFlags = static_cast<IFEflags>(extraFlags & ~IFEnocache);
}

CFileIO::~CFileIO()
{
    try
    {
        //note this will not call the virtual close() if anyone ever derived from this class.
        //the clean fix is to move this code to beforeDispose()
        close();
    }
    catch (IException * e)
    {
        EXCLOG(e, "CFileIO::~CFileIO");
        e->Release();
    }
}

void CFileIO::close()
{
    if (file != NULLFILE)
    {
        // ensure file handle is cleared before throwing exception, to avoid dtor recalling close() as stack unwinds.
        HANDLE tmpHandle = NULLFILE;
        std::swap(tmpHandle, file);

        if (!CloseHandle(tmpHandle))
            throw makeOsExceptionV(GetLastError(),"CFileIO::close for file '%s'", querySafeFilename());
    }
}

void CFileIO::flush()
{
    if (!FlushFileBuffers(file))
        throw makeOsExceptionV(GetLastError(),"CFileIO::flush for file '%s'", querySafeFilename());
}

offset_t CFileIO::size()
{
    LARGE_INTEGER pos;
    pos.LowPart = GetFileSize(file, (unsigned long *)&pos.HighPart);
    if (pos.LowPart==-1) {
        DWORD err = GetLastError();
        if (err!=0)     
            throw makeOsException(err,"CFileIO::size");
    }
    return pos.QuadPart;
}


size32_t CFileIO::read(offset_t pos, size32_t len, void * data)
{
    CriticalBlock procedure(cs);

    CCycleTimer timer;
    DWORD numRead;
    setPos(pos);
    if (ReadFile(file,data,len,&numRead,NULL) == 0)
        throw makeOsExceptionV(GetLastError(),"CFileIO::read for file '%s'", querySafeFilename());
    stats.ioReadCycles.fetch_add(timer.elapsedCycles());
    stats.ioReadBytes.fetch_add(numRead);
    ++stats.ioReads;
    return (size32_t)numRead;
}

void CFileIO::setPos(offset_t newPos)
{
    LARGE_INTEGER tempPos;
    tempPos.QuadPart = newPos; 
    tempPos.LowPart = SetFilePointer(file, tempPos.LowPart, &tempPos.HighPart, FILE_BEGIN);
}

size32_t CFileIO::write(offset_t pos, size32_t len, const void * data)
{
    CriticalBlock procedure(cs);

    CCycleTimer timer;
    DWORD numWritten;
    setPos(pos);
    if (!WriteFile(file,data,len,&numWritten,NULL))
        throw makeOsExceptionV(GetLastError(),"CFileIO::write for file '%s'", querySafeFilename());
    if (numWritten != len)
        throw makeOsExceptionV(DISK_FULL_EXCEPTION_CODE,"CFileIO::write for file '%s'", querySafeFilename());
    stats.ioWriteCycles.fetch_add(timer.elapsedCycles());
    stats.ioWriteBytes.fetch_add(numWritten);
    ++stats.ioWrites;
    return (size32_t)numWritten;
}


void CFileIO::setSize(offset_t pos)
{
    CriticalBlock procedure(cs);
    setPos(pos);
    if (!SetEndOfFile(file))
        throw makeOsExceptionV(GetLastError(), "CFileIO::setSize for file '%s'", querySafeFilename());
}

#else

//-- Unix implementation ----------------------------------------------------

static void doSync(const CFileIO &fileIO, int fd, bool dataOnly)
{
#ifdef F_FULLFSYNC
    // No EIO type retry available
    fcntl(fd, F_FULLFSYNC);
#else
    CCycleTimer timer;
    int ret = dataOnly ? fdatasync(fd) : fsync(fd);
    if (ret == 0)
    {
        if (timer.elapsedMs() >= 10000)
            IWARNLOG("doSync(%s): slow success: took %u ms", fileIO.querySafeFilename(), timer.elapsedMs());
    }
    else
    {
        int err = errno;
        printStackReport();
        Owned<IException> e = makeErrnoExceptionV(err, "doSync(%s): failed after %u ms", fileIO.querySafeFilename(), timer.elapsedMs());
        OWARNLOG(e);
        throw e.getClear();
    }
#endif
}

static void syncFileData(const CFileIO &fileIO, int fd, bool notReadOnly, IFEflags extraFlags, bool wait_previous=false)
{
    if (notReadOnly)
    {
        if (extraFlags & IFEsync)
            doSync(fileIO, fd, true);
#if defined(__linux__)
        else if (extraFlags & IFEnocache)
        {
            unsigned flags = SYNC_FILE_RANGE_WRITE;
            if (wait_previous)
                flags |= SYNC_FILE_RANGE_WAIT_BEFORE;
            sync_file_range(fd, 0, 0, flags);
        }
#endif
    }

#ifdef POSIX_FADV_DONTNEED
    if (extraFlags & IFEnocache)
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
}


// More errorno checking TBD
CFileIO::CFileIO(IFile * _creator, HANDLE handle, IFOmode _openmode, IFSHmode _sharemode, IFEflags _extraFlags)
    : creator(_creator), unflushedReadBytes(0), unflushedWriteBytes(0)
{
    assertex(handle != NULLFILE);
    throwOnError = false;
    file = handle;
    sharemode = _sharemode;
    openmode = _openmode;
    extraFlags = _extraFlags;

    // leave for compatibility
    if (extraFlags & IFEnocache)
        if (!isPCFlushAllowed())
            extraFlags = static_cast<IFEflags>(extraFlags & ~IFEnocache);

    extraFlags = static_cast<IFEflags>(extraFlags | expertEnableIFileFlagsMask);
    extraFlags = static_cast<IFEflags>(extraFlags & ~expertDisableIFileFlagsMask);

    if (isContainerized() && (openmode!=IFOread)) // only containerized (with planes writing to storage types like blob for now)
    {
        const char *filePath = querySafeFilename();
        if ('/' == filePath[0]) // only for absolute paths
        {
            unsigned __int64 value;
            if (findPlaneAttrFromPath(filePath, FileSyncWriteClose, defaultFileSyncWriteCloseEnabled ? 1 : 0, value)) // NB: returns only if plane found
            {
                if (value) // true or false
                    extraFlags = static_cast<IFEflags>(extraFlags | IFEsyncAtClose);
            }
        }
    }

#ifdef CFILEIOTRACE
    DBGLOG("CFileIO::CfileIO(%d,%d,%d,%d)", handle, _openmode, _sharemode, extraFlags);
#endif
}

CFileIO::~CFileIO()
{
#ifdef CHECK_FILE_CLOSED_BEFORE_DELETE
    //Any file that is being written to, should be closed before the object is destroyed, otherwise errors from failing to commit will be lost
    if ((file != NULLFILE) && (openmode!=IFOread))
    {
        OERRLOG("CFileIO::~CFileIO - file '%s' object destroyed without being closed first", querySafeFilename()); // A programmer problem, but the operator should know about it.
        PrintStackReport();
    }
#endif
    try
    {
        close();
    }
    catch (IException * e)
    {
        //An error closing a file cannot throw an exception, but should be logged as a very severe error in the logs.
        DISLOG(e, "CFileIO::~CFileIO");
        PrintStackReport();
        e->Release();
    }
}

void CFileIO::close()
{
    if (file != NULLFILE)
    {
        // ensure file handle is cleared before throwing exception, to avoid dtor recalling close() as stack unwinds.
        HANDLE tmpHandle = NULLFILE;
        std::swap(tmpHandle, file);

#ifdef CFILEIOTRACE
        DBGLOG("CFileIO::close(%d), extraFlags = %d", tmpHandle, extraFlags);
#endif
        if (extraFlags & (IFEnocache | IFEsync))
            syncFileData(*this, tmpHandle, openmode!=IFOread, extraFlags, false);
        else if (extraFlags & IFEsyncAtClose)
            doSync(*this, tmpHandle, false);

        if (::close(tmpHandle) < 0)
            throw makeErrnoExceptionV(errno, "CFileIO::close for file '%s'", querySafeFilename());
    }
}

void CFileIO::flush()
{
    if (0 == (extraFlags & (IFEnocache | IFEsync)))
        return;

    CriticalBlock procedure(cs);

    syncFileData(*this, file, true, extraFlags, false);
}


offset_t CFileIO::size()
{
#ifndef _WIN32
    //MORE: The current implementation of openHandle() calls fstat() to check the file is not a directory
    //If the file hasn't been modified since open() the size could be cached and returned.
    struct stat info;
    if (fstat(file, &info) >= 0)
        return info.st_size;
    return 0;
#else
    CriticalBlock procedure(cs);
    offset_t savedPos = lseek(file,0,SEEK_CUR);
    offset_t length = lseek(file,0,SEEK_END);
    setPos(savedPos);
    return length;
#endif
}

size32_t CFileIO::read(offset_t pos, size32_t len, void * data)
{
    if (0==len) return 0;

    CCycleTimer timer;
    size32_t ret = checked_pread(querySafeFilename(), file, data, len, pos);
    stats.ioReadCycles.fetch_add(timer.elapsedCycles());
    stats.ioReadBytes.fetch_add(ret);
    ++stats.ioReads;

    if ( (extraFlags & IFEnocache) && (ret > 0) )
    {
        if (unflushedReadBytes.add_fetch(ret) >= PGCFLUSH_BLKSIZE)
        {
            unflushedReadBytes.store(0);
            syncFileData(*this, file, false, extraFlags, false);
        }
    }
    return ret;
}

void CFileIO::setPos(offset_t newPos)
{
    if (file != NULLFILE)
        _llseek(file,newPos,SEEK_SET);
}

size32_t CFileIO::write(offset_t pos, size32_t len, const void * data)
{
    CCycleTimer timer;
    size32_t ret = pwrite(file,data,len,pos);
    stats.ioWriteCycles.fetch_add(timer.elapsedCycles());
    stats.ioWriteBytes.fetch_add(ret);
    ++stats.ioWrites;

    if (ret==(size32_t)-1)
        throw makeErrnoExceptionV(errno, "CFileIO::write for file '%s'", querySafeFilename());
    if (ret<len)
        throw makeOsExceptionV(DISK_FULL_EXCEPTION_CODE, "CFileIO::write for file '%s'", querySafeFilename());
    if ( (extraFlags & (IFEnocache | IFEsync)) && (ret > 0) )
    {
        if (unflushedWriteBytes.add_fetch(ret) >= PGCFLUSH_BLKSIZE)
        {
            unflushedWriteBytes.store(0);
            // request to write-out dirty pages
            syncFileData(*this, file, true, extraFlags, true);
        }
    }
    return ret;
}

void CFileIO::setSize(offset_t pos)
{
    if (0 != ftruncate(file, pos))
        throw makeErrnoExceptionV(errno, "CFileIO::setSize for file '%s'", querySafeFilename());
}
#endif

//---------------------------------------------------------------------------

CFileRangeIO::CFileRangeIO(IFileIO * _io, offset_t _headerSize, offset_t _maxLength)
{
    io.set(_io);
    headerSize = _headerSize;
    maxLength = _maxLength;
}

size32_t CFileRangeIO::read(offset_t pos, size32_t len, void * data)
{
    if (pos + len > maxLength)
    {
        if (pos > maxLength)
            pos = maxLength;
        len = (size32_t)(maxLength - pos);
    }
    return io->read(pos+headerSize, len, data);
}

offset_t CFileRangeIO::size()
{
    return maxLength;
}

size32_t CFileRangeIO::write(offset_t pos, size32_t len, const void * data)
{
    if (pos + len > maxLength)
    {
        if (pos > maxLength)
            pos = maxLength;
        len = (size32_t)(maxLength - pos);
    }
    return io->write(pos+headerSize, len, data);
}

//--------------------------------------------------------------------------

CCheckingFileIO::~CCheckingFileIO()
{
    if (lastWriteSize && !closed)
        report("FileCheck: IO for File %s destroyed before closing", filename.str());
}

size32_t CCheckingFileIO::read(offset_t pos, size32_t len, void * data)
{
    CriticalBlock block(cs);
    if ((pos == lastReadPos) && (len < minSeqReadSize) && (pos + len != size()))
        report("FileCheck: Sequential read [%s] of %u is < %u", filename.str(), len, minSeqReadSize);

    size32_t numRead = io->read(pos, len, data);
    lastReadPos = pos + numRead;
    return numRead;
}

offset_t CCheckingFileIO::size()
{
    return io->size();
}

size32_t CCheckingFileIO::write(offset_t pos, size32_t len, const void * data)
{
    CriticalBlock block(cs);
    if (len != 0)
    {
        if ((lastWriteSize != 0) && (lastWriteSize < minWriteSize))
            report("FileCheck: Sequential write to [%s] of size %u before offset %" I64F "u of %u is < %u", filename.str(), lastWriteSize, pos, len, minWriteSize);
        lastWriteSize = len;
    }
    else
        report("FileCheck: Unexpected zero byte write on %s at offset %" I64F "u", filename.str(), pos);

    return io->write(pos, len, data);
}

void CCheckingFileIO::setSize(offset_t size)
{
    io->setSize(size);
}

void CCheckingFileIO::flush()
{
    io->flush();
}

void CCheckingFileIO::close()
{
    io->close();
    closed = true;
}

unsigned __int64 CCheckingFileIO::getStatistic(StatisticKind kind)
{
    return io->getStatistic(kind);
}

void CCheckingFileIO::report(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    VALOG(MCdebugError, format, args);
    va_end(args);
    if (!traced)
    {
        printStackReport();
        traced = true;
    }
}



//--------------------------------------------------------------------------

CFileAsyncIO::~CFileAsyncIO()
{
    try
    {
        close();
    }
    catch (IException * e)
    {
        EXCLOG(e, "CFileAsyncIO::~CFileAsyncIO");
        e->Release();
    }
}

void CFileAsyncIO::flush()
{
    // wait for all outstanding results
    CriticalBlock block(cs);
    ForEachItemInRev(i,results) {
        size32_t dummy;
        results.item(i).getResult(dummy,true);
    }
}

unsigned __int64 CFileAsyncIO::getStatistic(StatisticKind kind)
{
    //MORE: Could implement - but I don't think this class is currently used
    return 0;
}

#ifdef _WIN32

//-- Windows implementation -------------------------------------------------

class CFileAsyncResult: implements IFileAsyncResult, public CInterface
{
protected: friend class CFileAsyncIO;
    OVERLAPPED overlapped;
    DWORD value;
    DWORD wrsize;
    CFileAsyncIO *parent;
public:
    IMPLEMENT_IINTERFACE;
    CFileAsyncResult(offset_t offset,size32_t _wrsize)
    {
        parent = NULL;
        memset(&overlapped,0,sizeof(overlapped));
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        overlapped.Offset = (DWORD)offset;
        overlapped.OffsetHigh = (DWORD)(offset>>32);
        value = (size32_t)-1;
        wrsize = _wrsize;
    }

    ~CFileAsyncResult()
    {
        size32_t dummy;
        getResult(dummy,true);
        CloseHandle(overlapped.hEvent);
    }
    bool getResult(size32_t &ret,bool wait)
    {
        if (value==(size32_t)-1) {
            if (parent) {
                if (GetOverlappedResult(parent->file,&overlapped,&value,wait)==0) {
                    int err = GetLastError();
                    if (err==ERROR_IO_INCOMPLETE) 
                        return false;
                    if (err!=ERROR_HANDLE_EOF) {
                        CriticalBlock block(parent->cs);
                        parent->results.zap(*this,true); // don't delete as array does not own
                        parent = NULL;
                        throw makeOsException(err,"CFileAsyncResult::getResult");
                    }
                    value = 0;
                }
                CriticalBlock block(parent->cs);
                parent->results.zap(*this,true); // don't delete as array does not own
                parent = NULL;
            }
            else
                return false;
        }
        ret = value;
        if (value<wrsize)
            throw makeOsException(DISK_FULL_EXCEPTION_CODE,"CFileAsyncResult::getResult");
        return true;
    }
};

CFileAsyncIO::CFileAsyncIO(HANDLE handle, IFSHmode _sharemode)
{
    assertex(handle != NULLFILE);
    throwOnError = false;
    file = handle;
    sharemode = _sharemode;
}

void CFileAsyncIO::close()
{
    flush();
    // wait for all outstanding results
    if (file != NULLFILE)
    {
        // ensure file handle is cleared before throwing exception, to avoid dtor recalling close() as stack unwinds.
        HANDLE tmpHandle = NULLFILE;
        std::swap(tmpHandle, file);

        if (!CloseHandle(tmpHandle))
            throw makeOsException(GetLastError(),"CFileAsyncIO::close");
    }
}

offset_t CFileAsyncIO::size()
{
    LARGE_INTEGER pos;
    pos.LowPart = GetFileSize(file, (unsigned long *)&pos.HighPart);
    if (pos.LowPart==-1) {
        DWORD err = GetLastError();
        if (err!=0)     
            throw makeOsException(GetLastError(),"CFileAsyncIO::size");
    }
    return pos.QuadPart;
}

size32_t CFileAsyncIO::read(offset_t pos, size32_t len, void * data)
{
    Owned<IFileAsyncResult> res = readAsync(pos,len,data);
    size32_t ret;
    res->getResult(ret,true);
    return ret;
}

size32_t CFileAsyncIO::write(offset_t pos, size32_t len, const void * data)
{
    Owned<IFileAsyncResult> res = writeAsync(pos,len,data);
    size32_t ret;
    res->getResult(ret,true);
    return ret;
}

void CFileAsyncIO::setSize(offset_t size)
{
    LARGE_INTEGER tempPos;
    tempPos.QuadPart = size; 
    tempPos.LowPart = SetFilePointer(file, tempPos.LowPart, &tempPos.HighPart, FILE_BEGIN);
    if (!SetEndOfFile(file))
        throw makeOsException(GetLastError(), "CFileIO::setSize");
}


IFileAsyncResult *CFileAsyncIO::readAsync(offset_t pos, size32_t len, void * data)
{
    CFileAsyncResult *res = new CFileAsyncResult(pos,0);
    DWORD val;
    if (ReadFile(file,data,len,&val,&res->overlapped) == 0) {
        int err = GetLastError();
        if (err == ERROR_HANDLE_EOF) { // bit weird
            res->value = 0; // won't need to wait
        }
        else if (err == ERROR_IO_PENDING) {
            CriticalBlock block(cs);
            res->parent = this;
            results.append(*res);
        }
        else
            throw makeOsException(GetLastError(),"CFileIO::readAsync");
    }   
    else {
        res->value = val; // won't need to wait
    }
    return res;
}

IFileAsyncResult *CFileAsyncIO::writeAsync(offset_t pos, size32_t len, const void * data)
{
    CFileAsyncResult *res = new CFileAsyncResult(pos,len);
    DWORD val;
    if (WriteFile(file,data,len,&val,&res->overlapped) == 0) {
        int err = GetLastError();
        if (err != ERROR_IO_PENDING)
            throw makeOsException(GetLastError(),"CFileIO::writeAsync");
        CriticalBlock block(cs);
        res->parent = this;
        results.append(*res);
    }   
    else {
        res->value = val; // wont need to wait
    }
    return res;
}



#else

//-- Unix implementation ----------------------------------------------------

class CFileAsyncResult: implements IFileAsyncResult, public CInterface
{
protected: 
    friend class CFileAsyncIO;

    DWORD value;
    DWORD wrsize;

    aiocb cb;

public:
    IMPLEMENT_IINTERFACE;


    CFileAsyncResult(offset_t offset,size32_t _wrsize)
    {
        value = (size32_t)-1;
        wrsize = _wrsize;
    }

    ~CFileAsyncResult()
    {
        size32_t dummy;
        getResult(dummy,true);
    }

    bool getResult(size32_t &ret,bool wait)
    {
#if defined(EMSCRIPTEN)
        throw makeErrnoException(ECANCELED, "TODO:  Add EMSCRIPTEN support");
#else
        if (value==(size32_t)-1) {
            for (;;) {
                int aio_errno = aio_error(&cb);
                if (aio_errno==ECANCELED)
                    return false;
                if (aio_errno != EINPROGRESS)
                {
                    if (aio_errno)
                        throw makeErrnoException(aio_errno, "CFileAsyncResult::getResult");
                    value = aio_return(&cb);
                    if (value<wrsize)
                        throw makeOsException(DISK_FULL_EXCEPTION_CODE, "CFileAsyncResult::getResult");
                    break;
                }
                if (!wait)
                    return false;
                for (;;) {
                    struct timespec timeout;
                    timeout.tv_sec = 60*60*24;  // a long time
                    timeout.tv_nsec = 0;
                    aiocb * cb_list[1];
                    cb_list[0] = &cb;
                    int rc = aio_suspend(cb_list, 1, &timeout);
                    if ((rc == 0)||(errno != EAGAIN))
                        break;
                    if (rc==ECANCELED)
                        return false;
                }
            }
        }
        ret = value;
#endif
        return true;
    }
};


CFileAsyncIO::CFileAsyncIO(HANDLE handle, IFSHmode _sharemode)
{
    assertex(handle != NULLFILE);
    throwOnError = false;
    file = handle;
    sharemode = _sharemode;
}


void CFileAsyncIO::close()
{
    if (file != NULLFILE)
    {
        // ensure file handle is cleared before throwing exception, to avoid dtor recalling close() as stack unwinds.
        HANDLE tmpHandle = NULLFILE;
        std::swap(tmpHandle, file);

#if defined(EMSCRIPTEN)
        throw makeErrnoException(ECANCELED, "TODO:  Add EMSCRIPTEN support");
#else
        aio_cancel(tmpHandle, NULL);
#endif
        if (_lclose(tmpHandle) < 0)
            throw makeErrnoException(errno, "CFileAsyncIO::close");
    }
}

offset_t CFileAsyncIO::size()
{
    CriticalBlock procedure(cs);
    offset_t savedPos = _llseek(file,0,SEEK_CUR);
    offset_t length = _llseek(file,0,SEEK_END);
    _llseek(file, savedPos, SEEK_SET);
    return length;
}

size32_t CFileAsyncIO::read(offset_t pos, size32_t len, void * data)
{
    CriticalBlock procedure(cs);
    _llseek(file,pos,SEEK_SET);
    size32_t ret = _lread(file,data,len);
    if (ret==(size32_t)-1)
        throw makeErrnoException(errno, "CFileAsyncIO::read");
    return ret;

}

size32_t CFileAsyncIO::write(offset_t pos, size32_t len, const void * data)
{
    CriticalBlock procedure(cs);
    _llseek(file,pos,SEEK_SET);
    size32_t ret = _lwrite(file,data,len);
    if (ret==(size32_t)-1)
        throw makeErrnoException(errno, "CFileAsyncIO::write");
    return ret;
}

void CFileAsyncIO::setSize(offset_t pos)
{
    CriticalBlock procedure(cs);
    if ((file != NULLFILE)&&(0 != ftruncate(file, pos)))
        throw makeErrnoException(errno, "CFileIO::setSize");
}

IFileAsyncResult *CFileAsyncIO::readAsync(offset_t pos, size32_t len, void * data)
{
    CFileAsyncResult *res = new CFileAsyncResult(pos,0);
    
    bzero( &(res->cb), sizeof (struct aiocb));
    res->cb.aio_fildes = file;
    res->cb.aio_offset = pos;
    res->cb.aio_buf = data;
    res->cb.aio_nbytes = len;
    res->cb.aio_sigevent.sigev_notify = SIGEV_NONE;

#if defined(EMSCRIPTEN)
    throw makeErrnoException(ECANCELED, "TODO:  Add EMSCRIPTEN support");
#else

    int retval = aio_read(&(res->cb));
    if (retval==-1)
        throw makeErrnoException(errno, "CFileAsyncIO::readAsync");
#endif
    return res;
}

IFileAsyncResult *CFileAsyncIO::writeAsync(offset_t pos, size32_t len, const void * data)
{
    CFileAsyncResult *res = new CFileAsyncResult(pos,len);

    bzero( &(res->cb), sizeof (struct aiocb));
    res->cb.aio_fildes = file;
    res->cb.aio_offset = pos;
    res->cb.aio_buf = (void*)data;
    res->cb.aio_nbytes = len;
    res->cb.aio_sigevent.sigev_signo = SIGUSR1;
    res->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
    res->cb.aio_sigevent.sigev_value.sival_ptr = (void*)res;

#if defined(EMSCRIPTEN)
    throw makeErrnoException(ECANCELED, "TODO:  Add EMSCRIPTEN support");
#else

    int retval = aio_write(&(res->cb));
    if (retval==-1)
        throw makeErrnoException(errno, "CFileAsyncIO::writeAsync");
#endif
    return res;
}


#endif

//---------------------------------------------------------------------------

CFileIOStream::CFileIOStream(IFileIO * _io)
{
    io.set(_io);
    curOffset = 0;
}


void CFileIOStream::flush()
{
    io->flush();
}


size32_t CFileIOStream::read(size32_t len, void * data)
{
    size32_t numRead = io->read(curOffset, len, data);
    curOffset += numRead;
    return numRead;
}

void CFileIOStream::seek(offset_t pos, IFSmode origin)
{
    switch (origin)
    {
    case IFScurrent:
        curOffset += pos;
        break;
    case IFSend:
        curOffset = io->size() + pos;
        break;
    case IFSbegin:
        curOffset = pos;
        break;
    }
}

offset_t CFileIOStream::size()
{
    return io->size();
}

offset_t CFileIOStream::tell()
{
    return curOffset;
}

size32_t CFileIOStream::write(size32_t len, const void * data)
{
    size32_t numWritten = io->write(curOffset, len, data);
    curOffset += numWritten;
    return numWritten;
}



//---------------------------------------------------------------------------

CNoSeekFileIOStream::CNoSeekFileIOStream(IFileIOStream * _stream) : stream(_stream)
{
}


void CNoSeekFileIOStream::flush()
{
    stream->flush();
}


size32_t CNoSeekFileIOStream::read(size32_t len, void * data)
{
    return stream->read(len, data);
}

void CNoSeekFileIOStream::seek(offset_t pos, IFSmode origin)
{
    offset_t prevOffset = stream->tell();
    offset_t nextOffset = 0;
    switch (origin)
    {
    case IFScurrent:
        nextOffset = prevOffset + pos;
        break;
    case IFSend:
        nextOffset = stream->size() + pos;
        break;
    case IFSbegin:
        nextOffset = pos;
        break;
    }
    if (prevOffset != nextOffset)
        throw makeStringExceptionV(0, "Seek on non-seekable CFileIOStream (from %" I64F "u to %" I64F "u)", prevOffset, nextOffset);

    //No need to call stream->seek since it will have no effect
}

offset_t CNoSeekFileIOStream::size()
{
    return stream->size();
}

offset_t CNoSeekFileIOStream::tell()
{
    return stream->tell();
}

size32_t CNoSeekFileIOStream::write(size32_t len, const void * data)
{
    return stream->write(len, data);
}



//---------------------------------------------------------------------------


class CBufferedFileIOStreamBase : public CBufferedIOStreamBase, implements IFileIOStream
{
protected:
    virtual offset_t directSize() = 0;
    offset_t                curOffset;

public:
    IMPLEMENT_IINTERFACE;

    CBufferedFileIOStreamBase(unsigned bufSize) : CBufferedIOStreamBase(bufSize), curOffset(0) { }

    virtual void flush() { doflush(); }




    void seek(offset_t pos, IFSmode origin)
    {
        offset_t newOffset = 0;
        switch (origin)
        {
        case IFScurrent:
            newOffset = tell() + pos;
            break;
        case IFSend:
            newOffset = size() + pos;
            break;
        case IFSbegin:
            newOffset = pos;
            break;
        default:
            throwUnexpected();
        }

        if (reading)
        {
            // slightly weird but curoffset is end of buffer when reading
            if ((newOffset >= curOffset-numInBuffer) && (newOffset <= curOffset))
            {
                curBufferOffset = (size32_t)(newOffset - (curOffset-numInBuffer));
                return;
            }
        }
        else
        {
            if ((newOffset >= curOffset) && (newOffset <= curOffset + numInBuffer))
            {
                curBufferOffset = (size32_t)(newOffset - curOffset);
                return;
            }
            flush();
        }

        curOffset = newOffset;
        numInBuffer = 0;
        curBufferOffset = 0;
    }

    offset_t size()
    {
        offset_t curSize = directSize();
        if (!reading)
            curSize = std::max(curSize, curOffset + numInBuffer);
        return curSize;
    }

    offset_t tell()
    {
        if (reading)
            return curOffset - numInBuffer + curBufferOffset;
        return curOffset + curBufferOffset;
    }

    size32_t read(size32_t len, void * data)
    {
        return CBufferedIOStreamBase::doread(len, data);
    }

    size32_t write(size32_t len, const void * data)
    {
        if (reading)
            curOffset -= bytesRemaining();

        return CBufferedIOStreamBase::dowrite(len, data);
    }


};


class CBufferedFileIOStream : public CBufferedFileIOStreamBase
{
public:
    CBufferedFileIOStream(IFileIO * _io, unsigned _bufferSize) : CBufferedFileIOStreamBase(_bufferSize), io(_io)
    {
        buffer = new byte[_bufferSize];
    }
    ~CBufferedFileIOStream()
    {
        flush();
        delete [] buffer;
    }

protected:
    virtual void doflush()
    {
        if (!reading && numInBuffer)
        {
            try {
                io->write(curOffset, numInBuffer, buffer);
            }
            catch (IException *) {
                // if we get exception, clear buffer so doen't reoccur on destructor as well
                numInBuffer = 0;
                curBufferOffset = 0;
                throw;
            }
            curOffset += curBufferOffset;
            numInBuffer = 0;
            curBufferOffset = 0;
        }
    }
    virtual bool fillBuffer()
    {
        reading = true;
        numInBuffer = io->read(curOffset, bufferSize, buffer);
        curOffset += numInBuffer;
        curBufferOffset = 0;
        return numInBuffer!=0;
    }
    virtual size32_t directRead(size32_t len, void * data)
    {
        size32_t sz = io->read(curOffset,len,data);
        curOffset += sz;
        return sz;
    }
    virtual size32_t directWrite(size32_t len, const void * data)
    {
        size32_t sz = io->write(curOffset,len,data);
        curOffset += sz;
        return sz;
    }
    virtual offset_t directSize() { return io->size(); }
    virtual unsigned __int64 getStatistic(StatisticKind kind)
    {
        return io->getStatistic(kind);
    }
    virtual void close() override
    {
        flush();
        io->close();
    }
protected:
    IFileIOAttr             io;
};


//---------------------------------------------------------------------------

class CBufferedAsyncIOStream: public CBufferedFileIOStreamBase
{
    Linked<IFileAsyncIO>    io;
    byte *                  blk1;
    byte *                  blk2;
    IFileAsyncResult        *readasyncres;
    IFileAsyncResult        *writeasyncres;
    bool                    readeof;
public:
    CBufferedAsyncIOStream(IFileAsyncIO * _io, size32_t _bufferSize)
        : CBufferedFileIOStreamBase(_bufferSize/2), io(_io)
    {
        blk1 = new byte[bufferSize];
        blk2 = new byte[bufferSize];
        buffer = blk1;
        readasyncres = NULL;
        writeasyncres = NULL;
        readeof = false;
        minDirectSize = (size32_t)-1; // async always writes using buffer
    }

    ~CBufferedAsyncIOStream()
    {
        flush();
        waitAsyncWrite();
        waitAsyncRead();
        delete [] blk1;
        delete [] blk2;
    }

    void waitAsyncWrite()
    {
        if (writeasyncres) {
            size32_t res;
            writeasyncres->getResult(res,true);
            writeasyncres->Release();
            writeasyncres = NULL;
        }
    }

    size32_t waitAsyncRead()
    {
        size32_t res = 0;
        if (readasyncres) {
            readasyncres->getResult(res,true);
            readasyncres->Release();
            readasyncres = NULL;
        }
        return res;
    }

    void primeAsyncRead(offset_t pos,size32_t size, void *dst)
    {
        assertex(!readasyncres);
        readasyncres = io->readAsync(pos, size, dst);
    }

    void primeAsyncWrite(offset_t pos,size32_t size, const void *src)
    {
        assertex(!writeasyncres);
        writeasyncres = io->writeAsync(pos, size, src);
    }

// CBufferedFileIOStream overloads
    virtual bool fillBuffer()
    {
        if (!reading) {
            waitAsyncWrite();
            reading = true;
        }
        if (readeof) {
            numInBuffer = 0;
            curBufferOffset = 0;
            return false;
        }
        buffer=(buffer==blk1)?blk2:blk1;
        if (readasyncres==NULL)                             // first time
            primeAsyncRead(curOffset, bufferSize, buffer);
        numInBuffer = waitAsyncRead();
        curOffset += numInBuffer;
        curBufferOffset = 0;
        if (numInBuffer)
            primeAsyncRead(curOffset, bufferSize, (buffer==blk1)?blk2:blk1);
        else
            readeof = true;
        return !readeof;
    }
    virtual void doflush()
    {
        if (!reading && numInBuffer)
        {
            waitAsyncWrite();
            primeAsyncWrite(curOffset, numInBuffer, buffer);
            buffer=(buffer==blk1)?blk2:blk1;
            curOffset += curBufferOffset;
            numInBuffer = 0;
            curBufferOffset = 0;
        }
    }
    virtual size32_t directRead(size32_t len, void * data) { assertex(false); return 0; }           // shouldn't get called
    virtual size32_t directWrite(size32_t len, const void * data) { assertex(false); return 0; }    // shouldn't get called
    virtual offset_t directSize() { waitAsyncWrite(); return io->size(); }
    virtual unsigned __int64 getStatistic(StatisticKind kind) { return io->getStatistic(kind); }
    virtual void close() override
    {
        flush();
        waitAsyncWrite();
        waitAsyncRead();
        io->close();
    }
};


//-- Helper routines --------------------------------------------------------

enum GblFlushEnum { FLUSH_INIT, FLUSH_DISALLOWED, FLUSH_ALLOWED };
static GblFlushEnum gbl_flush_allowed = FLUSH_INIT;
static CriticalSection flushsect;

static inline bool isPCFlushAllowed()
{
    CriticalBlock block(flushsect);
    if (gbl_flush_allowed == FLUSH_INIT)
    {
        gbl_flush_allowed = FLUSH_DISALLOWED;
        if (isContainerized())
        {
            if (getConfigBool("expert/@allowPGCacheFlush", true))
                gbl_flush_allowed = FLUSH_ALLOWED;
        }
        else if (queryEnvironmentConf().getPropBool("allow_pgcache_flush", true))
            gbl_flush_allowed = FLUSH_ALLOWED;
    }
    if (gbl_flush_allowed == FLUSH_ALLOWED)
        return true;
    return false;
}

//-- Helper routines --------------------------------------------------------

size32_t read(IFileIO * in, offset_t pos, size32_t len, MemoryBuffer & buffer)
{
    // it's assumed if len is specified the caller knows what they're doing,
    // and we don't want to wastefully call size() to check otherwise.
    if ((size32_t)-1 == len)
    {
        //Don't allocate a stupid amount of memory....
        offset_t fileLength = in->size();
        if (pos > fileLength)
            pos = fileLength;
        if ((len == (size32_t)-1) || (pos + len > fileLength))
            len = (size32_t)(fileLength - pos);
    }
    void * data = buffer.reserve(len);
    size32_t lenRead = in->read(pos, len, data);
    if (lenRead != len)
        buffer.rewrite(buffer.length() - (len - lenRead));
    return lenRead;
}

void renameFile(const char *target, const char *source, bool overwritetarget)
{
    OwnedIFile src = createIFile(source);
    if (!src)
        throw MakeStringException(-1, "renameFile: source '%s' not found", source);
    if (src->isFile()!=fileBool::foundYes)
        throw MakeStringException(-1, "renameFile: source '%s' is not a valid file", source);
    if (src->isReadOnly()!=fileBool::foundNo)
        throw MakeStringException(-1, "renameFile: source '%s' is readonly", source);

    OwnedIFile tgt = createIFile(target);
    if (!tgt)
        throw MakeStringException(-1, "renameFile: target path '%s' could not be created", target);
    if (tgt->exists() && !overwritetarget)
        throw MakeStringException(-1, "renameFile: target file already exists: '%s' will not overwrite", target);

    src->rename(target);
}

void copyFile(const char *target, const char *source, size32_t buffersize, ICopyFileProgress *progress, CFflags copyFlags)
{
    OwnedIFile src = createIFile(source);
    if (!src)
        throw MakeStringException(-1, "copyFile: source '%s' not found", source);
    OwnedIFile tgt = createIFile(target);
    if (!tgt)
        throw MakeStringException(-1, "copyFile: target path '%s' could not be created", target);
    copyFile(tgt, src, buffersize, progress, copyFlags);
}

void copyFile(IFile * target, IFile * source, size32_t buffersize, ICopyFileProgress *progress, CFflags copyFlags)
{
    source->copyTo(target, buffersize, progress, false, copyFlags);
}

void doCopyFile(IFile * target, IFile * source, size32_t buffersize, ICopyFileProgress *progress, ICopyFileIntercept *copyintercept, bool usetmp, CFflags copyFlags)
{
    if (!buffersize)
        buffersize = DEFAULT_COPY_BLKSIZE;
#ifdef _WIN32
    if (!usetmp) { 
        CFile *src = QUERYINTERFACE(source,CFile);
        CFile *dst = QUERYINTERFACE(target,CFile);
        if (src) {
            if (dst) {
                if (src->fastCopyFile(*dst, buffersize, progress))
                    return;
            }
            CWindowsRemoteFile *dst2 = QUERYINTERFACE(target,CWindowsRemoteFile);
            if (dst2) {
                if (dst2->fastCopyFileRev(*src, buffersize, progress))
                    return;
            }
        }
        CWindowsRemoteFile *src2 = QUERYINTERFACE(source,CWindowsRemoteFile);
        if (src2) {
            if (dst) {
                if (src2->fastCopyFile(*dst,buffersize, progress))
                    return;
            }
            CWindowsRemoteFile *dst2 = QUERYINTERFACE(target,CWindowsRemoteFile);
            if (dst2) {
                if (src2->fastCopyFile(*dst2, buffersize, progress))
                    return;
            }
        }
    }
#endif
    IFEflags srcFlags = IFEnone;
    if (copyFlags & CFflush_read)
        srcFlags = IFEnocache;
    OwnedIFileIO sourceIO = source->open(IFOread, srcFlags);
    if (!sourceIO)
        throw MakeStringException(-1, "copyFile: source '%s' not found", source->queryFilename());

#ifdef __linux__

    // this is not really needed in windows - if it is we will have to
    // test the file extenstion - .exe, .bat
    
    struct stat info;
    if (stat(source->queryFilename(), &info) == 0)  // cannot fail - exception would have been thrown above
        target->setCreateFlags(info.st_mode&(S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH|S_IXUSR|S_IXGRP|S_IXOTH));
#endif
    Owned<IFileIO> targetIO;
    Owned<IFile> tmpfile;
    IFile *dest;
    if (usetmp) {
        StringBuffer tmpname;
        makeTempCopyName(tmpname,target->queryFilename());
        tmpfile.setown(createIFile(tmpname.str()));
        dest = tmpfile;
    }
    else
        dest = target;
    IFEflags tgtFlags = IFEnone;
    if (copyFlags & CFflush_write)
        tgtFlags = IFEnocache;
    targetIO.setown(dest->open(IFOcreate, tgtFlags));
    if (!targetIO)
        throw MakeStringException(-1, "copyFile: target path '%s' could not be created", dest->queryFilename());
    MemoryAttr mb;
    void * buffer = copyintercept?NULL:mb.allocate(buffersize);
    
    offset_t offset = 0;
    offset_t total = 0;
    Owned<IException> exc;
    try
    {
        if (progress)
            total = sourceIO->size(); // only needed for progress
        for (;;)
        {
            size32_t got;
            if (copyintercept) {
                got = (size32_t)copyintercept->copy(sourceIO,targetIO,offset,buffersize);
                if (got == 0)
                    break;
            }
            else {
                got = sourceIO->read(offset, buffersize, buffer);
                if (got == 0)
                    break;
                targetIO->write(offset, got, buffer);
            }
            offset += got;
            if (progress && progress->onProgress(offset, total) != CFPcontinue)
                break;
        }
        targetIO->close(); // Ensure errors are reported.
        targetIO.clear();
        if (usetmp) {
            StringAttr tail(pathTail(target->queryFilename()));
            target->remove();
            dest->rename(tail);
        }
    }
    catch (IException *e)
    {
        // try to delete partial copy
        StringBuffer s;
        s.append("copyFile target=").append(dest->queryFilename()).append(" source=").append(source->queryFilename()).appendf("; read/write failure (%d): ",e->errorCode());
        e->errorMessage(s);
        exc.setown(makeStringException(e->errorCode(), s.str()));
        e->Release();
        EXCLOG(exc, "doCopyFile");
    }
    if (exc.get()) {
        try {
            sourceIO.clear();
        }
        catch (IException *e) { 
            EXCLOG(e, "doCopyFile closing source"); 
            e->Release();
        }
        try {
            targetIO.clear();
        }
        catch (IException *e) { 
            EXCLOG(e, "doCopyFile closing dest"); 
            e->Release();
        }
        try { 
            dest->remove(); 
        } 
        catch (IException *e) { 
            StringBuffer s;
            EXCLOG(e, s.clear().append("Removing partial copy file: ").append(dest->queryFilename()).str()); 
            e->Release();
        }
        throw exc.getClear();
    }
    CDateTime createTime, modifiedTime;
    if (source->getTime(&createTime, &modifiedTime, NULL))
        target->setTime(&createTime, &modifiedTime, NULL);
}



void makeTempCopyName(StringBuffer &tmpname,const char *destname)
{
    // simple for the moment (maybe used uid later)
    tmpname.append(destname).append("__");
#ifdef _WIN32
    genUUID(tmpname,true);
#else
    genUUID(tmpname);
#endif
    tmpname.append(".tmp");
}


//---------------------------------------------------------------------------

#ifndef _WIN32
/// This code is dangerous - pass it arrays smaller than _MAX_xxx and it will write off the end even if you KNOW your strings would fit
// Should NOT use strncpy here

void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext)
{
    strncpy(dir, path, (_MAX_DIR-1));

    char* last = strrchr(dir, '.');
    if (last != NULL)
    {
        if (strrchr(last, PATHSEPCHAR)==NULL)
        {
            strncpy(ext, last, (_MAX_EXT-1));
            *last = '\0';
        }
        else
        {
            *ext = '\0';
        }
    }
    else
    {
        *ext = '\0';
    }

    last = strrchr(dir, PATHSEPCHAR);
    if (last != NULL)
    {
        strncpy(fname, ++last, (_MAX_FNAME-1));
        *last = '\0';
    }
    else
    {
        strncpy(fname, dir, (_MAX_FNAME-1));
        *dir = '\0';
    }

    *drive = '\0';
}

#endif


void splitFilename(const char * filename, StringBuffer * drive, StringBuffer * path, StringBuffer * tail, StringBuffer * ext, bool longExt)
{
    char tdrive[_MAX_DRIVE];
    char tdir[_MAX_DIR];
    char ttail[_MAX_FNAME];
    char text[_MAX_EXT];

    ::_splitpath(filename, tdrive, tdir, ttail, text);
    char *longExtStart = longExt ? strchr(ttail, '.') : NULL;
    if (drive)
        drive->append(tdrive);
    if (path)
        path->append(tdir);
    if (tail)
        tail->append(longExtStart ? longExtStart-ttail : strlen(ttail), ttail);
    if (ext)
    {
        if (longExtStart)
            ext->append(longExtStart);
        ext->append(text);
    }
}


StringBuffer &createUNCFilename(const char * filename, StringBuffer &UNC, bool useHostNames)
{
    char buf[255];
#ifdef _WIN32
    char *dummy;
    GetFullPathName(filename, sizeof(buf), buf, &dummy);
    if (buf[1]==':')
    {
        // MORE - assumes it's a local drive not a mapped one
        UNC.append("\\\\");
        if (useHostNames)
            UNC.append(GetCachedHostName());
        else
            queryHostIP().getHostText(UNC);
        UNC.append("\\").append((char)tolower(buf[0])).append(getShareChar()).append(buf+2);
    }
    else 
    {
        assertex(buf[0]=='\\' && buf[1]=='\\');
        UNC.append(buf);
    }
    return UNC;
#else
    if (filename[0]=='/' && filename[1]=='/')
        UNC.append(filename);
    else
    {
        UNC.append("//");
        if (useHostNames)
            UNC.append(GetCachedHostName());
        else
            queryHostIP().getHostText(UNC);

        if (*filename != '/')
        {
            if (!GetCurrentDirectory(sizeof(buf), buf)) {
                IERRLOG("createUNCFilename: Current directory path too big, bailing out");
                throwUnexpected();
            }
            UNC.append(buf).append("/");
        }
        UNC.append(filename);
    }
    return UNC;
#endif
}

bool splitUNCFilename(const char * filename, StringBuffer * machine, StringBuffer * path, StringBuffer * tail, StringBuffer * ext)
{
    if (!filename || !isPathSepChar(filename[0]) || !isPathSepChar(filename[1]))
        return false;
    const char * cur = filename+2;
    while (*cur && !isPathSepChar(*cur))
        cur++;
    if (!*cur)
        return false;

    const char * startPath = cur;
    const char * lastExt = NULL;
    const char * startTail = NULL;
    char next;
    while ((next = *cur) != 0)
    {
        if (isPathSepChar(next))
        {
            lastExt = NULL;
            startTail = cur+1;
        }
        else if (next == '.')
            lastExt = cur;
        cur++;
    }
    assertex(startTail);
    if (machine)
        machine->append(startPath-filename, filename);
    if (path)
        path->append(startTail - startPath, startPath);
    if (lastExt)
    {
        if (tail)
            tail->append(lastExt - startTail, startTail);
        if (ext)
            ext->append(lastExt);
    }
    else
    {
        if (tail)
            tail->append(startTail);
    }
    return true;
}

/** 
 *  Ensure the filename has desired extension.
 *  If it has no extension, add the desired extension, return true.
 *  If it has an extension different from the desiredExtension, return false.
 *  Otherwise, return true.
 */
bool ensureFileExtension(StringBuffer& filename, const char* desiredExtension)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    
    _splitpath(filename.str(), drive, dir, fname, ext);
    if (ext[0]==0)
    {
        filename.append(desiredExtension);
        return true;
    }

    if (strcmp(ext,desiredExtension)!=0)
        return false;

    return true;
}

/* Get full file name. If noExtension is true, the extension (if any) will be trimmed */
StringBuffer& getFullFileName(StringBuffer& filename, bool noExtension)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    
    _splitpath(filename.str(), drive, dir, fname, ext);
    
    filename.clear();
    filename.append(drive).append(dir).append(fname);
    if (!noExtension)
        filename.append(ext);

    return filename;
}

/* Get the file name only. If noExtension is true, the extension (if any) will be trimmed */ 
StringBuffer& getFileNameOnly(StringBuffer& filename, bool noExtension)
{
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    
    _splitpath(filename.str(), drive, dir, fname, ext);
    
    filename.clear();
    filename.append(fname);
    if (!noExtension)
        filename.append(ext);

    return filename;
}

//---------------------------------------------------------------------------


class CNullDirectoryIterator : implements IDirectoryIterator, public CInterface
{
public:
    IMPLEMENT_IINTERFACE;

    bool  first()
    {
        return false;
    }

    bool  next()
    {
        return false;
    }

    StringBuffer &getName(StringBuffer &buf)
    {
        return buf;
    }

    __int64 getFileSize()
    {
        return -1;
    }


    bool getModifiedTime(CDateTime &ret)
    {
        return false;
    }

    virtual bool isValid()  { return false; }
    virtual IFile & query() { throwUnexpected(); }
    virtual bool isDir() {  return false; }

};

extern jlib_decl IDirectoryIterator *createNullDirectoryIterator()
{
    return new CNullDirectoryIterator;
}

class CDirectoryIterator : implements IDirectoryIterator, public CInterface
{
public:
    CDirectoryIterator(const char * _path, const char * _mask, bool _sub, bool _includedir)
    {
        StringBuffer tmp;
        if (!_path || !*_path) 
            _path = "." PATHSEPSTR;
        else if (_path[strlen(_path)-1] != PATHSEPCHAR)
            _path = tmp.append(_path).append(PATHSEPCHAR);
        path.set(_path);
        mask.set(_mask);
        sub = _sub;
        includedir = _includedir;
        subidx = 0;
        curisdir = false;
    }

    IMPLEMENT_IINTERFACE

    virtual bool first()=0;
    virtual bool next()=0;
    virtual bool isValid()  { return cur != NULL; }
    virtual IFile & query() { return *cur; }
    virtual StringBuffer &getName(StringBuffer &buf)=0;
    virtual bool isDir() {  return curisdir; }
protected:  
    Owned<IFile>    cur;
    bool            curisdir;
    StringAttr      path;
    StringAttr      mask;
    bool            includedir;
    bool            sub;    // TBD
    StringAttrArray subpaths;
    unsigned        subidx; // -1 to index subpaths
};

#ifdef _WIN32

class CWindowsDirectoryIterator : public CDirectoryIterator
{
    WIN32_FIND_DATA info;
    HANDLE          handle;


    bool setCurrent()
    {
        if (strcmp(info.cFileName, ".") == 0 || strcmp(info.cFileName, "..") == 0)
            return false;
        bool match = (!mask.length() || WildMatch(info.cFileName, mask, true));
        curisdir = (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if (!match&&!curisdir)
            return false;
        StringBuffer f(path);
        if (subidx) 
            f.append(subpaths.item(subidx-1).text).append('\\');
        f.append(info.cFileName);
        if (curisdir) {
            if (sub) {
                const char *s = f.str()+path.length();
                unsigned i;
                for (i=0;i<subpaths.ordinality();i++)
                    if (stricmp(subpaths.item(i).text,s)==0)
                        break;
                if (i==subpaths.ordinality())
                    subpaths.append(*new StringAttrItem(s));
            }
            if (!includedir)
                return false;
        }
        if (!match)
            return false;
        cur.setown(createIFile(f.str()));
        return true;
    }

    bool open()
    {
        close();
        while (subidx<=subpaths.ordinality()) {
            StringBuffer location(path);
            if (subidx)
                location.append(subpaths.item(subidx-1).text).append('\\');
            location.append("*");
            handle = FindFirstFile(location.str(), &info);
            if (handle != INVALID_HANDLE_VALUE)
                return true;
            subidx++;
        }
        return false;
    }

    void close()
    {
        cur.clear();
        if (handle != INVALID_HANDLE_VALUE) {
            FindClose(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };


public:
    CWindowsDirectoryIterator(const char * _path, const char * _mask, bool _sub, bool _includedir)
        : CDirectoryIterator(_path,_mask,_sub,_includedir)
    {
        handle = INVALID_HANDLE_VALUE;
    }

    ~CWindowsDirectoryIterator()
    {
        close();
    }


    bool first()
    {
        subpaths.kill();
        subidx = 0;
        if (!open())
            return false;
        if (setCurrent())
            return true;
        return next();
    }

    bool next()
    {
        for (;;) {
            for (;;) {
                if (!FindNextFile(handle, &info))
                    break;
                if (setCurrent())
                    return true;
            }
            subidx++;
            if (!open())
                break;
            if (setCurrent())
                return true;
        }
        return false;
    }

    StringBuffer &getName(StringBuffer &buf)
    {
        if (subidx)
            buf.append(subpaths.item(subidx-1).text).append('\\');
        return buf.append(info.cFileName);
    }


    __int64 getFileSize()
    {
        if (curisdir)
            return -1;
        LARGE_INTEGER x;
        x.LowPart = info.nFileSizeLow;
        x.HighPart = info.nFileSizeHigh;
        return x.QuadPart;
    }


    bool getModifiedTime(CDateTime &ret)
    {
        FILETIMEtoIDateTime(&ret, info.ftLastWriteTime);
        return true;
    }


};


IDirectoryIterator * createDirectoryIterator(const char * path, const char * mask, bool sub, bool includedirs)
{
    if (mask&&!*mask)   // only NULL is wild
        return new CNullDirectoryIterator;
    if (!path || !*path) // cur directory so no point in checking for remote etc.
        return new CWindowsDirectoryIterator(path, mask,sub,includedirs);
    OwnedIFile iFile = createIFile(path);
    if (!iFile||(iFile->isDirectory()!=fileBool::foundYes))
        return new CNullDirectoryIterator;
    return iFile->directoryFiles(mask, sub, includedirs);
}

IDirectoryIterator *CFile::directoryFiles(const char *mask,bool sub,bool includedirs)
{
    if ((mask&&!*mask)||    // only NULL is wild
        (isDirectory()!=fileBool::foundYes))
        return new CNullDirectoryIterator;
    return new CWindowsDirectoryIterator(filename, mask,sub,includedirs);
}

bool CFile::getInfo(bool &isdir,offset_t &size,CDateTime &modtime)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesEx(filename, GetFileExInfoStandard, &info) != 0) {
        LARGE_INTEGER x;
        x.LowPart = info.nFileSizeLow;
        x.HighPart = info.nFileSizeHigh;
        size = (offset_t)x.QuadPart;
        isdir = (info.dwFileAttributes != (DWORD)-1)&&(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        FILETIMEtoIDateTime(&modtime, info.ftLastWriteTime);
        return true;
    }
    size = 0;
    modtime.clear();
    isdir = false;
    return false;
}


#else

class CLinuxDirectoryIterator : public CDirectoryIterator
{
    StringAttr      tail;
    DIR *           handle;
    struct stat     st;
    bool            gotst;
    CriticalSection sect;

    bool loadst()
    {
        if (!gotst&&cur)
            gotst = (stat(cur->queryFilename(), &st) == 0); // prob should use safeStat, but leaving/being conservative for now
        return gotst;
    }
    
    
public:
    CLinuxDirectoryIterator(const char * _path, const char * _mask, bool _sub,bool _includedir)
        : CDirectoryIterator(_path,_mask,_sub,_includedir)
    {
        handle = NULL;
        gotst = false;
    }

    ~CLinuxDirectoryIterator()
    {
        close();
    }

    bool open()
    {
        close();
        while (subidx<=subpaths.ordinality()) {
            StringBuffer location(path);
            if (subidx)
                location.append(subpaths.item(subidx-1).text);
            // not sure if should remove trailing '/'  
            handle = ::opendir(location.str());
            // better error handling here?
            if (handle)
                return true;
            subidx++;
        }
        return false;
    }

    void close()
    {
        cur.clear();
        if (handle) {
            closedir(handle);
            handle = NULL;
        }
    }

    bool first()
    {
        subpaths.kill();
        subidx = 0;
        if (open()) 
            return next();
        return false;
    }

    bool next()
    {
        for (;;) {
            struct dirent *entry;
            for (;;) {
                gotst = false;
                CriticalBlock b(sect);
                entry = readdir(handle);
                // need better checking here?
                if (!entry)
                    break;
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
                    continue;

                bool match = (!mask.length() || WildMatch(entry->d_name, mask, false));
                curisdir = (entry->d_type==DT_DIR);
                bool islnk = (entry->d_type==DT_LNK);
                bool isunknown = (entry->d_type==DT_UNKNOWN); // to work around xfs bug
                if (match||curisdir||islnk||isunknown) {
                    StringBuffer f(path);
                    if (subidx) 
                        f.append(subpaths.item(subidx-1).text).append('/');
                    f.append(entry->d_name);
                    if (islnk||isunknown) {
                        struct stat info;
                        if (stat(f.str(), &info) == 0)  // will follow link. Prob should use safeStat, but leaving/being conservative for now
                            curisdir = S_ISDIR(info.st_mode);
                        else
                            curisdir = false;
                    }
                    if (curisdir) {
                        if (sub) {
                            const char *s = f.str()+path.length();
                            unsigned i;
                            for (i=0;i<subpaths.ordinality();i++)
                                if (strcmp(subpaths.item(i).text,s)==0)
                                    break;
                            if (i==subpaths.ordinality())
                                subpaths.append(*new StringAttrItem(s));
                        }
                        if (!includedir)
                            match = false;
                    }
                    if (match) {
                        tail.set(entry->d_name);
                        cur.setown(createIFile(f.str()));
                        return true;
                    }
                }
            }
            subidx++;
            if (!open())
                break;
        }
        return false;
    }

    StringBuffer &getName(StringBuffer &buf)
    {
        if (subidx)
            buf.append(subpaths.item(subidx-1).text).append('/');
        return buf.append(tail);
    }

    __int64 getFileSize()
    {
        if (curisdir)
            return -1;
        if (!loadst())
            return -1;
        return st.st_size ;
    }


    bool getModifiedTime(CDateTime &ret)
    {
        if (!loadst())
            return false;
        timetToIDateTime(&ret,st.st_mtime);
        return true;
    }

};

IDirectoryIterator * createDirectoryIterator(const char * path, const char * mask, bool sub, bool includedirs)
{
    if (mask&&!*mask)   // only NULL is wild
        return new CNullDirectoryIterator;
    if (!path || !*path) // no point in checking for remote etc.
        return new CLinuxDirectoryIterator(path, mask,sub,includedirs);
    OwnedIFile iFile = createIFile(path);
    if (!iFile||(iFile->isDirectory()!=fileBool::foundYes))
        return new CNullDirectoryIterator;
    return iFile->directoryFiles(mask, sub, includedirs);
}

IDirectoryIterator *CFile::directoryFiles(const char *mask,bool sub,bool includedirs)
{
    if ((mask&&!*mask)||    // only NULL is wild
        (isDirectory()!=fileBool::foundYes))
        return new CNullDirectoryIterator;
    return new CLinuxDirectoryIterator(filename, mask,sub,includedirs);
}


bool CFile::getInfo(bool &isdir,offset_t &size,CDateTime &modtime)
{
    struct stat info;
    if (safeStat(filename, info)) {
        size = (offset_t)info.st_size;
        isdir = S_ISDIR(info.st_mode);
        timetToIDateTime(&modtime,  info.st_mtime);
        return true;
    }
    size = 0;
    modtime.clear();
    isdir = false;
    return false;
}


#endif


class CDirEntry: extends CInterface
{
public:
    StringAttr name;
    Owned<IFile> file;
    __int64 size;
    CDateTime modified;
    byte flags; // IDDI*
    bool isdir;

    CDirEntry(IDirectoryIterator *iter)
    {
        StringBuffer n;
        name.set(iter->getName(n).str());
        size = iter->getFileSize();
        iter->getModifiedTime(modified);
        file.set(&iter->query());
        flags = 0;
        isdir = iter->isDir();
    }

    bool match(byte f) 
    {
        return (flags==0)||((f&flags)!=0);
    }

    int compare(const CDirEntry *e)
    {
        return strcmp(name.get(),e->name.get());
    }

    int compareProp(const CDirEntry *e)
    {
        int c = compare(e);
        if (c)
            return c;
        if (isdir!=e->isdir)
            return isdir?-1:1;
        if (size!=e->size)
            return (size<e->size)?-1:1;
        return modified.compare(e->modified,false);
    }

};


class CDirectoryDifferenceIterator : public CIArrayOf<CDirEntry>, extends CInterface, implements IDirectoryDifferenceIterator
{

    
    
    static int compare(CInterface * const *_a, CInterface * const *_b)
    {
        CDirEntry *a = *(CDirEntry **)_a;
        CDirEntry *b = *(CDirEntry **)_b;
        return a->compare(b);
    }
    unsigned idx;
    byte mask;

public:
    IMPLEMENT_IINTERFACE;
    CDirectoryDifferenceIterator(IDirectoryIterator *iter, CDirectoryDifferenceIterator *cmp)
    {
        mask = IDDIstandard;
        idx = 0;
        ForEach(*iter) 
            append(*new CDirEntry(iter));
        CIArrayOf<CDirEntry>::sort(compare);
        if (cmp) {  // assumes cmp sorted
            unsigned i = 0;
            unsigned ni = ordinality();
            unsigned j = 0;
            unsigned nj = cmp->ordinality();
            for (;;) {
                CDirEntry *a = NULL;
                CDirEntry *b = NULL;
                if (i>=ni) {
                    if (j>=nj)
                        break;
                    b = &cmp->item(j++);
                }
                else if (j>=nj) {
                    a = &item(i++);
                }
                else {
                    a = &item(i);
                    b = &cmp->item(j);
                    int c = a->compare(b);
                    if (c==0) {
                        if (a->compareProp(b)==0) {
                            a->flags = IDDIunchanged;
                            a = NULL;
                            b = NULL;
                        }
                        i++;
                        j++;
                    }
                    else if (c<0) {
                        b = NULL; 
                        i++;
                    }
                    else {
                        a = NULL;
                        j++;
                    }
                }
                if (a) {
                    if (b)
                        a->flags|=IDDImodified;
                    else
                        a->flags|=IDDIadded;
                }
                else if (b) {
                    b->Link();
                    b->flags = IDDIdeleted;
                    add(*b,i);
                    i++;
                    ni++;
                }
            }                   
        }
    }
    virtual bool first()
    {
        idx = 0;
        while (idx<ordinality()) {
            if (item(idx).match(mask))
                return true;
            idx++;
        }
        return false;
    }
    virtual bool next()
    {
        idx++;
        while (idx<ordinality()) {
            if (item(idx).match(mask))
                return true;
            idx++;
        }
        return false;
    }
    virtual bool isValid()  
    {
        return (idx<ordinality());
    }
    virtual IFile & query() 
    {
        if (isValid())
            return *item(idx).file;
        throwUnexpected();
    }
    virtual StringBuffer &getName(StringBuffer &buf)
    {
        if (isValid())
            buf.append(item(idx).name);
        return buf;
    }
    virtual bool isDir() 
    {
        if (isValid())
            return item(idx).isdir;
        return false;
    }
    __int64 getFileSize()
    {
        if (isValid())
            return item(idx).isdir?0:item(idx).size;
        return 0;
    }
    virtual bool getModifiedTime(CDateTime &ret)
    {
        if (isValid()) {
            ret.set(item(idx).modified);
        }
        return false;
    }

    virtual void setMask(unsigned _mask)
    {
        mask = (byte)_mask;
    }

    virtual unsigned getFlags() 
    {
        if (isValid())
            return item(idx).flags;
        return 0;
    }

    unsigned numChanges()
    {
        unsigned ret=0;
        ForEachItemIn(i,*this) {
            byte f = item(i).flags;
            if ((f!=0)&&(f!=IDDIunchanged))
                ret++;
        }
        return ret;
    }
};



IDirectoryDifferenceIterator *CFile::monitorDirectory(IDirectoryIterator *_prev,            // in
                             const char *mask,
                             bool sub,
                             bool includedirs,
                             unsigned checkinterval,
                             unsigned timeout,
                             Semaphore *abortsem)
{
    Linked<IDirectoryIterator> prev;
    if (_prev)
        prev.set(_prev);
    else
        prev.setown(directoryFiles(mask,sub,includedirs));
    if (!prev)
        return NULL;
    Owned<CDirectoryDifferenceIterator> base = new CDirectoryDifferenceIterator(prev,NULL);
    prev.clear(); // not needed now
    unsigned start=msTick();
    for (;;) {
        if (abortsem) {
            if (abortsem->wait(checkinterval))
                break;
        }
        else
            Sleep(checkinterval);
        Owned<IDirectoryIterator> current = directoryFiles(mask,sub,includedirs);
        if (!current)
            break;
        Owned<CDirectoryDifferenceIterator> cmp = new CDirectoryDifferenceIterator(current,base);
        current.clear();
        if (cmp->numChanges())
            return cmp.getClear();
        if (msTick()-start>timeout)
            break;
    }
    return NULL; // timed out
}



//---------------------------------------------------------------------------

bool recursiveCreateDirectory(const char * path)
{
    Owned<IFile> file = createIFile(path);
    if (!file)
        return false;
    return file->createDirectory();
}

bool recursiveCreateDirectoryForFile(const char *fullFileName)
{
    StringBuffer path;
    splitFilename(fullFileName, &path, &path, NULL, NULL);
    return recursiveCreateDirectory(path.str());
}

void recursiveRemoveDirectory(IFile *dir)
{
    Owned<IDirectoryIterator> files = dir->directoryFiles(NULL, false, true);
    ForEach(*files)
    {
        IFile *thisFile = &files->query();
        if (thisFile->isDirectory()==fileBool::foundYes)
            recursiveRemoveDirectory(thisFile);
        else
        {
            try
            {
                thisFile->remove();
                continue; // i.e. continue if returns true (removed), or false (file no longer exists)
            }
            catch(IException *e)
            {
                e->Release();
            }

            // NB: not sure this is worth it. In linux the file can be read-only and still be deleted if the directory is writable
            // Perhaps should ensure that the parent directory is writable, but that would require extra file ops. or interface changes
            thisFile->setReadOnly(false);
            thisFile->remove(); // if gets here, the file should exist (ignore return false if doesn't), throws an exception if fails for other reason
        }
    }
    dir->remove();
}

void recursiveRemoveDirectory(const char *dir)
{
    Owned<IFile> f = createIFile(dir);
    if (f->isDirectory()==fileBool::foundYes)
        recursiveRemoveDirectory(f);
}



//---------------------------------------------------------------------------


size32_t DirectBufferI::read(offset_t pos, size32_t len, void * data)
{
    if (pos + len > buffLen)
    {
        if (pos > buffLen)
            pos = buffLen;
        len = (size32_t)(buffLen - pos);
    }
    memcpy(data, buffer+pos, len);
    return len;
}

size32_t DirectBufferI::write(offset_t pos, size32_t len, const void * data)
{
    UNIMPLEMENTED;
}

size32_t DirectBufferIO::write(offset_t pos, size32_t len, const void * data)
{
    if (pos + len > buffLen)
    {
        if (pos > buffLen)
            pos = buffLen;
        len = (size32_t)(buffLen - pos);
    }
    memcpy(buffer+pos, data, len);
    return len;
}

//---------------------------------------------------------------------------

IFile * createIFile(const char * filename)
{
    if (isEmptyString(filename))
        return new CFile(""); // this is in effect a null implementation
    IFile *ret = createContainedIFileByHook(filename);
    if (ret)
        return ret;

    RemoteFilename rfn;
    rfn.setRemotePath(filename);

    if (rfn.isNull())
        throw MakeStringException(-1, "CreateIFile cannot resolve %s", filename);

    ret = createIFileByHook(rfn);           // use daliservix in preference
    if (ret)
        return ret;

    // NB: This is forcing OS path access if not a url beginning '//' or '\\'
    bool linremote=(memcmp(filename,"//",2)==0);
    if (!linremote&&(memcmp(filename,"\\\\",2)!=0)) // see if remote looking
        return new CFile(filename);

    if (rfn.isLocal())
    {
        StringBuffer tmplocal;
        rfn.getLocalPath(tmplocal);
        return new CFile(tmplocal);
    }

    /* NB: to get here, no hook has returned a result and the file is non-local and prefixed with // or \\ */
#ifdef _WIN32
    /* NB: this windows specific code below should really be refactored into the standard
     * hook mechanism. And any path translation should be left/done on the remote side
     * once it gets to dafilersv.
     */
    StringBuffer tmplocal;
    if (linremote||(rfn.queryEndpoint().port!=0))
    {
        while (*filename)                             // no daliservix so swap '/' for '\' and hope for best
        {
            if (*filename=='/')
                tmplocal.append('\\');
            else
                tmplocal.append(*filename);
            filename++;
        }
        filename =tmplocal.str();
    }
    return new CWindowsRemoteFile(filename);
#else
#ifdef USE_SAMBA
    if (memcmp(filename, "//", 2) == 0)
    {
        StringBuffer smbfile("smb:");
        smbfile.append(filename);
        return new CSambaRemoteFile(smbfile.str());
    }
    if (memcmp(filename, "\\\\", 2) == 0)
    {
        StringBuffer smbfile("smb:");
        int i = 0;
        while(filename[i])
        {
            if(filename[i] == '\\')
                smbfile.append('/');
            else
                smbfile.append(filename[i]);
            i++;
        }
        return new CSambaRemoteFile(smbfile.str());
    }
#else
    if (memcmp(filename,"smb://",6)==0)  // don't support samba - try remote
        return createIFile(filename+4);
#endif
    throw MakeStringException(-1, "createIFile: cannot attach to %s", filename);
#endif
}

void touchFile(IFile *iFile)
{
    Owned<IFileIO> iFileIO = iFile->open(IFOcreate);
    if (!iFileIO)
        throw makeStringExceptionV(0, "touchFile: failed to create file %s", iFile->queryFilename());
    iFileIO->close();
}

void touchFile(const char *filename)
{
    Owned<IFile> iFile = createIFile(filename);
    touchFile(iFile);
}


IFileIOStream * createIOStream(IFileIO * file)
{
    return new CFileIOStream(file);
}

IFileIOStream * createNoSeekIOStream(IFileIOStream * stream)
{
    return new CNoSeekFileIOStream(stream);
}

IFileIO * createIORange(IFileIO * io, offset_t header, offset_t length)
{
    return new CFileRangeIO(io, header, length);
}

IFileIOStream * createBufferedIOStream(IFileIO * io, unsigned bufsize)
{
    if (bufsize == (unsigned)-1)
        bufsize = DEFAULT_BUFFER_SIZE;
    return new CBufferedFileIOStream(io, bufsize);
}

IFileIOStream * createBufferedAsyncIOStream(IFileAsyncIO * io, unsigned bufsize)
{
    if (bufsize == (unsigned)-1)
        bufsize = DEFAULT_BUFFER_SIZE*2;
    return new CBufferedAsyncIOStream(io, bufsize);
}

IFileIOStream * createIOStreamFromFile(const char *fileNameWithPath, IFOmode mode)
{
    Owned<IFile> iFile = createIFile(fileNameWithPath);
    Owned<IFileIO> iFileIO = iFile->open(mode);
    if (!iFileIO)
        return nullptr;
    return createIOStream(iFileIO);
}

IFileIOStream * createBufferedIOStreamFromFile(const char *fileNameWithPath, IFOmode mode, unsigned bufsize)
{
    Owned<IFile> iFile = createIFile(fileNameWithPath);
    Owned<IFileIO> iFileIO = iFile->open(mode);
    if (!iFileIO)
        return nullptr;
    return createBufferedIOStream(iFileIO, bufsize);
}

IFileIOStream *createProgressIFileIOStream(IFileIOStream *iFileIOStream, offset_t totalSize, const char *msg, unsigned periodSecs)
{
    class CProgressIFileIOStream : public CSimpleInterfaceOf<IFileIOStream>
    {
        Linked<IFileIOStream> iFileIOStream;
        offset_t totalSize = 0;
        StringAttr msg;
        void log(double pct)
        {
            PROGLOG("%s - %.2f%% complete", msg.get(), pct);
        }
        PeriodicTimer periodTimer;
    public:
        CProgressIFileIOStream(IFileIOStream *_iFileIOStream, offset_t _totalSize, const char *_msg, unsigned periodSecs)
            : iFileIOStream(_iFileIOStream), totalSize(_totalSize), msg(_msg), periodTimer(periodSecs*1000, true)
        {
        }
        ~CProgressIFileIOStream()
        {
            offset_t pos = iFileIOStream->tell();
            if (pos == totalSize)
                log(100.0);
        }
        // implements ISimpleReadStream
        virtual size32_t read(size32_t max_len, void * data) override
        {
            if (periodTimer.hasElapsed())
            {
                offset_t pos = iFileIOStream->tell();
                double pct = ((double)pos) / totalSize * 100;
                log(pct);
            }
            return iFileIOStream->read(max_len, data);
        }
        // implements IIOStream
        virtual void flush() override
        {
            throwUnexpected();
        }
        virtual size32_t write(size32_t len, const void * data) override
        {
            throwUnexpected();
        }
        // implements IFileIOStream
        virtual void seek(offset_t pos, IFSmode origin) override
        {
            return iFileIOStream->seek(pos, origin);
        }
        virtual offset_t size() override
        {
            return iFileIOStream->size();
        }
        virtual offset_t tell() override
        {
            return iFileIOStream->tell();
        }
        virtual unsigned __int64 getStatistic(StatisticKind kind) override
        {
            return iFileIOStream->getStatistic(kind);
        }
        virtual void close() override
        {
            iFileIOStream->close();
        }
    };
    return new CProgressIFileIOStream(iFileIOStream, totalSize, msg, periodSecs);
}


extern jlib_decl offset_t filesize(const char *name)
{
    CFile f(name);
    return f.size();
}

extern jlib_decl offset_t getFreeSpace(const char* name)
{
    offset_t freeBytesToCaller;

#ifdef _WIN32
    offset_t totalBytes;
    offset_t freeBytes;

    int fResult = GetDiskFreeSpaceEx (name,
                                     (PULARGE_INTEGER)&freeBytesToCaller,
                                     (PULARGE_INTEGER)&totalBytes,
                                     (PULARGE_INTEGER)&freeBytes);

    if (fResult == 0)  // error
    {
        return 0;
    }


#elif defined (__linux__) || defined (__APPLE__)
    struct statfs buf;
    int fResult = statfs(name, &buf);

    if (fResult == -1) // error
    {
        return 0;
    }


    // from "man statfs the def of f_bavail and f_bsize...
    // mult "free blocks avail to non-superuser" by "optimal transfer block size" to get the available size
    
    freeBytesToCaller = (offset_t)buf.f_bavail * buf.f_bsize;
#else
    UNIMPLEMENTED;
#endif

    
    return freeBytesToCaller;
}

extern jlib_decl void createHardLink(const char* fileName, const char* existingFileName)
{
#ifdef _WIN32
    // requirements...need to make sure that the directory already exists
    //                and the 2 directories need to be on the same drive
    if (!CreateHardLink(fileName, existingFileName, NULL))
    {
        LPVOID lpMsgBuf;
        FormatMessage( 
            FORMAT_MESSAGE_ALLOCATE_BUFFER | 
            FORMAT_MESSAGE_FROM_SYSTEM | 
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            GetLastError(),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
            (LPTSTR) &lpMsgBuf,
            0,
            NULL 
        );
        StringBuffer err;
        err.appendf("Failed to create log alias %s for %s: %s", fileName, existingFileName, lpMsgBuf);
        LocalFree( lpMsgBuf );

        throw MakeStringException(-1, "createHardLink:: %s.",err.str());

    }
#else
    if (link(existingFileName, fileName) != 0) // error
        throw MakeStringException(-1, "Failed to create log alias %s for %s: error code = %d", fileName, existingFileName, errno);
#endif

}


#if 0
void testDirectory()
{
    //setDefaultUser("administrator", "password");
    Owned<IFile> dir = createIFile("\\test1\\gavin\\home\\base");
    dir->createDirectory();
    Owned<IFile> dir2 = createIFile("\\test1\\gavin\\home\\base2\\");
    dir2->createDirectory();
    dir2->createDirectory();
    Owned<IFile> dir3 = createIFile("c:\\test1\\gavin\\home2\\base");
    dir3->createDirectory();
    Owned<IFile> dir4 = createIFile("\\\\ghalliday\\c$\\test1\\gavin\\home3\\base");
    dir4->createDirectory();
    Owned<IFile> dir5 = createIFile("\\\\192.168.1.3\\d$\\test2\\gavin\\home3\\base");
    dir5->createDirectory();

    StringBuffer x;
    x.clear();
    splitUNCFilename("\\\\127.0.0.1\\gavin\\abc.ext", &x, &x, &x, &x);
    x.clear();
    splitUNCFilename("\\\\127.0.0.1\\gavin\\abc.ext\\etc", &x, &x, &x, &x);
    x.clear();
    splitUNCFilename("\\\\127.0.0.1\\", &x, &x, &x, &x);
}
#endif



// -- RemoteFilename class (file location encapsulation)

//#define _TRACERFN

void RemoteFilename::badFilename(const char * filename)
{
    throw MakeStringException(-1, "Badly formatted file entry %s", filename);
}

bool RemoteFilename::equals(const RemoteFilename & other) const
{   
    if (isNull())
        return other.isNull();
    if (other.isNull())
        return false;
    if (!ep.ipequals(other.ep)) // only use ip for compare
        return false;
    if (isUnixPath()) { // case sensitive
        if (localhead.length()&&other.localhead.length()) {
            if (strcmp(localhead.get(),other.localhead.get())!=0)
                return false;
        }
        else if (sharehead.length()&&other.sharehead.length()) {
            if (strcmp(sharehead.get(),other.sharehead.get())!=0)
                return false;
        }
        else if (sharehead.length()!=other.sharehead.length())
            return false;
        if (strcmp(tailpath.get(),other.tailpath.get())!=0)
            return false;
    }
    else {
        if (localhead.length()&&other.localhead.length()) {
            if (stricmp(localhead.get(),other.localhead.get())!=0)
                return false;
        }
        else if (sharehead.length()&&other.sharehead.length()) {
            if (stricmp(sharehead.get(),other.sharehead.get())!=0)
                return false;
        }
        else if (sharehead.length()!=other.sharehead.length())
            return false;
        if (stricmp(tailpath.get(),other.tailpath.get())!=0)
            return false;
    }
    return true;
}

bool RemoteFilename::isNull() const
{
    return (ep.isNull()||(tailpath.length()==0));
}


void RemoteFilename::clear()
{
    ep.set(NULL, 0);
    localhead.clear();
    sharehead.clear();
    tailpath.clear();
}

void RemoteFilename::serialize(MemoryBuffer & out)
{
    ep.serialize(out);
    out.append(localhead);
    out.append(sharehead);
    out.append(tailpath);
}

void RemoteFilename::deserialize(MemoryBuffer & in)
{
    ep.deserialize(in);
    in.read(localhead);
    in.read(sharehead);
    in.read(tailpath);
}

StringBuffer & RemoteFilename::getTail(StringBuffer &name) const
{
    split(NULL, NULL, &name, &name);
    return name;
}

StringBuffer & RemoteFilename::getPath(StringBuffer & name) const
{
    if (isLocal())
        return getLocalPath(name);
    else
        return getRemotePath(name);
}

bool RemoteFilename::isUnixPath() const // bit arbitrary
{
    if (tailpath.length()!=0) {
        char c = tailpath[0];
        if (c=='/')
            return true;
        if (c=='\\') 
            return false;
    }
    if (sharehead.length()!=0) {
        char c = sharehead[0];
        if (c=='/')
            return true;
        if (c=='\\') 
            return false;
    }
    if (localhead.length()!=0) {
        const char *s=localhead;   
        if (*s=='/')
            return true;
        if ((s[1]==':')&&(s[2]=='/'))
            return false;
        // those are the expected cases, otherwise look for first separator (legacy)
        while (*s) {
            if (*s=='\\')
                return false;
            s++;
            if (*s=='/')
                return true;
        }
    }
#ifdef _WIN32           // assume local on starting machine
    return false;
#else
    return true;
#endif
}

bool RemoteFilename::isUrl() const
{
    return ::isUrl(tailpath);
}


char RemoteFilename::getPathSeparator() const
{
    return isUnixPath()?'/':'\\';
}

StringBuffer & RemoteFilename::getLocalPath(StringBuffer & out) const
{
    if (tailpath.length()!=0) {
        out.append(localhead).append(tailpath);
    }
#ifdef _TRACERFN
    StringBuffer eps;
    PROGLOG("getLocalPath '%s' '%s' '%s' -> '%s'",sharehead.get()?sharehead.get():"NULL",localhead.get()?localhead.get():"NULL",tailpath.get()?tailpath.get():"NULL",out.str());
#endif
    return out;
}

StringBuffer & RemoteFilename::getRemotePath(StringBuffer & out) const
{   // this creates a name that can be used by windows or linux

    // Any filenames in the format protocol:// should not be converted to //ip:....
    if (isUrl())
        return getLocalPath(out);

    char c=getPathSeparator();
    out.append(c).append(c);
    ep.getEndpointHostText(out);
    const char *fn;
    StringBuffer loc;
    if (sharehead.length()) 
        fn = loc.append(sharehead).append(tailpath).str();
    else // try and guess from just tail (may likely fail other than for windows) 
        fn=getLocalPath(loc).str();
    if ((fn[1]==':') && (fn[2]=='/' || fn[2]=='\\')) {  // windows \\d$
        out.append((char)tolower(c)).append(*fn).append(getShareChar());
        fn+=2;
    }
    out.append(fn);
#ifdef _TRACERFN
    StringBuffer eps;
    PROGLOG("getRemotePath '%s' '%s' '%s' -> '%s'",sharehead.get()?sharehead.get():"NULL",localhead.get()?localhead.get():"NULL",tailpath.get()?tailpath.get():"NULL",out.str());
#endif
    return out;
}

const FileSystemProperties & RemoteFilename::queryFileSystemProperties() const
{
    return ::queryFileSystemProperties(tailpath);
}

bool RemoteFilename::isLocal() const
{
    if (ep.port&&(ep.port!=DAFILESRV_PORT && ep.port!=SECURE_DAFILESRV_PORT))
        return false;  // treat non-dafilesrv port as remote
    return ep.isLocal() || ep.isNull();
}


void RemoteFilename::set(const RemoteFilename & other)
{
    ep.set(other.ep);
    localhead.set(other.localhead);
    sharehead.set(other.sharehead);
    tailpath.set(other.tailpath);

}


void RemoteFilename::setExtension(const char * newext)
{
    const char * dot = NULL;
    const char * start = tailpath;
    const char * cur = start;
    const char pathSep=getPathSeparator();
    while (*cur)
    {
        // if it is a "." inside the path then skip it.
        if (dot && (*cur == pathSep))
            dot = NULL;
        else if (*cur == '.')
            dot = cur;
        cur++;
    }
    StringBuffer newtail;
    if (dot)
        newtail.append(dot-start,start);
    else
        newtail.append(start);
    if (newext)
    {
        if (newext[0] != '.')
            newtail.append('.');
        newtail.append(newext);
    }
    tailpath.set(newtail);
}


void RemoteFilename::setPath(const SocketEndpoint & _ep, const char * _filename)
{
    const char * filename=_filename;
    StringBuffer full;
    ep.set(_ep);
    localhead.clear();
    sharehead.clear();
    if (filename&&*filename) {
        if (isSpecialPath(filename)) {
            tailpath.set(filename);
            return;
        }
        if (isLocal()&&!isAbsolutePath(filename)) {
            char dir[_MAX_PATH];
            if (!GetCurrentDirectory(sizeof(dir), dir)) {
                IERRLOG("RemoteFilename::setPath: Current directory path too big, bailing out");
                throwUnexpected();
            }
            if (*filename==PATHSEPCHAR) {
#ifdef _WIN32
                if (*dir && (dir[1]==':')) 
                    dir[2] = 0;
#endif
                filename++;
            }
            full.append(dir);
            if (full.charAt(full.length()-1) != PATHSEPCHAR)
                full.append(PATHSEPCHAR);
            full.append(filename);
            filename = full.str();
        }
        bool isunix;
        if (filename[0]=='/')
            isunix=true;
        else if (filename[0]=='\\')
            isunix=false;
        else if ((filename[1]==':')&&(filename[2]=='\\'))
            isunix = false;
        else // invalid call really as supposed to be full local path!
#ifdef _WIN32
            isunix = false;
#else
            isunix = true;
#endif
        StringBuffer tmpxlat;
        char sepchar = isunix?'/':'\\';
        char altchar = isunix?'\\':'/';
        // fix mixed separator path 
        if (strchr(filename,altchar)) {
            while (*filename) {
                if (*filename==altchar)
                    tmpxlat.append(sepchar);
                else
                    tmpxlat.append(*filename);
                filename++;
            }
            filename = tmpxlat.str();
        }
        if (isunix) {
            if (filename[0]=='/') {
                const char *tail;
                tail=strchr(filename+1,'/');
                if (tail) {
                    sharehead.set(filename,tail-filename); // we don't know share so guess same as leading dir
                    localhead.set(filename,tail-filename); 
                    filename = tail;
                }
            }
        }
        else { // windows can guess anonymous share
            StringBuffer sharestr;

            if ((filename[1]==':')&&(filename[2]=='\\')) { // this should be always true if true full windows path
                localhead.set(filename,2);
                sharestr.append('\\').append((char)tolower(filename[0])).append(getShareChar());
                filename += 2;
            }
            else if (filename[0]=='\\') {
                const char *tail = strchr(filename+1,'\\');
                if (tail) {
                    sharestr.append(tail-filename,filename);
                    localhead.set(filename,tail-filename);
                    filename = tail;
                }
                else {
                    localhead.set("c:");
                    sharestr.append("\\c").append(getShareChar());
                }
            }   
                        
            sharehead.set(sharestr);
        }
        tailpath.set(filename);
    }
    else
        tailpath.clear();
#ifdef _TRACERFN
    StringBuffer eps;
    PROGLOG("setPath (%s,%s) -> '%s' '%s' '%s'",ep.getEndpointHostText(eps).str(),_filename?_filename:"NULL",sharehead.get()?sharehead.get():"NULL",localhead.get()?localhead.get():"NULL",tailpath.get()?tailpath.get():"NULL");
#endif
}

void RemoteFilename::setRemotePath(const char * _url,const char *localpath)
{
    // url should be full (share) path including ep
    const char *url=_url;
    // if ep not included then assume local (bit weird though)
    char sep = url?url[0]:0;
    if (isPathSepChar(sep)&&(sep==url[1])) {
        url+=2;
        const char *end = findPathSepChar(url);
        if (end) {
            StringBuffer eps(end-url,url);
            ep.set(eps.str());
            url = end;
        }
        else {
            ep.set(NULL,0);
            url--; // don't know what is going on!
        }
        if (isPathSepChar(*url)||!*url)
            sep = *url;
    }
    else
        ep.setLocalHost(0);
    if (localpath&&*localpath) 
        setPath(ep,localpath);
    else {
        localhead.clear();
        sharehead.clear();
        tailpath.clear();
    }
    if (sep&&*url) {
        // url should point to the share now
        const char *tail=findPathSepChar(url+1); 
        if (tail) { // hopefully must be!
            sharehead.set(url,tail-url);
            url = tail;
        }
        if (localhead.length()==0) { // we don't know so guess
            if (sharehead.length()!=0) {
                const char *share=sharehead.get();
                if (sep=='\\') {
                    StringBuffer locstr;
                    if (sharehead.length()>2) {
                        if (isShareChar(share[2]))  {
                            locstr.append(share[1]).append(':');
                            share+=3;
                        }
                        else  // we haven't a clue!
                            locstr.append("c:");
                    }
                    else if (sharehead[1]!='$')  // data format
                        locstr.append("c:");
                    locstr.append(share);
                    localhead.set(locstr);
                }
                else { // we haven't a clue so assume same as share
                    localhead.set(sharehead);
                }
            }
        }

    }
    if (tailpath.length()==0)
        tailpath.set(url);
#ifdef _TRACERFN
    StringBuffer eps;
    PROGLOG("setRemotePath ('%s','%s') -> '%s' '%s' '%s'",_url,localpath?localpath:"NULL",sharehead.get()?sharehead.get():"NULL",localhead.get()?localhead.get():"NULL",tailpath.get()?tailpath.get():"NULL");
#endif
}


void RemoteFilename::setLocalPath(const char *filename)
{
    SocketEndpoint localEP;
    localEP.setLocalHost(0);
    setPath(localEP, filename);
}


void RemoteFilename::split(StringBuffer * drive, StringBuffer * path, StringBuffer * tail, StringBuffer * ext) const
{
    StringBuffer localname;
    const char *s = getLocalPath(localname).str();
    char sep;
    if (isUnixPath())
        sep = '/';
    else {
        sep = '\\';
        if (s[1]==':') {
            if (drive)
                drive->append(2,s);
            s+=2;
        }
    }
    const char *t=NULL;
    const char *e=s;
    while (*e) {
        if (*e==sep)
            t = e+1;
        e++;
    }
    if (t) {
        if (path)
            path->append(t-s, s);
        s=t;
    }
    if (!tail&&!ext)
        return;
    t = NULL;
    e=s;
    while (*e) {
        if (*e=='.')        // NB final extension
            t = e;
        e++;
    }
    if (t) {
        if (tail)
            tail->append(t-s,s);
        if (ext)
            ext->append(e-t,t);
    }
    else if (tail)
        tail->append(e-s,s);
}

void RemoteMultiFilename::append(const char *mpath,const char *defaultdir)
{
    StringArray paths;
    expand(mpath, paths);
    StringBuffer fullpath;
    StringBuffer deftmp;
    if (paths.ordinality()&&(defaultdir==NULL)&&isAbsolutePath(paths.item(0))) {
        splitDirTail(paths.item(0),deftmp);
        defaultdir = deftmp.str();
    }
    ForEachItemIn(i,paths) {
        const char *filename = paths.item(i);
        RemoteFilename rfn;
        if (isPathSepChar(*filename)&&isPathSepChar(filename[1]))  // full URL
            rfn.setRemotePath(filename);
        else {
            if (defaultdir&&!isAbsolutePath(filename)) 
                filename = addPathSepChar(fullpath.clear().append(defaultdir)).append(filename).str();
            rfn.setPath(ep,filename);
        }
        append(rfn);
    }
}

void RemoteMultiFilename::append(const RemoteFilename &inrfn)
{
    RemoteFilename rfn;
    rfn.set(inrfn);
    if (ep.isNull())
        ep = rfn.queryEndpoint();
    else if (rfn.queryIP().isNull()) {
        rfn.setEp(ep);
    }
    else if (!rfn.queryIP().ipequals(ep)) {
        StringBuffer path;
        rfn.getRemotePath(path);
        throw MakeStringException(-1, "Component file IP does not match: %s", path.str());          
    }
    RemoteFilenameArray::append(rfn);           
}

void RemoteMultiFilename::deserialize(MemoryBuffer & in)
{
    clear();
    ep.deserialize(in);
    unsigned n;
    in.read(n);
    StringBuffer last;
    StringBuffer filename;
    RemoteFilename rfn;
    for (unsigned i=0;i<n;i++) {
        byte l;
        in.read(l);
        filename.clear();
        if (l) 
            filename.append((size32_t)l,last.str());
        StringAttr s;
        in.read(s);
        filename.append(s);
        rfn.setPath(ep,filename.str());
        RemoteFilenameArray::append(rfn);           
        last.swapWith(filename);
    }
}

void RemoteMultiFilename::serialize(MemoryBuffer & out)
{
    ep.serialize(out);
    unsigned n=ordinality();
    out.append(n);
    // do simple compression
    StringBuffer last;
    StringBuffer filename;
    for (unsigned i=0;i<n;i++) {
        item(i).getLocalPath(filename.clear());
        const char *s1 = last.str();
        const char *s2 = filename.str();
        byte l=0;
        while ((l<255)&&*s1&&*s2&&(*s1==*s2)) {
            s1++;
            s2++;
            l++;
        }
        out.append(l);
        out.append(s2);
        last.swapWith(filename);
    }
}

bool RemoteMultiFilename::isWild(unsigned idx) const
{
    if (idx==(unsigned)-1) {
        ForEachItem(i)
            if (isWild(i))
                return true;
    }
    else {
        StringBuffer s;
        item(idx).getTail(s);
        if (containsFileWildcard(s.str()))
            return true;
    }
    return false;
}

void RemoteMultiFilename::expandWild() 
{
    bool anywild = false;
    BoolArray iswild; 
    ForEachItem(i1) {
        if (isWild(i1)) {
            anywild = true;
            iswild.append(true);
        }
        else
            iswild.append(false);
    }
    if (!anywild)
        return;         // nothing to do
    // first cache old values (bit long winded but want to preserve order)
    RemoteFilenameArray tmpa;
    Int64Array tmpsz;
    ForEachItem(i2) {
        RemoteFilename rfn(item(i2));
        tmpa.append(rfn);
        if (i2<sizescache.ordinality())
            tmpsz.append(sizescache.item(i2));
        else
            tmpsz.append(-1);
    }
    RemoteFilenameArray::kill(); 
    sizescache.kill();
    ForEachItemIn(i3,tmpa) {
        RemoteFilename rfn(tmpa.item(i3));
        if (iswild.item(i3)) {
            StringBuffer name;
            rfn.getLocalPath(name);
            const char *s=name.str();
            const char *t=s;
            for (;;) {
                const char *sep=findPathSepChar(t);
                if (!sep)
                    break;
                t = sep+1;
            }
            StringAttr tail(t);
            name.setLength(t-s);
            rfn.setPath(rfn.queryEndpoint(),name);
            Owned<IFile> dir = createIFile(rfn);
            Owned<IDirectoryIterator> iter = dir->directoryFiles(tail.get());
            ForEach(*iter) {
                append(iter->query().queryFilename());
                sizescache.append(iter->getFileSize());
            }
        }
        else {
            append(rfn);
            sizescache.append(tmpsz.item(i3));
        }
    }
}

offset_t RemoteMultiFilename::getSize(unsigned i)
{
    __int64 ret = (i<sizescache.ordinality())?sizescache.item(i):-1;
    if (ret==-1) {
        RemoteFilename rfn(item(i));
        Owned<IFile> file = createIFile(rfn);
        ret = file->size();
    }
    return (offset_t)ret;
}


void RemoteMultiFilename::setIp(const IpAddress & ip)
{
    ep.ipset(ip);
    ForEachItem(i)
        element(i).setIp(ip);
}

void RemoteMultiFilename::setEp(const SocketEndpoint & _ep)
{
    ep.set(_ep);
    ForEachItem(i)
        element(i).setEp(_ep);
}

void RemoteMultiFilename::setPort(unsigned short port)
{
    ep.port = port;
    ForEachItem(i)
        element(i).setPort(port);
}

void RemoteMultiFilename::set(const RemoteMultiFilename & other)
{
    clear();
    ep.set(other.ep);
    ForEachItemIn(i,other) {
        append(other.item(i));
        if (i<other.sizescache.ordinality())
            sizescache.append(other.sizescache.item(i));
    }
}


bool RemoteMultiFilename::equals(const RemoteMultiFilename & other)
{
    if (!ep.equals(other.ep))
        return false;
    if (ordinality()!=other.ordinality())
        return false;
    ForEachItem(i) 
        if (!item(i).equals(other.item(i)))
            return false;
    return true;
}


void RemoteMultiFilename::expand(const char *mpath, StringArray &array)
{
    StringBuffer path;
    StringBuffer fullpath;
    for (;;) {
        while (isspace(*mpath))
            mpath++;
        if (!*mpath)
            break;
        bool inquote=false;
        while (*mpath) {
            if (*mpath=='"') {
                mpath++;
                if (inquote) {
                    if (*mpath!='"') {
                        inquote = false;
                        continue;
                    }
                }
                else {
                    inquote = true;
                    continue;
                }
            }
            if ((*mpath==',')&&!inquote) {
                mpath++;
                break;
            }
            path.append(*mpath);
            mpath++;
        }
        path.clip();
        if (path.length()) {
            array.append(path.str());
            path.clear();
        }
    }
}

void RemoteMultiFilename::tostr(StringArray &array,StringBuffer &out)
{
    ForEachItemIn(i,array) {
        const char *s = array.item(i);
        if (!s||!*s)
            continue;
        if (i!=0)
            out.append(',');
        bool needquote=false;
        for (const char *e=s;*e;e++)
            if (isspace(*e)||(*e==',')) {
                needquote = true;
                break;
            }
        if (needquote)
            out.append('"');
        out.append(s);
        if (needquote)
            out.append('"');
    }
}

//===================================================================================================

static IArrayOf<IContainedFileHook> containedFileHooks;
static ReadWriteLock containedFileHookLock;

void addContainedFileHook(IContainedFileHook *hook)
{
    if (hook)
    {
        hook->Link();
        WriteLockBlock block(containedFileHookLock);
        containedFileHooks.append(*hook);
    }
}

void removeContainedFileHook(IContainedFileHook *hook)
{
    WriteLockBlock block(containedFileHookLock);
    containedFileHooks.zap(*hook);
}

static IFile *createContainedIFileByHook(const char *filename)
{
    ReadLockBlock block(containedFileHookLock);
    ForEachItemIn(i, containedFileHooks)
    {
        IFile * ret = containedFileHooks.item(i).createIFile(filename);
        if (ret)
            return ret;
    }
    return NULL;
}

static IArrayOf<IRemoteFileCreateHook> remoteFileHooks;
static ReadWriteLock remoteFileHookLock;

void addIFileCreateHook(IRemoteFileCreateHook *hook)
{
    if (hook)
    {
        hook->Link();
        WriteLockBlock block(remoteFileHookLock);
        remoteFileHooks.append(*hook);
    }
}

void removeIFileCreateHook(IRemoteFileCreateHook *hook)
{
    WriteLockBlock block(remoteFileHookLock);
    remoteFileHooks.zap(*hook);
}


static IFile *createIFileByHook(const RemoteFilename & filename)
{
    ReadLockBlock block(remoteFileHookLock);
    ForEachItemIn(i, remoteFileHooks)
    {
        IFile * ret = remoteFileHooks.item(i).createIFile(filename);
        if (ret)
            return ret;
    }
    return NULL;
}


IFile * createIFile(const RemoteFilename & filename)
{
    IFile * ret = createIFileByHook(filename);
    if (ret)
        return ret;
    StringBuffer name;
    return createIFile(getLocalOrRemoteName(name,filename).str());
}

StringBuffer &makePathUniversal(const char *path, StringBuffer &out)
{
    if (!path||!*path)
        return out;
    if (path[1]==':')
    {
        out.append('/').append(*path);
        path+=2;
    }
    for (; *path; path++)
        out.append(isPathSepChar(*path) ? '/' : *path);
    return out;
}

// A filename is a URL if it starts xxxx://
bool isUrl(const char *path)
{
    if (!path||!*path)
        return false;

    const char * cur = path;
    for (;;)
    {
        switch (*cur++)
        {
        case '/':
        case '\\':
        case '\0':
            return false;
        case ':':
            return cur[0]=='/' && cur[1]=='/';
        }
    }
}


//Treat a filename as absolute if:
//  a) The filename begins with a path separator character   e.g. /home/hpcc/blah    \Users\hpcc\blah
//  b) If there is a colon before the first path separator   e.g. c:\Users\hpcc\blah   s3://mycontainer/myblob
//
// Do not match:
//  A) regress::myfile::x::y
//  B) local/mydir
//  C) mylocal
bool isAbsolutePath(const char *path)
{
    if (!path||!*path)
        return false;
    if (isPathSepChar(path[0]))
        return true;
    const char * cur = path;
    bool hadColon = false;
    for (;;)
    {
        switch (*cur++)
        {
        case '/':
        case '\\':
            return hadColon;
        case '\0':
            return false;
        case ':':
            hadColon = true;
            break;
        }
    }
}

//Treat a filename as remote if:
//  a) The filename begins \\ or //
//  b) The filename begins xxx://
//
bool isRemotePath(const char *path)
{
    if (!path||!*path)
        return false;
    if (isPathSepChar(path[0]) && isPathSepChar(path[1]))
        return true;

    const char * cur = path;
    for (;;)
    {
        switch (*cur++)
        {
        case '/':
        case '\\':
        case '\0':
            return false;
        case ':':
            return cur[0]=='/' && cur[1]=='/';
        }
    }
}

bool containsRelPaths(const char *path)
{
    if (isEmptyString(path))
        return false;
    if (*path == '~')
        return true;

    const char *cur = path;
    char sepChar = getPathSepCharEx(path);
    char previousChar = sepChar;
    for (;;)
    {
        switch (*cur)
        {
        case '.':
        {
            if (previousChar == sepChar)
            {
                cur++;
                if ((*cur == sepChar) || (*cur == '\0')) // '.'
                    return true;
                    
                if ('.' == *cur)
                {
                    cur++;
                    if ((*cur == sepChar) || (*cur == '\0')) // '..'
                        return true;
                }
            }
            break;
        }
        case '\0':
            return false;
        }

        previousChar = *cur;
        cur++;
    }
}

StringBuffer &makeAbsolutePath(const char *relpath,StringBuffer &out, bool mustExist)
{
    // NOTE - this function also normalizes the supplied path to remove . and .. references
    if (isRemotePath(relpath))
    {
        if (mustExist)
        {
            OwnedIFile iFile = createIFile(relpath);
            if (!iFile->exists())
                throw makeStringExceptionV(-1, "makeAbsolutePath: could not resolve absolute path for %s", relpath);
        }
        return out.append(relpath); // if remote then already should be absolute
    }

#ifdef _WIN32
    char rPath[MAX_PATH];
    char *filepart;
    if (!relpath || '\0' == *relpath)
        relpath = ".";
    DWORD res = GetFullPathName(relpath, sizeof(rPath), rPath, &filepart);
    if (0 == res)
        throw makeOsExceptionV(GetLastError(), "makeAbsolutePath: could not resolve absolute path for %s", relpath);
    else if (mustExist)
    {
        OwnedIFile iFile = createIFile(rPath);
        if (!iFile->exists())
            throw makeStringExceptionV(-1, "makeAbsolutePath: could not resolve absolute path for %s", rPath);
    }
    out.append(rPath);
#else

    StringBuffer expanded;
    //Expand ~ on the front of a filename - useful for paths not passed on the command line
    //Note, it does not support the ~user/ version of the syntax
    if ((*relpath == '~') && isPathSepChar(relpath[1]))
    {
        getHomeDir(expanded);
        expanded.append(relpath+1);
        relpath = expanded.str();
    }
    char rPath[PATH_MAX];
    if (mustExist)
    {
        if (!realpath(relpath, rPath))
            throw makeErrnoExceptionV(errno, "makeAbsolutePath: could not resolve absolute path for %s", relpath);
        out.append(rPath);
    }
    else
    {
        // no error, will attempt to resolve(realpath) as much of relpath as possible and append rest
        if (strlen(relpath))
        {
            const char *end = relpath+strlen(relpath);
            const char *path = relpath;
            const char *tail = end;
            StringBuffer head;
            for (;;)
            {
                if (realpath(path,rPath))
                {
                    out.append(rPath);
                    if (tail != end)
                        out.append(tail);
                    return removeTrailingPathSepChar(out);
                }
                // mark next tail
                for (;;)
                {
                    --tail;
                    if (tail == relpath)
                        break;
                    else if ('/' == *tail)
                        break;
                }
                if (tail == relpath)
                    break; // bail out and guess
                head.clear().append(tail-relpath, relpath);
                path = head.str();
            }
        }
        if (isAbsolutePath(relpath))
            out.append(relpath);
        else
        {
            appendCurrentDirectory(out, true);
            if (strlen(relpath))
                addPathSepChar(out).append(relpath);
        }
    }
#endif
    return removeTrailingPathSepChar(out);
}

StringBuffer &makeAbsolutePath(StringBuffer &relpath,bool mustExist)
{
    StringBuffer out;
    makeAbsolutePath(relpath.str(),out,mustExist);
    relpath.swapWith(out);
    return relpath;
}

StringBuffer &makeAbsolutePath(const char *relpath, const char *basedir, StringBuffer &out)
{
    StringBuffer combined;
    if (basedir && !isAbsolutePath(relpath))
        relpath = combined.append(basedir).append(relpath);
    return makeAbsolutePath(relpath, out);
}

const char *splitRelativePath(const char *full,const char *basedir,StringBuffer &reldir)
{
    if (basedir&&*basedir) {
        size_t bl = strlen(basedir);
        if (isPathSepChar(basedir[bl-1]))
            bl--;
        if ((memicmp(full,basedir,bl)==0)&&isPathSepChar(full[bl]))
            full += bl+1;
    }
    const char *t = full;
    for (;;) {
        const char *n = findPathSepChar(t);
        if (!n) 
            break;
        t = n+1;
    }
    if (t!=full) 
        reldir.append(t-full,full);
    return t;
}

const char *getRelativePath(const char *path,const char *leadingPath)
{
    size_t pathLen = strlen(path);
    size_t leadingLen = strlen(leadingPath);
    if ((pathLen==leadingLen-1)&&isPathSepChar(leadingPath[leadingLen-1]))
        --leadingLen;
    if (0 == strncmp(path,leadingPath,leadingLen))
    {
        const char *rel = path + leadingLen;
        if ('\0' == *rel)
            return rel;
        if (isPathSepChar(*rel))
            return rel+1;
        return rel;
    }
    return nullptr;
}

const char *splitDirMultiTail(const char *multipath,StringBuffer &dir,StringBuffer &tail)
{
    // the first directory is the significant one
    // others only removed if same
    dir.clear();
    StringArray files;
    RemoteMultiFilename::expand(multipath,files);
    StringBuffer reldir;
    ForEachItemIn(i,files) {
        const char *s = files.item(i);
        if (i==0) {
            if (isAbsolutePath(s)) {
                StringAttr tail(splitDirTail(s,dir));
                if (dir.length())
                    files.replace(tail,i);
            }
        }
        else if (dir.length()) {
            s= splitRelativePath(s,dir.str(),reldir.clear());
            if (reldir.length()) {
                reldir.append(s);
                files.replace(reldir.str(),i);
            }
        }
    }
    RemoteMultiFilename::tostr(files,tail);
    return tail.str();
}

StringBuffer &mergeDirMultiTail(const char *dir,const char *tail, StringBuffer &multipath)
{
    StringArray files;
    RemoteMultiFilename::expand(tail,files);
    StringBuffer reldir;
    if (dir && *dir) {
        ForEachItemIn(i,files) {
            const char *s = files.item(i);
            if (!isAbsolutePath(s)) {
                reldir.clear().append(dir);
                addPathSepChar(reldir).append(s);
                files.replace(reldir.str(),i);
            }
        }
    }
    RemoteMultiFilename::tostr(files,multipath);
    return multipath;

}

StringBuffer &removeRelativeMultiPath(const char *full,const char *reldir,StringBuffer &res)
{
    StringArray files;
    RemoteMultiFilename::expand(full,files);
    StringBuffer tmp1;
    StringBuffer tmp2;
    StringBuffer dir;
    ForEachItemIn(i,files) {
        const char *s = files.item(i);
        if (isAbsolutePath(s)) {
            if (!dir.length()) 
                splitDirTail(s,dir);
        }
        else if (dir.length()) {
            tmp1.clear().append(dir);
            addPathSepChar(tmp1).append(s);
            s = tmp1.str();
        }
        s = splitRelativePath(s,reldir,tmp2.clear());
        tmp2.append(s);
        files.replace(tmp2.str(),i);
    }
    RemoteMultiFilename::tostr(files,res);
    return res;
}
// removes basedir if matches, returns relative multipath


//===================================================================================================

ExtractedBlobInfo::ExtractedBlobInfo(const char * _filename, offset_t _length, offset_t _offset)
{
    filename.set(_filename);
    length = _length;
    offset = _offset;
}

void ExtractedBlobInfo::serialize(MemoryBuffer & buffer)
{
    ::serialize(buffer, filename.get());
    buffer.append(length).append(offset);
}

void ExtractedBlobInfo::deserialize(MemoryBuffer & buffer)
{
    ::deserialize(buffer, filename);
    buffer.read(length).read(offset);
}

//----------------------------------------------------------------------------

#define DFTERR_InvalidSplitPrefixFormat         8091
#define DFTERR_InvalidSplitPrefixFormat_Text    "Cannot process file %s using the splitprefix supplied"

static void * readLength(MemoryBuffer & buffer, IFileIOStream * in, size_t len, const char * filenameText)
{
    void * ptr = buffer.clear().reserve(len);
    if (in->read(len, ptr) != len)
        throwError1(DFTERR_InvalidSplitPrefixFormat, filenameText);
    return ptr;
}

void extractBlobElements(const char * prefix, const RemoteFilename &filename, ExtractedBlobArray & extracted)
{
    StringBuffer filenameText;
    filename.getPath(filenameText);
    Owned<IFile> file = createIFile(filename);
    Owned<IFileIO> inIO = file->open(IFOread);
    if (!inIO)
        throw MakeStringException(-1, "extractBlobElements: file '%s' not found", filenameText.str());
    Owned<IFileIOStream> in = createIOStream(inIO);

    MemoryBuffer buffer;
    offset_t endOffset = in->size();
    while (in->tell() != endOffset)
    {
        StringAttr blobFilename;
        offset_t blobLength = (offset_t)-1;
        const char * finger = prefix;
        while (finger)
        {
            StringAttr command;
            const char * comma = strchr(finger, ',');
            if (comma)
            {
                command.set(finger, comma-finger);
                finger = comma+1;
            }
            else
            {
                command.set(finger);
                finger = NULL;
            }

            command.toUpperCase();
            if (memcmp(command, "FILENAME", 8) == 0)
            {
                if (command[8] == ':')
                {
                    unsigned maxLen = atoi(command+9);
                    const char * nameptr = (const char *)readLength(buffer, in, maxLen, filenameText.str());
                    blobFilename.set(nameptr, maxLen);
                }
                else
                {
                    unsigned * lenptr = (unsigned *)readLength(buffer, in, sizeof(unsigned), filenameText.str());
#if __BYTE_ORDER != __LITTLE_ENDIAN
                    _rev(sizeof(*lenptr), lenptr);
#endif
                    unsigned filenamelen = *lenptr;
                    const char * nameptr = (const char *)readLength(buffer, in, filenamelen, filenameText.str());
                    blobFilename.set(nameptr, filenamelen);
                }
            }
            else if ((memcmp(command, "FILESIZE", 8) == 0) || (command.length() == 2))
            {
                const char * format = command;
                if (memcmp(format, "FILESIZE", 8) == 0)
                {
                    if (format[8] == ':')
                        format = format+9;
                    else
                        format = "L4";
                }

                bool bigEndian;
                char c = format[0];
                if (c == 'B')
                    bigEndian = true;
                else if (c == 'L')
                    bigEndian = false;
                else
                    throwError1(DFTERR_InvalidSplitPrefixFormat, format);
                c = format[1];
                if ((c <= '0') || (c > '8'))
                    throwError1(DFTERR_InvalidSplitPrefixFormat, format);

                unsigned length = (c - '0');
                byte * lenptr = (byte *)readLength(buffer, in, length, filenameText.str());
                if (!bigEndian)
                    _rev(length, lenptr);

                blobLength = 0;
                for (unsigned i=0; i<length; i++)
                {
                    blobLength = (blobLength << 8) | lenptr[i];
                }
            }
            else if (memcmp(command, "SKIP:", 5) == 0)
            {
                unsigned skipLen = atoi(command+5);
                in->seek(in->tell()+skipLen, IFSbegin);
            }
            else if (memcmp(command, "SEQ:", 4) == 0)
            {
                unsigned skipLen = atoi(command+4);
                in->seek(in->tell()+skipLen, IFSbegin);
            }
            else
                throwError1(DFTERR_InvalidSplitPrefixFormat, command.get());
        }

        if ((blobLength == (offset_t)-1) || !blobFilename.get())
            throwError1(DFTERR_InvalidSplitPrefixFormat, filenameText.str());

        offset_t blobOffset = in->tell();
        extracted.append(* new ExtractedBlobInfo(blobFilename, blobLength, blobOffset));
        in->seek(blobOffset + blobLength, IFSbegin);
    }
}

bool mountDrive(const char *drv,const RemoteFilename &rfn)
{
#ifdef _WIN32
    return false;
#else
    unmountDrive(drv);
    localCreateDirectory(drv);
    int ret;
    for (unsigned vtry=0;vtry<2;vtry++) {
        StringBuffer cmd;
        cmd.append("mount ");
        rfn.queryIP().getHostText(cmd);
        cmd.append(':');
        rfn.getLocalPath(cmd);
        cmd.append(' ').append(drv).append(" -t nfs ");
        if (vtry==0)
            cmd.append("-o nfsvers=v3 "); // prefer v3
        cmd.append("2> /dev/null");
        ret = system(cmd.str());
        if (ret==0)
            break;
    }
    return (ret==0);
#endif

}

bool unmountDrive(const char *drv)
{
#ifdef _WIN32
    return false;
#else
    StringBuffer cmd;
    cmd.append("umount ").append(drv).append(" 2> /dev/null");
    int ret = system(cmd.str());
    return (ret==0);
#endif

}

IFileIO *createUniqueFile(const char *dir, const char *prefix, const char *ext, StringBuffer &filename, IFOmode mode)
{
    CDateTime dt;
    dt.setNow();
    unsigned t = (unsigned)dt.getSimple();
    unsigned attempts = 5; // max attempts
    if (!ext || !*ext)
        ext = "tmp";
    for (;;)
    {
        filename.clear();
        if (dir)
        {
            filename.append(dir);
            addPathSepChar(filename);
        }
        if (prefix && *prefix)
            filename.append(prefix);
        else
            filename.append("uniq");
        filename.appendf("_%" I64F "x.%x.%x.%s", (__int64)GetCurrentThreadId(), (unsigned)GetCurrentProcessId(), t, ext);
        OwnedIFile iFile = createIFile(filename.str());
        if (!iFile->exists())
        {
            try { return iFile->openShared(mode, IFSHnone); } // NB: could be null if path not found
            catch (IException *e)
            {
                EXCLOG(e, "createUniqueFile");
                e->Release();
            }
        }
        if (0 == --attempts)
            return nullptr;
        t += getRandom();
    }
}

IFile * writeToProtectedTempFile(const char * component, const char * prefix, size_t len, const void * data)
{
    Owned<IFile> protectedFile;
    StringBuffer tempDir;
    getTempFilePath(tempDir, component, nullptr);

    StringBuffer tempFilename;
    OwnedIFileIO io = createUniqueFile(tempDir, prefix, NULL, tempFilename);
    io->write(0, len, data);
    io->close();

    //Prevent any other users from accessing the file
    protectedFile.setown(createIFile(tempFilename));
    protectedFile->setFilePermissions(0700);

    return protectedFile.getClear();
}

unsigned sortDirectory( CIArrayOf<CDirectoryEntry> &sortedfiles,
                        IDirectoryIterator &iter, 
                        SortDirectoryMode mode,
                        bool rev,
                        bool includedirs
                      ) 
{
    sortedfiles.kill();
    StringBuffer name;
    ForEach(iter) {
        if (!iter.isDir()||includedirs) 
            sortedfiles.append(*new CDirectoryEntry(iter));
    }
    if (mode!=SD_nosort) {
        struct icmp: implements ICompare
        {
            SortDirectoryMode mode;
            bool rev;
            int docompare(const void *l,const void *r) const
            {
                int ret=0;
                const CDirectoryEntry *dl = (const CDirectoryEntry *)l;
                const CDirectoryEntry *dr = (const CDirectoryEntry *)r;
                switch (mode) {
                case SD_byname:
                    ret = strcmp(dl->name,dr->name);
                    break;
                case SD_bynameNC:
                    ret = stricmp(dl->name,dr->name);
                    break;
                case SD_bydate:
                    ret = dl->modifiedTime.compare(dr->modifiedTime);
                    break;
                case SD_bysize:
                    ret = (dl->size>dr->size)?1:((dl->size<dr->size)?-1:0);
                    break;
                }
                if (rev)
                    ret = -ret;
                return ret;
            }
        } cmp;
        cmp.mode = mode;
        cmp.rev = rev;
        qsortvec((void **)sortedfiles.getArray(), sortedfiles.ordinality(), cmp);
    }
    return sortedfiles.ordinality();
}


class CReplicatedFile : implements IReplicatedFile, public CInterface
{
    RemoteFilenameArray copies;
public:
    IMPLEMENT_IINTERFACE;

    RemoteFilenameArray &queryCopies()
    {
        return copies;
    }

    
    IFile *open()
    {
        StringBuffer locations;
        Owned<IException> exc;
        ForEachItemIn(copy,copies) {
            const RemoteFilename &rfn = copies.item(copy);
            try {
                OwnedIFile iFile = createIFile(rfn);
                if (iFile->exists())
                    return iFile.getClear();
                if (locations.length())
                    locations.append(", ");
                rfn.getRemotePath(locations);
            }
            catch(IException *e) {
                EXCLOG(e,"CReplicatedFile::open");
                if (exc)
                    e->Release();
                else
                    exc.setown(e);
            }
        }
        if (exc.get())
            throw exc.getClear();
        throw MakeStringException(0, "%s: Failed to open part file at any of the following locations: ", locations.str());
    }
};

IReplicatedFile *createReplicatedFile()
{
    return new CReplicatedFile;
}


// ---------------------------------------------------------------------------------

class CSimpleInputStream : public CInterfaceOf<ISerialInputStream>
{
public:
    virtual void reset(offset_t _offset, offset_t _flen)
    {
        throwUnimplementedX("reset() not supported by this input stream");
    }

    //MORE: This should possibly be a global helper function instead of a member - the logic should be the same for all stream instances.
    virtual void get(size32_t len, void * ptr) override
    {
        assertex(len);
        size32_t totalRead = 0;
        //Keep reading until either the whole requested size is returned, or 0 is return from read() i.e. no more data.
        for (;;)
        {
            size32_t sizeRead = read(len - totalRead, (byte *)ptr + totalRead);
            if (sizeRead == 0)
                break;
            totalRead += sizeRead;
            if (totalRead == len)
                return;
        }

        Owned<IException> e = makeStringExceptionV(-1, "InputStream::get read past end of stream (%u,%u) @offset %llu", (unsigned)len, (unsigned)totalRead, tell()-totalRead);
        ERRLOG(e);
        throw e.getClear();
    }

    // A very poor base implementation - suitable for small data streams that reads and discards data
    virtual void skip(size32_t sz)
    {
        constexpr size_t tempSkipSize = 0x4000;
        byte tempSkipBuffer[tempSkipSize];
        while (sz > tempSkipSize)
        {
            read(tempSkipSize, tempSkipBuffer);
            sz -= tempSkipSize;
        }

        if (sz)
            read(sz, tempSkipBuffer);
    }
};

// ---------------------------------------------------------------------------------

IBufferedSerialInputStream *createFileSerialStream(IFileIO *fileio,offset_t ofs, offset_t flen, size32_t bufsize)
{
    if (!fileio)
        return NULL;

    Owned<ISerialInputStream> fileStream = createSerialInputStream(fileio, ofs, flen);
    return createBufferedInputStream(fileStream, bufsize);
}

//---------------------------------------------------------------------------------------------------------------------

class CSimpleSocketInputStream final : public CSimpleInputStream
{
public:
    CSimpleSocketInputStream(ISocket * _socket, unsigned _timeout)
    : socket(_socket), timeout(_timeout)
    {
    }

    virtual size32_t read(size32_t len, void * ptr) override
    {
        if (closed)
            return 0;
        size32_t size_read;
        closed = readtmsAllowClose(socket, ptr, 1, len, size_read, timeout);
        lastpos += size_read;
        return size_read;
    }
    virtual offset_t tell() const override
    {
        return lastpos;
    }
protected:
    Linked<ISocket> socket;
    offset_t lastpos = 0;
    unsigned timeout;
    bool closed = false;
};


//NOTE: This class/factory method is not currently used.  It is here as an example.
ISerialInputStream *createSocketSerialStream(ISocket * socket, unsigned timeoutms, size32_t bufsize)
{
    if (!socket)
        return NULL;
    //MORE: This should probably be wrapped in a buffer if it was actually used.
    return new CSimpleSocketInputStream(socket,timeoutms);
}

//---------------------------------------------------------------------------------------------------------------------

class CSimpleReadInputStream final : public CSimpleInputStream
{
public:
    CSimpleReadInputStream(ISimpleReadStream * _input)
    : input(_input)
    {
    }

    virtual size32_t read(size32_t len, void * ptr) override
    {
        size32_t rd = input->read(len, ptr);
        lastpos += rd;
        return rd;
    }
    virtual offset_t tell() const override
    {
        return lastpos;
    }
protected:
    Linked<ISimpleReadStream> input;
    offset_t lastpos = 0;
};


//NOTE: This class/factory method is not currently used.  It is here as an example.
IBufferedSerialInputStream * createSimpleSerialStream(ISimpleReadStream * input, size32_t bufsize)
{
    if (!input)
        return NULL;
    Owned<ISerialInputStream> stream = new CSimpleReadInputStream(input);
    return createBufferedInputStream(stream, bufsize);
}

//---------------------------------------------------------------------------------------------------------------------
class CMemoryMappedSerialStream: implements IBufferedSerialInputStream, public CInterface
{
    Linked<IMemoryMappedFile> mmfile;
    const byte *mmbase;
    memsize_t mmsize;
    memsize_t mmofs;
    bool eoinput;

public:
    IMPLEMENT_IINTERFACE;
    CMemoryMappedSerialStream(IMemoryMappedFile *_mmfile, offset_t ofs, offset_t flen)
        : mmfile(_mmfile)
    {
        offset_t fs = mmfile->fileSize();
        if ((memsize_t)fs!=fs)
            throw MakeStringException(-1,"CMemoryMappedSerialStream file too big to be mapped");
        if ((flen!=(offset_t)-1)&&(fs>flen))
            fs = flen;
        mmsize = (memsize_t)fs;
        mmofs = (memsize_t)((ofs<fs)?ofs:fs);
        mmbase = (const byte *)mmfile->base();
        eoinput = false;
    }

    virtual void reset(offset_t _ofs, offset_t _len) override
    {
        offset_t fs = mmfile->fileSize();
        if ((_len!=(offset_t)-1)&&(fs>_len))
            fs = _len;
        mmsize = (memsize_t)fs;
        mmofs = (memsize_t)((_ofs<fs)?_ofs:fs);
        mmsize = (memsize_t)fs;
        eoinput = false;
    }

    CMemoryMappedSerialStream(const void *buf, memsize_t len)
    {
        mmsize = len;
        mmofs = 0;

        mmbase = (const byte *)buf;
        eoinput = false;
    }

    virtual const void *peek(size32_t sz,size32_t &got) override
    {
        memsize_t left = mmsize-mmofs;
        if (sz>left)
            sz = (size32_t)left;
        else if (left>=UINT_MAX) 
            got = UINT_MAX-1;
        else
            got = (size32_t)left;
        return mmbase+mmofs;
    }

    virtual void get(size32_t len, void * ptr) override
    {
        memsize_t left = mmsize-mmofs;
        if (len>left) {
            PrintStackReport();
            IERRLOG("CFileSerialStream::get read past end of stream.3 (%u,%u)",(unsigned)len,(unsigned)left);
            throw MakeStringException(-1,"CMemoryMappedSerialStream::get read past end of stream (%u,%u)",(unsigned)len,(unsigned)left);
        }
        memcpy(ptr,mmbase+mmofs,len);
        mmofs += len;
    }

    virtual size32_t read(size32_t len, void * ptr) override
    {
        memsize_t left = mmsize-mmofs;
        if (len>left)
            len = left;
        memcpy(ptr,mmbase+mmofs,len);
        mmofs += len;
        return len;
    }

    virtual bool eos() override
    {
        return (mmsize<=mmofs);
    }

    virtual void skip(size32_t len) override
    {
        memsize_t left = mmsize-mmofs;
        if (len>left)
            throw MakeStringException(-1,"CMemoryMappedSerialStream::skip read past end of stream (%u,%u)",(unsigned)len,(unsigned)left);
        mmofs += len;
    }

    virtual offset_t tell() const override
    {
        return mmofs;
    }

};

IBufferedSerialInputStream *createFileSerialStream(IMemoryMappedFile *mmfile, offset_t ofs, offset_t flen)
{
    return new CMemoryMappedSerialStream(mmfile,ofs,flen);
}

IBufferedSerialInputStream *createMemorySerialStream(const void *buffer, memsize_t len)
{
    return new CMemoryMappedSerialStream(buffer,len);
}

class CMemoryBufferSerialStream: implements IBufferedSerialInputStream, public CInterface
{
    MemoryBuffer & buffer;

public:
    IMPLEMENT_IINTERFACE;
    CMemoryBufferSerialStream(MemoryBuffer & _buffer)
        : buffer(_buffer)
    {
    }

    virtual const void *peek(size32_t sz,size32_t &got) override
    {
        got = buffer.remaining();
        return buffer.readDirect(0);
    }

    virtual void get(size32_t len, void * ptr) override
    {
        if (len>buffer.remaining()) {
            IERRLOG("CMemoryBufferSerialStream::get read past end of stream.4(%u,%u)",(unsigned)len,(unsigned)buffer.remaining());
            throw MakeStringException(-1,"CMemoryBufferSerialStream::get read past end of stream (%u,%u)",(unsigned)len,(unsigned)buffer.remaining());
        }
        const void * data = buffer.readDirect(len);
        memcpy(ptr,data,len);
    }

    virtual size32_t read(size32_t len, void * ptr) override
    {
        if (len>buffer.remaining())
            len = buffer.remaining();
        const void * data = buffer.readDirect(len);
        memcpy(ptr,data,len);
        return len;
    }

    virtual bool eos() override
    {
        return buffer.remaining() == 0;
    }

    virtual void skip(size32_t len) override
    {
        if (len>buffer.remaining())
            throw MakeStringException(-1,"CMemoryBufferSerialStream::skip read past end of stream (%u,%u)",(unsigned)len,(unsigned)buffer.remaining());

        buffer.skip(len);
    }

    virtual offset_t tell() const override
    {
        return buffer.getPos();
    }

    virtual void reset(offset_t _offset,offset_t _len) override
    {
        size32_t ofs = (size32_t)_offset;
        assertex(ofs==_offset);
        assertex((_len==(offset_t)-1)||(_len>=buffer.length())); // don't support len on memory buffer
        buffer.reset(ofs);
    }
};

IBufferedSerialInputStream *createMemoryBufferSerialStream(MemoryBuffer & buffer)
{
    return new CMemoryBufferSerialStream(buffer);
}



// Memory Mapped Files

#define MEMORYMAP_PAGESIZE  (0x1000) // could be different but won't ever be!

class CMemoryMappedFile: implements IMemoryMappedFile, public CInterface
{
    byte *ptr;            // base
    offset_t ofs;       
    offset_t realofs;    // rounded down to page size
    offset_t filesize;
    memsize_t size;
    bool writeaccess;          
    memsize_t windowsize;
    
    HANDLE hfile;
#ifdef _WIN32
    static size32_t pagesize;
    HANDLE hmap;
#endif  

    Linked<IFile> file;

    inline offset_t pageround(offset_t o)
    {
#ifdef _WIN32
        return o-(o%pagesize);
#else
        return o-(o%MEMORYMAP_PAGESIZE);
#endif
    }

    inline void * realptr()
    {
        return ptr?(ptr-(ofs-realofs)):NULL;
    }

    inline memsize_t realsize()
    {
        return (memsize_t)(size+(ofs-realofs));
    }

public:
    IMPLEMENT_IINTERFACE;

    CMemoryMappedFile(HANDLE _hfile, offset_t _filesize, offset_t _ofs, memsize_t _size, bool _writeaccess)
    {
        hfile = _hfile;
#ifdef _WIN32
        if (pagesize==0) {
            SYSTEM_INFO sysinfo;
            GetSystemInfo(&sysinfo);
            pagesize =  sysinfo.dwAllocationGranularity;
        }
        hmap = NULLFILE;
#endif
        ptr = NULL;
        filesize = _filesize;
        windowsize = _size;     // first open size assumed to be window size
        reinit(_ofs, _size, _writeaccess);
    }

    ~CMemoryMappedFile()
    {
#ifdef _WIN32
        if (hmap!=NULLFILE)
            CloseHandle(hmap);
        CloseHandle(hfile);
#else
        close(hfile);
#endif
#if defined(__linux__)
        if (ptr)
        {
            munmap(realptr(),realsize());
            ptr = NULL;
        }
#endif
    }

    byte *base()                        { return ptr; }
    offset_t offset()                   { return ofs; }
    virtual memsize_t length()          { return size; }
    virtual offset_t fileSize()         { return filesize; }
    virtual int compareWithin(const void *p)
    {
        if (p<ptr)
            return -1;
        if (p>=size+ptr)
            return 1;
        return 0;
    }
    bool writeAccess()                  { return writeaccess; }
    void flush()
    {
        if (ptr) {
#ifdef _WIN32
            FlushViewOfFile(realptr(),0);
#elif defined(__linux__)
            msync(realptr(),realsize(),MS_SYNC);
#else
            UNIMPLEMENTED;
#endif
        }
    }
    
    byte *nextPtr(const void *p,offset_t skip, memsize_t req, memsize_t &got)
    {
        // for scanning in sequence
        if (p==NULL) 
            p = ptr;
        else if ((p<ptr)||(p>ptr+size))
            throw MakeStringException(-1,"CMemoryMappedFile::nextPtr - outside map");           
        memsize_t d = (byte *)p-ptr;
        offset_t o = ofs+d+skip;
        if (o>=filesize) {
            got = 0;
            return NULL;
        }
        offset_t left = filesize-o;
        if (left<req)
            req = (memsize_t)left;
        if (o+req-ofs>size) {
            reinit(o,windowsize,writeaccess);
            assertex(o==ofs);
            got = windowsize;
            if (left<got)
                got = (memsize_t)left;
            return ptr;
        }
        got = (memsize_t)(size+o-ofs);
        if (left<got)
            got = (memsize_t)left;
        return (byte *)p+skip;
    }
        
    virtual void reinit(offset_t _ofs, memsize_t _size, bool _writeaccess)
    {
        writeaccess = _writeaccess;
        if (ptr) {
#ifdef _WIN32
            // no need to unmap view
            if (hmap != INVALID_HANDLE_VALUE) {
                CloseHandle(hmap);
                hmap = INVALID_HANDLE_VALUE;
            }
#elif defined(__linux__)
            munmap(realptr(),realsize());
            // error checking TBD
#else
            UNIMPLEMENTED;
#endif
            ptr = NULL;
        }
        if (_ofs>filesize)
            ofs = filesize;
        else
            ofs = _ofs;
        realofs = pageround(ofs);
        if (_size==(memsize_t)-1) {
            size = (memsize_t)(filesize-_ofs);
            if (size!=(filesize-_ofs)) 
                throw MakeStringException(-1,"CMemoryMappedFile::reinit file too big");         
        }
        else
            size = _size;
        memsize_t mapsz = realsize();
        if (filesize-realofs<mapsz)
            mapsz = (memsize_t)(filesize-realofs);
#ifdef _WIN32
        LARGE_INTEGER li;
        if (hmap == INVALID_HANDLE_VALUE) {
            hmap = CreateFileMapping(hfile,NULL,writeaccess?PAGE_READWRITE:PAGE_READONLY,0, 0, NULL);
            if (!hmap) {
                DWORD err = GetLastError();
                throw makeOsException(err,"CMemoryMappedFile::reinit");
            }
        }
        li.QuadPart = realofs; 
        ptr = (byte *) MapViewOfFile(hmap, writeaccess?(FILE_MAP_READ|FILE_MAP_WRITE):FILE_MAP_READ, li.HighPart, li.LowPart, mapsz);
        if (!ptr) {
            DWORD err = GetLastError();
            throw makeOsException(err,"CMemoryMappedFile::reinit");
        }
#elif defined (__linux__)
        ptr = (byte *) mmap(NULL, mapsz, writeaccess?(PROT_READ|PROT_WRITE):PROT_READ, MAP_SHARED|MAP_NORESERVE, hfile, realofs);
        if (ptr == MAP_FAILED)
            throw makeOsException(errno, "CMemoryMappedFile::reinit");
#else
        UNIMPLEMENTED;
#endif
        if (ptr)
            ptr += (ofs-realofs);
    }


};

#ifdef _WIN32
size32_t CMemoryMappedFile::pagesize=0;
#endif

// TBD MADV_SEQUENTIAL & MADV_WILLNEED?



IMemoryMappedFile *CFile::openMemoryMapped(offset_t ofs, memsize_t len, bool write)
{
    HANDLE hfile = openHandle(write?IFOcreaterw:IFOread, IFSHread, false);
    if (hfile == NULLFILE) 
        return NULL;
    return new CMemoryMappedFile(hfile,size(),ofs,len,write);
}


bool isSpecialPath(const char *path)
{
    // used for remote queries
    if (!path)
        return false;
    if (isPathSepChar(path[0])&&(path[0]==path[1])) {
        path += 2;
        for (;;) {
            if (*path=='/') 
                return (path[1]=='>');
            if (!*path||(*path=='\\'))
                return false;
            path++;
        }
    }
    return (path&&(*path=='/')&&(path[1]=='>'));
}


size32_t SendFile(ISocket *target, IFileIO *fileio,offset_t start,size32_t len)
{
    assertex(target);
    assertex(fileio);
    offset_t fsz = fileio->size();
    if (start>=fsz)
        return 0;
    if (start+len>fsz)
        len = (size32_t)(fsz-start);
    if (!len)
        return 0;
    CFileIO *cfile = QUERYINTERFACE(fileio,CFileIO);
    if (cfile) {
        HANDLE fh = cfile->queryHandle();
        if ((fh!=(HANDLE)0)&&(fh!=(HANDLE)-1)) {
            unsigned sh = target->OShandle();
            if ((sh!=(unsigned)0)&&(sh!=(unsigned)-1)) {
#ifdef _WIN32
                // MORE - should there be something here?
#elif defined(__linux__)
                // TransmitFile not in std DLLs so don't bother with
                off_t ofs = start;
                ssize_t sent = ::sendfile(sh,fh,&ofs,len);
                if (sent!=(ssize_t)-1) 
                    return (size32_t)sent;
                int err = errno;
                if ((err!=EINVAL)&&(err!=ENOSYS))
                    throw makeOsException(err, "sendfile");
#else
                UNIMPLEMENTED;
#endif
            }
        }

    }

#ifdef _DEBUG
#ifndef _WIN32
    IWARNLOG("SendFile falling back");
#endif
#endif

    // fallback
    MemoryAttr ma;
    void *buf = ma.allocate(len);
    size32_t rd = fileio->read(start,len,buf);
    target->write(buf,rd);
    return rd;
}


void asyncClose(IFileIO *io)
{
    if (!io)
        return;
    static CriticalSection ADsect;
    CriticalBlock block(ADsect);
    static Owned<IWorkQueueThread> adwp = createWorkQueueThread();
    class cWQI: public CInterface,implements IWorkQueueItem
    {
        Owned<IFileIO> io; 
    public:
        IMPLEMENT_IINTERFACE;
        cWQI(IFileIO *_io)
            : io(_io)
        {
        }

        void execute()
        {
            io.clear();
        }
    };
    adwp->post(new cWQI(io));
};

int stdIoHandle(const char *path)
{
    if (strcmp(path,"stdin:")==0)
        return 0;
    if (strcmp(path,"stdout:")==0)
        return 1;
    if (strcmp(path,"stderr:")==0)
        return 2;
    return -1;
}

extern jlib_decl bool containsFileWildcard(const char * path)
{
    if (!path)
        return false;
    return strchr(path, '*') || strchr(path, '?');
}

extern jlib_decl bool isDirectory(const char * path)
{
    Owned<IFile> file = createIFile(path);
    return file->isDirectory()==fileBool::foundYes;
}

// IFileIOCache

class CLazyFileIOCache;

class CCachedFileIO: implements IFileIO, public CInterface
{
    CLazyFileIOCache &owner;
    Owned<IFile> inFile;
    RemoteFilename filename;
    CriticalSection &sect;
    CRuntimeStatisticCollection fileStats;
    IFOmode mode;

    void writeNotSupported(const char *s)
    {
        StringBuffer tmp;
        filename.getRemotePath(tmp);
        throw MakeStringException(-1, "CCachedFileIO(%s) %s not supported", tmp.str(), s);
    }
public:
    unsigned accesst;
    Owned<IFileIO> cachedio;


    CCachedFileIO(CLazyFileIOCache &_owner, CriticalSection &_sect, RemoteFilename &_filename, IFOmode _mode)
        : owner(_owner), sect(_sect), fileStats(diskLocalStatistics)
    {
        filename.set(_filename);
        mode = _mode;
        accesst = 0;
        inFile.setown(createIFile(filename));
    }

    virtual void Link(void) const       { CInterface::Link(); }                     \

    virtual bool Release(void) const;
    
    unsigned __int64 getStatistic(StatisticKind kind) override
    {
        CriticalBlock block(sect);
        unsigned __int64 openValue = cachedio ? cachedio->getStatistic(kind) : 0;
        return openValue + fileStats.getStatisticValue(kind);
    }

    IFileIO *open();

    virtual size32_t read(offset_t pos, size32_t len, void * data)
    {
        CriticalBlock block(sect);
        Owned<IFileIO> io = open();
        return io->read(pos,len,data);
    }
    virtual offset_t size() override
    {
        CriticalBlock block(sect);
        Owned<IFileIO> io = open();
        return io->size();
    }
    virtual size32_t write(offset_t pos, size32_t len, const void * data) override
    {
        CriticalBlock block(sect);
        Owned<IFileIO> io = open();
        return io->write(pos,len,data);
    }
    virtual void flush() override
    {
        CriticalBlock block(sect);
        if (cachedio)
            cachedio->flush();
    }
    virtual void close() override
    {
        CriticalBlock block(sect);
        if (cachedio)
            forceClose();
    }
    virtual IFile * queryFile() const override
    {
        return inFile;
    }
    void setSize(offset_t size) 
    {
        CriticalBlock block(sect);
        Owned<IFileIO> io = open();
        io->setSize(size);
    }

    void forceClose()
    {
        cachedio->close();
        mergeStats(fileStats, cachedio);
        cachedio.clear();
    }
};

class CLazyFileIOCache: implements IFileIOCache, public CInterface
{
    CriticalSection sect;
    unsigned max;
    IPointerArrayOf<CCachedFileIO> cache;
public:
    IMPLEMENT_IINTERFACE;

    CLazyFileIOCache(unsigned _max)
    {
        max = _max;
    }
    CCachedFileIO *addFile( RemoteFilename  &filename, IFOmode mode )
    {
        // check exists
        unsigned i = cache.ordinality();
        ForEachItemIn(i2,cache) {
            if (!cache.item(i2)) {
                i = i2;
                break;
            }
        }
        Linked<CCachedFileIO> ret = new CCachedFileIO(*this,sect,filename,mode);
        if (i<cache.ordinality())
            cache.replace(ret,i);
        else
            cache.append(ret);
        return ret.getClear();
    }

    void checkCache(unsigned wanted=1)
    {
        // called in sect
        for (;;) {
            CCachedFileIO *oldest = NULL;
            unsigned oldestt = 0;
            unsigned t = msTick();
            unsigned n = wanted;
            ForEachItemIn(i1,cache) {
                CCachedFileIO *cio = cache.item(i1);
                if (cio&&cio->cachedio.get()) {
                    if (t-cio->accesst>=oldestt) {
                        oldest = cio;
                        oldestt = t-cio->accesst;
                    }
                    n++;
                }
            }
            if (n<max)
                break;
            if (!oldest)
                break;
            oldest->forceClose();
            //If previously had max ios then we now have space.
            if (n == max)
                break;
        }
    }

    const CCachedFileIO *removeFile(const CCachedFileIO *io) 
    {
        CriticalBlock block(sect);
        ForEachItemIn(idx, cache)
        {
            if (io == cache.item(idx))
            {
                cache.replace(NULL, idx, true);
                break;
            }
        }
        return io;
    }
};


bool CCachedFileIO::Release(void) const 
{ 
    if (CInterface::Release())
        return true;
    if (!CInterface::IsShared()) 
        return owner.removeFile(this)->Release();
    return false;
}

IFileIO *CCachedFileIO::open()
{
    // called in sect
    if (!cachedio) {
        owner.checkCache();
        cachedio.setown(inFile->open(mode));
        if (!cachedio.get()) {
            StringBuffer tmp;
            filename.getRemotePath(tmp);
            throw MakeStringException(-1, "CCachedFileIO::open(%d) '%s' failed", (int)mode, tmp.str());
        }
    }
    accesst = msTick();
    return cachedio.getLink();
}


IFileIOCache* createFileIOCache(unsigned max)
{
    return new CLazyFileIOCache(max);
}


extern jlib_decl IFile * createSentinelTarget(const char *suffix)
{
    const char * sentinelFilename = getenv("SENTINEL");
    if (sentinelFilename && *sentinelFilename)
    {
        VStringBuffer usename("%s%s", sentinelFilename, suffix ? suffix : "");
        return createIFile(usename);
    }
    else
        return NULL;
}

extern jlib_decl void removeSentinelFile(IFile * sentinelFile)
{
    if (sentinelFile)
    {
        if(sentinelFile->exists() && sentinelFile->isDirectory()!=fileBool::foundYes)
        {
            DBGLOG("Removing sentinel file %s", sentinelFile->queryFilename());
            try
            {
                sentinelFile->remove();
            }
            catch(IException *E)
            {
                StringBuffer s;
                EXCLOG(E, s.appendf("Failed to remove sentinel file %s", sentinelFile->queryFilename()).str());
                E->Release();
                throw makeOsException(errno, "removeSentinelFile - file cannot be removed.");
            }
        }
    }
}

extern jlib_decl void writeSentinelFile(IFile * sentinelFile)
{
    if ( sentinelFile )
    {
        DBGLOG("Creating sentinel file %s", sentinelFile->queryFilename());
        try
        {
            Owned<IFileIO> sentinel = sentinelFile->open(IFOcreate);
            sentinel->write(0, 5, "rerun");
            sentinel->close();
        }
        catch(IException *E)
        {
            StringBuffer s;
            EXCLOG(E, s.appendf("Failed to create sentinel file %s", sentinelFile->queryFilename()).str());
            E->Release();
            throw makeOsException(errno, "writeSentinelFile - file not created.");
        }
    }
}

jlib_decl StringBuffer & appendCurrentDirectory(StringBuffer & target, bool blankIfFails)
{
    char temp[_MAX_PATH+1];
    if (!getcwd(temp,sizeof(temp)))
    {
        if (blankIfFails)
            return target;
        throw MakeStringException(JLIBERR_InternalError, "getcwd failed (%d)", errno);
    }
    return target.append(temp);
}

void removeFileTraceIfFail(const char * filename)
{
    if (remove(filename) != 0)
        DBGLOG("Could not remove file '%s'", filename);
}

timestamp_type getTimeStamp(IFile * file)
{
    CDateTime modified;
    file->getTime(nullptr, &modified, nullptr);
    return modified.getTimeStamp();
}

class CSortedDirectoryIterator : public CSimpleInterfaceOf<IDirectoryIterator>
{
    CIArrayOf<CDirectoryEntry> sortedFiles;
    CDirectoryEntry *curDirectoryEntry = nullptr;
    unsigned        sortedFileCount = 0;
    unsigned        sortedFileIndex = 0;
    Owned<IFile>    cur;
    bool            curIsDir = false;

public:
    CSortedDirectoryIterator(IDirectoryIterator &itr, SortDirectoryMode mode, bool rev, bool includedirs)
    {
        sortDirectory(sortedFiles, itr, mode, rev, includedirs);
        sortedFileCount = sortedFiles.length();
    }

    //IIteratorOf
    virtual bool first() override
    {
        sortedFileIndex = 0;
        return next();
    }
    virtual bool next() override
    {
        if (sortedFileIndex >= sortedFileCount)
        {
            cur.clear();
            curDirectoryEntry = nullptr;
            return false;
        }

        curDirectoryEntry = &sortedFiles.item(sortedFileIndex);
        cur.setown(createIFile(curDirectoryEntry->file->queryFilename()));
        curIsDir = curDirectoryEntry->isdir;
        sortedFileIndex++;
        return true;
    }
    virtual bool isValid() override { return cur != nullptr; }
    virtual IFile &query() override { return *cur; }

    //IDirectoryIterator
    virtual bool isDir() override { return curIsDir; }
    virtual StringBuffer &getName(StringBuffer &buf) override
    {
        if (curDirectoryEntry)
            return buf.set(curDirectoryEntry->name.get());
        return buf;
    }

    virtual __int64 getFileSize() override
    {
        return curDirectoryEntry ? curDirectoryEntry->size : -1;
    }

    virtual bool getModifiedTime(CDateTime &ret) override
    {
        if (!curDirectoryEntry)
            return false;
        ret = curDirectoryEntry->modifiedTime;
        return true;
    }
};

IDirectoryIterator *getSortedDirectoryIterator(IFile *directory, SortDirectoryMode mode, bool rev, const char *mask, bool sub, bool includedirs)
{
    if (!directory || directory->isDirectory()!=fileBool::foundYes)
        throw MakeStringException(-1, "Invalid IFile input in getSortedDirectoryIterator()");

    Owned<IDirectoryIterator> files = directory->directoryFiles(mask, sub, includedirs);
    if (SD_nosort == mode)
        return files;
    return new CSortedDirectoryIterator(*files, mode, rev, includedirs);
}

IDirectoryIterator *getSortedDirectoryIterator(const char *dirName, SortDirectoryMode mode, bool rev, const char *mask, bool sub, bool includedirs)
{
    if (isEmptyString(dirName))
        throw MakeStringException(-1, "Invalid dirName input in getSortedDirectoryIterator()");

    Owned<IFile> dir = createIFile(dirName); 
    return getSortedDirectoryIterator(dir, mode, rev, mask, sub, includedirs);
}

//--------------------------------------------------------------------------------------------------------------------

unsigned __int64 FileIOStats::getStatistic(StatisticKind kind)
{
    switch (kind)
    {
    case StCycleDiskReadIOCycles:
        return ioReadCycles.load();
    case StCycleDiskWriteIOCycles:
        return ioWriteCycles.load();
    case StTimeDiskReadIO:
        return cycle_to_nanosec(ioReadCycles.load());
    case StTimeDiskWriteIO:
        return cycle_to_nanosec(ioWriteCycles.load());
    case StSizeDiskRead:
        return ioReadBytes.load();
    case StSizeDiskWrite:
        return ioWriteBytes.load();
    case StNumDiskReads:
        return ioReads.load();
    case StNumDiskWrites:
        return ioWrites.load();
    }
    return 0;
}

void FileIOStats::trace()
{
    if (ioReads)
        printf("Reads: %u  Bytes: %u  TimeMs: %u\n", (unsigned)ioReads, (unsigned)ioReadBytes, (unsigned)cycle_to_millisec(ioReadCycles));
    if (ioWrites)
        printf("Writes: %u  Bytes: %u  TimeMs: %u\n", (unsigned)ioWrites, (unsigned)ioWriteBytes, (unsigned)cycle_to_millisec(ioWriteCycles));
}

//--------------------------------------------------------------------------------------------------------------------

static constexpr FileSystemProperties linuxFileSystemProperties     {true, false};
static constexpr FileSystemProperties defaultUrlFileSystemProperties{false, false};

//This implementation should eventually make use of the file hook.
const FileSystemProperties & queryFileSystemProperties(const char * filename)
{
    if (isUrl(filename))
        return defaultUrlFileSystemProperties;
    else
        return linuxFileSystemProperties;
}


#if defined(__linux__)
class CFileEventWatcher : public CInterfaceOf<IFileEventWatcher>, implements IThreaded
{
    class CMonitoredItem : public CInterface
    {
    public:
        CMonitoredItem(const char *_filename, int _watchFd, FileWatchEvents _events) : filename(_filename), watchFd(_watchFd), events(_events)
        {
        }
        std::string filename;
        int watchFd = -1;
        FileWatchEvents events = FileWatchEvents::none;
        bool pendingRemoval = false;
    };
    int inotifyFd = -1;
    int pipefd[2] = {-1, -1};
    pollfd toPoll[2];
    std::unordered_map<int, Linked<CMonitoredItem>> monitoredFilesByFd;
    std::unordered_map<std::string, Linked<CMonitoredItem>> monitoredFiles;
    std::unordered_set<int> pendingRemovals;
    CriticalSection crit;
    FileWatchFunc callback;
    CThreaded threaded;
    std::atomic<bool> stopped{true};

    void handleEvents(int fd)
    {
        byte buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));

        while (true)
        {
            ssize_t len = read(fd, buf, sizeof(buf));
            if (len == -1 && errno != EAGAIN)
                throw makeErrnoException("CFileEventWatcher::handleEvents - read");

            if (len <= 0)
                break;

            const byte *ptr = buf;

            do
            {
                const struct inotify_event *event = (const struct inotify_event *) ptr;
                std::string filename;
                if (event->len) // if watching directory event->name is populated with file within that dir.
                    filename = event->name;
                else
                {
                    // if watching a file, lookup filename from event handle
                    CriticalBlock b(crit);
                    auto iter = monitoredFilesByFd.find(event->wd);
                    if (iter != monitoredFilesByFd.end())
                    {
                        filename = iter->second->filename;
                        break;
                    }
                }
                /* NB: in this impl. inotify_event flags are same as FileWatchEvents bit masks
                 * If other implementations differ, they will need mapping to FileWatchEvents
                 */
                FileWatchEvents events = static_cast<FileWatchEvents>(event->mask);

                callback(filename.c_str(), events);

                ptr += sizeof(struct inotify_event) + event->len;
            }
            while (ptr < (buf + len));
        }
    }
    void checkClose(int h)
    {
        if (-1 != h)
            close(h);
    }
public:
    CFileEventWatcher(FileWatchFunc _callback) : callback(_callback), threaded("CFileEventWatcher", this)
    {
        inotifyFd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
        if (-1 == inotifyFd)
            throw makeErrnoException(-1, "CFileEventWatcher - inotify_init1");

        if (pipe2(pipefd, O_NONBLOCK|O_CLOEXEC))
            throw makeErrnoException(-1, "CFileEventWatcher - create pipe failed");

        /*
         * Setup an array of file handles to use with poll.
         * Add the inotify fd that will receive event info on
         * Add the pipe input fd created above, this is used to write to, to "wake up" the poll loop when watcher is stopped.
         */
        toPoll[0].fd = pipefd[0];
        toPoll[0].events = POLLIN;
        toPoll[1].fd = inotifyFd;
        toPoll[1].events = POLLIN;
    }
    ~CFileEventWatcher()
    {
        stop();
        checkClose(inotifyFd);
        checkClose(pipefd[0]);
        checkClose(pipefd[1]);
    }
    virtual void start() override
    {
        if (stopped)
        {
            stopped = false;
            threaded.start(false);
        }
    }
    virtual void stop() override
    {
        if (stopped)
            return;
        stopped = true;

        const byte c = 0;
        if (write(pipefd[1], &c, 1) != 1)
        {
            Owned<IException> e = makeErrnoException("CFileEventWatcher::stop() - write -> pipefd[1]");
            EXCLOG(e);
        }
        threaded.join();
    }
    virtual bool add(const char *filename, FileWatchEvents events) override
    {
        if (isEmptyString(filename))
            throw makeStringException(-1, "CFileEventWatcher::add() - invalid empty filename");
        CriticalBlock b(crit);
        auto iter = monitoredFiles.find(filename);
        if (iter != monitoredFiles.end()) // filename already monitored
            return false;

        /* NB: in this impl. inotify_event flags are same as FileWatchEvents bit masks
         * If other implementations differ, events will need mapping to the underlying impl.
         */
        int iNotifyEvents = static_cast<int>(events);
        int wd = inotify_add_watch(inotifyFd, filename, iNotifyEvents);
        if (-1 == wd)
            throw makeErrnoExceptionV(-1, "CFileEventWatcher::add() - Cannot watch '%s'", filename);
        Owned<CMonitoredItem> item = new CMonitoredItem(filename, wd, events);
        monitoredFiles.emplace(filename, item);
        monitoredFilesByFd.emplace(wd, item);

        return true;
    }
    virtual bool remove(const char *filename) override
    {
        if (isEmptyString(filename))
            return false;

        CriticalBlock b(crit);
        auto iter = monitoredFiles.find(filename);
        if (iter == monitoredFiles.end()) // filename is not being monitored
            return false;
        CMonitoredItem *item = iter->second;
        if (item->pendingRemoval) // already flagged for removal
            return false;

        item->pendingRemoval = true;
        pendingRemovals.emplace(item->watchFd);

        if (-1 == inotify_rm_watch(inotifyFd, item->watchFd))
            throw makeErrnoException("CFileEventWatcher::remove() - inotify_rm_watch()");
        return true;
    }
// IThreaded impl.    
    virtual void threadmain() override
    {
        try
        {
            while (true)
            {
                int poll_num = poll(toPoll, 2, -1);
                if (-1 == poll_num)
                {
                    if (EINTR == errno)
                        continue;
                    throw makeErrnoException("CFileEventWatcher - poll error");
                }

                if (poll_num)
                {
                    if (toPoll[0].revents & POLLIN)
                    {
                        if (stopped)
                            break;
                    }
                    else if (toPoll[1].revents & POLLIN)
                    {
                        handleEvents(inotifyFd);

                        // clear any pending deletes
                        CriticalBlock b(crit);
                        for (const auto fd: pendingRemovals)
                        {
                            auto iter = monitoredFilesByFd.find(fd);
                            if (iter != monitoredFilesByFd.end())
                            {
                                CMonitoredItem *item = iter->second;
                                if (item->pendingRemoval) // if false, it implies the handle was removed, but has been reused and re-added since removal.
                                {
                                    monitoredFiles.erase(item->filename);
                                    monitoredFilesByFd.erase(iter);
                                }
                            }
                        }
                        pendingRemovals.clear();
                    }
                }
            }
        }
        catch (IException *e)
        {
            EXCLOG(e);
            e->Release();
        }
    }
};

IFileEventWatcher *createFileEventWatcher(FileWatchFunc callback)
{
    return new CFileEventWatcher(callback);
}
#else
IFileEventWatcher *createFileEventWatcher(FileWatchFunc callback)
{
    UNIMPLEMENTED_X("createFileEventWatcher() not supported on this platform");
}
#endif // __linux__


//---- Storage plane related functions ----------------------------------------------------



// NB: This implementation is not thread-safe.
// Therefore it should only be used by use cases that are single threaded
class CBlockedFileIO : public CSimpleInterfaceOf<IFileIO>
{
    Owned<IFileIO> io;
    size32_t blockSize = 0;
    size32_t readLen = 0;
    void *buffer = nullptr;
    offset_t lastReadPos = (offset_t)-1;
    MemoryBuffer mb;
    std::atomic<bool> isActive{false};
public:
    CBlockedFileIO(IFileIO *_io, size32_t _blockSize) : io(_io), blockSize(_blockSize)
    {
        buffer = mb.reserveTruncate(blockSize);
    }
    virtual size32_t read(offset_t pos, size32_t len, void *data) override
    {
        if (unlikely(len > blockSize))
            return io->read(pos, len, data);

        //A sanity check to catch calls from multiple threads - (see HPCC-31852).
        if (unlikely(isActive))
            throw makeStringExceptionV(99, "Reentrant call to CBlockedFileIO::read(%llu, %u) [%llu]", pos, len, lastReadPos);
        isActive = true;

        size32_t totalCopied = 0;
        try
        {
            byte *dest = (byte *) data;
            while (len)
            {
                offset_t readPos = (pos / blockSize) * blockSize; // NB: could be beyond end of file
                if (readPos != lastReadPos)
                {
                    readLen = io->read(readPos, blockSize, buffer); // NB: can be less than blockSize (and 0 if beyond end of file)
                    lastReadPos = readPos;
                }
                offset_t endPos = readPos+readLen;
                size32_t copyNow;
                if (pos+len <= endPos) // common case hopefully
                    copyNow = len;
                else if (pos < endPos)
                    copyNow = endPos-pos;
                else // nothing to copy
                    break;
                memcpy(dest, ((byte *)buffer) + pos-readPos, copyNow);
                len -= copyNow;
                pos += copyNow;
                dest += copyNow;
                totalCopied += copyNow;
            }
        }
        catch (...)
        {
            isActive = false;
            throw;
        }
        isActive = false;
        return totalCopied;
    }
    virtual offset_t size() override { return io->size(); }
    virtual size32_t write(offset_t pos, size32_t len, const void * data) override { throwUnexpected(); }
    virtual void setSize(offset_t size) override { throwUnexpected(); }
    virtual void flush() override { throwUnexpected(); }
    virtual void close() override { io->close(); }
    virtual unsigned __int64 getStatistic(StatisticKind kind) override { return io->getStatistic(kind); }
    virtual IFile * queryFile() const { return io->queryFile(); }
};

extern IFileIO *createBlockedIO(IFileIO *base, size32_t blockSize)
{
    return new CBlockedFileIO(base, blockSize);
}

///---------------------------------------------------------------------------------------------------------------------

// Module-level global that will contain a list of pluggable file type
// names (e.g. "parquet", "csv") that are supported through the
// generic disk reader and writer interfaces
static StringArray genericFileTypeNameList;

void addAvailableGenericFileTypeName(const char * name)
{
    if (!genericFileTypeNameList.contains(name))
        genericFileTypeNameList.append(name);
}

// Determine if file type is defined; used by the ECL parser
bool hasGenericFiletypeName(const char * name)
{
    ForEachItemIn(idx, genericFileTypeNameList)
    {
        if (strieq(genericFileTypeNameList.item(idx), name))
            return true;
    }

    return false;
}

IAPICopyClient * createApiCopyClient(IStorageApiInfo * source, IStorageApiInfo * target)
{
    ReadLockBlock block(containedFileHookLock);
    ForEachItemIn(i, containedFileHooks)
    {
        IAPICopyClient * copyClient = containedFileHooks.item(i).getCopyApiClient(source, target);
        if (copyClient)
            return copyClient;
    }
    return nullptr;
}

//---------------------------------------------------------------------------------------------------------------------

// Cache/update plane attributes settings
static unsigned jFileHookId = 0;

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    auto updateFunc = [&](const IPropertyTree *oldComponentConfiguration, const IPropertyTree *oldGlobalConfiguration)
    {
        expertEnableIFileFlagsMask = IFEnone;
        expertDisableIFileFlagsMask = IFEnone;

        Owned<IPropertyTree> componentConfig = getComponentConfig();
        Owned<IPropertyTree> globalConfig = getGlobalConfig();
        StringBuffer fileFlagsStr;
        if (componentConfig->getProp("expert/@enableIFileMask", fileFlagsStr) || globalConfig->getProp("expert/@enableIFileMask", fileFlagsStr))
            expertEnableIFileFlagsMask = (IFEflags)strtoul(fileFlagsStr, NULL, 0);

        if (componentConfig->getProp("expert/@disableIFileMask", fileFlagsStr.clear()) || globalConfig->getProp("expert/@disableIFileMask", fileFlagsStr))
            expertDisableIFileFlagsMask = (IFEflags)strtoul(fileFlagsStr, NULL, 0);

        safeStatBehaviour = static_cast<SafeStatBehaviour>(componentConfig->getPropInt("expert/@safeStatBehaviour", globalConfig->getPropInt("expert/@safeStatBehaviour", static_cast<unsigned>(defaultSafeStatBehaviour))));
        if (safeStatBehaviour >= SafeStatBehaviour::Count) // safeguard against bad config value
            safeStatBehaviour = defaultSafeStatBehaviour;

        std::string propName = "expert/@" + std::string(getPlaneAttributeString(FileSyncWriteClose));
        if (componentConfig->hasProp(propName.c_str()))
            defaultFileSyncWriteCloseEnabled = componentConfig->getPropBool(propName.c_str());
        else
        {
            if (globalConfig->hasProp(propName.c_str()))
                defaultFileSyncWriteCloseEnabled = globalConfig->getPropBool(propName.c_str());
        }
    };
    jFileHookId = installConfigUpdateHook(updateFunc, true);

    return true;
}

MODULE_EXIT()
{
    removeConfigUpdateHook(jFileHookId);
}

bool getFileSyncWriteCloseEnabled(const char *planeName)
{
    return 0 != getPlaneAttributeValue(planeName, FileSyncWriteClose, defaultFileSyncWriteCloseEnabled ? 1 : 0);
}

unsigned getWriteSyncMarginMs(const char * planeName)
{
    constexpr unsigned dft = 0;
    return (unsigned)getPlaneAttributeValue(planeName, WriteSyncMarginMs, dft);
}

static constexpr bool defaultConcurrentWriteSupport = isContainerized() ? false : true;
bool getConcurrentWriteSupported(const char *planeName)
{
    return 0 != getPlaneAttributeValue(planeName, ConcurrentWriteSupport, defaultConcurrentWriteSupport ? 1 : 0);
}
