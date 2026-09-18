#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace libera::plugin {

enum class PluginState {
    Loaded,
    NotAPlugin,
    FailedLoad,
    FailedValidation,
    FailedBackend,
};

struct PluginRuntimeError {
    std::chrono::system_clock::time_point time;
    std::string code;
    std::string message;
};

struct PluginInfo {
    std::string path;
    std::string filename;
    PluginState state = PluginState::FailedLoad;
    std::string pluginId;
    std::string version;
    std::string typeName;
    std::string displayName;
    std::string vendor;
    std::string description;
    std::string packageRoot;
    std::string packageSha256;
    std::optional<std::string> readmePath;
    std::optional<std::string> licensePath;
    std::optional<std::string> loadError;
    std::vector<PluginRuntimeError> runtimeErrors;
};

class PluginRegistry {
public:
    static PluginRegistry& instance();

    std::vector<PluginInfo> snapshot() const;

    void recordLoaded(const std::string& path,
                      const std::string& pluginId,
                      const std::string& version,
                      const std::string& typeName,
                      const std::string& displayName,
                      const std::string& vendor = {},
                      const std::string& description = {},
                      const std::string& packageRoot = {},
                      const std::string& packageSha256 = {},
                      const std::optional<std::string>& readmePath = std::nullopt,
                      const std::optional<std::string>& licensePath = std::nullopt);
    void recordFailure(const std::string& path,
                       PluginState state,
                       const std::string& reason,
                       const std::string& typeName = {},
                       const std::string& displayName = {},
                       const std::string& pluginId = {},
                       const std::string& version = {});
    void forget(const std::string& path);

    void pushRuntimeError(const std::string& path,
                          const std::string& code,
                          const std::string& message);

    static constexpr std::size_t maximumRuntimeErrors = 100;

private:
    struct Entry {
        PluginInfo info;
        std::deque<PluginRuntimeError> errors;
    };

    Entry& entryLocked(const std::string& path);

    mutable std::mutex mutex;
    std::vector<Entry> entries;
};

struct PluginInstallResult {
    bool success = false;
    std::string installedPath;
    std::string message;
    bool restartRequired = false;
};

PluginInstallResult validatePluginPackageForInstall(const std::string& sourcePath);

} // namespace libera::plugin
