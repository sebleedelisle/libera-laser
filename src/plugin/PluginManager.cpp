#include "libera/plugin/PluginManager.hpp"

#include "libera/plugin/PluginControllerInfo.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace libera::plugin {

namespace fs = std::filesystem;

// ---- platform helpers --------------------------------------------------- //

namespace {

void* openLibrary(const std::string& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibrary(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

template <typename Fn>
Fn resolveSymbol(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<Fn>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return reinterpret_cast<Fn>(dlsym(handle, name));
#endif
}

std::string libraryError() {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == 0) return {};
    LPSTR buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                   nullptr, err, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = buf ? buf : "unknown error";
    LocalFree(buf);
    return msg;
#else
    const char* msg = dlerror();
    return msg ? msg : "unknown error";
#endif
}

bool isSharedLibrary(const fs::path& p) {
    auto ext = p.extension().string();
    return ext == ".dylib" || ext == ".so" || ext == ".dll";
}

bool resolvePluginFunctions(void* handle, PluginFunctions& f) {
#define RESOLVE(field, symbol)                                       \
    f.field = resolveSymbol<decltype(f.field)>(handle, #symbol);     \
    if (!f.field) {                                                  \
        libera::log::logError("Plugin: missing symbol " #symbol);    \
        return false;                                                \
    }

    RESOLVE(api_version,        libera_plugin_api_version)
    RESOLVE(type,               libera_plugin_type)
    RESOLVE(name,               libera_plugin_name)
    RESOLVE(init,               libera_plugin_init)
    RESOLVE(shutdown,           libera_plugin_shutdown)
    RESOLVE(discover,           libera_plugin_discover)
    RESOLVE(connect,            libera_plugin_connect)
    RESOLVE(send_points,        libera_plugin_send_points)
    RESOLVE(get_buffer_fullness, libera_plugin_get_buffer_fullness)
    RESOLVE(set_armed,          libera_plugin_set_armed)
    RESOLVE(disconnect,         libera_plugin_disconnect)

#undef RESOLVE
    return true;
}

std::shared_ptr<LoadedPlugin> loadPlugin(const fs::path& path) {
    void* handle = openLibrary(path.string());
    if (!handle) {
        libera::log::logError("Plugin: failed to load ", path.string(),
                              ": ", libraryError());
        return nullptr;
    }

    PluginFunctions funcs;
    if (!resolvePluginFunctions(handle, funcs)) {
        closeLibrary(handle);
        return nullptr;
    }

    const uint32_t version = funcs.api_version();
    if (version != LIBERA_PLUGIN_API_VERSION) {
        libera::log::logError("Plugin: ", path.string(),
                              " has API version ", version,
                              ", expected ", LIBERA_PLUGIN_API_VERSION);
        closeLibrary(handle);
        return nullptr;
    }

    const int rc = funcs.init();
    if (rc != 0) {
        libera::log::logError("Plugin: ", path.string(),
                              " init() failed with code ", rc);
        closeLibrary(handle);
        return nullptr;
    }

    auto plugin = std::make_shared<LoadedPlugin>();
    plugin->libraryHandle = handle;
    plugin->funcs = funcs;
    plugin->typeName = funcs.type();
    plugin->displayName = funcs.name();
    plugin->initialised = true;

    libera::log::logInfo("Plugin: loaded \"", plugin->displayName,
                         "\" (type=", plugin->typeName, ") from ",
                         path.filename().string());
    return plugin;
}

} // anonymous namespace

// ---- PluginDelegateManager ---------------------------------------------- //

PluginDelegateManager::PluginDelegateManager(std::shared_ptr<LoadedPlugin> plugin)
: plugin(std::move(plugin)) {}

PluginDelegateManager::~PluginDelegateManager() {
    closeAll();
}

std::string_view PluginDelegateManager::managedType() const {
    return plugin->typeName;
}

std::vector<std::unique_ptr<core::ControllerInfo>>
PluginDelegateManager::discover() {
    constexpr int MAX_DEVICES = 32;
    libera_controller_info_t infos[MAX_DEVICES];

    const int count = plugin->funcs.discover(infos, MAX_DEVICES);
    if (count < 0) {
        libera::log::logError("Plugin \"", plugin->displayName,
                              "\": discover() returned ", count);
        return {};
    }

    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    results.reserve(static_cast<std::size_t>(count));

    std::lock_guard lock(activeMutex);

    for (int i = 0; i < count; ++i) {
        auto& ci = infos[i];
        std::string id(ci.id);
        std::string label(ci.label);

        auto info = std::make_unique<PluginControllerInfo>(
            id, label, ci.maxPointRate, plugin->typeName);

        // Mark as active if we already have an open controller for this id.
        auto it = activeControllers.find(id);
        if (it != activeControllers.end() && !it->second.expired()) {
            info->setUsageState(core::ControllerUsageState::Active);
        }

        results.emplace_back(std::move(info));
    }
    return results;
}

std::shared_ptr<core::LaserController>
PluginDelegateManager::connectController(const core::ControllerInfo& info) {
    std::lock_guard lock(activeMutex);

    // Reuse existing connection if still alive.
    auto it = activeControllers.find(info.idValue());
    if (it != activeControllers.end()) {
        if (auto existing = it->second.lock()) {
            return existing;
        }
        activeControllers.erase(it);
    }

    auto controller = std::make_shared<PluginController>(
        plugin->funcs, info.idValue());

    if (!controller->open()) {
        return nullptr;
    }

    activeControllers[info.idValue()] = controller;

    controller->startFrameMode();
    controller->start();

    return controller;
}

void PluginDelegateManager::closeAll() {
    std::lock_guard lock(activeMutex);
    for (auto& [id, weak] : activeControllers) {
        if (auto ctrl = weak.lock()) {
            ctrl->stop();
        }
    }
    activeControllers.clear();

    if (plugin && plugin->initialised) {
        plugin->funcs.shutdown();
        plugin->initialised = false;
    }
}

// ---- Public loader ------------------------------------------------------ //

void loadPluginsFromDirectory(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        // No plugin directory — that's fine, just skip.
        return;
    }

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (entry.is_regular_file() && isSharedLibrary(entry.path())) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        auto plugin = loadPlugin(candidate);
        if (!plugin) continue;

        core::AddControllerManager([plugin]() {
            return std::make_unique<PluginDelegateManager>(plugin);
        });
    }
}

} // namespace libera::plugin
