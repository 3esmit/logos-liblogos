#include "module_manager.h"
#include "module_registry.h"
#include "access_policy.h"
#include "dependency_resolver.h"
#include "module_loader_registry.h"
#include "composite_module_loader.h"
#include <logos_container/container_factory.h>
#include <logos_module_loader/format_loader_factory.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>
#include <cassert>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_module.h"
#include "logos_protocol.h"
#include "protocol_gate.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "instance_persistence.h"

namespace {
    ModuleRegistry& registryInstance() {
        static ModuleRegistry instance;
        return instance;
    }

    std::mutex& loadMutex() {
        static std::mutex mutex;
        return mutex;
    }

    LogosCore::ModuleAddress defaultAddress(const std::string& moduleName) {
        return {moduleName, {}};
    }

    std::string addressLabel(const LogosCore::ModuleAddress& address) {
        return address.isDefaultInstance()
            ? address.moduleName
            : address.moduleName + "@" + address.instanceId;
    }

    // Per-runtime transport set. The default address retains the historical
    // name-only behavior; explicit addresses never overwrite one another.
    std::unordered_map<LogosCore::ModuleAddress, std::string,
                       LogosCore::ModuleAddressHash>& moduleTransportsMap() {
        static std::unordered_map<LogosCore::ModuleAddress, std::string,
                                  LogosCore::ModuleAddressHash> m;
        return m;
    }

    std::string& persistenceBasePath() {
        static std::string path;
        return path;
    }

    // Both guarded by loadMutex(). parsedEnforcePolicy is set only in enforce mode.
    std::string& accessPolicyJson() {
        static std::string s;
        return s;
    }

    std::optional<LogosCore::AccessPolicy>& parsedEnforcePolicy() {
        static std::optional<LogosCore::AccessPolicy> p;
        return p;
    }

    // Always allowed past the dependency check, so they're never locked out.
    const std::vector<std::string> kTrustedCallers = {"core", "core_service"};

    // Never restricted as targets, even if an explicit policy names them.
    // TODO: re-eval this; probably is required to restrict core/core_service
    const std::vector<std::string> kExemptTargets =
        {"capability_module", "core", "core_service"};

    // Built-in default loader, composed from the container + format-loader the
    // build linked in. The concrete implementations are chosen at link time via
    // the contract factory seams (LogosCore::makeContainer / makeFormatLoader);
    // the core names no specific container or loader. Frontends can still
    // register additional loaders via ModuleManager::loaders().registerLoader().
    LogosCore::ModuleLoaderRegistry& loaderRegistry() {
        static LogosCore::ModuleLoaderRegistry reg;
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            auto container = LogosCore::makeContainer();
            auto loader    = LogosCore::makeFormatLoader();
            if (container && loader)
                reg.registerLoader(std::make_shared<LogosCore::CompositeModuleLoader>(container, loader));
        });
        return reg;
    }

    char** toNullTerminatedArray(const std::vector<std::string>& list) {
        int count = static_cast<int>(list.size());
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }

        char** result = new char*[count + 1];
        for (int i = 0; i < count; ++i) {
            result[i] = new char[list[i].size() + 1];
            strcpy(result[i], list[i].c_str());
        }
        result[count] = nullptr;
        return result;
    }

    // Dial capability_module from a long-lived "core" LogosAPI. Prefer the
    // operator's first configured transport; fall back to the global
    // default (LocalSocket). Needed because the single-arg getClient()
    // always uses the global default, which hangs against a tcp-only
    // capability_module that never bound a LocalSocket.
    LogosAPIClient* capabilityModuleClient() {
        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI(std::string("core"));

        if (auto it = moduleTransportsMap().find(defaultAddress("capability_module"));
            it != moduleTransportsMap().end() && !it->second.empty()) {
            const auto ts = logos::transportSetFromJsonString(it->second);
            if (!ts.empty()) {
                return s_coreApi->getClient(
                    QStringLiteral("capability_module"), ts.front());
            }
        }
        return s_coreApi->getClient(std::string("capability_module"));
    }

    // Token authenticates the call. Best-effort; assumes capability_module loaded.
    void registerRestrictionRpc(const std::string& target,
                                const std::vector<std::string>& callers) {
        nlohmann::json args = nlohmann::json::array();
        args.push_back(TokenManager::instance().getToken(std::string("capability_module")));
        args.push_back(target);
        args.push_back(callers);

        nlohmann::json result = capabilityModuleClient()->invokeRemoteMethod(
            std::string("capability_module"),
            std::string("registerRestriction"),
            args);

        if (!result.is_boolean() || !result.get<bool>())
            spdlog::warn("Failed to register access restriction for target: {}", target);
        else
            spdlog::info("Registered access restriction for target: {} ({} allowed callers)",
                         target, callers.size());
    }

    // Explicit-policy restrictions, including targets not yet loaded (the
    // derived path covers only loaded ones).
    void pushAccessRestrictionsToCapabilityModule() {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        const auto& policy = parsedEnforcePolicy();
        if (!policy)
            return;

        for (const auto& restriction : policy->restrictions) {
            if (std::find(kExemptTargets.begin(), kExemptTargets.end(),
                          restriction.target) != kExemptTargets.end())
                continue;
            registerRestrictionRpc(restriction.target, restriction.allowedCallers);
        }
    }

    // A module may only call modules it declared as a dependency, so `target`'s
    // allowed callers are its loaded dependents plus the trusted set. Empty when
    // exempt or no enforce policy (fail-open); explicit policy overrides verbatim.
    std::vector<std::string> computeDerivedAllowedCallersLocked(const std::string& target) {
        if (std::find(kExemptTargets.begin(), kExemptTargets.end(), target)
                != kExemptTargets.end())
            return {};

        const auto& policy = parsedEnforcePolicy();
        if (!policy)
            return {};

        for (const auto& r : policy->restrictions)
            if (r.target == target)
                return r.allowedCallers;

        // Deduped; no dependents => trusted only (deny-by-default for peers).
        std::vector<std::string> callers;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& c) {
            if (seen.insert(c).second)
                callers.push_back(c);
        };
        for (const auto& d : registryInstance().moduleDependents(target, /*recursive=*/false))
            if (registryInstance().isLoaded(d))
                add(d);
        for (const auto& t : kTrustedCallers)
            add(t);
        return callers;
    }

    void pushDerivedRestrictionForTarget(const std::string& target) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        auto callers = computeDerivedAllowedCallersLocked(target);
        if (!callers.empty())
            registerRestrictionRpc(target, callers);
    }

    // On load/unload of `name`, re-push the targets whose caller set changed:
    // its declared dependencies, plus `name` itself.
    void refreshDerivedRestrictionsForDependenciesOf(const std::string& name) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        for (const auto& dep : registryInstance().moduleDependencies(name, /*recursive=*/false))
            pushDerivedRestrictionForTarget(dep);
        pushDerivedRestrictionForTarget(name);
    }

    void notifyCapabilityModule(const LogosCore::ModuleAddress& address,
                                const std::string& token) {
        if (!registryInstance().isLoaded("capability_module"))
            return;

        TokenManager& tokenManager = TokenManager::instance();
        std::string capabilityModuleToken = tokenManager.getToken(std::string("capability_module"));

        LogosAPIClient* client = capabilityModuleClient();

        if (address.isDefaultInstance()) {
            if (!client->informModuleToken(capabilityModuleToken, address.moduleName, token)) {
                spdlog::warn("Failed to register token with capability module for: {}",
                             address.moduleName);
            }
            return;
        }

        // The scoped capability API is additive. Use the generic remote call
        // rather than a generated interface method so liblogos only requires
        // the public Q_INVOKABLE contract and remains decoupled from the
        // capability plugin's C++ implementation type.
        nlohmann::json args = nlohmann::json::array();
        args.push_back(capabilityModuleToken);
        args.push_back(address.moduleName);
        args.push_back(address.instanceId);
        args.push_back(token);
        const nlohmann::json result = client->invokeRemoteMethod(
            std::string("capability_module"),
            std::string("informModuleTokenScoped"),
            args);
        if (!result.is_boolean() || !result.get<bool>()) {
            spdlog::warn("Failed to register scoped token with capability module for: {}",
                         addressLabel(address));
        }
    }

    bool loadModuleInternal(const LogosCore::ModuleAddress& address) {
        if (!address.isValid()) {
            spdlog::warn("Cannot load module with invalid runtime address: {}",
                         addressLabel(address));
            return false;
        }

        const std::string& name = address.moduleName;

        if (!registryInstance().isKnown(name)) {
            spdlog::warn("Cannot load unknown module: {}", name);
            return false;
        }

        // The default address preserves historical "ensure loaded" semantics.
        // An explicit address instead refuses a duplicate request so a caller
        // cannot mistake an already-running Zone runtime for a fresh one.
        // Callers (basecamp's PluginLoader::loadCoreDependencies,
        // logoscore-cli, etc.) use loadModule as "ensure loaded";
        // returning false here aborted UI-plugin loads whose core
        // dependency had been pre-loaded at startup (e.g. clicking
        // the package-manager launcher after basecamp pre-loaded
        // `package_manager`).
        if (registryInstance().isRuntimeLoaded(address)) {
            if (address.isDefaultInstance()) {
                spdlog::debug("Module already loaded (no-op): {}", name);
                return true;
            }
            spdlog::warn("Cannot load duplicate module runtime: {}", addressLabel(address));
            return false;
        }

        std::string modPath = registryInstance().modulePath(name);

        // Build a descriptor for the loader to inspect.
        LogosCore::ModuleDescriptor desc;
        desc.name        = name;
        desc.instanceId  = address.instanceId;
        desc.path        = modPath;
        desc.format      = "qt-plugin";
        desc.dependencies = registryInstance().moduleDependencies(name);
        desc.modulesDirs  = registryInstance().modulesDirs();

        if (!persistenceBasePath().empty()) {
            const auto mode = address.isDefaultInstance()
                ? ModuleLib::InstancePersistence::ResolveMode::ReuseOrCreate
                : ModuleLib::InstancePersistence::ResolveMode::UseExplicit;
            auto info = ModuleLib::InstancePersistence::resolveInstance(
                persistenceBasePath(), name, mode, address.instanceId);
            if (info.persistencePath.empty()) {
                spdlog::warn("Failed to resolve persistence for module runtime: {}",
                             addressLabel(address));
                return false;
            }
            desc.instancePersistencePath = info.persistencePath;
        }

        // Per-module transport set, if the daemon registered one before
        // calling load. The loader threads it through to the child via
        // a CLI argument so the child's LogosAPIProvider binds the right
        // listeners. Modules without an entry inherit the global default.
        if (auto it = moduleTransportsMap().find(address);
            it != moduleTransportsMap().end()) {
            desc.transportSetJson = it->second;
        }

        // ── Protocol-version load gate ─────────────────────────────────
        // Read the module's embedded metadata without loading it and apply
        // the one compatibility rule: equal logos-protocol MAJOR loads,
        // different MAJOR is refused, a missing stamp (pre-protocol module)
        // loads permissively with a warning.
        std::string moduleProtocolVersion;
        if (auto meta = ModuleLib::LogosModule::extractMetadata(modPath)) {
            // While we have it, hand the full metadata to the loader.
            desc.rawMetadata = nlohmann::json::parse(
                meta->rawMetadataJson, nullptr, /*allow_exceptions=*/false);
            if (desc.rawMetadata.is_discarded())
                desc.rawMetadata = nlohmann::json::object();
            if (auto it = desc.rawMetadata.find("logos_protocol_version");
                it != desc.rawMetadata.end() && it->is_string())
                moduleProtocolVersion = it->get<std::string>();
        }
        const auto gate = LogosCore::evaluateProtocolGate(
            moduleProtocolVersion, LOGOS_PROTOCOL_VERSION_MAJOR);
        switch (gate.decision) {
        case LogosCore::ProtocolGateDecision::Refuse:
            spdlog::error(
                "Refusing to load module {}: built against logos-protocol {} "
                "(major {}), this host speaks major {} ({}) — incompatible "
                "protocol majors",
                name, moduleProtocolVersion, gate.moduleMajor,
                LOGOS_PROTOCOL_VERSION_MAJOR, LOGOS_PROTOCOL_VERSION_STRING);
            return false;
        case LogosCore::ProtocolGateDecision::AllowLegacy:
            spdlog::warn(
                "Module {} carries no usable logos_protocol_version "
                "(pre-protocol build) — loading permissively",
                name);
            break;
        case LogosCore::ProtocolGateDecision::Allow:
            spdlog::debug("Module {} protocol version {} compatible with host {}",
                          name, moduleProtocolVersion,
                          LOGOS_PROTOCOL_VERSION_STRING);
            break;
        }

        auto loader = loaderRegistry().select(desc);
        if (!loader) {
            spdlog::warn("No loader available to load module runtime: {}", addressLabel(address));
            return false;
        }

        const auto instanceLoader = address.isDefaultInstance()
            ? std::shared_ptr<LogosCore::InstanceAwareModuleLoader>{}
            : std::dynamic_pointer_cast<LogosCore::InstanceAwareModuleLoader>(loader);
        if (!address.isDefaultInstance() && !instanceLoader) {
            spdlog::warn("Selected loader cannot host explicit module runtime: {}",
                         addressLabel(address));
            return false;
        }

        const std::optional<uint64_t> generation = registryInstance().reserveRuntime(address);
        if (!generation) {
            spdlog::warn("Cannot reserve duplicate module runtime: {}", addressLabel(address));
            return false;
        }

        LogosCore::LoadedModuleHandle handle;
        bool loaded = false;
        if (address.isDefaultInstance()) {
            loaded = loader->load(
                desc,
                [address, generation = *generation](const std::string& terminatedName) {
                    if (terminatedName == address.moduleName) {
                        registryInstance().markRuntimeUnloadedIfGeneration(address, generation);
                    }
                },
                handle);
        } else {
            loaded = instanceLoader->loadInstance(
                desc,
                [address, generation = *generation](const LogosCore::ModuleAddress& terminated) {
                    if (terminated == address) {
                        registryInstance().markRuntimeUnloadedIfGeneration(address, generation);
                    }
                },
                handle);
        }
        if (!loaded) {
            registryInstance().cancelRuntime(address, *generation);
            return false;
        }

        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        const bool tokenDelivered = address.isDefaultInstance()
            ? loader->sendToken(name, authToken)
            : instanceLoader->sendTokenToInstance(address, authToken);
        if (!tokenDelivered) {
            if (address.isDefaultInstance())
                loader->terminate(name);
            else
                instanceLoader->terminateInstance(address);
            registryInstance().cancelRuntime(address, *generation);
            return false;
        }

        if (!registryInstance().completeRuntime(address, *generation, loader, std::move(handle))) {
            if (address.isDefaultInstance())
                loader->terminate(name);
            else
                instanceLoader->terminateInstance(address);
            return false;
        }

        const std::string tokenKey = address.isDefaultInstance()
            ? name
            : logos::scopedModuleTokenKey(
                  QString::fromStdString(address.moduleName),
                  QString::fromStdString(address.instanceId)).toStdString();
        TokenManager::instance().saveToken(tokenKey, authToken);

        notifyCapabilityModule(address, authToken);

        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module loaded: {}", addressLabel(address));

        return true;
    }

    bool loadModuleInternal(const char* moduleName) {
        return loadModuleInternal(defaultAddress(std::string(moduleName)));
    }

    // Unload helper that assumes loadMutex() is already held by the caller.
    // unloadModuleWithDependents() needs a single lock span so a late-arriving
    // load can't interleave between tearing down the dependents and the target.
    bool unloadModuleInternalLocked(const LogosCore::ModuleAddress& address) {
        if (!registryInstance().isRuntimeLoaded(address)) {
            spdlog::warn("Cannot unload module runtime (not loaded): {}", addressLabel(address));
            return false;
        }

        auto loader = registryInstance().loaderForRuntime(address);
        if (loader) {
            if (address.isDefaultInstance()) {
                if (!loader->hasModule(address.moduleName)) {
                    spdlog::warn("No module entry found for module: {}", address.moduleName);
                    return false;
                }
                loader->terminate(address.moduleName);
            } else {
                const auto instanceLoader =
                    std::dynamic_pointer_cast<LogosCore::InstanceAwareModuleLoader>(loader);
                if (!instanceLoader || !instanceLoader->hasInstance(address)) {
                    spdlog::warn("No module entry found for runtime: {}", addressLabel(address));
                    return false;
                }
                if (!instanceLoader->terminateInstance(address)) {
                    spdlog::warn("Failed to terminate module runtime: {}", addressLabel(address));
                    return false;
                }
            }
        } else {
            // Fallback: module was loaded via markLoaded(name) directly (test
            // scenarios or external setup), so no loader was recorded. Ask the
            // registered loaders to terminate it by its exact runtime address.
            const bool terminated = address.isDefaultInstance()
                ? loaderRegistry().terminate(address.moduleName)
                : loaderRegistry().terminateInstance(address);
            if (!terminated) {
                spdlog::warn("No live module entry found for runtime: {}", addressLabel(address));
                return false;
            }
        }

        registryInstance().markRuntimeUnloaded(address);

        // markUnloaded keeps the dependency edges, so this still resolves them.
        refreshDerivedRestrictionsForDependenciesOf(address.moduleName);

        spdlog::info("Module unloaded: {}", addressLabel(address));
        return true;
    }

    bool unloadModuleInternalLocked(const std::string& name) {
        return unloadModuleInternalLocked(defaultAddress(name));
    }

    bool loadModuleWithDependenciesInternalLocked(const LogosCore::ModuleAddress& target) {
        if (!target.isValid()) {
            spdlog::warn("Cannot resolve dependencies for invalid runtime address: {}",
                         addressLabel(target));
            return false;
        }

        std::vector<std::string> requested{target.moduleName};
        auto resolved = DependencyResolver::resolve(
            requested,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); }
        );

        // Treat missing dependencies and cycles as hard failures. The public
        // ABI promises no partial dependency resolution.
        if (!resolved.ok()) {
            spdlog::warn("Cannot resolve dependencies for: {}", target.moduleName);
            return false;
        }

        const bool targetFound = std::find(
            resolved.order.begin(), resolved.order.end(), target.moduleName) != resolved.order.end();
        if (resolved.order.empty() || !targetFound) {
            spdlog::warn("Cannot resolve dependencies for: {}", target.moduleName);
            return false;
        }

        bool allSucceeded = true;
        for (const std::string& name : resolved.order) {
            // Dependency metadata describes packages, not runtime instances.
            // A scoped target therefore gets its own address while all shared
            // dependencies retain the default compatibility instance.
            const LogosCore::ModuleAddress address = name == target.moduleName
                ? target
                : defaultAddress(name);
            if (!loadModuleInternal(address)) {
                spdlog::warn("Failed to load module runtime: {}", addressLabel(address));
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    bool unloadModuleWithDependentsInternalLocked(const std::string& name) {
        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload module (not loaded): {}", name);
            return false;
        }

        // Build the set of modules that need to come down: the target plus
        // every currently-loaded recursive dependent. Materialise the loaded
        // set into a hash once so the membership check below is O(1).
        const std::vector<std::string> loadedNames = registryInstance().loadedModuleNames();
        const std::unordered_set<std::string> loaded(loadedNames.begin(), loadedNames.end());
        const std::vector<std::string> dependents =
            registryInstance().moduleDependents(name, /*recursive=*/true);

        std::vector<std::string> teardownSet{name};
        std::unordered_set<std::string> teardownSetMembers{name};
        for (const std::string& dependent : dependents) {
            if (loaded.count(dependent) && teardownSetMembers.insert(dependent).second)
                teardownSet.push_back(dependent);
        }

        // Order leaves-first: resolve load-order for the teardown set, then
        // reverse. Teardown remains best-effort for a malformed graph.
        const std::vector<std::string> loadOrder = DependencyResolver::resolve(
            teardownSet,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); }
        ).order;
        std::vector<std::string> teardownOrder;
        std::unordered_set<std::string> teardownOrderMembers;
        for (auto it = loadOrder.rbegin(); it != loadOrder.rend(); ++it) {
            if (teardownSetMembers.count(*it) && teardownOrderMembers.insert(*it).second)
                teardownOrder.push_back(*it);
        }
        for (const std::string& current : teardownSet) {
            if (teardownOrderMembers.insert(current).second)
                teardownOrder.push_back(current);
        }

        bool allSucceeded = true;
        for (const std::string& current : teardownOrder) {
            if (!registryInstance().isLoaded(current)) continue;
            if (!unloadModuleInternalLocked(current)) {
                spdlog::warn("Failed to unload module during cascade: {}", current);
                allSucceeded = false;
            }
        }
        return allSucceeded;
    }
}

namespace ModuleManager {

    ModuleRegistry& registry() {
        return registryInstance();
    }

    LogosCore::ModuleLoaderRegistry& loaders() {
        return loaderRegistry();
    }

    void setModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().setModulesDir(std::string(modules_dir));
    }

    void addModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().addModulesDir(std::string(modules_dir));
    }

    void setPersistenceBasePath(const char* path) {
        assert(path != nullptr);
        persistenceBasePath() = std::string(path);
    }

    void setModuleTransports(const std::string& moduleName,
                             const std::string& transportSetJson) {
        setModuleInstanceTransports(moduleName, {}, transportSetJson);
    }

    void setModuleInstanceTransports(const std::string& moduleName,
                                     const std::string& instanceId,
                                     const std::string& transportSetJson) {
        // Same mutex as loadModule()'s read of the map (see line ~122
        // for the lookup). Without this, an operator can race with
        // an in-flight loadModule and the child gets garbled JSON
        // (or sees an empty transport set after the operator
        // overwrote what the child was about to read).
        const LogosCore::ModuleAddress address{moduleName, instanceId};
        if (!address.isValid()) {
            spdlog::warn("Ignoring transport configuration for invalid runtime address: {}",
                         addressLabel(address));
            return;
        }
        std::lock_guard<std::mutex> g(loadMutex());
        if (transportSetJson.empty())
            moduleTransportsMap().erase(address);
        else
            moduleTransportsMap()[address] = transportSetJson;
    }

    void setAccessPolicy(const std::string& policyJson) {
        std::lock_guard<std::mutex> g(loadMutex());  // guards the read at push time
        accessPolicyJson() = policyJson;
        // Cache the parse only in enforce mode; malformed/non-enforce stays empty.
        parsedEnforcePolicy().reset();
        if (!policyJson.empty()) {
            auto parsed = LogosCore::parseAccessPolicy(policyJson);
            if (!parsed) {
                spdlog::warn("logos_core_set_access_policy: policy is not valid JSON "
                             "— no restrictions will be enforced");
            } else if (parsed->enforce()) {
                parsedEnforcePolicy() = std::move(parsed);
            }
        }
    }

    void discoverInstalledModules() {
        registryInstance().discoverInstalledModules();
    }

    std::string processModule(const std::string& modulePath) {
        return registryInstance().processModule(modulePath);
    }

    char* processModuleCStr(const char* modulePath) {
        std::string path(modulePath);

        std::string moduleName = registryInstance().processModule(path);
        if (moduleName.empty()) {
            spdlog::warn("Failed to process module: {}", path);
            return nullptr;
        }

        char* result = new char[moduleName.size() + 1];
        strcpy(result, moduleName.c_str());
        return result;
    }

    bool loadModule(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return loadModuleInternal(moduleName);
    }

    bool loadModuleWithDependencies(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return loadModuleWithDependenciesInternalLocked(defaultAddress(std::string(moduleName)));
    }

    bool loadModuleInstance(const char* moduleName,
                            const char* instanceId,
                            bool withDependencies) {
        if (!moduleName) return false;
        const LogosCore::ModuleAddress address{
            std::string(moduleName), instanceId ? std::string(instanceId) : std::string{}};
        std::lock_guard lock(loadMutex());
        if (!address.isValid()) {
            spdlog::warn("Cannot load module with invalid runtime address: {}",
                         addressLabel(address));
            return false;
        }
        if (!address.isDefaultInstance() && registryInstance().isRuntimeLoaded(address)) {
            spdlog::warn("Cannot load duplicate module runtime: {}", addressLabel(address));
            return false;
        }
        return withDependencies
            ? loadModuleWithDependenciesInternalLocked(address)
            : loadModuleInternal(address);
    }

    bool initializeCapabilityModule() {
        std::lock_guard lock(loadMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadModuleInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        // Register restrictions before any other module can call out: explicit
        // entries, then derived for anything already loaded (usually nothing —
        // only the exempt capability_module is up here).
        pushAccessRestrictionsToCapabilityModule();
        for (const auto& loaded : registryInstance().loadedModuleNames())
            pushDerivedRestrictionForTarget(loaded);

        return true;
    }

    bool unloadModule(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return unloadModuleInternalLocked(std::string(moduleName));
    }

    bool unloadModuleInstance(const char* moduleName,
                              const char* instanceId,
                              bool withDependents) {
        if (!moduleName) return false;
        const LogosCore::ModuleAddress address{
            std::string(moduleName), instanceId ? std::string(instanceId) : std::string{}};
        std::lock_guard lock(loadMutex());
        if (!address.isValid()) {
            spdlog::warn("Cannot unload module with invalid runtime address: {}",
                         addressLabel(address));
            return false;
        }
        if (address.isDefaultInstance()) {
            return withDependents
                ? unloadModuleWithDependentsInternalLocked(address.moduleName)
                : unloadModuleInternalLocked(address);
        }
        if (withDependents) {
            spdlog::warn("Cannot cascade unload explicit module runtime: {}",
                         addressLabel(address));
            return false;
        }
        return unloadModuleInternalLocked(address);
    }

    bool unloadModuleWithDependents(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return unloadModuleWithDependentsInternalLocked(std::string(moduleName));
    }

    void terminateAll() {
        std::lock_guard lock(loadMutex());
        loaderRegistry().terminateAll();
        registryInstance().clearLoaded();
    }

    void clear() {
        std::lock_guard lock(loadMutex());
        loaderRegistry().terminateAll();
        registryInstance().clear();
        // Per-module transport overrides are part of the manager's
        // mutable state — without clearing them here, a daemon
        // restart in the same process (or a unit test that calls
        // clear() between scenarios) would inherit the previous
        // run's transport map and bind unexpected ports.
        moduleTransportsMap().clear();
        accessPolicyJson().clear();  // same rationale — don't leak across restarts
        parsedEnforcePolicy().reset();
    }

    char** getLoadedModulesCStr() {
        return toNullTerminatedArray(registryInstance().loadedModuleNames());
    }

    char** getKnownModulesCStr() {
        std::vector<std::string> known = registryInstance().knownModuleNames();
        if (known.empty()) {
            spdlog::warn("No known modules to return");
        }
        return toNullTerminatedArray(known);
    }

    bool isModuleLoaded(const std::string& name) {
        return registryInstance().isLoaded(name);
    }

    bool isModuleInstanceLoaded(const std::string& moduleName,
                                const std::string& instanceId) {
        const LogosCore::ModuleAddress address{moduleName, instanceId};
        return address.isValid() && registryInstance().isRuntimeLoaded(address);
    }

    std::unordered_map<std::string, int64_t> getModuleProcessIds() {
        return loaderRegistry().getAllPids();
    }

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& name) { return registryInstance().isKnown(name); },
            [](const std::string& name) { return registryInstance().moduleDependencies(name); }
        ).order;
    }

    std::vector<std::string> getDependencies(const std::string& name, bool recursive) {
        std::vector<std::string> deps = registryInstance().moduleDependencies(name, recursive);
        std::vector<std::string> knownDeps;
        knownDeps.reserve(deps.size());
        for (const std::string& dep : deps) {
            if (registryInstance().isKnown(dep))
                knownDeps.push_back(dep);
        }
        return knownDeps;
    }

    std::vector<std::string> getDependents(const std::string& name, bool recursive) {
        return registryInstance().moduleDependents(name, recursive);
    }

    char** getDependenciesCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependencies(std::string(name), recursive));
    }

    char** getDependentsCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependents(std::string(name), recursive));
    }

    std::string getModulesInfoJson() {
        return registryInstance().allModulesInfo().dump();
    }

    char* getModulesInfoCStr() {
        std::string json = getModulesInfoJson();
        char* result = new char[json.size() + 1];
        strcpy(result, json.c_str());
        return result;
    }

    std::string getModuleInstancesInfoJson() {
        return registryInstance().allRuntimeInstancesInfo().dump();
    }

    char* getModuleInstancesInfoCStr() {
        std::string json = getModuleInstancesInfoJson();
        char* result = new char[json.size() + 1];
        strcpy(result, json.c_str());
        return result;
    }

    std::vector<std::string> computeDerivedAllowedCallers(const std::string& target) {
        std::lock_guard lock(loadMutex());
        return computeDerivedAllowedCallersLocked(target);
    }
}
