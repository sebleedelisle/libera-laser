#include "libera/core/DacDiscovery.hpp"

namespace libera::core {

std::vector<DiscovererFactory>& getDiscovererFactories() {
    // Singleton registry of discoverer factories. Each concrete discoverer
    // registers a lambda that knows how to construct an instance.
    static std::vector<DiscovererFactory> factories;
    return factories;
}

DiscovererRegistry::DiscovererRegistry(DiscovererFactory factory) {
    // Automatically invoked when a translation unit defines a static registrar.
    getDiscovererFactories().push_back(std::move(factory));
}

void AddDiscoverer(DiscovererFactory factory) {
    getDiscovererFactories().push_back(std::move(factory));
}

DacDiscoveryManager::DacDiscoveryManager() {
    // Instantiate one of each registered discoverer up front.
    for (const auto& factory : getDiscovererFactories()) {
        if (factory) {
            discoverers.emplace_back(factory());
        }
    }
}

std::vector<std::unique_ptr<DacInfo>> DacDiscoveryManager::discoverAll() {
    std::vector<std::unique_ptr<DacInfo>> results;
    for (auto& discoverer : discoverers) {
        if (!discoverer) continue;
        auto subset = discoverer->discover();
        results.reserve(results.size() + subset.size());
        for (auto& item : subset) {
            results.emplace_back(std::move(item));
        }
    }
    return results;
}

} // namespace libera::core
