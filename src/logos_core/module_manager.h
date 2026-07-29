#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include "module_loader_registry.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

class ModuleRegistry;

namespace ModuleManager {
    ModuleRegistry& registry();

    // Access the loader registry. Frontends can register their own loaders /
    // containers (Docker, WASM, in-process, ...) before logos_core_start():
    //     ModuleManager::loaders().registerLoader(myLoader);
    // Loaders are consulted in registration order, or pinned per-module via
    // loaderConfig["id"]. They compose with the built-in subprocess default
    // (see module_manager.cpp). Also used by tests to install a FakeModuleLoader.
    LogosCore::ModuleLoaderRegistry& loaders();

    void setModulesDir(const char* modules_dir);
    void addModulesDir(const char* modules_dir);
    void setPersistenceBasePath(const char* path);

    // Register a per-module transport set (serialized JSON, see
    // logos-cpp-sdk/cpp/logos_transport_config_json.h for the wire
    // shape). The loader threads this through to the child subprocess
    // when the module loads, so the child's LogosAPIProvider binds
    // every transport in the set instead of only the global default
    // (LocalSocket).
    //
    // Must be called BEFORE the module is loaded — by the daemon, this
    // means before logos_core_start() for capability_module, or before
    // any explicit loadModule() call for user modules. Unset modules
    // continue to use the global default.
    //
    // Empty `transportSetJson` clears any previously set entry.
    void setModuleTransports(const std::string& moduleName,
                             const std::string& transportSetJson);

    // Address-aware transport override. An empty instance ID selects the
    // legacy/default runtime; a non-empty ID stays isolated from sibling
    // instances of the same module package.
    void setModuleInstanceTransports(const std::string& moduleName,
                                     const std::string& instanceId,
                                     const std::string& transportSetJson);

    // Store the inter-module access policy (the raw JSON document set via
    // logos_core_set_access_policy). Core parses it and registers the
    // concrete per-target restrictions with capability_module once that
    // module is loaded (see initializeCapabilityModule). Must be called
    // BEFORE logos_core_start(). Empty clears any previously set policy.
    void setAccessPolicy(const std::string& policyJson);

    // Allowed callers core would register for `target` (see the .cpp).
    // A pure read with no RPC — exposed so tests can observe the derivation.
    std::vector<std::string> computeDerivedAllowedCallers(const std::string& target);

    void discoverInstalledModules();

    std::string processModule(const std::string& modulePath);
    char* processModuleCStr(const char* modulePath);
    bool loadModule(const char* moduleName);
    bool loadModuleWithDependencies(const char* moduleName);

    // Load an explicitly addressed runtime. The default instance remains the
    // legacy idempotent path; an already-running explicit address is refused
    // so callers cannot mistake a no-op for a newly created Zone runtime.
    bool loadModuleInstance(const char* moduleName,
                            const char* instanceId,
                            bool withDependencies);
    bool initializeCapabilityModule();
    bool unloadModule(const char* moduleName);

    // Explicit-instance unload never cascades package dependents: dependency
    // metadata is package-scoped and those shared default runtimes may serve
    // sibling instances. withDependents is accepted only for the default path.
    bool unloadModuleInstance(const char* moduleName,
                              const char* instanceId,
                              bool withDependents);

    // Cascading unload: unload the named module together with every currently
    // loaded module that (transitively) depends on it. The order is
    // leaves-first (dependents before dependencies) so no process is left
    // briefly pointing at a dead parent.
    // Returns true only if every step succeeded.
    bool unloadModuleWithDependents(const char* moduleName);

    void terminateAll();
    void clear();

    char** getLoadedModulesCStr();
    char** getKnownModulesCStr();

    bool isModuleLoaded(const std::string& name);
    bool isModuleInstanceLoaded(const std::string& moduleName,
                                const std::string& instanceId);
    std::unordered_map<std::string, int64_t> getModuleProcessIds();

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules);

    // Returns the declared dependencies of `name` among known modules.
    // Names that appear only in module metadata and are not known to the
    // registry are not included in the returned list.
    // `recursive=true` walks the forward dependency graph transitively.
    std::vector<std::string> getDependencies(const std::string& name, bool recursive);

    // Returns the declared dependents of `name` among known modules.
    // Only known modules tracked by the registry are included in the
    // returned list.
    // `recursive=true` walks the reverse dependency graph transitively.
    std::vector<std::string> getDependents(const std::string& name, bool recursive);

    // Null-terminated char** variants of the two accessors above. Caller
    // owns the returned array and each entry. Pass-through of the registry
    // result — unknown names produce a zero-length (just a trailing null)
    // array, matching the C API contract used by the other getters.
    char** getDependenciesCStr(const char* name, bool recursive);
    char** getDependentsCStr(const char* name, bool recursive);

    // JSON (string) describing every known module: name, path, loaded flag,
    // direct dependencies, direct dependents, and full embedded metadata.
    // See ModuleRegistry::allModulesInfo for the shape.
    std::string getModulesInfoJson();
    // char* variant. Caller owns the returned string. Never null.
    char* getModulesInfoCStr();

    std::string getModuleInstancesInfoJson();
    char* getModuleInstancesInfoCStr();
}

#endif // MODULE_MANAGER_H
