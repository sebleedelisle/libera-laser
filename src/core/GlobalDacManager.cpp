#include "libera/core/GlobalDacManager.hpp"

namespace libera::core {

std::vector<DacManagerFactory>& getDacManagerFactories() {
    static std::vector<DacManagerFactory> factories;
    return factories;
}

DacManagerRegistry::DacManagerRegistry(DacManagerFactory factory) {
    getDacManagerFactories().push_back(std::move(factory));
}

void AddDacManager(DacManagerFactory factory) {
    getDacManagerFactories().push_back(std::move(factory));
}

GlobalDacManager::GlobalDacManager() {
    for (const auto& factory : getDacManagerFactories()) {
        if (!factory) continue;
        auto manager = factory();
        if (!manager) continue;
        auto type = std::string(manager->managedType());
        managerByType[type] = manager.get();
        managers.emplace_back(std::move(manager));
    }
}

GlobalDacManager::~GlobalDacManager() {
    close();
}

std::vector<std::unique_ptr<DacInfo>> GlobalDacManager::discoverAll() {
    std::vector<std::unique_ptr<DacInfo>> results;
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

std::shared_ptr<LaserController>
GlobalDacManager::getAndConnectToDac(const DacInfo& info) {
    auto it = managerByType.find(info.type());
    if (it == managerByType.end() || !it->second) {
        return nullptr;
    }
    return it->second->getAndConnectToDac(info);
}

void GlobalDacManager::close() {
    for (auto& manager : managers) {
        if (manager) {
            manager->closeAll();
        }
    }
    managerByType.clear();
}

} // namespace libera::core
