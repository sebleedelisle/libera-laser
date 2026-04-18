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
    std::string typeName;
    std::string displayName;
    std::optional<std::string> loadError;
    std::vector<PluginRuntimeError> runtimeErrors;
};

class PluginRegistry {
public:
    static PluginRegistry& instance();

    std::vector<PluginInfo> snapshot() const;

    void recordLoaded(const std::string& path,
                      const std::string& typeName,
                      const std::string& displayName);
    void recordFailure(const std::string& path,
                       PluginState state,
                       const std::string& reason,
                       const std::string& typeName = {},
                       const std::string& displayName = {});
    void forget(const std::string& path);

    void pushRuntimeError(const std::string& path,
                          const std::string& code,
                          const std::string& message);

    static constexpr std::size_t kMaxRuntimeErrors = 100;

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
};

PluginInstallResult validatePluginFile(const std::string& sourcePath);

PluginInstallResult installPluginFile(const std::string& sourcePath,
                                      const std::string& destDir);

bool removePluginFile(const std::string& path, std::string* error = nullptr);

} // namespace libera::plugin
