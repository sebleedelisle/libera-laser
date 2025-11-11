#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace libera::core {

class DacInfo {
public:
    DacInfo(std::string id, std::string label) {
        this->id = std::move(id);
        this->label = std::move(label);
    }

    virtual ~DacInfo() = default;

    const std::string& idValue() const { return id; }
    const std::string& labelValue() const { return label; }
    virtual const std::string& type() const = 0;

protected:
    std::string id;
    std::string label;
};

class DacDiscovererBase {
public:
    virtual ~DacDiscovererBase() = default;

    virtual std::vector<std::unique_ptr<DacInfo>> discover() = 0;
};

// Convenience alias for a callable that constructs a discoverer.
using DiscovererFactory = std::function<std::unique_ptr<DacDiscovererBase>()>;

// Returns the global list of registered discoverer factories.
std::vector<DiscovererFactory>& getDiscovererFactories();

// Helper that auto-registers a factory when a static instance is created.
struct DiscovererRegistry {
    explicit DiscovererRegistry(DiscovererFactory factory);
};

// Manual helper for adding discoverers at runtime if needed.
void AddDiscoverer(DiscovererFactory factory);

class DacDiscoveryManager {
public:
    DacDiscoveryManager();

    std::vector<std::unique_ptr<DacInfo>> discoverAll();

private:
    std::vector<std::unique_ptr<DacDiscovererBase>> discoverers;
};

} // namespace libera::core
