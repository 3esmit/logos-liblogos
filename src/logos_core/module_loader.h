#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include <logos_container/module_descriptor.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

// Qt-free abstract interface for module loading strategies.
// An implementation decides *how* a module is loaded, isolated, and communicated with.
// The core (ModuleManager) decides *what* to load and *when*.
//
// ModuleDescriptor / LoadedModuleHandle — the value types passed across this
// interface and the container boundary — live in the logos-container contract
// package (<logos_container/module_descriptor.h>), since the ModuleContainer
// interface there needs them too. The loader strategy itself is a core concern
// and stays here.

namespace LogosCore {

// Abstract base: one instance per loader kind, shared across all modules it manages.
// All implementations must be Qt-free at the interface level.
class ModuleLoader {
public:
    virtual ~ModuleLoader() = default;

    // Unique identifier for this loader (e.g. "qt-subprocess", "inproc", "extism").
    virtual std::string id() const = 0;

    // Return true if this loader knows how to load the described module.
    virtual bool canHandle(const ModuleDescriptor& desc) const = 0;

    // Load the module. On success, populate `out` and return true.
    // `onTerminated` may be called from a background thread when the module exits.
    virtual bool load(const ModuleDescriptor& desc,
                      std::function<void(const std::string& name)> onTerminated,
                      LoadedModuleHandle& out) = 0;

    // Deliver the auth token to the named module. Called immediately after a successful load().
    virtual bool sendToken(const std::string& name, const std::string& token) = 0;

    // Terminate a single module by name.
    virtual void terminate(const std::string& name) = 0;

    // Terminate all modules managed by this loader.
    virtual void terminateAll() = 0;

    // Return true if this loader currently has an active entry for the named module.
    virtual bool hasModule(const std::string& name) const = 0;

    // Return the PID of the named module, or nullopt if not process-based.
    virtual std::optional<int64_t> pid(const std::string& /*name*/) const { return std::nullopt; }

    // Return all (name -> pid) mappings. PIDs are -1 for non-process loaders.
    virtual std::unordered_map<std::string, int64_t> getAllPids() const { return {}; }
};

// Optional extension for loaders that can host more than one runtime for the
// same package.  It deliberately derives from the unchanged name-only loader
// contract: existing loader binaries keep serving the legacy/default instance,
// while the core opts into this interface only for an explicit instance ID.
class InstanceAwareModuleLoader : public ModuleLoader {
public:
    virtual bool loadInstance(
        const ModuleDescriptor& desc,
        std::function<void(const ModuleAddress& address)> onTerminated,
        LoadedModuleHandle& out) = 0;

    virtual bool sendTokenToInstance(const ModuleAddress& address,
                                     const std::string& token) = 0;

    virtual bool terminateInstance(const ModuleAddress& address) = 0;

    virtual bool hasInstance(const ModuleAddress& address) const = 0;

    virtual std::optional<int64_t> instancePid(
        const ModuleAddress& address) const = 0;

    virtual std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>
    getAllInstancePids() const = 0;
};

} // namespace LogosCore

#endif // MODULE_LOADER_H
