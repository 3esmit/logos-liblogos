// =============================================================================
// Tests for CompositeModuleLoader — verifies that it correctly delegates to its
// ModuleContainer and ModuleFormatLoader, producing a valid ModuleLoader.
//
// Uses fake/stub implementations to test the composition logic in isolation.
// No real processes are spawned.
// =============================================================================
#include <gtest/gtest.h>
#include "composite_module_loader.h"
#include <logos_container/module_container.h>
#include <logos_module_loader/module_format_loader.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// Stubs — defined outside anonymous namespace to avoid Clang
// std::make_shared / compressed_pair issues with internal-linkage types.
// ---------------------------------------------------------------------------

struct FakeContainer : public InstanceAwareModuleContainer {
    std::string id() const override { return "fake-container"; }
    bool canHandle(const ModuleDescriptor&) const override { return containerCanHandle; }

    bool launch(const ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string&)>,
                LoadedModuleHandle& out) override {
        launchCalls.push_back({desc.name, hostBinary, args});
        if (!launchShouldSucceed) return false;
        out.name = desc.name;
        out.pid  = 42;
        activeModules.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return sendTokenResult;
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

    std::optional<int64_t> pid(const std::string& name) const override {
        if (activeModules.count(name)) return 42;
        return std::nullopt;
    }

    std::unordered_map<std::string, int64_t> getAllPids() const override {
        std::unordered_map<std::string, int64_t> pids;
        for (const auto& m : activeModules) pids[m] = 42;
        return pids;
    }

    bool launchInstance(const ModuleDescriptor& desc,
                        const std::string& hostBinary,
                        const std::vector<std::string>& args,
                        std::function<void(const ModuleAddress&)> onTerminated,
                        LoadedModuleHandle& out) override {
        const ModuleAddress address = desc.address();
        instanceLaunchCalls.push_back({address, hostBinary, args});
        if (!address.isValid() || !instanceLaunchShouldSucceed) return false;
        out.name = address.moduleName;
        out.instanceId = address.instanceId;
        out.pid = 43;
        activeInstances.insert(address);
        instanceCallbacks[address] = std::move(onTerminated);
        return true;
    }

    bool sendTokenToInstance(const ModuleAddress& address,
                             const std::string& token) override {
        instanceTokenCalls.push_back({address, token});
        return activeInstances.count(address) > 0 && instanceSendTokenResult;
    }

    bool terminateInstance(const ModuleAddress& address) override {
        instanceTerminateCalls.push_back(address);
        return activeInstances.erase(address) > 0;
    }

    bool hasInstance(const ModuleAddress& address) const override {
        return activeInstances.count(address) > 0;
    }

    std::optional<int64_t> instancePid(const ModuleAddress& address) const override {
        return activeInstances.count(address) > 0
            ? std::optional<int64_t>{43}
            : std::nullopt;
    }

    std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>
    getAllInstancePids() const override {
        std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash> pids;
        for (const auto& address : activeInstances) pids[address] = 43;
        return pids;
    }

    struct LaunchRecord {
        std::string name;
        std::string hostBinary;
        std::vector<std::string> args;
    };

    struct InstanceLaunchRecord {
        ModuleAddress address;
        std::string hostBinary;
        std::vector<std::string> args;
    };

    bool containerCanHandle = true;
    bool launchShouldSucceed = true;
    bool sendTokenResult = true;
    bool instanceLaunchShouldSucceed = true;
    bool instanceSendTokenResult = true;
    int terminateAllCount = 0;
    std::vector<LaunchRecord> launchCalls;
    std::vector<std::pair<std::string, std::string>> sendTokenCalls;
    std::vector<std::string> terminateCalls;
    std::vector<InstanceLaunchRecord> instanceLaunchCalls;
    std::vector<std::pair<ModuleAddress, std::string>> instanceTokenCalls;
    std::vector<ModuleAddress> instanceTerminateCalls;
    std::unordered_set<std::string> activeModules;
    std::unordered_set<ModuleAddress, ModuleAddressHash> activeInstances;
    std::unordered_map<ModuleAddress, std::function<void(const ModuleAddress&)>,
                       ModuleAddressHash> instanceCallbacks;
};

struct FakeLoader : public ModuleFormatLoader {
    std::string id() const override { return "fake-loader"; }
    bool canHandle(const ModuleDescriptor&) const override { return loaderCanHandle; }

    std::string resolveHostBinary(const ModuleDescriptor&) const override {
        return hostBinary;
    }

    std::vector<std::string> buildArguments(const ModuleDescriptor& desc) const override {
        return {"--name", desc.name, "--path", desc.path};
    }

    bool loaderCanHandle = true;
    std::string hostBinary = "/usr/bin/fake_host";
};

class CompositeModuleLoaderTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeContainer> container;
    std::shared_ptr<FakeLoader> loader;
    std::shared_ptr<CompositeModuleLoader> composite;

    void SetUp() override {
        container.reset(new FakeContainer);
        loader.reset(new FakeLoader);
        std::shared_ptr<ModuleContainer> c = container;
        std::shared_ptr<ModuleFormatLoader> l = loader;
        composite.reset(new CompositeModuleLoader(c, l));
    }
};

// ---------------------------------------------------------------------------
// id
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, Id_CombinesLoaderAndContainerIds) {
    EXPECT_EQ(composite->id(), "fake-loader+fake-container");
}

// ---------------------------------------------------------------------------
// canHandle
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, CanHandle_TrueWhenBothCanHandle) {
    ModuleDescriptor desc;
    EXPECT_TRUE(composite->canHandle(desc));
}

TEST_F(CompositeModuleLoaderTest, CanHandle_FalseWhenContainerCannot) {
    container->containerCanHandle = false;
    ModuleDescriptor desc;
    EXPECT_FALSE(composite->canHandle(desc));
}

TEST_F(CompositeModuleLoaderTest, CanHandle_FalseWhenLoaderCannot) {
    loader->loaderCanHandle = false;
    ModuleDescriptor desc;
    EXPECT_FALSE(composite->canHandle(desc));
}

// ---------------------------------------------------------------------------
// load: resolves host + builds args via loader, launches via container
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, Load_DelegatesResolveAndLaunch) {
    ModuleDescriptor desc;
    desc.name = "mod_a";
    desc.path = "/lib/mod_a.so";
    LoadedModuleHandle handle;

    bool ok = composite->load(desc, nullptr, handle);
    EXPECT_TRUE(ok);
    EXPECT_EQ(handle.name, "mod_a");
    EXPECT_EQ(handle.pid, 42);

    ASSERT_EQ(container->launchCalls.size(), 1u);
    EXPECT_EQ(container->launchCalls[0].name, "mod_a");
    EXPECT_EQ(container->launchCalls[0].hostBinary, "/usr/bin/fake_host");
    std::vector<std::string> expected = {"--name", "mod_a", "--path", "/lib/mod_a.so"};
    EXPECT_EQ(container->launchCalls[0].args, expected);
}

TEST_F(CompositeModuleLoaderTest, Load_FailsWhenHostBinaryEmpty) {
    loader->hostBinary = "";
    ModuleDescriptor desc;
    desc.name = "no_host";
    LoadedModuleHandle handle;

    EXPECT_FALSE(composite->load(desc, nullptr, handle));
    EXPECT_TRUE(container->launchCalls.empty());
}

TEST_F(CompositeModuleLoaderTest, Load_FailsWhenContainerLaunchFails) {
    container->launchShouldSucceed = false;
    ModuleDescriptor desc;
    desc.name = "fail_launch";
    LoadedModuleHandle handle;

    EXPECT_FALSE(composite->load(desc, nullptr, handle));
}

TEST_F(CompositeModuleLoaderTest, LoadInstance_DelegatesFullAddressToInstanceAwareContainer) {
    ModuleDescriptor desc;
    desc.name = "lez_indexer_module";
    desc.instanceId = "zone_alpha";
    desc.path = "/lib/lez_indexer_module.so";
    LoadedModuleHandle handle;

    bool callbackCalled = false;
    ASSERT_TRUE(composite->loadInstance(
        desc,
        [&callbackCalled](const ModuleAddress& address) {
            callbackCalled = address == ModuleAddress{"lez_indexer_module", "zone_alpha"};
        },
        handle));

    ASSERT_EQ(container->instanceLaunchCalls.size(), 1u);
    EXPECT_EQ(container->instanceLaunchCalls[0].address, desc.address());
    EXPECT_EQ(container->instanceLaunchCalls[0].hostBinary, "/usr/bin/fake_host");
    EXPECT_EQ(handle.address(), desc.address());
    EXPECT_TRUE(composite->hasInstance(desc.address()));
    EXPECT_EQ(composite->instancePid(desc.address()), std::optional<int64_t>{43});

    EXPECT_TRUE(composite->sendTokenToInstance(desc.address(), "scoped-token"));
    ASSERT_EQ(container->instanceTokenCalls.size(), 1u);
    EXPECT_EQ(container->instanceTokenCalls[0].first, desc.address());

    const auto pids = composite->getAllInstancePids();
    ASSERT_EQ(pids.size(), 1u);
    EXPECT_EQ(pids.at(desc.address()), 43);

    EXPECT_TRUE(composite->terminateInstance(desc.address()));
    EXPECT_FALSE(composite->hasInstance(desc.address()));
    EXPECT_FALSE(callbackCalled);  // Explicit teardown is container-owned.
}

// ---------------------------------------------------------------------------
// sendToken, terminate, terminateAll delegate to container
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, SendToken_DelegatesToContainer) {
    EXPECT_TRUE(composite->sendToken("mod_a", "tok123"));
    ASSERT_EQ(container->sendTokenCalls.size(), 1u);
    EXPECT_EQ(container->sendTokenCalls[0].first, "mod_a");
    EXPECT_EQ(container->sendTokenCalls[0].second, "tok123");
}

TEST_F(CompositeModuleLoaderTest, Terminate_DelegatesToContainer) {
    ModuleDescriptor desc;
    desc.name = "to_term";
    LoadedModuleHandle h;
    composite->load(desc, nullptr, h);

    composite->terminate("to_term");
    ASSERT_EQ(container->terminateCalls.size(), 1u);
    EXPECT_EQ(container->terminateCalls[0], "to_term");
}

TEST_F(CompositeModuleLoaderTest, TerminateAll_DelegatesToContainer) {
    composite->terminateAll();
    EXPECT_EQ(container->terminateAllCount, 1);
}

// ---------------------------------------------------------------------------
// hasModule, pid, getAllPids delegate to container
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, HasModule_DelegatesToContainer) {
    EXPECT_FALSE(composite->hasModule("x"));

    ModuleDescriptor desc;
    desc.name = "x";
    LoadedModuleHandle h;
    composite->load(desc, nullptr, h);

    EXPECT_TRUE(composite->hasModule("x"));
}

TEST_F(CompositeModuleLoaderTest, Pid_DelegatesToContainer) {
    EXPECT_FALSE(composite->pid("x").has_value());

    ModuleDescriptor desc;
    desc.name = "x";
    LoadedModuleHandle h;
    composite->load(desc, nullptr, h);

    auto p = composite->pid("x");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, 42);
}

TEST_F(CompositeModuleLoaderTest, GetAllPids_DelegatesToContainer) {
    ModuleDescriptor d1;
    d1.name = "a";
    LoadedModuleHandle h1;
    composite->load(d1, nullptr, h1);

    ModuleDescriptor d2;
    d2.name = "b";
    LoadedModuleHandle h2;
    composite->load(d2, nullptr, h2);

    auto pids = composite->getAllPids();
    EXPECT_EQ(pids.size(), 2u);
    EXPECT_EQ(pids.at("a"), 42);
    EXPECT_EQ(pids.at("b"), 42);
}

// ---------------------------------------------------------------------------
// Container accessor
// ---------------------------------------------------------------------------

TEST_F(CompositeModuleLoaderTest, ContainerAccessor_ReturnsSameInstance) {
    EXPECT_EQ(&composite->container(), static_cast<ModuleContainer*>(container.get()));
}
