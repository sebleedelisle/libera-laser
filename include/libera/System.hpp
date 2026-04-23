#pragma once

#include <functional>
#include <memory>
#include <set>
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

    // The base owns the backend type string so discovery records and manager
    // routing stay aligned without every ControllerInfo subclass repeating the
    // same trivial type() override.
    ControllerInfo(std::string_view typeValue,
                   std::string id,
                   std::string label,
                   std::uint32_t maxPointRateValue = 0,
                   std::optional<NetworkInfo> networkInfo = std::nullopt)
    : typeString(typeValue)
    , id(std::move(id))
    , label(std::move(label))
    , maxPointRateValue(maxPointRateValue)
    , networkInfoValue(std::move(networkInfo)) {}

    virtual ~ControllerInfo() = default;

    const std::string& type() const { return typeString; }
    const std::string& idValue() const { return id; }
    const std::string& labelValue() const { return label; }
    std::uint32_t maxPointRate() const { return maxPointRateValue; }
    void setMaxPointRate(std::uint32_t value) { maxPointRateValue = value; }
    const std::optional<NetworkInfo>& networkInfo() const { return networkInfoValue; }
    ControllerUsageState usageState() const { return usageStateValue; }
    void setUsageState(ControllerUsageState value) { usageStateValue = value; }

protected:
    std::string typeString;
    std::string id;
    std::string label;
    std::uint32_t maxPointRateValue = 0;
    std::optional<NetworkInfo> networkInfoValue;
    ControllerUsageState usageStateValue = ControllerUsageState::Unknown;
};

class AbstractControllerManager {
public:
    virtual ~AbstractControllerManager() = default;

    virtual std::vector<std::unique_ptr<ControllerInfo>> discover() = 0;
    virtual std::string_view managedType() const = 0;
    virtual std::shared_ptr<LaserController> connectController(const ControllerInfo& info) = 0;
    virtual void closeAll() = 0;
};

using ControllerManagerFactory = std::function<std::unique_ptr<AbstractControllerManager>()>;

struct ControllerManagerInfo {
    std::string type;
    std::string displayName;
    std::string description;
};

struct ControllerManagerRegistration {
    ControllerManagerInfo info;
    ControllerManagerFactory factory;
};

std::vector<ControllerManagerFactory>& getControllerManagerFactories();
std::vector<ControllerManagerRegistration>& getControllerManagerRegistrations();
std::vector<ControllerManagerInfo> registeredControllerManagers();

struct ControllerManagerRegistry {
    explicit ControllerManagerRegistry(ControllerManagerFactory factory);
    explicit ControllerManagerRegistry(ControllerManagerRegistration registration);
};

void AddControllerManager(ControllerManagerFactory factory);
void AddControllerManager(ControllerManagerRegistration registration);

} // namespace libera::core

namespace libera {

struct SystemOptions {
    std::set<std::string> disabledControllerTypes;
};

class System {
public:
    System();
    explicit System(SystemOptions options);
    ~System();

    /// Replace the default plugin search paths with one directory.
    /// Call before constructing System. If never called, Libera searches the
    /// directories from LIBERA_PLUGIN_DIR when that environment variable is
    /// set, otherwise it falls back to built-in defaults such as
    /// "plugins" and "../plugins" relative to the executable.
    /// Pass an empty string to disable plugin loading entirely.
    /// Replaces any previously configured directories.
    static void setPluginDirectory(const std::string& path);
    static const std::string& pluginDirectory();

    /// Append an additional directory to the plugin search list.
    /// Each directory will be scanned when System is constructed.
    /// Absolute paths are used as-is; relative paths are resolved against
    /// the executable directory first, then the current working directory.
    static void addPluginDirectory(const std::string& path);
    static const std::vector<std::string>& pluginDirectories();
    static std::vector<core::ControllerManagerInfo> availableControllerManagers();

    std::vector<std::unique_ptr<core::ControllerInfo>> discoverControllers();
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info);
    void shutdown();

private:
    std::vector<std::unique_ptr<core::AbstractControllerManager>> managers;
    std::unordered_map<std::string, core::AbstractControllerManager*> managerByType;
    bool shutdownComplete = false;
};

} // namespace libera
