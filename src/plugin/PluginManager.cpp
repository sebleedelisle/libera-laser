#include "libera/plugin/PluginManager.hpp"

#include "libera/plugin/PluginControllerInfo.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <cstdint>
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

namespace {

constexpr std::uint32_t minimumSupportedPluginApiVersion = LIBERA_PLUGIN_API_VERSION_1;

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

const libera_host_services_t kHostServices = {
    /* abi_version    */ LIBERA_PLUGIN_HOST_SERVICES_VERSION,
    /* log            */ &hostLogCallback,
    /* record_latency */ &hostRecordLatencyCallback,
    /* report_error   */ &hostReportErrorCallback,
};

bool pluginApiSupportsFrameTransport(const libera_plugin_api_t* api) {
    return api &&
           api->abi_version >= LIBERA_PLUGIN_API_VERSION_2 &&
           api->get_frame_requirements &&
           api->send_frame;
}

bool isSharedLibrary(const fs::path& path) {
    const auto ext = path.extension().string();
    return ext == ".dylib" || ext == ".so" || ext == ".dll";
}

bool validatePluginApi(const libera_plugin_api_t* api, const fs::path& path) {
    if (!api) {
        libera::log::logError("Plugin: ", path.string(),
                              " returned a null API table");
        return false;
    }

    if (api->abi_version < minimumSupportedPluginApiVersion ||
        api->abi_version > LIBERA_PLUGIN_API_VERSION) {
        libera::log::logError("Plugin: ", path.string(),
                              " has API version ", api->abi_version,
                              ", expected ",
                              minimumSupportedPluginApiVersion,
                              "-",
                              LIBERA_PLUGIN_API_VERSION);
        return false;
    }

    if (!api->type_name || !*api->type_name) {
        libera::log::logError("Plugin: ", path.string(),
                              " did not provide a type_name");
        return false;
    }

    if (!api->display_name || !*api->display_name) {
        libera::log::logError("Plugin: ", path.string(),
                              " did not provide a display_name");
        return false;
    }

    if (!api->discover) {
        libera::log::logError("Plugin: ", path.string(),
                              " is missing discover()");
        return false;
    }

    if (!api->connect_controller) {
        libera::log::logError("Plugin: ", path.string(),
                              " is missing connect_controller()");
        return false;
    }

    if (!api->destroy_controller) {
        libera::log::logError("Plugin: ", path.string(),
                              " is missing destroy_controller()");
        return false;
    }

    const bool hasPointTransport = api->send_points != nullptr;

    bool hasFrameTransport = false;
    if (api->abi_version >= LIBERA_PLUGIN_API_VERSION_2) {
        const bool hasFrameRequirements = api->get_frame_requirements != nullptr;
        const bool hasFrameSender = api->send_frame != nullptr;
        if (hasFrameRequirements != hasFrameSender) {
            libera::log::logError(
                "Plugin: ",
                path.string(),
                " must provide both get_frame_requirements() and send_frame()");
            return false;
        }
        hasFrameTransport = hasFrameRequirements && hasFrameSender;
    }

    if (!hasPointTransport && !hasFrameTransport) {
        libera::log::logError(
            "Plugin: ",
            path.string(),
            " is missing send_points() or get_frame_requirements()+send_frame()");
        return false;
    }

    if (api->property_count > 0 && !api->properties) {
        libera::log::logError("Plugin: ", path.string(),
                              " declared properties without a property table");
        return false;
    }

    if (api->property_count > 0 && !api->read_property) {
        libera::log::logError("Plugin: ", path.string(),
                              " declared properties without read_property()");
        return false;
    }

    return true;
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

std::shared_ptr<LoadedPlugin> loadPlugin(const fs::path& path) {
    void* handle = openLibrary(path.string());
    if (!handle) {
        libera::log::logError("Plugin: failed to load ", path.string(),
                              ": ", libraryError());
        return nullptr;
    }

    auto getApi = resolveSymbol<decltype(&libera_plugin_get_api)>(
        handle, "libera_plugin_get_api");
    if (!getApi) {
        // Not a Libera plugin — silently skip things like vendor SDKs.
        closeLibrary(handle);
        return nullptr;
    }

    const libera_plugin_api_t* api = getApi();
    if (!validatePluginApi(api, path)) {
        closeLibrary(handle);
        return nullptr;
    }

    void* backendHandle = nullptr;
    if (api->create_backend) {
        backendHandle = api->create_backend(&kHostServices);
        if (!backendHandle) {
            libera::log::logError("Plugin: ", path.string(),
                                  " create_backend() failed");
            closeLibrary(handle);
            return nullptr;
        }
    }

    auto plugin = std::make_shared<LoadedPlugin>();
    plugin->libraryHandle = handle;
    plugin->api = api;
    plugin->backendHandle = backendHandle;
    plugin->typeName = api->type_name;
    plugin->displayName = api->display_name;
    plugin->initialised = true;

    libera::log::logInfo("Plugin: loaded \"", plugin->displayName,
                         "\" (type=",
                         plugin->typeName,
                         ", api=",
                         api->abi_version,
                         ", transport=",
                         pluginApiSupportsFrameTransport(api) ? "frame" : "point",
                         ") from ",
                         path.filename().string());
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

std::vector<std::unique_ptr<core::ControllerInfo>>
PluginDelegateManager::discover() {
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
    const auto activeSnapshot = liveControllers();

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
    return std::make_shared<PluginController>(
        plugin->api,
        plugin->backendHandle,
        info.pluginInfo());
}

PluginDelegateManager::NewControllerDisposition
PluginDelegateManager::prepareNewController(PluginController& controller,
                                            const PluginControllerInfo& info) {
    (void)info;
    if (!controller.open()) {
        return NewControllerDisposition::DropController;
    }
    controller.useFrameQueue();
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void PluginDelegateManager::afterCloseControllers() {
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
        if (entry.is_regular_file() && isSharedLibrary(entry.path())) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        auto plugin = loadPlugin(candidate);
        if (!plugin) {
            continue;
        }

        core::AddControllerManager([plugin]() {
            return std::make_unique<PluginDelegateManager>(plugin);
        });
    }
}

} // namespace libera::plugin
