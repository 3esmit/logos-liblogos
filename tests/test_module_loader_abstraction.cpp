// =============================================================================
// Tests for the ModuleLoader abstraction seam.
//
// Installs a FakeModuleLoader into ModuleManager's ModuleLoaderRegistry and drives the
// full ModuleManager load/unload/terminateAll path. Proves that:
//   - load(), sendToken(), terminate(), terminateAll() are routed through the
//     loader abstraction (not directly to a subprocess or Qt mechanism).
//   - Dependency-ordered loads call load() in the correct (topo) order.
//   - Error paths (load returns false) prevent sendToken from being called.
// No child processes are spawned; no Qt Remote Objects are used.
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include "module_manager.h"
#include "module_registry.h"
#include "module_loader_registry.h"
#include "module_loader.h"
#include "subprocess_manager.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// FakeModuleLoader: records all calls; configurable per-module load result.
// Placed in an anonymous namespace to avoid ODR conflicts with the FakeModuleLoader
// stub in test_module_loader_registry.cpp (same binary, different definition).
// ---------------------------------------------------------------------------
namespace {

struct FakeModuleLoader : public InstanceAwareModuleLoader {
    std::string id() const override { return "fake"; }

    bool canHandle(const ModuleDescriptor&) const override { return true; }

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LoadedModuleHandle& out) override {
        loadCalls.push_back(desc.name);
        if (failOn.count(desc.name)) return false;
        out.name = desc.name;
        out.pid  = 1234;
        out.endpoint = "fake://" + desc.name;
        activeModules.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return true;
    }

    void terminate(const std::string& name) override {
        terminateCalls.push_back(name);
        activeModules.erase(name);
    }

    void terminateAll() override {
        terminateAllCount++;
        activeModules.clear();
        activeInstances.clear();
    }

    bool hasModule(const std::string& name) const override {
        return activeModules.count(name) > 0;
    }

    bool loadInstance(const ModuleDescriptor& desc,
                      std::function<void(const ModuleAddress&)> onTerminated,
                      LoadedModuleHandle& out) override {
        const ModuleAddress address = desc.address();
        if (!address.isValid() || failInstances.count(address) || activeInstances.count(address))
            return false;
        instanceLoadCalls.push_back(desc);
        out.name = address.moduleName;
        out.instanceId = address.instanceId;
        out.pid = 2000 + static_cast<int64_t>(instanceLoadCalls.size());
        out.endpoint = "fake://" + address.moduleName + "/" + address.instanceId;
        activeInstances.insert(address);
        instanceCallbacks[address] = std::move(onTerminated);
        return true;
    }

    bool sendTokenToInstance(const ModuleAddress& address,
                             const std::string& token) override {
        if (activeInstances.count(address) == 0) return false;
        instanceTokenCalls.push_back({address, token});
        return true;
    }

    bool terminateInstance(const ModuleAddress& address) override {
        instanceTerminateCalls.push_back(address);
        return activeInstances.erase(address) > 0;
    }

    bool hasInstance(const ModuleAddress& address) const override {
        return activeInstances.count(address) > 0;
    }

    std::optional<int64_t> instancePid(const ModuleAddress& address) const override {
        if (activeInstances.count(address) == 0) return std::nullopt;
        return 2000;
    }

    std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>
    getAllInstancePids() const override {
        std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash> result;
        for (const auto& address : activeInstances)
            result.emplace(address, 2000);
        return result;
    }

    std::function<void(const ModuleAddress&)> callbackFor(const ModuleAddress& address) const {
        const auto it = instanceCallbacks.find(address);
        return it == instanceCallbacks.end()
            ? std::function<void(const ModuleAddress&)>{}
            : it->second;
    }

    // Call records
    std::vector<std::string>                         loadCalls;
    std::vector<std::pair<std::string,std::string>>  sendTokenCalls;
    std::vector<std::string>                         terminateCalls;
    std::vector<ModuleDescriptor>                     instanceLoadCalls;
    std::vector<std::pair<ModuleAddress, std::string>> instanceTokenCalls;
    std::vector<ModuleAddress>                        instanceTerminateCalls;
    int                                              terminateAllCount = 0;

    // Modules to fail on load
    std::unordered_set<std::string>                  failOn;
    std::unordered_set<ModuleAddress, ModuleAddressHash> failInstances;
    // Modules currently "running"
    std::unordered_set<std::string>                  activeModules;
    std::unordered_set<ModuleAddress, ModuleAddressHash> activeInstances;
    std::unordered_map<ModuleAddress, std::function<void(const ModuleAddress&)>,
                       ModuleAddressHash> instanceCallbacks;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture: installs FakeModuleLoader, cleans up registry after each test.
// ---------------------------------------------------------------------------

class ModuleLoaderAbstractionTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeModuleLoader> fake;
    std::filesystem::path persistenceRoot;

    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();

        fake = std::make_shared<FakeModuleLoader>();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(fake);

        persistenceRoot = std::filesystem::temp_directory_path() /
            ("logos-liblogos-instance-test-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(persistenceRoot);
        const std::string root = persistenceRoot.string();
        logos_core_set_persistence_base_path(root.c_str());
    }

    void TearDown() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();
        // Restore default loader so other test suites aren't affected.
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(
            std::make_shared<SubprocessManager>());
        std::filesystem::remove_all(persistenceRoot);
    }

    void registerModule(const std::string& name,
                        const std::vector<std::string>& deps = {}) {
        std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
        std::vector<const char*> depPtrs;
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_module_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));
    }
};

// =============================================================================
// Basic load/unload routing
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadModule_CallsFakeModuleLoaderLoad) {
    registerModule("foo");

    int result = logos_core_load_module("foo", false);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "foo");
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_CallsSendTokenAfterLoad) {
    registerModule("foo");

    logos_core_load_module("foo", false);

    ASSERT_EQ(fake->sendTokenCalls.size(), 1u);
    EXPECT_EQ(fake->sendTokenCalls[0].first, "foo");
    EXPECT_FALSE(fake->sendTokenCalls[0].second.empty());
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_MarksModuleAsLoaded) {
    registerModule("foo");

    logos_core_load_module("foo", false);

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 1);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_StoresLoaderInRegistry) {
    registerModule("foo");
    logos_core_load_module("foo", false);

    auto rt = ModuleManager::registry().loaderFor("foo");
    EXPECT_EQ(rt.get(), fake.get());
}

TEST_F(ModuleLoaderAbstractionTest, UnloadModule_CallsFakeModuleLoaderTerminate) {
    registerModule("foo");
    logos_core_load_module("foo", false);

    int result = logos_core_unload_module("foo", false);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->terminateCalls.size(), 1u);
    EXPECT_EQ(fake->terminateCalls[0], "foo");
}

TEST_F(ModuleLoaderAbstractionTest, UnloadModule_MarksModuleAsUnloaded) {
    registerModule("foo");
    logos_core_load_module("foo", false);
    logos_core_unload_module("foo", false);

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Explicit runtime-instance routing
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest,
       ScopedInstances_KeepPersistenceTransportTokenAndInspectionSeparate) {
    registerModule("lez_indexer_module");
    const ModuleAddress alpha{"lez_indexer_module", "zone_alpha"};
    const ModuleAddress beta{"lez_indexer_module", "zone_beta"};

    logos_core_set_module_instance_transports(
        alpha.moduleName.c_str(), alpha.instanceId.c_str(), "[\"alpha-transport\"]");
    logos_core_set_module_instance_transports(
        beta.moduleName.c_str(), beta.instanceId.c_str(), "[\"beta-transport\"]");

    ASSERT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 1);
    ASSERT_EQ(logos_core_load_module_instance(
                  beta.moduleName.c_str(), beta.instanceId.c_str(), false), 1);

    ASSERT_EQ(fake->instanceLoadCalls.size(), 2u);
    EXPECT_EQ(fake->instanceLoadCalls[0].address(), alpha);
    EXPECT_EQ(fake->instanceLoadCalls[1].address(), beta);
    EXPECT_EQ(fake->instanceLoadCalls[0].transportSetJson, "[\"alpha-transport\"]");
    EXPECT_EQ(fake->instanceLoadCalls[1].transportSetJson, "[\"beta-transport\"]");
    EXPECT_EQ(fake->instanceLoadCalls[0].instancePersistencePath,
              (persistenceRoot / alpha.moduleName / alpha.instanceId).string());
    EXPECT_EQ(fake->instanceLoadCalls[1].instancePersistencePath,
              (persistenceRoot / beta.moduleName / beta.instanceId).string());

    ASSERT_EQ(fake->instanceTokenCalls.size(), 2u);
    EXPECT_EQ(fake->instanceTokenCalls[0].first, alpha);
    EXPECT_EQ(fake->instanceTokenCalls[1].first, beta);
    EXPECT_NE(fake->instanceTokenCalls[0].second, fake->instanceTokenCalls[1].second);
    const std::string alphaTokenKey = logos::scopedModuleTokenKey(
        QString::fromStdString(alpha.moduleName),
        QString::fromStdString(alpha.instanceId)).toStdString();
    const std::string betaTokenKey = logos::scopedModuleTokenKey(
        QString::fromStdString(beta.moduleName),
        QString::fromStdString(beta.instanceId)).toStdString();
    EXPECT_EQ(TokenManager::instance().getToken(alphaTokenKey),
              fake->instanceTokenCalls[0].second);
    EXPECT_EQ(TokenManager::instance().getToken(betaTokenKey),
              fake->instanceTokenCalls[1].second);

    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str()), 1);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  beta.moduleName.c_str(), beta.instanceId.c_str()), 1);
    // The legacy name-only state describes only the default runtime.
    EXPECT_EQ(logos_core_is_module_loaded("lez_indexer_module"), 0);

    char* rawInfo = logos_core_get_module_instances_info();
    ASSERT_NE(rawInfo, nullptr);
    const nlohmann::json info = nlohmann::json::parse(rawInfo);
    delete[] rawInfo;
    ASSERT_EQ(info.size(), 2u);
    EXPECT_EQ(info[0]["module_name"], alpha.moduleName);
    EXPECT_EQ(info[0]["instance_id"], alpha.instanceId);
    EXPECT_EQ(info[1]["module_name"], beta.moduleName);
    EXPECT_EQ(info[1]["instance_id"], beta.instanceId);

    // Explicit creation is intentionally not idempotent: a caller gets a
    // deterministic failure rather than falsely believing it created a third
    // Zone runtime at the same address.
    EXPECT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 0);
}

TEST_F(ModuleLoaderAbstractionTest,
       ScopedInstance_TerminationCannotClearSiblingOrReplacement) {
    registerModule("lez_indexer_module");
    const ModuleAddress alpha{"lez_indexer_module", "zone_alpha"};
    const ModuleAddress beta{"lez_indexer_module", "zone_beta"};

    ASSERT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 1);
    ASSERT_EQ(logos_core_load_module_instance(
                  beta.moduleName.c_str(), beta.instanceId.c_str(), false), 1);
    const auto staleAlphaCallback = fake->callbackFor(alpha);
    ASSERT_TRUE(staleAlphaCallback);

    // A crash notification for alpha leaves beta intact.
    staleAlphaCallback(alpha);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str()), 0);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  beta.moduleName.c_str(), beta.instanceId.c_str()), 1);

    // Simulate the container reaping the old process before the next launch.
    fake->activeInstances.erase(alpha);
    ASSERT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 1);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str()), 1);

    // A delayed callback from the old alpha generation must not erase the
    // replacement at the same address.
    staleAlphaCallback(alpha);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str()), 1);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  beta.moduleName.c_str(), beta.instanceId.c_str()), 1);
}

TEST_F(ModuleLoaderAbstractionTest,
       ScopedInstance_CallbackAfterCoreClearCannotClearNewLifetime) {
    registerModule("lez_indexer_module");
    const ModuleAddress alpha{"lez_indexer_module", "zone_alpha"};
    ASSERT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 1);
    const auto staleCallback = fake->callbackFor(alpha);
    ASSERT_TRUE(staleCallback);

    // clear() is a real daemon-restart boundary, but process exit callbacks
    // are asynchronous and can arrive afterwards.
    logos_core_clear();
    registerModule("lez_indexer_module");
    ASSERT_EQ(logos_core_load_module_instance(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str(), false), 1);

    staleCallback(alpha);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  alpha.moduleName.c_str(), alpha.instanceId.c_str()), 1);
}

TEST_F(ModuleLoaderAbstractionTest,
       ScopedInstance_ValidatesAddressAndKeepsDefaultCompatibilityPath) {
    registerModule("lez_indexer_module");

    EXPECT_EQ(logos_core_load_module_instance("lez_indexer_module", "bad/id", false), 0);
    EXPECT_EQ(logos_core_load_module_instance("", "zone_alpha", false), 0);

    // Empty instance IDs explicitly select the established default runtime.
    EXPECT_EQ(logos_core_load_module_instance("lez_indexer_module", "", false), 1);
    EXPECT_EQ(logos_core_is_module_loaded("lez_indexer_module"), 1);
    EXPECT_EQ(logos_core_is_module_instance_loaded("lez_indexer_module", ""), 1);
    EXPECT_EQ(logos_core_load_module_instance("lez_indexer_module", nullptr, false), 1);
}

TEST_F(ModuleLoaderAbstractionTest,
       ScopedInstance_LoadsSharedDependenciesButNeverCascadesThemOnUnload) {
    registerModule("storage_module");
    registerModule("lez_indexer_module", {"storage_module"});
    const ModuleAddress indexer{"lez_indexer_module", "zone_alpha"};

    ASSERT_EQ(logos_core_load_module_instance(
                  indexer.moduleName.c_str(), indexer.instanceId.c_str(), true), 1);
    EXPECT_EQ(logos_core_is_module_loaded("storage_module"), 1);
    EXPECT_EQ(logos_core_is_module_instance_loaded(
                  indexer.moduleName.c_str(), indexer.instanceId.c_str()), 1);

    EXPECT_EQ(logos_core_unload_module_instance(
                  indexer.moduleName.c_str(), indexer.instanceId.c_str(), true), 0);
    EXPECT_EQ(logos_core_unload_module_instance(
                  indexer.moduleName.c_str(), indexer.instanceId.c_str(), false), 1);
    EXPECT_EQ(logos_core_is_module_loaded("storage_module"), 1);
}

// =============================================================================
// Dependency-ordered loads
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_LoadsInTopologicalOrder) {
    // Chain: c depends on b, b depends on a.
    // Expected load order: a, b, c.
    registerModule("a");
    registerModule("b", {"a"});
    registerModule("c", {"b"});

    int result = logos_core_load_module("c", true);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 3u);
    EXPECT_EQ(fake->loadCalls[0], "a");
    EXPECT_EQ(fake->loadCalls[1], "b");
    EXPECT_EQ(fake->loadCalls[2], "c");
}

TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_SkipsAlreadyLoadedModules) {
    registerModule("a");
    registerModule("b", {"a"});

    logos_core_load_module("a", false);
    fake->loadCalls.clear();

    logos_core_load_module("b", true);

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "b");
}

// LoadsInTopologicalOrder (above) pins the *call sequence*. This one pins the
// *observable end state*: requesting a single top-level module with
// with_dependencies=true must leave that module's entire transitive
// dependency closure loaded, as reported by the public query API. This is the
// guarantee callers actually rely on ("load app, get everything it needs"),
// and it exercises a diamond (app → ui, core; ui → core) so a dependency
// reachable by two paths is still loaded exactly once and not skipped.
TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_LeavesTransitiveClosureLoaded) {
    registerModule("core");
    registerModule("ui",  {"core"});
    registerModule("app", {"ui", "core"});

    // Precondition: nothing in the closure is loaded yet.
    ASSERT_EQ(logos_core_is_module_loaded("app"),  0);
    ASSERT_EQ(logos_core_is_module_loaded("ui"),   0);
    ASSERT_EQ(logos_core_is_module_loaded("core"), 0);

    // Only the top module is requested.
    int result = logos_core_load_module("app", true);
    ASSERT_EQ(result, 1);

    // The whole closure ends up loaded — the deps were auto-resolved.
    EXPECT_EQ(logos_core_is_module_loaded("app"),  1);
    EXPECT_EQ(logos_core_is_module_loaded("ui"),   1);
    EXPECT_EQ(logos_core_is_module_loaded("core"), 1);

    // The diamond's shared dependency loads exactly once despite two paths.
    int coreLoads = 0;
    for (const auto& n : fake->loadCalls)
        if (n == "core") ++coreLoads;
    EXPECT_EQ(coreLoads, 1);
}

// =============================================================================
// terminateAll routing
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, TerminateAll_CallsFakeTerminateAll) {
    registerModule("foo");
    logos_core_load_module("foo", false);

    logos_core_terminate_all();

    EXPECT_EQ(fake->terminateAllCount, 1);
    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Error paths
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadModule_ReturnsFalseWhenLoaderLoadFails) {
    registerModule("bad");
    fake->failOn.insert("bad");

    int result = logos_core_load_module("bad", false);
    EXPECT_EQ(result, 0);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_DoesNotCallSendTokenOnLoadFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad", false);

    EXPECT_TRUE(fake->sendTokenCalls.empty());
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_DoesNotMarkAsLoadedOnFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad", false);

    EXPECT_EQ(logos_core_is_module_loaded("bad"), 0);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_ReturnsFalseForUnknownModule) {
    int result = logos_core_load_module("not_registered", false);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(fake->loadCalls.empty());
}
