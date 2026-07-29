#include "composite_module_loader.h"

namespace LogosCore {

CompositeModuleLoader::CompositeModuleLoader(std::shared_ptr<ModuleContainer> container,
                                             std::shared_ptr<ModuleFormatLoader> loader)
    : container_(std::move(container))
    , loader_(std::move(loader))
{}

std::string CompositeModuleLoader::id() const
{
    return loader_->id() + "+" + container_->id();
}

bool CompositeModuleLoader::canHandle(const ModuleDescriptor& desc) const
{
    return loader_->canHandle(desc) && container_->canHandle(desc);
}

bool CompositeModuleLoader::load(const ModuleDescriptor& desc,
                                 std::function<void(const std::string& name)> onTerminated,
                                 LoadedModuleHandle& out)
{
    std::string host = loader_->resolveHostBinary(desc);
    if (host.empty())
        return false;

    auto args = loader_->buildArguments(desc);
    return container_->launch(desc, host, args, std::move(onTerminated), out);
}

bool CompositeModuleLoader::sendToken(const std::string& name, const std::string& token)
{
    return container_->sendToken(name, token);
}

void CompositeModuleLoader::terminate(const std::string& name)
{
    container_->terminate(name);
}

void CompositeModuleLoader::terminateAll()
{
    container_->terminateAll();
}

bool CompositeModuleLoader::hasModule(const std::string& name) const
{
    return container_->hasModule(name);
}

std::optional<int64_t> CompositeModuleLoader::pid(const std::string& name) const
{
    return container_->pid(name);
}

std::unordered_map<std::string, int64_t> CompositeModuleLoader::getAllPids() const
{
    return container_->getAllPids();
}

InstanceAwareModuleContainer* CompositeModuleLoader::instanceContainer() const
{
    return dynamic_cast<InstanceAwareModuleContainer*>(container_.get());
}

bool CompositeModuleLoader::loadInstance(
    const ModuleDescriptor& desc,
    std::function<void(const ModuleAddress& address)> onTerminated,
    LoadedModuleHandle& out)
{
    InstanceAwareModuleContainer* container = instanceContainer();
    if (!container || !desc.address().isValid())
        return false;

    std::string host = loader_->resolveHostBinary(desc);
    if (host.empty())
        return false;

    auto args = loader_->buildArguments(desc);
    return container->launchInstance(desc, host, args, std::move(onTerminated), out);
}

bool CompositeModuleLoader::sendTokenToInstance(const ModuleAddress& address,
                                                 const std::string& token)
{
    InstanceAwareModuleContainer* container = instanceContainer();
    return container && address.isValid()
        && container->sendTokenToInstance(address, token);
}

bool CompositeModuleLoader::terminateInstance(const ModuleAddress& address)
{
    InstanceAwareModuleContainer* container = instanceContainer();
    return container && address.isValid() && container->terminateInstance(address);
}

bool CompositeModuleLoader::hasInstance(const ModuleAddress& address) const
{
    InstanceAwareModuleContainer* container = instanceContainer();
    return container && address.isValid() && container->hasInstance(address);
}

std::optional<int64_t> CompositeModuleLoader::instancePid(
    const ModuleAddress& address) const
{
    InstanceAwareModuleContainer* container = instanceContainer();
    if (!container || !address.isValid())
        return std::nullopt;
    return container->instancePid(address);
}

std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>
CompositeModuleLoader::getAllInstancePids() const
{
    InstanceAwareModuleContainer* container = instanceContainer();
    return container ? container->getAllInstancePids()
                     : std::unordered_map<ModuleAddress, int64_t, ModuleAddressHash>{};
}

} // namespace LogosCore
