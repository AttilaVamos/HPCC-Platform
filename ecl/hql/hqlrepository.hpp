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

#ifndef _HQLREPOSITORY_HPP_
#define _HQLREPOSITORY_HPP_

#include "hql.hpp"
#include "hqlcollect.hpp"

typedef IArrayOf<IEclRepository> EclRepositoryArray;
class EclRepositoryMapping : public CInterface
{
public:
    EclRepositoryMapping(const char * _url, const char * _version, const char * _path)
    : url(_url), version(_version), path(_path)
    {
    }

    StringAttr url;
    StringAttr version;
    StringAttr path;
};

class HQL_API EclRepositoryManager
{
public:
    EclRepositoryManager(ICodegenContextCallback * _callback) : callback(_callback)
    {
    }
    EclRepositoryManager(const EclRepositoryManager & other) = delete;

    void addNestedRepository(IIdAtom * scopeId, IEclSourceCollection * source, bool includeInArchive);
    void addQuerySourceFileEclRepository(IErrorReceiver *errs, const char * path, unsigned flags, unsigned trace);
    void addSharedSourceFileEclRepository(IErrorReceiver *errs, const char * path, unsigned flags, unsigned trace, bool includeInArchive);
    void addSingleDefinitionEclRepository(const char * moduleName, const char * attrName, IFileContents * contents, bool includeInArchive);
    void addRepository(IEclSourceCollection * source, const char * rootScopeFullName, bool includeInArchive);

    void addMapping(const char * url, const char * path);

    IEclPackage * createPackage(const char * packageName);
    void gatherPackagesUsed(StringArray & used) const;
    void inherit(const EclRepositoryManager & other);
    void kill();
    unsigned __int64 getStatistic(StatisticKind kind) const;

    void processArchive(IPropertyTree * archiveTree);
    IEclPackage * queryDependentRepository(IIdAtom * name, const char * defaultUrl);
    IEclPackage * queryRepositoryAsRoot(const char * defaultUrl, IEclSourceCollection * overrideSources);

    void setOptions(const char * _eclRepoPath, const char * _gitUser, const char * _gitPasswordPath, const char * _defaultGitPrefix,
                    bool _fetchRepos, bool _updateRepos, bool _cleanRepos, bool _cleanInvalidRepos, bool _verbose)
    {
        options.eclRepoPath.set(_eclRepoPath);
        options.gitUser.set(_gitUser);
        options.gitPasswordPath.set(_gitPasswordPath);
        options.defaultGitPrefix.set(_defaultGitPrefix);
        options.fetchRepos = _fetchRepos;
        options.updateRepos = _updateRepos;
        options.cleanRepos = _cleanRepos;
        options.cleanInvalidRepos = _cleanInvalidRepos;
        options.optVerbose = _verbose;
    }

    IEclSourceCollection * resolveGitCollection(const char * repoPath, const char * defaultUrl);
    void setErrorReceiver(IErrorReceiver * _errorReceiver) const
    {
        errorReceiver = _errorReceiver;
    }
protected:
    IEclRepository * createNewSourceFileEclRepository(IErrorReceiver *errs, const char * path, unsigned flags, unsigned trace, bool includeInArchive);
    IEclRepository * createGitRepository(IErrorReceiver *errs, const char * path, const char * urn, unsigned flags, unsigned trace, bool includeInArchive);
    IEclRepository * createSingleDefinitionEclRepository(const char * moduleName, const char * attrName, IFileContents * contents, bool includeInArchive);
    IEclRepository * createRepository(IEclSourceCollection * source, const char * rootScopeFullName, bool includeInArchive);

    unsigned runGitCommand(StringBuffer * output, const char *args, const char * cwd, bool needCredentials);
    IEclPackage * queryRepository(IIdAtom * name, const char * defaultUrl, IEclSourceCollection * overrideSource, bool includeDefinitions);
    IInterface * getGitUpdateLock(const char * path)
    {
        if (!callback)
            return nullptr;
        return callback->getGitUpdateLock(path);
    }

private:
    mutable IErrorReceiver * errorReceiver = nullptr; // mutable to allow const methods to set it, it logically doesn't change the object
    using DependencyInfo = std::pair<std::string, Shared<IEclPackage>>;
    ICodegenContextCallback * callback;
    CIArrayOf<EclRepositoryMapping> repos;
    std::vector<DependencyInfo> dependencies;
    IArrayOf<IEclRepository> sharedSources;     // plugins, std library, bundles
    IArrayOf<IEclRepository> overrideSources;   // -D options
    IArrayOf<IEclRepository> allSources;        // also includes -D options
    cycle_t gitDownloadCycles = 0;
    cycle_t gitDownloadBlockedCycles = 0;

    //Include all options in a nested struct to make it easy to ensure they are cloned
    struct {
        StringAttr eclRepoPath;
        StringAttr gitUser;
        StringAttr gitPasswordPath;
        StringAttr defaultGitPrefix;
        bool fetchRepos = false;
        bool updateRepos = false;
        bool cleanRepos = false;
        bool cleanInvalidRepos = false;
        bool optVerbose = false;
    } options;
};


extern HQL_API IEclPackage * createCompoundRepositoryF(const char * packageName, IEclRepository * repository, ...);
extern HQL_API IEclPackage * createCompoundRepository(const char * packageName, EclRepositoryArray & repositories);
extern HQL_API IEclRepository * createNestedRepository(IIdAtom * name, IEclRepository * root);

extern HQL_API void getRootScopes(HqlScopeArray & rootScopes, IHqlScope * scope, HqlLookupContext & ctx);
extern HQL_API void getRootScopes(HqlScopeArray & rootScopes, IEclRepository * repository, HqlLookupContext & ctx);
extern HQL_API void getImplicitScopes(HqlScopeArray & implicitScopes, IEclRepository * repository, IHqlScope * scope, HqlLookupContext & ctx);
extern HQL_API void importRootModulesToScope(IHqlScope * scope, HqlLookupContext & ctx);

extern HQL_API IHqlScope * getResolveDottedScope(const char * modname, unsigned lookupFlags, HqlLookupContext & ctx);
extern HQL_API IHqlExpression * getResolveAttributeFullPath(const char * attrname, unsigned lookupFlags, HqlLookupContext & ctx, IEclPackage * optPackage);
extern HQL_API bool looksLikeGitPackage(const char * urn);
extern HQL_API bool canReadPackageFrom(const char * urn);
extern HQL_API bool checkAbortGitFetch();  // Return false if a git operation is in process (and force an abort when it finishes)

#endif
