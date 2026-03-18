#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>

#include "libera/core/LaserController.hpp"

namespace libera::core {

enum class ControllerUsageState {
    Unknown,
    Idle,
    Active,
    BusyExclusive
};

class ControllerInfo {
public:
    struct NetworkInfo {
        std::string ip;
        std::uint16_t port = 0;
    };

    ControllerInfo(std::string id,
                   std::string label,
                   std::uint32_t maxPointRateValue = 0,
                   std::optional<NetworkInfo> networkInfo = std::nullopt)
    : id(std::move(id))
    , label(std::move(label))
    , maxPointRateValue(maxPointRateValue)
    , networkInfoValue(std::move(networkInfo)) {}

    virtual ~ControllerInfo() = default;

    const std::string& idValue() const { return id; }
    const std::string& labelValue() const { return label; }
    std::uint32_t maxPointRate() const { return maxPointRateValue; }
    void setMaxPointRate(std::uint32_t value) { maxPointRateValue = value; }
    const std::optional<NetworkInfo>& networkInfo() const { return networkInfoValue; }
    ControllerUsageState usageState() const { return usageStateValue; }
    void setUsageState(ControllerUsageState value) { usageStateValue = value; }
    virtual const std::string& type() const = 0;

protected:
    std::string id;
    std::string label;
    std::uint32_t maxPointRateValue = 0;
    std::optional<NetworkInfo> networkInfoValue;
    ControllerUsageState usageStateValue = ControllerUsageState::Unknown;
};

class ControllerManagerBase {
public:
    virtual ~ControllerManagerBase() = default;

    virtual std::vector<std::unique_ptr<ControllerInfo>> discover() = 0;
    virtual std::string_view managedType() const = 0;
    virtual std::shared_ptr<LaserController> connectController(const ControllerInfo& info) = 0;
    virtual void closeAll() = 0;
};

using ControllerManagerFactory = std::function<std::unique_ptr<ControllerManagerBase>()>;

std::vector<ControllerManagerFactory>& getControllerManagerFactories();

struct ControllerManagerRegistry {
    explicit ControllerManagerRegistry(ControllerManagerFactory factory);
};

void AddControllerManager(ControllerManagerFactory factory);

} // namespace libera::core

namespace libera {

class System {
public:
    System();
    ~System();

    std::vector<std::unique_ptr<core::ControllerInfo>> discoverControllers();
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info);
    void shutdown();

private:
    std::vector<std::unique_ptr<core::ControllerManagerBase>> managers;
    std::unordered_map<std::string, core::ControllerManagerBase*> managerByType;
    bool shutdownComplete = false;
};

} // namespace libera
