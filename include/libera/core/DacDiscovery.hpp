#pragma once

#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace libera::core {

class DiscoveredDac {
public:
    DiscoveredDac(std::string id, std::string label) {
        this->id = std::move(id);
        this->label = std::move(label);
    }

    virtual ~DiscoveredDac() = default;

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

    virtual std::vector<std::unique_ptr<DiscoveredDac>> discover() = 0;
};

} // namespace libera::core
