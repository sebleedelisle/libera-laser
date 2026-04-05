#include "libera/System.hpp"

#if LIBERA_ENABLE_PLUGINS
#include "libera/plugin/PluginManager.hpp"
#include <cstdlib>
#include <filesystem>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

namespace libera::core {

std::vector<ControllerManagerFactory>& getControllerManagerFactories() {
    static std::vector<ControllerManagerFactory> factories;
    return factories;
}

ControllerManagerRegistry::ControllerManagerRegistry(ControllerManagerFactory factory) {
    getControllerManagerFactories().push_back(std::move(factory));
}

void AddControllerManager(ControllerManagerFactory factory) {
    getControllerManagerFactories().push_back(std::move(factory));
}

} // namespace libera::core

namespace libera {

#if LIBERA_ENABLE_PLUGINS

namespace {

std::filesystem::path executableDirectory() {
    namespace fs = std::filesystem;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto canonical = fs::weakly_canonical(fs::path(buf), ec);
    return canonical.parent_path();
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf).parent_path();
#else
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return exe.parent_path();
#endif
}

std::string resolvePluginDirectory(const std::string& requested) {
    namespace fs = std::filesystem;
    if (requested.empty()) return requested;

    fs::path p(requested);
    std::error_code ec;

    // Absolute paths are used as-is.
    if (p.is_absolute()) return requested;

    // Try the executable's directory first — gives a self-contained bundle
    // that works regardless of the caller's current working directory.
    auto exeRelative = executableDirectory() / p;
    if (fs::is_directory(exeRelative, ec)) {
        return exeRelative.string();
    }

    // Fall back to the original (cwd-relative) path so users can still
    // override by launching from a directory that contains `plugins/`.
    return requested;
}

std::vector<std::string>& pluginDirStorage() {
    static std::vector<std::string> dirs = [] {
        const char* env = std::getenv("LIBERA_PLUGIN_DIR");
        return std::vector<std::string>{ env ? std::string(env) : std::string("plugins") };
    }();
    return dirs;
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

System::System() {
#if LIBERA_ENABLE_PLUGINS
    for (const auto& dir : pluginDirectories()) {
        if (dir.empty()) continue;
        plugin::loadPluginsFromDirectory(resolvePluginDirectory(dir));
    }
#endif

    for (const auto& factory : core::getControllerManagerFactories()) {
        if (!factory) continue;
        auto manager = factory();
        if (!manager) continue;
        auto type = std::string(manager->managedType());
        managerByType[type] = manager.get();
        managers.emplace_back(std::move(manager));
    }
}

System::~System() {
    shutdown();
}

std::vector<std::unique_ptr<core::ControllerInfo>> System::discoverControllers() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    for (auto& manager : managers) {
        if (!manager) continue;
        auto subset = manager->discover();
        results.reserve(results.size() + subset.size());
        for (auto& item : subset) {
            results.emplace_back(std::move(item));
        }
    }
    return results;
}

std::shared_ptr<core::LaserController>
System::connectController(const core::ControllerInfo& info) {
    auto it = managerByType.find(info.type());
    if (it == managerByType.end() || !it->second) {
        return nullptr;
    }
    return it->second->connectController(info);
}

void System::shutdown() {
    if (shutdownComplete) {
        return;
    }

    for (auto it = managers.rbegin(); it != managers.rend(); ++it) {
        if (*it) {
            (*it)->closeAll();
        }
    }
    managerByType.clear();
    managers.clear();
    shutdownComplete = true;
}

} // namespace libera
