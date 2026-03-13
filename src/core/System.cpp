#include "libera/System.hpp"

namespace libera::core {

std::vector<ControllerManagerFactory>& getControllerManagerFactories() {
    static std::vector<ControllerManagerFactory> factories;
    return factories;
}

ControllerManagerRegistry::ControllerManagerRegistry(ControllerManagerFactory factory) {
    getControllerManagerFactories().push_back(std::move(factory));
}

void AddControllerManager(ControllerManagerFactory factory) {
    getControllerManagerFactories().push_back(std::move(factory));
}

} // namespace libera::core

namespace libera {

System::System() {
    for (const auto& factory : core::getControllerManagerFactories()) {
        if (!factory) continue;
        auto manager = factory();
        if (!manager) continue;
        auto type = std::string(manager->managedType());
        managerByType[type] = manager.get();
        managers.emplace_back(std::move(manager));
    }
}

System::~System() {
    shutdown();
}

std::vector<std::unique_ptr<core::ControllerInfo>> System::discoverControllers() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    for (auto& manager : managers) {
        if (!manager) continue;
        auto subset = manager->discover();
        results.reserve(results.size() + subset.size());
        for (auto& item : subset) {
            results.emplace_back(std::move(item));
        }
    }
    return results;
}

std::shared_ptr<core::LaserController>
System::connectController(const core::ControllerInfo& info) {
    auto it = managerByType.find(info.type());
    if (it == managerByType.end() || !it->second) {
        return nullptr;
    }
    return it->second->connectController(info);
}

void System::shutdown() {
    for (auto& manager : managers) {
        if (manager) {
            manager->closeAll();
        }
    }
    managerByType.clear();
}

} // namespace libera
