#include "libera/System.hpp"

#if LIBERA_ENABLE_PLUGINS
#include "libera/plugin/PluginManager.hpp"
#include <cstdlib>
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

std::string& pluginDirStorage() {
    static std::string dir = [] {
        const char* env = std::getenv("LIBERA_PLUGIN_DIR");
        return env ? std::string(env) : std::string("plugins");
    }();
    return dir;
}

} // anonymous namespace

void System::setPluginDirectory(const std::string& path) {
    pluginDirStorage() = path;
}

const std::string& System::pluginDirectory() {
    return pluginDirStorage();
}

#else

void System::setPluginDirectory(const std::string&) {}

const std::string& System::pluginDirectory() {
    static const std::string empty;
    return empty;
}

#endif

System::System() {
#if LIBERA_ENABLE_PLUGINS
    const auto& dir = pluginDirectory();
    if (!dir.empty()) {
        plugin::loadPluginsFromDirectory(dir);
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
