#include "libera/System.hpp"

#include <algorithm>
#include <utility>

#if LIBERA_ENABLE_PLUGINS
#include "libera/plugin/PluginManager.hpp"
#include "libera/plugin/PluginManagement.hpp"
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#endif

namespace libera::core {

std::vector<ControllerManagerRegistration>& getControllerManagerRegistrations() {
    static std::vector<ControllerManagerRegistration> registrations;
    return registrations;
}

namespace {

ControllerManagerRegistration normalizeRegistration(ControllerManagerRegistration registration) {
    if (registration.info.displayName.empty()) {
        registration.info.displayName = registration.info.type;
    }
    return registration;
}

bool hasRegisteredDriverId(const std::string& driverId) {
    if (driverId.empty()) {
        return false;
    }
    const auto& registrations = getControllerManagerRegistrations();
    return std::any_of(
        registrations.begin(), registrations.end(),
        [&](const ControllerManagerRegistration& registration) {
            return registration.info.driverId == driverId;
        });
}

} // namespace

ControllerManagerRegistry::ControllerManagerRegistry(ControllerManagerRegistration registration) {
    AddControllerManager(std::move(registration));
}

void AddControllerManager(ControllerManagerRegistration registration) {
    registration = normalizeRegistration(std::move(registration));
    if (!registration.factory) {
        return;
    }
    if (registration.info.driverId.empty() || registration.info.type.empty()) {
        return;
    }
    if (hasRegisteredDriverId(registration.info.driverId)) {
        return;
    }
    getControllerManagerRegistrations().push_back(std::move(registration));
}

std::vector<ControllerManagerInfo> registeredControllerManagers() {
    std::vector<ControllerManagerInfo> infos;
    for (const auto& registration : getControllerManagerRegistrations()) {
        if (!registration.info.type.empty()) {
            infos.push_back(registration.info);
        }
    }
    std::sort(infos.begin(), infos.end(),
              [](const ControllerManagerInfo& a, const ControllerManagerInfo& b) {
                  return a.displayName < b.displayName;
              });
    return infos;
}

} // namespace libera::core

namespace libera {

#if LIBERA_ENABLE_PLUGINS

namespace {

#ifndef _WIN32
std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
}
#endif

std::string resolvePluginDirectory(const std::string& requested) {
    namespace fs = std::filesystem;
    if (requested.empty()) return requested;

    const fs::path path = fs::u8path(requested);
    std::error_code ec;
    auto absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    auto canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute.lexically_normal() : canonical).u8string();
}

std::filesystem::path defaultUserPluginDirectory() {
    namespace fs = std::filesystem;
    fs::path baseDir;

#ifdef _WIN32
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (localAppData && *localAppData) {
        baseDir = localAppData;
    } else {
        const wchar_t* userProfile = _wgetenv(L"USERPROFILE");
        if (userProfile && *userProfile) {
            baseDir = fs::path(userProfile) / "AppData" / "Local";
        }
    }
#elif defined(__APPLE__)
    const auto home = envValue("HOME");
    if (!home.empty()) {
        baseDir = fs::path(home) / "Library" / "Application Support";
    }
#else
    const auto xdgDataHome = envValue("XDG_DATA_HOME");
    if (!xdgDataHome.empty()) {
        baseDir = xdgDataHome;
    } else {
        const auto home = envValue("HOME");
        if (!home.empty()) {
            baseDir = fs::path(home) / ".local" / "share";
        }
    }
#endif

    if (baseDir.empty()) {
        baseDir = fs::current_path();
    }

#if defined(_WIN32) || defined(__APPLE__)
    return baseDir / "Libera" / "Plugins";
#else
    return baseDir / "libera" / "plugins";
#endif
}

std::vector<std::string> defaultPluginDirectories() {
    // Libera apps share one user-level plugin folder so installing a plugin
    // once makes it available to every app in the Libera ecosystem.
    return {defaultUserPluginDirectory().u8string()};
}

std::vector<std::string>& pluginDirStorage() {
    static std::vector<std::string> dirs = defaultPluginDirectories();
    return dirs;
}

std::set<std::string>& attemptedPluginDirectories() {
    static std::set<std::string> directories;
    return directories;
}

std::recursive_mutex& pluginLoadMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void loadConfiguredPluginDirectories() {
    std::lock_guard lock(pluginLoadMutex());
    for (const auto& dir : System::pluginDirectories()) {
        if (dir.empty()) continue;
        const auto resolved = resolvePluginDirectory(dir);
        // Package activation is restart-based. Once a store has been examined
        // in this process, later management changes remain pending rather than
        // unexpectedly dlopen()ing code from an arbitrary UI query.
        if (attemptedPluginDirectories().insert(resolved).second) {
            plugin::loadPluginsFromDirectory(resolved);
        }
    }
}

} // anonymous namespace

void System::setPluginDirectory(const std::string& path) {
    auto& dirs = pluginDirStorage();
    dirs.clear();
    if (!path.empty()) dirs.push_back(path);
}

const std::string& System::pluginDirectory() {
    static const std::string empty;
    const auto& dirs = pluginDirStorage();
    return dirs.empty() ? empty : dirs.front();
}

void System::addPluginDirectory(const std::string& path) {
    if (path.empty()) return;
    pluginDirStorage().push_back(path);
}

const std::vector<std::string>& System::pluginDirectories() {
    return pluginDirStorage();
}

#else

namespace {

void loadConfiguredPluginDirectories() {}

} // anonymous namespace

void System::setPluginDirectory(const std::string&) {}

const std::string& System::pluginDirectory() {
    static const std::string empty;
    return empty;
}

void System::addPluginDirectory(const std::string&) {}

const std::vector<std::string>& System::pluginDirectories() {
    static const std::vector<std::string> empty;
    return empty;
}

#endif

System::System()
: System(SystemOptions{}) {}

System::System(SystemOptions options) {
    loadConfiguredPluginDirectories();

    // Persisted UI choices provide the default, while an embedding
    // application can still override any family for one System instance.
    std::unordered_map<std::string, std::string> selectedDrivers;
#if LIBERA_ENABLE_PLUGINS
    selectedDrivers = plugin::selectedControllerDrivers();
#endif
    for (const auto& [type, driverId] : options.selectedControllerDrivers) {
        selectedDrivers[type] = driverId;
    }

    auto controllerTypeEnabled = [&](std::string_view type) {
        return type.empty() ||
               options.disabledControllerTypes.find(std::string(type)) ==
                   options.disabledControllerTypes.end();
    };

    // Group registrations before constructing anything. Only one driver for a
    // controller family should probe hardware; otherwise competing drivers can
    // bind the same ports or present the same physical controller twice.
    std::unordered_map<std::string,
                       std::vector<const core::ControllerManagerRegistration*>>
        registrationsByType;
    for (const auto& registration : core::getControllerManagerRegistrations()) {
        if (!registration.factory || !controllerTypeEnabled(registration.info.type)) {
            continue;
        }
        registrationsByType[registration.info.type].push_back(&registration);
    }

    for (const auto& [type, registrations] : registrationsByType) {
        const core::ControllerManagerRegistration* selected = nullptr;

        const auto requested = selectedDrivers.find(type);
        if (requested != selectedDrivers.end()) {
            const auto found = std::find_if(
                registrations.begin(), registrations.end(),
                [&](const auto* registration) {
                    return registration->info.driverId == requested->second;
                });
            if (found != registrations.end()) {
                selected = *found;
            }
        }

        // A newly installed plugin must not silently replace a built-in
        // implementation. Keep the built-in selected until the host records a
        // deliberate user choice.
        if (!selected) {
            const auto builtIn = std::find_if(
                registrations.begin(), registrations.end(),
                [](const auto* registration) {
                    return registration->info.builtIn;
                });
            if (builtIn != registrations.end()) {
                selected = *builtIn;
            }
        }

        // A plugin-only controller type is unambiguous when exactly one plugin
        // implements it. Multiple plugin-only implementations require an
        // explicit choice and remain inactive until one is supplied.
        if (!selected && registrations.size() == 1) {
            selected = registrations.front();
        }
        if (!selected) {
            continue;
        }

        auto manager = selected->factory();
        if (!manager || manager->managedType() != type) {
            if (manager) {
                manager->closeAll();
            }
            continue;
        }
        managerByDriverId[selected->info.driverId] = manager.get();
        managers.push_back({selected->info.driverId, std::move(manager)});
    }
}

std::vector<core::ControllerManagerInfo> System::availableControllerManagers() {
    loadConfiguredPluginDirectories();
    return core::registeredControllerManagers();
}

System::~System() {
    shutdown();
}

std::vector<std::unique_ptr<core::ControllerInfo>> System::discoverControllers() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    for (auto& active : managers) {
        if (!active.manager) continue;
        auto subset = active.manager->discover();
        results.reserve(results.size() + subset.size());
        for (auto& item : subset) {
            if (item) {
                item->setDriverId(active.driverId);
            }
            results.emplace_back(std::move(item));
        }
    }
    return results;
}

std::shared_ptr<core::LaserController>
System::connectController(const core::ControllerInfo& info) {
    auto it = managerByDriverId.find(info.driverId());
    if (it == managerByDriverId.end() || !it->second) {
        return nullptr;
    }
    auto controller = it->second->connectController(info);
    if (controller) {
        controller->setControllerIdentity(info.idValue(), info.labelValue());
    }
    return controller;
}

bool System::disconnectController(std::string_view driverId, std::string_view id) {
    auto it = managerByDriverId.find(std::string(driverId));
    if (it == managerByDriverId.end() || !it->second) {
        return false;
    }

    return it->second->disconnectController(id);
}

void System::shutdown() {
    if (shutdownComplete) {
        return;
    }

    for (auto it = managers.rbegin(); it != managers.rend(); ++it) {
        if (it->manager) {
            it->manager->closeAll();
        }
    }
    managerByDriverId.clear();
    managers.clear();
    shutdownComplete = true;
}

} // namespace libera
