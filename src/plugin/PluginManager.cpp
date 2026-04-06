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

void hostLogCallback(libera_log_level_t level, const char* message) {
    if (!message) return;
    switch (level) {
        case LIBERA_LOG_ERROR:
            libera::log::logError(message);
            break;
        case LIBERA_LOG_WARNING:
        default:
            libera::log::logInfo(message);
            break;
    }
}

void hostRecordLatencyCallback(libera_host_ctx_t host_ctx, uint64_t nanoseconds) {
    if (!host_ctx) return;
    auto* ctrl = static_cast<PluginController*>(host_ctx);
    ctrl->recordLatencyFromPlugin(nanoseconds);
}

void hostReportErrorCallback(libera_host_ctx_t host_ctx,
                             const char* code,
                             const char* label) {
    if (!host_ctx) return;
    auto* ctrl = static_cast<PluginController*>(host_ctx);
    ctrl->reportErrorFromPlugin(code, label);
}

// Single host services table shared across all loaded plugins.
const libera_host_services_t kHostServices = {
    /* abi_version    */ LIBERA_HOST_SERVICES_VERSION,
    /* log            */ &hostLogCallback,
    /* record_latency */ &hostRecordLatencyCallback,
    /* report_error   */ &hostReportErrorCallback,
};

bool isSharedLibrary(const fs::path& p) {
    auto ext = p.extension().string();
    return ext == ".dylib" || ext == ".so" || ext == ".dll";
}

bool resolvePluginFunctions(void* handle, PluginFunctions& f, bool& isPlugin) {
    // Check for the version symbol first — if absent, this shared library
    // is not a libera plugin (e.g. a vendor SDK sitting in the same directory).
    f.api_version = resolveSymbol<decltype(f.api_version)>(handle, "libera_plugin_api_version");
    if (!f.api_version) {
        isPlugin = false;
        return false;
    }
    isPlugin = true;

#define RESOLVE(field, symbol)                                       \
    f.field = resolveSymbol<decltype(f.field)>(handle, #symbol);     \
    if (!f.field) {                                                  \
        libera::log::logError("Plugin: missing symbol " #symbol);    \
        return false;                                                \
    }

    RESOLVE(type,               libera_plugin_type)
    RESOLVE(name,               libera_plugin_name)
    RESOLVE(init,               libera_plugin_init)
    RESOLVE(shutdown,           libera_plugin_shutdown)
    RESOLVE(discover,           libera_plugin_discover)
    RESOLVE(connect,            libera_plugin_connect)
    RESOLVE(set_point_rate,     libera_plugin_set_point_rate)
    RESOLVE(send_points,        libera_plugin_send_points)
    RESOLVE(get_buffer_state,   libera_plugin_get_buffer_state)
    RESOLVE(set_armed,          libera_plugin_set_armed)
    RESOLVE(disconnect,         libera_plugin_disconnect)

#undef RESOLVE

    // Optional exports — absence is fine, just means no capability.
    f.rescan          = resolveSymbol<decltype(f.rescan)>(handle, "libera_plugin_rescan");
    f.list_properties = resolveSymbol<decltype(f.list_properties)>(handle, "libera_plugin_list_properties");
    f.get_property    = resolveSymbol<decltype(f.get_property)>(handle, "libera_plugin_get_property");
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
    bool isPlugin = false;
    if (!resolvePluginFunctions(handle, funcs, isPlugin)) {
        closeLibrary(handle);
        if (!isPlugin) {
            // Not a libera plugin — silently skip (e.g. a vendor SDK).
            return nullptr;
        }
        // It is a plugin but is missing required symbols.
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

    const int rc = funcs.init(&kHostServices);
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
    // Let network-discovery plugins refresh their cache before we enumerate.
    if (plugin->funcs.rescan) {
        plugin->funcs.rescan();
    }

    struct DiscoverCtx {
        std::vector<libera_controller_info_t> infos;
    } ctx;

    auto emit = [](void* raw, const libera_controller_info_t* info) {
        auto* c = static_cast<DiscoverCtx*>(raw);
        // Defensive: ensure the strings are null-terminated before we copy.
        libera_controller_info_t safe = *info;
        safe.id[sizeof(safe.id) - 1] = '\0';
        safe.label[sizeof(safe.label) - 1] = '\0';
        c->infos.push_back(safe);
    };

    plugin->funcs.discover(emit, &ctx);

    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    results.reserve(ctx.infos.size());

    std::lock_guard lock(activeMutex);

    for (const auto& ci : ctx.infos) {
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
    controller->startThread();

    return controller;
}

void PluginDelegateManager::closeAll() {
    std::lock_guard lock(activeMutex);
    for (auto& [id, weak] : activeControllers) {
        if (auto ctrl = weak.lock()) {
            ctrl->stopThread();
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
