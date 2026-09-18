#include "libera/plugin/PluginManagement.hpp"

#include "libera/System.hpp"
#include "libera/plugin/PluginPackage.hpp"

#include "PluginStoreInternal.hpp"
#include "PluginFileLock.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace libera::plugin {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

struct StoreState {
    std::unordered_map<std::string, std::string> activePackages;
    std::unordered_map<std::string, std::string> controllerDrivers;
};

std::mutex& storeMutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<std::string, std::optional<InstalledPluginRevision>>&
revisionCache() {
    static std::unordered_map<std::string,
                              std::optional<InstalledPluginRevision>> value;
    return value;
}

std::string revisionCacheKey(const fs::path& root,
                             const std::string& pluginId,
                             const std::string& revision) {
    return root.u8string() + "\n" + pluginId + "\n" + revision;
}

fs::path statePath(const fs::path& root) {
    return root / "state.json";
}

fs::path pathFromUtf8(const std::string& value) {
    return fs::u8path(value);
}

std::string pathToUtf8(const fs::path& path) {
    return path.u8string();
}

std::string normalizedPathString(const fs::path& path) {
    std::error_code ec;
    auto absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    auto canonical = fs::weakly_canonical(absolute, ec);
    return pathToUtf8(ec ? absolute.lexically_normal() : canonical);
}

StoreState readState(const fs::path& root, bool* valid = nullptr) {
    if (valid) *valid = true;
    StoreState state;
    std::ifstream input(statePath(root));
    if (!input) {
        return state;
    }

    Json document;
    try {
        input >> document;
        input >> std::ws;
        if (!input.eof() || !document.is_object() ||
            document.value("schemaVersion", 0) != 1 ||
            !document.contains("activePackages") ||
            !document["activePackages"].is_object() ||
            !document.contains("controllerDrivers") ||
            !document["controllerDrivers"].is_object()) {
            if (valid) *valid = false;
            return {};
        }
        const auto readMap = [](const Json& object, bool* mapValid) {
            std::unordered_map<std::string, std::string> values;
            for (auto item = object.begin(); item != object.end(); ++item) {
                if (item.key().empty() || !item.value().is_string() ||
                    item.value().get_ref<const std::string&>().empty()) {
                    *mapValid = false;
                    return std::unordered_map<std::string, std::string>{};
                }
                values[item.key()] = item.value().get<std::string>();
            }
            return values;
        };
        bool mapsValid = true;
        state.activePackages = readMap(document["activePackages"], &mapsValid);
        state.controllerDrivers = readMap(document["controllerDrivers"],
                                          &mapsValid);
        if (!mapsValid) {
            if (valid) *valid = false;
            return {};
        }
    } catch (const std::exception&) {
        if (valid) *valid = false;
        return {};
    }
    return state;
}

bool replaceFile(const fs::path& temporary,
                 const fs::path& destination,
                 std::string* error) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) {
            *error = "Failed to replace plugin state file";
        }
        return false;
    }
#else
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    if (ec) {
        if (error) {
            *error = "Failed to replace plugin state file: " + ec.message();
        }
        return false;
    }
#endif
    return true;
}

bool writeState(const fs::path& root,
                const StoreState& state,
                std::string* error) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        if (error) *error = "Failed to create plugin store: " + ec.message();
        return false;
    }

    Json document = {
        {"schemaVersion", 1},
        {"activePackages", state.activePackages},
        {"controllerDrivers", state.controllerDrivers},
    };
    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    fs::path temporary = statePath(root);
    temporary += ".tmp." + std::to_string(suffix);
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) {
            if (error) *error = "Failed to write plugin state";
            fs::remove(temporary, ec);
            return false;
        }
    }
    if (!replaceFile(temporary, statePath(root), error)) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

std::optional<InstalledPluginRevision> resolveRevision(
    const fs::path& root,
    const std::string& pluginId,
    const std::string& revision) {
    const fs::path packageRoot = root / "packages" / pluginId / revision;
    const fs::path archive = packageRoot / "archive.liberaplugin";
    const auto inspection = inspectPluginPackage(pathToUtf8(archive));
    if (!inspection.success || inspection.manifest.id != pluginId) {
        return std::nullopt;
    }
    const std::string expectedRevision = inspection.manifest.version + "-" +
        inspection.packageSha256.substr(0, 12);
    if (revision != expectedRevision) {
        return std::nullopt;
    }

    const fs::path entrypoint = packageRoot / inspection.selectedEntrypoint;
    std::error_code ec;
    if (!fs::is_regular_file(entrypoint, ec)) {
        return std::nullopt;
    }

    InstalledPluginRevision installed;
    installed.manifest = inspection.manifest;
    installed.revision = revision;
    installed.packageRoot = normalizedPathString(packageRoot);
    installed.entrypointPath = normalizedPathString(entrypoint);
    installed.archivePath = normalizedPathString(archive);
    installed.packageSha256 = inspection.packageSha256;
    return installed;
}

std::string documentPath(const InstalledPluginRevision& installed,
                         const std::optional<std::string>& relative) {
    return relative
        ? normalizedPathString(pathFromUtf8(installed.packageRoot) / *relative)
        : std::string{};
}

ManagedPluginInfo managedFromInstalled(const InstalledPluginRevision& installed) {
    ManagedPluginInfo info;
    info.path = installed.packageRoot;
    info.filename = installed.manifest.id + ".liberaplugin";
    info.state = ManagedPluginState::PendingRestart;
    info.source = ManagedPluginSource::UserPluginDirectory;
    info.typeName = installed.manifest.controllerType;
    info.pluginId = installed.manifest.id;
    info.version = installed.manifest.version;
    info.displayName = installed.manifest.name;
    info.vendor = installed.manifest.vendor;
    info.description = installed.manifest.description;
    info.packageSha256 = installed.packageSha256;
    const auto readme = documentPath(installed, installed.manifest.documents.readme);
    const auto license = documentPath(installed, installed.manifest.documents.license);
    if (!readme.empty()) info.readmePath = readme;
    if (!license.empty()) info.licensePath = license;
    info.fileExists = true;
    info.canRemove = true;
    info.restartRequired = true;
    return info;
}

ManagedPluginState toManagedState(PluginState state) {
    switch (state) {
        case PluginState::Loaded: return ManagedPluginState::Loaded;
        case PluginState::NotAPlugin: return ManagedPluginState::NotAPlugin;
        case PluginState::FailedLoad: return ManagedPluginState::FailedLoad;
        case PluginState::FailedValidation: return ManagedPluginState::FailedValidation;
        case PluginState::FailedBackend: return ManagedPluginState::FailedBackend;
    }
    return ManagedPluginState::FailedLoad;
}

} // namespace

const std::string& userPluginDirectory() {
    return System::pluginDirectory();
}

std::vector<InstalledPluginRevision> activePluginRevisions(
    const std::string& storeRoot) {
    std::lock_guard lock(storeMutex());
    std::vector<InstalledPluginRevision> result;
    if (storeRoot.empty()) return result;
    const fs::path root = pathFromUtf8(
        normalizedPathString(pathFromUtf8(storeRoot)));
    const auto state = readState(root);
    for (const auto& [pluginId, revision] : state.activePackages) {
        const std::string cacheKey = revisionCacheKey(root, pluginId, revision);
        auto cached = revisionCache().find(cacheKey);
        if (cached == revisionCache().end()) {
            cached = revisionCache().emplace(
                cacheKey, resolveRevision(root, pluginId, revision)).first;
        }
        if (cached->second) result.push_back(*cached->second);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.manifest.id < b.manifest.id;
    });
    return result;
}

std::unordered_map<std::string, std::string> controllerDriverSelections(
    const std::string& storeRoot) {
    std::lock_guard lock(storeMutex());
    return storeRoot.empty()
        ? std::unordered_map<std::string, std::string>{}
        : readState(pathFromUtf8(
              normalizedPathString(pathFromUtf8(storeRoot)))).controllerDrivers;
}

std::vector<ManagedPluginInfo> listManagedPlugins() {
    std::unordered_map<std::string, ManagedPluginInfo> byKey;
    for (const auto& installed : activePluginRevisions(userPluginDirectory())) {
        byKey[installed.manifest.id] = managedFromInstalled(installed);
    }

    std::vector<std::string> configuredStores;
    for (const auto& directory : System::pluginDirectories()) {
        if (!directory.empty()) {
            configuredStores.push_back(
                normalizedPathString(pathFromUtf8(directory)));
        }
    }
    const auto storeIndexForPath = [&](const std::string& path) {
        const std::string location = normalizedPathString(pathFromUtf8(path));
        std::optional<std::size_t> bestMatch;
        for (std::size_t index = 0; index < configuredStores.size(); ++index) {
            const auto& store = configuredStores[index];
            if (location.size() > store.size() &&
                location.compare(0, store.size(), store) == 0 &&
                (location[store.size()] == fs::path::preferred_separator ||
                 location[store.size()] == '/')) {
                if (!bestMatch ||
                    store.size() > configuredStores[*bestMatch].size()) {
                    bestMatch = index;
                }
            }
        }
        return bestMatch;
    };

    for (const auto& runtime : PluginRegistry::instance().snapshot()) {
        const std::string runtimeRoot = !runtime.packageRoot.empty()
            ? normalizedPathString(pathFromUtf8(runtime.packageRoot))
            : normalizedPathString(pathFromUtf8(runtime.path));
        const auto storeIndex = storeIndexForPath(runtimeRoot);
        if (!storeIndex) continue;
        const bool isUserStore = *storeIndex == 0;
        const std::string key = isUserStore
            ? runtime.pluginId
            : runtime.pluginId + "\n" + std::to_string(*storeIndex);
        auto it = byKey.find(key);
        ManagedPluginInfo* managed = nullptr;
        if (it == byKey.end()) {
            ManagedPluginInfo runtimeInfo;
            runtimeInfo.path = runtime.packageRoot.empty()
                ? runtime.path
                : runtime.packageRoot;
            runtimeInfo.filename = runtime.filename;
            runtimeInfo.pluginId = runtime.pluginId;
            runtimeInfo.version = runtime.version;
            runtimeInfo.typeName = runtime.typeName;
            runtimeInfo.displayName = runtime.displayName;
            runtimeInfo.vendor = runtime.vendor;
            runtimeInfo.description = runtime.description;
            runtimeInfo.packageSha256 = runtime.packageSha256;
            runtimeInfo.readmePath = runtime.readmePath;
            runtimeInfo.licensePath = runtime.licensePath;
            runtimeInfo.source = isUserStore
                ? ManagedPluginSource::UserPluginDirectory
                : ManagedPluginSource::OtherConfiguredDirectory;
            runtimeInfo.state = isUserStore
                ? ManagedPluginState::RemovedPendingRestart
                : toManagedState(runtime.state);
            runtimeInfo.restartRequired = isUserStore;
            std::error_code ec;
            runtimeInfo.fileExists = !isUserStore &&
                fs::exists(runtimeInfo.path, ec);
            managed = &byKey.emplace(key, std::move(runtimeInfo)).first->second;
        } else {
            const std::string activeRoot = normalizedPathString(
                pathFromUtf8(it->second.path));
            const bool isActiveRevision = runtimeRoot == activeRoot ||
                (runtimeRoot.size() > activeRoot.size() &&
                 runtimeRoot.compare(0, activeRoot.size(), activeRoot) == 0 &&
                 (runtimeRoot[activeRoot.size()] == fs::path::preferred_separator ||
                  runtimeRoot[activeRoot.size()] == '/'));
            if (!isActiveRevision) {
                continue;
            }
            managed = &it->second;
            managed->state = toManagedState(runtime.state);
            managed->restartRequired = false;
        }
        managed->loadError = runtime.loadError;
        managed->runtimeErrors = runtime.runtimeErrors;
    }

    std::vector<ManagedPluginInfo> plugins;
    for (auto& [key, plugin] : byKey) {
        (void)key;
        plugins.push_back(std::move(plugin));
    }
    std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b) {
        return a.displayName == b.displayName
            ? a.pluginId < b.pluginId
            : a.displayName < b.displayName;
    });
    return plugins;
}

PluginInstallResult validatePluginPackageForInstall(const std::string& sourcePath) {
    const auto inspection = inspectPluginPackage(sourcePath);
    if (!inspection.success) return {false, {}, inspection.message};
    return {true, {}, "Valid unsigned plugin package: " + inspection.manifest.name};
}

PluginInstallResult installPlugin(const std::string& sourcePath) {
    const std::string rootString = userPluginDirectory();
    if (rootString.empty()) return {false, {}, "Plugin directory is disabled"};
    const auto inspection = inspectPluginPackage(sourcePath);
    if (!inspection.success) return {false, {}, inspection.message};

    std::lock_guard lock(storeMutex());
    const fs::path root = pathFromUtf8(
        normalizedPathString(pathFromUtf8(rootString)));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return {false, {}, "Failed to create plugin store: " + ec.message()};
    ExclusivePluginFileLock fileLock(root / ".store.lock");
    if (!fileLock.locked()) return {false, {}, fileLock.error()};
    bool stateValid = false;
    auto state = readState(root, &stateValid);
    if (!stateValid) {
        return {false, {},
                "Plugin state.json is invalid; refusing to overwrite it"};
    }
    const std::string revision = inspection.manifest.version + "-" +
        inspection.packageSha256.substr(0, 12);
    const fs::path destination = root / "packages" /
        inspection.manifest.id / revision;
    fs::create_directories(root / "staging", ec);
    if (ec) return {false, {}, "Failed to create plugin store: " + ec.message()};
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return {false, {}, "Failed to create plugin store: " + ec.message()};

    if (!fs::is_directory(destination, ec)) {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        const fs::path staging = root / "staging" /
            (inspection.manifest.id + "-" + std::to_string(suffix));
        fs::create_directory(staging, ec);
        if (ec) {
            return {false, {}, "Failed to create staging directory: " + ec.message()};
        }
        const fs::path stagedArchive = staging / "source.liberaplugin";
        const fs::path stagedPayload = staging / "payload";
        fs::copy_file(pathFromUtf8(sourcePath),
                      stagedArchive,
                      fs::copy_options::none,
                      ec);
        if (ec) {
            const std::string message = ec.message();
            fs::remove_all(staging, ec);
            return {false, {}, "Failed to stage package archive: " + message};
        }
        const auto stagedInspection = inspectPluginPackage(
            pathToUtf8(stagedArchive));
        if (!stagedInspection.success ||
            stagedInspection.packageSha256 != inspection.packageSha256) {
            fs::remove_all(staging, ec);
            return {false, {},
                    "Plugin package changed while it was being installed"};
        }
        PluginPackageInspection extracted;
        std::string extractError;
        if (!extractPluginPackage(pathToUtf8(stagedArchive),
                                  pathToUtf8(stagedPayload),
                                  &extracted,
                                  &extractError)) {
            fs::remove_all(staging, ec);
            return {false, {}, extractError};
        }
        fs::rename(stagedArchive,
                   stagedPayload / "archive.liberaplugin",
                   ec);
        if (ec) {
            const std::string message = ec.message();
            fs::remove_all(staging, ec);
            return {false, {}, "Failed to preserve package archive: " + message};
        }
        fs::rename(stagedPayload, destination, ec);
        if (ec) {
            if (!fs::is_directory(destination)) {
                const std::string message = ec.message();
                fs::remove_all(staging, ec);
                return {false, {}, "Failed to commit plugin revision: " + message};
            }
        }
        fs::remove_all(staging, ec);
    }

    const auto committed = resolveRevision(
        root, inspection.manifest.id, revision);
    if (!committed || committed->packageSha256 != inspection.packageSha256) {
        return {false, pathToUtf8(destination),
                "Installed revision is missing, corrupt, or has a hash-prefix collision"};
    }
    revisionCache()[revisionCacheKey(
        root, inspection.manifest.id, revision)] = committed;

    const std::string committedRoot = normalizedPathString(destination);
    const auto runtime = PluginRegistry::instance().snapshot();
    const bool alreadyExaminedInThisProcess = std::any_of(
        runtime.begin(), runtime.end(), [&](const PluginInfo& info) {
            const std::string runtimeRoot = normalizedPathString(
                pathFromUtf8(info.packageRoot.empty() ? info.path : info.packageRoot));
            return runtimeRoot == committedRoot;
        });
    state.activePackages[inspection.manifest.id] = revision;
    std::string stateError;
    if (!writeState(root, state, &stateError)) {
        return {false, pathToUtf8(destination), stateError};
    }
    return {true,
            committedRoot,
            alreadyExaminedInThisProcess
                ? "Plugin is already active."
                : "Plugin installed. Restart required to load native code.",
            !alreadyExaminedInThisProcess};
}

PluginRemoveResult removePlugin(const std::string& pluginIdOrPath) {
    const std::string rootString = userPluginDirectory();
    if (rootString.empty()) {
        return {false, {}, "Plugin directory is disabled", false};
    }

    std::lock_guard lock(storeMutex());
    const fs::path root = pathFromUtf8(
        normalizedPathString(pathFromUtf8(rootString)));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        return {false, {}, "Failed to create plugin store: " + ec.message(), false};
    }
    ExclusivePluginFileLock fileLock(root / ".store.lock");
    if (!fileLock.locked()) return {false, {}, fileLock.error(), false};
    bool stateValid = false;
    auto state = readState(root, &stateValid);
    if (!stateValid) {
        return {false, {},
                "Plugin state.json is invalid; refusing to overwrite it", false};
    }
    std::string pluginId = pluginIdOrPath;
    if (state.activePackages.find(pluginId) == state.activePackages.end()) {
        const std::string normalizedInput = normalizedPathString(
            pathFromUtf8(pluginIdOrPath));
        for (const auto& [candidate, revision] : state.activePackages) {
            const auto expected = normalizedPathString(
                root / "packages" / candidate / revision);
            if (normalizedInput == expected) {
                pluginId = candidate;
                break;
            }
        }
    }
    const auto active = state.activePackages.find(pluginId);
    if (active == state.activePackages.end()) {
        return {false, {}, "Plugin is not active in this store", false};
    }
    const fs::path revisionPath = root / "packages" / pluginId / active->second;
    const auto runtime = PluginRegistry::instance().snapshot();
    const std::string normalizedRoot = normalizedPathString(root);
    const bool restartRequired = std::any_of(
        runtime.begin(), runtime.end(), [&](const PluginInfo& info) {
            if (info.pluginId != pluginId || info.state != PluginState::Loaded) {
                return false;
            }
            const std::string location = normalizedPathString(
                info.packageRoot.empty() ? info.path : info.packageRoot);
            return location.size() > normalizedRoot.size() &&
                   location.compare(0, normalizedRoot.size(), normalizedRoot) == 0 &&
                   (location[normalizedRoot.size()] == fs::path::preferred_separator ||
                    location[normalizedRoot.size()] == '/');
        });
    state.activePackages.erase(active);
    for (auto it = state.controllerDrivers.begin();
         it != state.controllerDrivers.end();) {
        if (it->second == pluginId) it = state.controllerDrivers.erase(it);
        else ++it;
    }
    std::string error;
    if (!writeState(root, state, &error)) {
        return {false, pathToUtf8(revisionPath), error, false};
    }
    return {true,
            normalizedPathString(revisionPath),
            restartRequired
                ? "Plugin deactivated. Restart required to unload native code."
                : "Plugin deactivated.",
            restartRequired};
}

bool selectControllerDriver(const std::string& controllerType,
                            const std::string& driverId,
                            std::string* error) {
    if (controllerType.empty()) {
        if (error) *error = "Controller type is empty";
        return false;
    }
    const std::string rootString = userPluginDirectory();
    if (rootString.empty()) {
        if (error) *error = "Plugin directory is disabled";
        return false;
    }
    std::lock_guard lock(storeMutex());
    const fs::path root = pathFromUtf8(
        normalizedPathString(pathFromUtf8(rootString)));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        if (error) *error = "Failed to create plugin store: " + ec.message();
        return false;
    }
    ExclusivePluginFileLock fileLock(root / ".store.lock");
    if (!fileLock.locked()) {
        if (error) *error = fileLock.error();
        return false;
    }
    bool stateValid = false;
    auto state = readState(root, &stateValid);
    if (!stateValid) {
        if (error) *error =
            "Plugin state.json is invalid; refusing to overwrite it";
        return false;
    }
    if (driverId.empty()) state.controllerDrivers.erase(controllerType);
    else state.controllerDrivers[controllerType] = driverId;
    return writeState(root, state, error);
}

std::unordered_map<std::string, std::string> selectedControllerDrivers() {
    return controllerDriverSelections(userPluginDirectory());
}

bool isPluginPackageFile(const std::string& path) {
    std::string extension = pathFromUtf8(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".liberaplugin";
}

} // namespace libera::plugin
