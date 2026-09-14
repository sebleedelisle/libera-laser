#include "libera/plugin/PluginManager.hpp"

#include "libera/plugin/PluginControllerInfo.hpp"
#include "libera/plugin/PluginRegistry.hpp"
#include "libera/log/Log.hpp"

#include "PluginValidation.hpp"
#include "PluginSettingsInternal.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace libera::plugin {

namespace fs = std::filesystem;

namespace {

void* openLibrary(const std::string& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibrary(void* handle) {
    if (!handle) {
        return;
    }
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
    if (err == 0) {
        return {};
    }
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
    if (!message) {
        return;
    }
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

void hostRecordLatencyCallback(libera_host_ctx_t host_ctx,
                               uint64_t nanoseconds) {
    if (!host_ctx) {
        return;
    }
    auto* ctrl = static_cast<PluginController*>(host_ctx);
    ctrl->recordLatencyFromPlugin(nanoseconds);
}

void hostReportErrorCallback(libera_host_ctx_t host_ctx,
                             const char* code,
                             const char* label) {
    if (!host_ctx) {
        return;
    }
    auto* ctrl = static_cast<PluginController*>(host_ctx);
    ctrl->reportErrorFromPlugin(code, label);
}

const libera_host_services_t hostServices = {
    /* abi_version    */ LIBERA_PLUGIN_HOST_SERVICES_VERSION,
    /* struct_size    */ sizeof(libera_host_services_t),
    /* log            */ &hostLogCallback,
    /* record_latency */ &hostRecordLatencyCallback,
    /* report_error   */ &hostReportErrorCallback,
};

bool pluginApiSupportsFrameTransport(const libera_plugin_api_t* api) {
    return api &&
           api->get_frame_requirements &&
           api->send_frame;
}

core::ControllerUsageState toUsageState(libera_controller_usage_state_t usageState) {
    switch (usageState) {
        case LIBERA_CONTROLLER_USAGE_IDLE:
            return core::ControllerUsageState::Idle;
        case LIBERA_CONTROLLER_USAGE_ACTIVE:
            return core::ControllerUsageState::Active;
        case LIBERA_CONTROLLER_USAGE_BUSY_EXCLUSIVE:
            return core::ControllerUsageState::BusyExclusive;
        case LIBERA_CONTROLLER_USAGE_UNKNOWN:
        default:
            return core::ControllerUsageState::Unknown;
    }
}

std::string canonicalPluginPath(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    return ec ? path.string() : canonical.string();
}

std::unordered_set<std::string>& loadedPluginPaths() {
    static std::unordered_set<std::string> paths;
    return paths;
}

std::shared_ptr<LoadedPlugin> loadPlugin(const fs::path& path) {
    auto& registry = PluginRegistry::instance();
    const std::string pathString = canonicalPluginPath(path);

    void* handle = openLibrary(pathString);
    if (!handle) {
        const std::string error = libraryError();
        libera::log::logError("Plugin: failed to load ", pathString,
                              ": ", error);
        registry.recordFailure(pathString, PluginState::FailedLoad, error);
        return nullptr;
    }

    auto getApi = resolveSymbol<decltype(&libera_plugin_get_api)>(
        handle, "libera_plugin_get_api");
    if (!getApi) {
        closeLibrary(handle);
        registry.recordFailure(pathString,
                               PluginState::NotAPlugin,
                               "Missing libera_plugin_get_api symbol");
        return nullptr;
    }

    const libera_plugin_api_t* api = getApi();
    const std::string validationError = validatePluginApi(api);
    if (!validationError.empty()) {
        const std::string typeName =
            (api && api->type_name) ? api->type_name : "";
        const std::string displayName =
            (api && api->display_name) ? api->display_name : "";
        libera::log::logError("Plugin: ", pathString, " ", validationError);
        closeLibrary(handle);
        registry.recordFailure(pathString,
                               PluginState::FailedValidation,
                               validationError,
                               typeName,
                               displayName);
        return nullptr;
    }

    auto plugin = std::make_shared<LoadedPlugin>();
    plugin->libraryHandle = handle;
    plugin->api = api;
    plugin->typeName = api->type_name;
    plugin->displayName = api->display_name;
    plugin->path = pathString;
    registerLoadedPlugin(plugin);

    libera::log::logInfo("Plugin: loaded \"", plugin->displayName,
                         "\" (type=",
                         plugin->typeName,
                         ", api=",
                         api->abi_version,
                         ", transport=",
                         pluginApiSupportsFrameTransport(api) ? "frame" : "point",
                         ") from ",
                         path.filename().string());
    registry.recordLoaded(pathString, plugin->typeName, plugin->displayName);
    return plugin;
}

} // namespace

PluginDelegateManager::PluginDelegateManager(std::shared_ptr<LoadedPlugin> plugin)
: core::ControllerManagerBase<PluginControllerInfo,
                              PluginController>(plugin ? plugin->typeName : std::string{})
, plugin(std::move(plugin)) {}

PluginDelegateManager::~PluginDelegateManager() {
    closeAll();
}

bool PluginDelegateManager::ensureBackend() {
    if (!plugin || !plugin->api) {
        return false;
    }
    std::lock_guard lifecycleLock(plugin->lifecycleMutex);
    if (plugin->initialised) {
        return true;
    }

    if (plugin->api->create_backend) {
        plugin->backendHandle = plugin->api->create_backend(&hostServices);
        if (!plugin->backendHandle) {
            const std::string message = "create_backend() returned null";
            libera::log::logError("Plugin: ", plugin->path, " ", message);
            PluginRegistry::instance().recordFailure(
                plugin->path,
                PluginState::FailedBackend,
                message,
                plugin->typeName,
                plugin->displayName);
            return false;
        }
    }

    // Plugin-wide settings may affect discovery, so restore them before the
    // first rescan rather than waiting for a controller connection.
    std::string settingsError;
    if (!applySavedPluginSettings(plugin, &settingsError)) {
        PluginRegistry::instance().recordFailure(
            plugin->path,
            PluginState::FailedBackend,
            settingsError,
            plugin->typeName,
            plugin->displayName);
        if (plugin->api->destroy_backend) {
            plugin->api->destroy_backend(plugin->backendHandle);
        }
        plugin->backendHandle = nullptr;
        return false;
    }

    plugin->initialised = true;
    PluginRegistry::instance().recordLoaded(
        plugin->path, plugin->typeName, plugin->displayName);
    return true;
}

std::vector<std::unique_ptr<core::ControllerInfo>>
PluginDelegateManager::discover() {
    if (!ensureBackend()) {
        return {};
    }

    // Controller creation takes the cache lock before it may initialize the
    // plugin backend. Snapshot in that same lock order to avoid discovery
    // taking the backend lock and then trying to acquire the cache lock.
    const auto activeSnapshot = liveControllers();
    std::lock_guard lifecycleLock(plugin->lifecycleMutex);

    if (plugin->api->rescan) {
        plugin->api->rescan(plugin->backendHandle);
    }

    struct DiscoverCtx {
        std::vector<libera_controller_info_t> infos;
    } ctx;

    auto emit = [](void* raw, const libera_controller_info_t* info) {
        auto* discoverCtx = static_cast<DiscoverCtx*>(raw);
        libera_controller_info_t safe = *info;

        safe.id[sizeof(safe.id) - 1] = '\0';
        safe.label[sizeof(safe.label) - 1] = '\0';
        safe.network.ip[sizeof(safe.network.ip) - 1] = '\0';
        safe.connect_cookie_size = std::min<std::uint32_t>(
            safe.connect_cookie_size,
            static_cast<std::uint32_t>(sizeof(safe.connect_cookie)));

        discoverCtx->infos.push_back(safe);
    };

    plugin->api->discover(plugin->backendHandle, emit, &ctx);

    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    results.reserve(ctx.infos.size());

    for (const auto& pluginInfo : ctx.infos) {
        auto info = std::make_unique<PluginControllerInfo>(
            pluginInfo, plugin->typeName);
        info->setUsageState(toUsageState(pluginInfo.usage_state));

        // If we already own this controller in-process, report it as active
        // rather than whatever discovery saw externally.
        if (activeSnapshot.find(info->idValue()) != activeSnapshot.end()) {
            info->setUsageState(core::ControllerUsageState::Active);
        }

        results.emplace_back(std::move(info));
    }
    return results;
}

std::shared_ptr<PluginController>
PluginDelegateManager::createController(const PluginControllerInfo& info) {
    if (!ensureBackend()) {
        return nullptr;
    }
    auto controller = std::make_shared<PluginController>(
        plugin->api,
        plugin->backendHandle,
        info.pluginInfo(),
        plugin->path);
    return controller;
}

PluginDelegateManager::NewControllerDisposition
PluginDelegateManager::prepareNewController(PluginController& controller,
                                            const PluginControllerInfo& info) {
    (void)info;
    if (!controller.open()) {
        return NewControllerDisposition::DropController;
    }

    // A controller's opaque plugin handle exists now, but its worker has not
    // started yet. This is the only race-free place to restore persisted
    // controller settings before the first frame can be submitted.
    std::string settingsError;
    if (!applySavedControllerSettings(controller, &settingsError)) {
        PluginRegistry::instance().pushRuntimeError(
            plugin->path, "settings.controller_apply", settingsError);
        controller.close();
        return NewControllerDisposition::DropController;
    }
    // Publish the controller for live setting changes only after its initial
    // settings have been applied to the fully constructed plugin handle.
    registerPluginController(plugin->typeName,
                             info.idValue(),
                             controller.shared_from_this());
    controller.useFrameQueue();
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void PluginDelegateManager::closeController(const std::string& key,
                                             PluginController& controller) {
    (void)key;
    // Destroy controller handles before their shared backend. External
    // shared_ptr owners may keep the C++ wrapper alive after System shutdown.
    controller.close();
}

void PluginDelegateManager::afterCloseControllers() {
    if (!plugin) {
        return;
    }
    std::lock_guard lifecycleLock(plugin->lifecycleMutex);
    if (plugin && plugin->initialised) {
        if (plugin->api->destroy_backend) {
            plugin->api->destroy_backend(plugin->backendHandle);
        }
        plugin->backendHandle = nullptr;
        plugin->initialised = false;
    }
}

void loadPluginsFromDirectory(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return;
    }

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (entry.is_regular_file() && isSharedLibraryPath(entry.path())) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        const std::string candidatePath = canonicalPluginPath(candidate);
        if (loadedPluginPaths().find(candidatePath) != loadedPluginPaths().end()) {
            continue;
        }

        auto plugin = loadPlugin(candidate);
        if (!plugin) {
            continue;
        }
        loadedPluginPaths().insert(candidatePath);

        core::AddControllerManager(core::ControllerManagerRegistration{
            core::ControllerManagerInfo{
                plugin->typeName,
                plugin->displayName,
                "Plugin controller from " + fs::path(plugin->path).filename().string(),
            },
            [plugin]() {
                return std::make_unique<PluginDelegateManager>(plugin);
            },
        });
    }
}

} // namespace libera::plugin
