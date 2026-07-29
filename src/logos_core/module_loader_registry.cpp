#include "module_loader_registry.h"

namespace LogosCore {

void ModuleLoaderRegistry::registerLoader(std::shared_ptr<ModuleLoader> loader)
{
    std::lock_guard lock(m_mutex);
    m_loaders.push_back(std::move(loader));
}

std::shared_ptr<ModuleLoader> ModuleLoaderRegistry::select(const ModuleDescriptor& desc) const
{
    std::lock_guard lock(m_mutex);

    // Explicit id override: caller pinned a specific loader id.
    if (desc.loaderConfig.contains("id")) {
        std::string requestedId = desc.loaderConfig.at("id").get<std::string>();
        for (const auto& loader : m_loaders) {
            if (loader->id() == requestedId)
                return loader;
        }
        // Unknown explicit id — don't fall through to canHandle.
        return nullptr;
    }

    // Format/capability-based: first loader that accepts this descriptor.
    for (const auto& loader : m_loaders) {
        if (loader->canHandle(desc))
            return loader;
    }
    return nullptr;
}

void ModuleLoaderRegistry::terminateAll()
{
    std::lock_guard lock(m_mutex);
    for (const auto& loader : m_loaders)
        loader->terminateAll();
}

bool ModuleLoaderRegistry::terminate(const std::string& name)
{
    std::lock_guard lock(m_mutex);
    for (const auto& loader : m_loaders) {
        if (loader->hasModule(name)) {
            loader->terminate(name);
            return true;
        }
    }
    return false;
}

bool ModuleLoaderRegistry::terminateInstance(const ModuleAddress& address)
{
    if (!address.isValid()) return false;

    std::lock_guard lock(m_mutex);
    for (const auto& loader : m_loaders) {
        auto instanceAware = std::dynamic_pointer_cast<InstanceAwareModuleLoader>(loader);
        if (instanceAware && instanceAware->hasInstance(address)) {
            return instanceAware->terminateInstance(address);
        }
    }
    return false;
}

std::unordered_map<std::string, int64_t> ModuleLoaderRegistry::getAllPids() const
{
    std::lock_guard lock(m_mutex);
    std::unordered_map<std::string, int64_t> result;
    for (const auto& loader : m_loaders) {
        auto pids = loader->getAllPids();
        result.insert(pids.begin(), pids.end());
    }
    return result;
}

std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>
ModuleLoaderRegistry::getAllInstancePids() const
{
    std::lock_guard lock(m_mutex);
    std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash> result;
    for (const auto& loader : m_loaders) {
        auto instanceAware = std::dynamic_pointer_cast<InstanceAwareModuleLoader>(loader);
        if (!instanceAware) continue;
        auto pids = instanceAware->getAllInstancePids();
        result.insert(pids.begin(), pids.end());
    }
    return result;
}

void ModuleLoaderRegistry::clearForTests()
{
    std::lock_guard lock(m_mutex);
    m_loaders.clear();
}

} // namespace LogosCore
