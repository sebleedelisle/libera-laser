#include "libera/plugin/PluginRegistry.hpp"

#include <algorithm>
#include <filesystem>

namespace libera::plugin {

namespace fs = std::filesystem;

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
}

PluginRegistry::Entry& PluginRegistry::entryLocked(const std::string& path) {
    for (auto& entry : entries) {
        if (entry.info.path == path) {
            return entry;
        }
    }

    Entry entry;
    entry.info.path = path;
    entry.info.filename = fs::u8path(path).filename().u8string();
    entries.push_back(std::move(entry));
    return entries.back();
}

std::vector<PluginInfo> PluginRegistry::snapshot() const {
    std::lock_guard lock(mutex);
    std::vector<PluginInfo> snapshot;
    snapshot.reserve(entries.size());
    for (const auto& entry : entries) {
        PluginInfo info = entry.info;
        info.runtimeErrors.assign(entry.errors.begin(), entry.errors.end());
        snapshot.push_back(std::move(info));
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const PluginInfo& a, const PluginInfo& b) {
                  return a.path < b.path;
              });
    return snapshot;
}

void PluginRegistry::recordLoaded(const std::string& path,
                                  const std::string& pluginId,
                                  const std::string& version,
                                  const std::string& typeName,
                                  const std::string& displayName,
                                  const std::string& vendor,
                                  const std::string& description,
                                  const std::string& packageRoot,
                                  const std::string& packageSha256,
                                  const std::optional<std::string>& readmePath,
                                  const std::optional<std::string>& licensePath) {
    std::lock_guard lock(mutex);
    auto& entry = entryLocked(path);
    entry.info.state = PluginState::Loaded;
    entry.info.pluginId = pluginId;
    entry.info.version = version;
    entry.info.typeName = typeName;
    entry.info.displayName = displayName;
    entry.info.vendor = vendor;
    entry.info.description = description;
    entry.info.packageRoot = packageRoot;
    entry.info.packageSha256 = packageSha256;
    entry.info.readmePath = readmePath;
    entry.info.licensePath = licensePath;
    entry.info.loadError.reset();
}

void PluginRegistry::recordFailure(const std::string& path,
                                   PluginState state,
                                   const std::string& reason,
                                   const std::string& typeName,
                                   const std::string& displayName,
                                   const std::string& pluginId,
                                   const std::string& version) {
    std::lock_guard lock(mutex);
    auto& entry = entryLocked(path);
    entry.info.state = state;
    entry.info.loadError = reason;
    if (!typeName.empty()) {
        entry.info.typeName = typeName;
    }
    if (!displayName.empty()) {
        entry.info.displayName = displayName;
    }
    if (!pluginId.empty()) {
        entry.info.pluginId = pluginId;
    }
    if (!version.empty()) {
        entry.info.version = version;
    }
}

void PluginRegistry::forget(const std::string& path) {
    std::lock_guard lock(mutex);
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const Entry& entry) { return entry.info.path == path; }),
        entries.end());
}

void PluginRegistry::pushRuntimeError(const std::string& path,
                                      const std::string& code,
                                      const std::string& message) {
    std::lock_guard lock(mutex);
    auto& entry = entryLocked(path);
    entry.errors.push_back({std::chrono::system_clock::now(), code, message});
    while (entry.errors.size() > maximumRuntimeErrors) {
        entry.errors.pop_front();
    }
}

} // namespace libera::plugin
