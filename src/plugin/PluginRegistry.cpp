#include "libera/plugin/PluginRegistry.hpp"
#include "libera/plugin/libera_plugin.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace libera::plugin {

namespace fs = std::filesystem;

namespace {

void* openLibraryRaw(const std::string& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibraryRaw(void* handle) {
    if (!handle) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void* resolveSymbolRaw(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::string libraryErrorRaw() {
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

bool isSharedLibraryExt(const fs::path& path) {
    const auto ext = path.extension().string();
    return ext == ".dylib" || ext == ".so" || ext == ".dll";
}

} // namespace

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
    entry.info.filename = fs::path(path).filename().string();
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
                                  const std::string& typeName,
                                  const std::string& displayName) {
    std::lock_guard lock(mutex);
    auto& entry = entryLocked(path);
    entry.info.state = PluginState::Loaded;
    entry.info.typeName = typeName;
    entry.info.displayName = displayName;
    entry.info.loadError.reset();
}

void PluginRegistry::recordFailure(const std::string& path,
                                   PluginState state,
                                   const std::string& reason,
                                   const std::string& typeName,
                                   const std::string& displayName) {
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
    while (entry.errors.size() > kMaxRuntimeErrors) {
        entry.errors.pop_front();
    }
}

PluginInstallResult validatePluginFile(const std::string& sourcePath) {
    PluginInstallResult result;
    std::error_code ec;

    if (!fs::is_regular_file(sourcePath, ec)) {
        result.message = "Not a regular file";
        return result;
    }
    if (!isSharedLibraryExt(sourcePath)) {
        result.message = "File is not a shared library (.dylib/.so/.dll)";
        return result;
    }

    void* handle = openLibraryRaw(sourcePath);
    if (!handle) {
        result.message = "Failed to load: " + libraryErrorRaw();
        return result;
    }

    auto* getApiSym = resolveSymbolRaw(handle, "libera_plugin_get_api");
    if (!getApiSym) {
        closeLibraryRaw(handle);
        result.message = "Not a Libera plugin (missing libera_plugin_get_api)";
        return result;
    }

    using GetApiFn = const libera_plugin_api_t* (*)();
    auto getApi = reinterpret_cast<GetApiFn>(getApiSym);
    const libera_plugin_api_t* api = getApi();

    if (!api) {
        closeLibraryRaw(handle);
        result.message = "Plugin returned a null API table";
        return result;
    }
    if (api->abi_version != LIBERA_PLUGIN_API_VERSION) {
        result.message = "ABI version mismatch (plugin=" +
                         std::to_string(api->abi_version) +
                         ", host=" + std::to_string(LIBERA_PLUGIN_API_VERSION) + ")";
        closeLibraryRaw(handle);
        return result;
    }

    if (api->display_name && *api->display_name) {
        result.message = std::string("OK: ") + api->display_name;
    } else {
        result.message = "OK";
    }

    closeLibraryRaw(handle);
    result.success = true;
    return result;
}

PluginInstallResult installPluginFile(const std::string& sourcePath,
                                      const std::string& destDir) {
    PluginInstallResult validated = validatePluginFile(sourcePath);
    if (!validated.success) {
        return validated;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        return {false, {}, "Failed to create plugin directory: " + ec.message()};
    }

    fs::path destination = fs::path(destDir) / fs::path(sourcePath).filename();
    fs::copy_file(sourcePath,
                  destination,
                  fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
        return {false, {}, "Failed to copy plugin: " + ec.message()};
    }

    PluginInstallResult result;
    result.success = true;
    result.installedPath = destination.string();
    result.message = validated.message;
    return result;
}

bool removePluginFile(const std::string& path, std::string* error) {
    std::error_code ec;
    if (!fs::remove(path, ec)) {
        if (error) {
            *error = ec ? ec.message() : "File not found";
        }
        return false;
    }

    PluginRegistry::instance().forget(path);
    return true;
}

} // namespace libera::plugin
