#include "module_registry.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <ctime>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <unordered_set>
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

namespace logos {

// See the declaration in module_registry.h for the rule and why it lives at
// this trust boundary. Charset already excludes "." and ".."; they are
// re-checked defensively for clarity.
bool isValidModuleName(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok)
            return false;
    }
    if (name == "." || name == "..")
        return false;
    return true;
}

}  // namespace logos

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

void ModuleRegistry::setModulesDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    m_modulesDirs.clear();
    m_modulesDirs.push_back(dir);
}

void ModuleRegistry::addModulesDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    if (std::find(m_modulesDirs.begin(), m_modulesDirs.end(), dir) != m_modulesDirs.end())
        return;
    m_modulesDirs.push_back(dir);
}

std::vector<std::string> ModuleRegistry::modulesDirs() const {
    std::shared_lock lock(m_mutex);
    return m_modulesDirs;
}

void ModuleRegistry::discoverInstalledModules() {
    std::unique_lock lock(m_mutex);

    PackageManagerLib& pm = packageManagerInstance();
    if (!m_modulesDirs.empty()) {
        pm.setEmbeddedModulesDirectory(m_modulesDirs.front());
        for (std::size_t i = 1; i < m_modulesDirs.size(); ++i) {
            pm.addEmbeddedModulesDirectory(m_modulesDirs[i]);
        }
    }

    std::vector<InstalledPackage> modules = pm.getInstalledModules();

    // Collect names seen in this scan. Used after the upsert loop to prune
    // entries for modules whose files disappeared (typical path: the user
    // uninstalls a module — its directory is removed, but without pruning
    // the stale ModuleInfo would stay in m_modules forever and
    // knownModuleNames()/`logos_core_get_known_modules` would keep returning
    // it, so the UI would never see the uninstall land.
    std::unordered_set<std::string> scannedNames;

    for (const InstalledPackage& mod : modules) {
        if (mod.name.empty() || mod.mainFilePath.empty())
            continue;

        // Bind identity to the TRUSTED package name (mod.name), not the
        // self-asserted name embedded in the plugin binary. processModuleInternal
        // refuses the plugin if its embedded metadata name disagrees, so a
        // package cannot register under a privileged name it doesn't own.
        std::string moduleName = processModuleInternal(mod.mainFilePath, mod.name);
        if (moduleName.empty()) {
            spdlog::warn("Failed to process module: {}", mod.mainFilePath);
            continue;
        }
        scannedNames.insert(moduleName);
    }

    // Prune entries that aren't on disk anymore. Preserve currently-loaded
    // modules even if their backing files are gone — the module is still
    // running, and the metadata is still needed by unloadModule / cascade
    // teardown until it exits. The next discovery after that unload will
    // evict the entry.
    std::vector<std::string> toRemove;
    for (const auto& [name, info] : m_modules) {
        if (scannedNames.count(name) == 0 && !hasActiveRuntimeForModuleLocked(name))
            toRemove.push_back(name);
    }
    for (const std::string& name : toRemove) {
        m_modules.erase(name);
    }

    // Graph has its final shape (upserts + prunes applied). Re-derive
    // dependents so cascade / ModuleManager::getDependents can read them
    // directly from ModuleInfo without re-querying PackageManagerLib.
    recomputeDependentsLocked();
}

std::string ModuleRegistry::processModule(const std::string& modulePath) {
    std::unique_lock lock(m_mutex);
    std::string name = processModuleInternal(modulePath);
    // A single module changed, but its new dependency list can invert edges
    // elsewhere in the graph (e.g. an upgrade that drops a dep). Full
    // rebuild is simpler and still O(N * avg_deps) — cheap at module scale.
    recomputeDependentsLocked();
    return name;
}

std::string ModuleRegistry::processModuleInternal(const std::string& modulePath,
                                                  const std::string& trustedName) {
    // The plugin's *self-asserted* identity, read verbatim from its embedded
    // metadata. This is attacker-controlled for any plugin we didn't build,
    // so it must never be trusted as the module's identity on its own.
    std::string embedded = ModuleLib::LogosModule::getModuleName(modulePath);
    if (embedded.empty()) {
        spdlog::warn("No valid metadata for module: {}", modulePath);
        return {};
    }

    // When discovery supplies a trusted package name, the plugin's
    // embedded name MUST match it. Otherwise a package installed under an
    // innocuous name could ship a binary claiming a privileged name (e.g.
    // "capability_module") and get wired into that module's token/trust
    // relationships. Refuse the mismatch rather than silently honoring either
    // name. With no trusted name (the raw processModule() host API), fall back
    // to the embedded name as before.
    if (!trustedName.empty() && embedded != trustedName) {
        spdlog::error(
            "Refusing module {}: embedded name '{}' does not match package name '{}'",
            modulePath, embedded, trustedName);
        return {};
    }

    const std::string& name = trustedName.empty() ? embedded : trustedName;

    // The module name comes from untrusted plugin JSON metadata and later
    // becomes the registry map key, the LogosAPI RPC target, and the
    // instance-persistence directory segment. Reject any name that is not a
    // valid module identifier here, at the trust boundary, so a crafted name
    // like "x/../../victim" cannot escape the data dir (CWE-22) or collide
    // with another module's key. isValidModuleName is the single source of
    // truth, so every downstream sink inherits the same guarantee.
    if (!logos::isValidModuleName(name)) {
        spdlog::warn("Rejecting module with invalid name '{}' from {}", name, modulePath);
        return {};
    }

    // Update module info in place so re-discovery preserves the loaded flag
    // (and any other state that lives on ModuleInfo).
    ModuleInfo& info = m_modules[name];
    info.path = modulePath;
    info.metadataJson = ModuleLib::LogosModule::getRawMetadataJson(modulePath);
    info.dependencies.clear();
    for (const auto& d : ModuleLib::LogosModule::getModuleDependencies(modulePath)) {
        info.dependencies.push_back(d);
    }

    return name;
}

bool ModuleRegistry::isKnown(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    return m_modules.count(name) > 0;
}

std::string ModuleRegistry::modulePath(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.path : std::string{};
}

nlohmann::json ModuleRegistry::allModulesInfo() const {
    std::shared_lock lock(m_mutex);
    nlohmann::json modules = nlohmann::json::array();
    for (const auto& [name, info] : m_modules) {
        nlohmann::json entry;
        entry["name"]         = name;
        entry["path"]         = info.path;
        entry["loaded"]       = info.loaded;
        // Unix-seconds timestamp of the current load (0 when not loaded).
        // Callers compute uptime as now - loaded_at while loaded.
        entry["loaded_at"]    = info.loadedAt;
        entry["dependencies"] = info.dependencies;
        entry["dependents"]   = info.dependents;
        // Parse the cached metadata JSON back into structured form. Tolerate a
        // missing/garbled blob by reporting null rather than aborting the call.
        if (info.metadataJson.empty()) {
            entry["metadata"] = nlohmann::json(nullptr);
        } else {
            nlohmann::json meta = nlohmann::json::parse(
                info.metadataJson, nullptr, /*allow_exceptions=*/false);
            entry["metadata"] = meta.is_discarded() ? nlohmann::json(nullptr) : meta;
        }
        modules.push_back(std::move(entry));
    }
    return modules;
}

std::vector<std::string> ModuleRegistry::moduleDependencies(const std::string& name,
                                                            bool recursive) const {
    std::shared_lock lock(m_mutex);
    return moduleDependenciesLocked(name, recursive);
}

std::vector<std::string> ModuleRegistry::moduleDependenciesLocked(const std::string& name,
                                                                  bool recursive) const {
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return {};

    if (!recursive)
        return it->second.dependencies;

    // BFS over the forward graph. `seen` is pre-seeded with `name` so a
    // dependency cycle that leads back to the target can't append the
    // target to the output — callers treat "transitive deps of X" as
    // "everything needed besides X itself". `out` preserves first-visit
    // order so callers get a stable traversal across diamonds.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    seen.insert(name);
    std::deque<std::string> queue(it->second.dependencies.begin(),
                                   it->second.dependencies.end());
    while (!queue.empty()) {
        std::string current = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        auto depIt = m_modules.find(current);
        if (depIt == m_modules.end()) continue;
        for (const std::string& d : depIt->second.dependencies) {
            if (seen.count(d) == 0) queue.push_back(d);
        }
    }
    return out;
}

std::vector<std::string> ModuleRegistry::moduleDependents(const std::string& name,
                                                          bool recursive) const {
    std::shared_lock lock(m_mutex);
    return moduleDependentsLocked(name, recursive);
}

std::vector<std::string> ModuleRegistry::moduleDependentsLocked(const std::string& name,
                                                                bool recursive) const {
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return {};

    if (!recursive)
        return it->second.dependents;

    // BFS over the reverse graph. Same invariants as the forward walk:
    // `seen` is pre-seeded with `name` so a cyclic edge back to the target
    // doesn't append it to the output; duplicate entries in diamonds are
    // de-duped; `out` preserves first-visit order.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    seen.insert(name);
    std::deque<std::string> queue(it->second.dependents.begin(),
                                   it->second.dependents.end());
    while (!queue.empty()) {
        std::string current = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        auto depIt = m_modules.find(current);
        if (depIt == m_modules.end()) continue;
        for (const std::string& d : depIt->second.dependents) {
            if (seen.count(d) == 0) queue.push_back(d);
        }
    }
    return out;
}

void ModuleRegistry::recomputeDependentsLocked() {
    // Wipe the reverse edges in place — we don't want to reallocate each
    // ModuleInfo, so clear() keeps any existing vector capacity.
    for (auto& [k, v] : m_modules)
        v.dependents.clear();

    // Invert every forward edge. An entry whose dependency points at an
    // unknown module is silently skipped — we can't register a reverse
    // edge against something we don't track, and logging per-edge here
    // would flood the log during every discovery pass.
    for (const auto& [depender, info] : m_modules) {
        for (const std::string& dep : info.dependencies) {
            auto depIt = m_modules.find(dep);
            if (depIt == m_modules.end()) continue;
            auto& deps = depIt->second.dependents;
            if (std::find(deps.begin(), deps.end(), depender) == deps.end())
                deps.push_back(depender);
        }
    }
}

std::vector<std::string> ModuleRegistry::knownModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_modules.size());
    for (const auto& [k, v] : m_modules)
        keys.push_back(k);
    return keys;
}

void ModuleRegistry::registerModule(const std::string& name, const std::string& path,
                                    const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    ModuleInfo& info = m_modules[name];
    info.path = path;
    // Always assign dependencies (even when empty) and recompute reverse
    // edges. Two reasons we can't gate this on `dependencies.empty()`:
    //   1. Registering "b" with `{}` after an earlier registerDependencies("a",
    //      {"b"}) must give "b" a dependent entry for "a" — the earlier
    //      recompute skipped the unknown edge, and this is the registration
    //      that makes "a → b" visible.
    //   2. Callers need a way to clear forward edges by passing `{}`.
    info.dependencies = dependencies;
    recomputeDependentsLocked();
}

void ModuleRegistry::registerDependencies(const std::string& name, const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_modules[name].dependencies = dependencies;
    // Same reasoning as registerModule: this is a direct graph mutator used
    // by tests. Keep the dependents-consistent-with-dependencies invariant
    // holding across every path that edits forward edges.
    recomputeDependentsLocked();
}

bool ModuleRegistry::isLoaded(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() && it->second.loaded;
}

// Current wall-clock time in unix seconds. Stamped on load so callers can
// derive a module's uptime; a free function so both markLoaded overloads
// agree on the source.
static int64_t nowUnixSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

void ModuleRegistry::markLoaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    const LogosCore::ModuleAddress address{name, {}};
    ModuleRuntimeInfo& runtime = m_runtimes[address];
    runtime.generation = m_nextRuntimeGeneration++;
    runtime.loading = false;
    runtime.loadedAt = nowUnixSeconds();
    runtime.loader.reset();
    runtime.handle = {};
    runtime.handle.name = name;
    syncDefaultRuntimeToModuleInfoLocked(address, &runtime);
}

void ModuleRegistry::markLoaded(const std::string& name,
                                 std::shared_ptr<LogosCore::ModuleLoader> loader,
                                 LogosCore::LoadedModuleHandle handle) {
    std::unique_lock lock(m_mutex);
    const LogosCore::ModuleAddress address{name, {}};
    ModuleRuntimeInfo& runtime = m_runtimes[address];
    runtime.generation = m_nextRuntimeGeneration++;
    runtime.loading = false;
    runtime.loadedAt = nowUnixSeconds();
    runtime.loader = std::move(loader);
    runtime.handle = std::move(handle);
    runtime.handle.name = name;
    runtime.handle.instanceId.clear();
    syncDefaultRuntimeToModuleInfoLocked(address, &runtime);
}

std::shared_ptr<LogosCore::ModuleLoader>
ModuleRegistry::loaderFor(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return nullptr;
    return it->second.loader;
}

void ModuleRegistry::markUnloaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    const LogosCore::ModuleAddress address{name, {}};
    m_runtimes.erase(address);
    syncDefaultRuntimeToModuleInfoLocked(address, nullptr);
}

std::vector<std::string> ModuleRegistry::loadedModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_modules) {
        if (v.loaded)
            result.push_back(k);
    }
    return result;
}

void ModuleRegistry::clearLoaded() {
    std::unique_lock lock(m_mutex);
    m_runtimes.clear();
    for (auto& [k, v] : m_modules) {
        v.loaded = false;
        v.loadedAt = 0;
        v.loader.reset();
        v.handle = {};
    }
}

bool ModuleRegistry::hasActiveRuntimeForModuleLocked(const std::string& name) const {
    return std::any_of(m_runtimes.begin(), m_runtimes.end(),
        [&name](const auto& entry) {
            return entry.first.moduleName == name;
        });
}

void ModuleRegistry::syncDefaultRuntimeToModuleInfoLocked(
    const LogosCore::ModuleAddress& address,
    const ModuleRuntimeInfo* runtime) {
    if (!address.isDefaultInstance()) return;

    ModuleInfo& info = m_modules[address.moduleName];
    const bool loaded = runtime && !runtime->loading;
    info.loaded = loaded;
    info.loadedAt = loaded ? runtime->loadedAt : 0;
    info.loader = loaded ? runtime->loader : nullptr;
    info.handle = loaded ? runtime->handle : LogosCore::LoadedModuleHandle{};
}

std::optional<uint64_t> ModuleRegistry::reserveRuntime(
    const LogosCore::ModuleAddress& address) {
    if (!address.isValid()) return std::nullopt;

    std::unique_lock lock(m_mutex);
    if (m_runtimes.count(address) != 0)
        return std::nullopt;

    ModuleRuntimeInfo runtime;
    runtime.generation = m_nextRuntimeGeneration++;
    runtime.loading = true;
    const uint64_t generation = runtime.generation;
    m_runtimes.emplace(address, std::move(runtime));
    return generation;
}

void ModuleRegistry::cancelRuntime(const LogosCore::ModuleAddress& address,
                                   uint64_t generation) {
    std::unique_lock lock(m_mutex);
    const auto it = m_runtimes.find(address);
    if (it == m_runtimes.end() || it->second.generation != generation)
        return;

    m_runtimes.erase(it);
    syncDefaultRuntimeToModuleInfoLocked(address, nullptr);
}

bool ModuleRegistry::completeRuntime(
    const LogosCore::ModuleAddress& address,
    uint64_t generation,
    std::shared_ptr<LogosCore::ModuleLoader> loader,
    LogosCore::LoadedModuleHandle handle) {
    std::unique_lock lock(m_mutex);
    const auto it = m_runtimes.find(address);
    if (it == m_runtimes.end() || it->second.generation != generation || !it->second.loading)
        return false;

    // The loader must preserve the address it was asked to launch. Accept an
    // omitted name only for old test doubles, then normalize it; an explicit
    // instance ID, however, must never be silently collapsed to default.
    if (handle.name.empty()) handle.name = address.moduleName;
    if (handle.name != address.moduleName || handle.instanceId != address.instanceId) {
        spdlog::error("Loader returned a handle for a different runtime address");
        m_runtimes.erase(it);
        syncDefaultRuntimeToModuleInfoLocked(address, nullptr);
        return false;
    }

    ModuleRuntimeInfo& runtime = it->second;
    runtime.loading = false;
    runtime.loadedAt = nowUnixSeconds();
    runtime.loader = std::move(loader);
    runtime.handle = std::move(handle);
    syncDefaultRuntimeToModuleInfoLocked(address, &runtime);
    return true;
}

void ModuleRegistry::markRuntimeUnloaded(const LogosCore::ModuleAddress& address) {
    std::unique_lock lock(m_mutex);
    m_runtimes.erase(address);
    syncDefaultRuntimeToModuleInfoLocked(address, nullptr);
}

void ModuleRegistry::markRuntimeUnloadedIfGeneration(
    const LogosCore::ModuleAddress& address,
    uint64_t generation) {
    std::unique_lock lock(m_mutex);
    const auto it = m_runtimes.find(address);
    if (it == m_runtimes.end() || it->second.generation != generation)
        return;

    m_runtimes.erase(it);
    syncDefaultRuntimeToModuleInfoLocked(address, nullptr);
}

bool ModuleRegistry::isRuntimeLoaded(const LogosCore::ModuleAddress& address) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_runtimes.find(address);
    return it != m_runtimes.end() && !it->second.loading;
}

std::shared_ptr<LogosCore::ModuleLoader>
ModuleRegistry::loaderForRuntime(const LogosCore::ModuleAddress& address) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_runtimes.find(address);
    if (it == m_runtimes.end() || it->second.loading)
        return nullptr;
    return it->second.loader;
}

nlohmann::json ModuleRegistry::allRuntimeInstancesInfo() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::pair<LogosCore::ModuleAddress, const ModuleRuntimeInfo*>> runtimes;
    runtimes.reserve(m_runtimes.size());
    for (const auto& [address, runtime] : m_runtimes) {
        if (!runtime.loading)
            runtimes.emplace_back(address, &runtime);
    }
    std::sort(runtimes.begin(), runtimes.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first.moduleName != rhs.first.moduleName)
            return lhs.first.moduleName < rhs.first.moduleName;
        return lhs.first.instanceId < rhs.first.instanceId;
    });

    nlohmann::json result = nlohmann::json::array();
    for (const auto& [address, runtime] : runtimes) {
        result.push_back({
            {"module_name", address.moduleName},
            {"instance_id", address.instanceId},
            {"default_instance", address.isDefaultInstance()},
            {"loaded_at", runtime->loadedAt},
            {"pid", runtime->handle.pid},
            {"endpoint", runtime->handle.endpoint},
        });
    }
    return result;
}

void ModuleRegistry::clear() {
    std::unique_lock lock(m_mutex);
    m_modulesDirs.clear();
    m_modules.clear();
    m_runtimes.clear();
    // Do not reset m_nextRuntimeGeneration: a container callback can arrive
    // after clear() and must never collide with a newly launched runtime in a
    // subsequent core lifetime inside the same process.
}
