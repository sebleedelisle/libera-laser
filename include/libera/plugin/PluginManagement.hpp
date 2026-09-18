#pragma once

#include "libera/plugin/PluginRegistry.hpp"
#include "libera/plugin/PluginPackage.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace libera::plugin {

enum class ManagedPluginState {
    Loaded,
    NotAPlugin,
    FailedLoad,
    FailedValidation,
    FailedBackend,
    PendingRestart,
    RemovedPendingRestart,
};

enum class ManagedPluginSource {
    UserPluginDirectory,
    OtherConfiguredDirectory,
};

struct ManagedPluginInfo {
    std::string path;
    std::string filename;
    ManagedPluginState state = ManagedPluginState::PendingRestart;
    ManagedPluginSource source = ManagedPluginSource::OtherConfiguredDirectory;
    std::string typeName;
    std::string pluginId;
    std::string version;
    std::string displayName;
    std::string vendor;
    std::string description;
    std::string packageSha256;
    std::optional<std::string> readmePath;
    std::optional<std::string> licensePath;
    std::optional<std::string> loadError;
    std::vector<PluginRuntimeError> runtimeErrors;
    bool fileExists = false;
    bool canRemove = false;
    bool restartRequired = false;
};

struct PluginRemoveResult {
    bool success = false;
    std::string removedPath;
    std::string message;
    bool restartRequired = false;
};

/*
 * Directory used by the plugin installer/remover.
 *
 * This is the first configured System plugin directory. With the default
 * System configuration it is the shared per-user Libera plugin folder.
 */
const std::string& userPluginDirectory();

/*
 * Merge the runtime registry with active revisions in the user plugin store.
 *
 * Runtime registry entries report plugins loaded, rejected, or errored during
 * startup. Installed revisions not yet in the registry are PendingRestart because
 * System loads native entrypoints only during startup.
 */
std::vector<ManagedPluginInfo> listManagedPlugins();

/*
 * Install and activate an unsigned .liberaplugin package. Native code is not
 * loaded during installation; activation takes effect after restart.
 */
PluginInstallResult installPlugin(const std::string& sourcePath);

/*
 * Deactivate a plugin in userPluginDirectory(). Immutable revision files are
 * retained so loaded code is never overwritten or deleted in place.
 *
 * Native plugin libraries remain loaded by the operating system until process
 * restart, so removing a loaded plugin returns restartRequired=true.
 */
PluginRemoveResult removePlugin(const std::string& pluginId);

/* Select the driver used for a controller family on the next System start. */
bool selectControllerDriver(const std::string& controllerType,
                            const std::string& driverId,
                            std::string* error = nullptr);
std::unordered_map<std::string, std::string> selectedControllerDrivers();

bool isPluginPackageFile(const std::string& path);

} // namespace libera::plugin
