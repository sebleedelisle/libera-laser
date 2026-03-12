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

class DacInfo {
public:
    struct NetworkInfo {
        std::string ip;
        std::uint16_t port = 0;
    };

    DacInfo(std::string id,
            std::string label,
            std::uint32_t maxPointRateValue = 0,
            std::optional<NetworkInfo> networkInfo = std::nullopt)
    : id(std::move(id))
    , label(std::move(label))
    , maxPointRateValue(maxPointRateValue)
    , networkInfoValue(std::move(networkInfo)) {}

    virtual ~DacInfo() = default;

    const std::string& idValue() const { return id; }
    const std::string& labelValue() const { return label; }
    std::uint32_t maxPointRate() const { return maxPointRateValue; }
    void setMaxPointRate(std::uint32_t value) { maxPointRateValue = value; }
    const std::optional<NetworkInfo>& networkInfo() const { return networkInfoValue; }
    virtual const std::string& type() const = 0;

protected:
    std::string id;
    std::string label;
    std::uint32_t maxPointRateValue = 0;
    std::optional<NetworkInfo> networkInfoValue;
};

class DacManagerBase {
public:
    virtual ~DacManagerBase() = default;

    virtual std::vector<std::unique_ptr<DacInfo>> discover() = 0;
    virtual std::string_view managedType() const = 0;
    virtual std::shared_ptr<LaserController> getAndConnectToDac(const DacInfo& info) = 0;
    virtual void closeAll() = 0;
};

using DacManagerFactory = std::function<std::unique_ptr<DacManagerBase>()>;

std::vector<DacManagerFactory>& getDacManagerFactories();

struct DacManagerRegistry {
    explicit DacManagerRegistry(DacManagerFactory factory);
};

void AddDacManager(DacManagerFactory factory);

class GlobalDacManager {
public:
    GlobalDacManager();
    ~GlobalDacManager();

    std::vector<std::unique_ptr<DacInfo>> discoverAll();
    std::shared_ptr<LaserController> getAndConnectToDac(const DacInfo& info);
    void close();

private:
    std::vector<std::unique_ptr<DacManagerBase>> managers;
    std::unordered_map<std::string, DacManagerBase*> managerByType;
};

} // namespace libera::core
