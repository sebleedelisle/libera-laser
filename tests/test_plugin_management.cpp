#include "libera/System.hpp"
#include "libera/log/Log.hpp"
#include "libera/plugin/PluginController.hpp"
#include "libera/plugin/PluginManagement.hpp"
#include "libera/plugin/PluginRegistry.hpp"
#include "libera/plugin/PluginSettings.hpp"
#include "libera/plugin/libera_plugin.h"

#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef TEST_VALID_PLUGIN_PATH
#error "TEST_VALID_PLUGIN_PATH must point at the valid plugin fixture"
#endif

#ifndef TEST_MISSING_TRANSPORT_PLUGIN_PATH
#error "TEST_MISSING_TRANSPORT_PLUGIN_PATH must point at the invalid plugin fixture"
#endif

using namespace libera;
using namespace libera::plugin;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_STRING_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT STRING EQ FAILED", (msg), "lhs", _va, "rhs", _vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

std::filesystem::path uniqueTempDirectory();

const char* fixtureOs() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* fixtureArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "x86";
#endif
}

std::string makeFixturePackage(const std::string& binaryPath,
                               const std::string& pluginId,
                               const std::string& controllerType,
                               const std::string& displayName) {
    namespace fs = std::filesystem;
    const fs::path directory = uniqueTempDirectory();
    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path package = directory / (pluginId + ".liberaplugin");
    const std::string entrypoint = std::string("bin/") + fixtureOs() + "/" +
        fixtureArch() + "/plugin" + fs::path(binaryPath).extension().string();
    const std::string manifest =
        "{\"schemaVersion\":1,\"id\":\"" + pluginId +
        "\",\"version\":\"1.0.0\",\"name\":\"" + displayName +
        "\",\"vendor\":\"Libera Tests\",\"description\":\"Fixture\","
        "\"controllerType\":\"" + controllerType +
        "\",\"libera\":{\"abiVersion\":" +
        std::to_string(LIBERA_PLUGIN_API_VERSION) + "},\"entrypoints\":[{"
        "\"os\":\"" + fixtureOs() + "\",\"arch\":\"" + fixtureArch() +
        "\",\"path\":\"" + entrypoint + "\"}]}";

    mz_zip_archive zip{};
    ASSERT_TRUE(mz_zip_writer_init_file(&zip, package.string().c_str(), 0),
                "fixture ZIP should open");
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip,
                                      "manifest.json",
                                      manifest.data(),
                                      manifest.size(),
                                      MZ_BEST_COMPRESSION),
                "fixture manifest should be added");
    ASSERT_TRUE(mz_zip_writer_add_file(&zip,
                                       entrypoint.c_str(),
                                       binaryPath.c_str(),
                                       nullptr,
                                       0,
                                       MZ_BEST_COMPRESSION),
                "fixture entrypoint should be added");
    ASSERT_TRUE(mz_zip_writer_finalize_archive(&zip),
                "fixture ZIP should finalize");
    mz_zip_writer_end(&zip);
    return package.string();
}

const std::string& validPackagePath() {
    static const std::string path = makeFixturePackage(
        TEST_VALID_PLUGIN_PATH,
        "org.libera.test-valid",
        "TestValidPlugin",
        "Test Valid Plugin");
    return path;
}

const std::string& missingTransportPackagePath() {
    static const std::string path = makeFixturePackage(
        TEST_MISSING_TRANSPORT_PLUGIN_PATH,
        "org.libera.test-missing-transport",
        "TestMissingTransportPlugin",
        "Test Missing Transport Plugin");
    return path;
}

std::string normalizedPathString(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }

    auto canonical = fs::weakly_canonical(absolute, ec);
    if (!ec) {
        return canonical.string();
    }

    return absolute.lexically_normal().string();
}

std::filesystem::path uniqueTempDirectory() {
    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    return std::filesystem::temp_directory_path() /
           ("libera-plugin-management-test-" + std::to_string(suffix));
}

bool containsText(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

const ManagedPluginInfo* findManagedPlugin(
    const std::vector<ManagedPluginInfo>& plugins,
    const std::string& path) {
    const std::string normalized = normalizedPathString(path);
    auto it = std::find_if(plugins.begin(), plugins.end(),
                           [&](const ManagedPluginInfo& info) {
                               return info.path == normalized;
                           });
    return it == plugins.end() ? nullptr : &*it;
}

const Setting* findSetting(const std::vector<Setting>& settings,
                           const std::string& key) {
    const auto it = std::find_if(settings.begin(), settings.end(),
                                 [&](const Setting& setting) {
                                     return setting.definition.key == key;
                                 });
    return it == settings.end() ? nullptr : &*it;
}

bool waitForProperty(PluginController& controller,
                     const std::string& key,
                     const std::string& expected,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto value = controller.getProperty(key);
        if (value && *value == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void testValidationUsesRuntimeRules() {
    const auto valid = validatePluginPackageForInstall(validPackagePath());
    ASSERT_TRUE(valid.success, "valid plugin should pass validation");
    ASSERT_TRUE(containsText(valid.message, "Test Valid Plugin"),
                "valid plugin message should include display name");

    const auto structurallyValid =
        validatePluginPackageForInstall(missingTransportPackagePath());
    ASSERT_TRUE(structurallyValid.success,
                "package validation should not execute incoming native code");

    const auto pluginDir = uniqueTempDirectory();
    System::setPluginDirectory(pluginDir.string());
    const auto install = installPlugin(missingTransportPackagePath());
    ASSERT_TRUE(install.success, "invalid native fixture package should install safely");
    System system;
    const auto plugins = listManagedPlugins();
    const auto failed = std::find_if(plugins.begin(), plugins.end(), [](const auto& info) {
        return info.pluginId == "org.libera.test-missing-transport";
    });
    ASSERT_TRUE(failed != plugins.end(), "failed native fixture should be listed");
    if (failed != plugins.end()) {
        ASSERT_TRUE(failed->state == ManagedPluginState::FailedValidation,
                    "missing transport should be rejected at restart/load");
        ASSERT_TRUE(failed->loadError && containsText(
                        *failed->loadError,
                        "missing send_points() or get_frame_requirements()+send_frame()"),
                    "deferred ABI validation should explain the missing transport");
    }
    system.shutdown();
}

void testPluginAndControllerSettings() {
    namespace fs = std::filesystem;

    const fs::path pluginDir = uniqueTempDirectory();
    std::error_code ec;
    fs::create_directories(pluginDir, ec);
    ASSERT_TRUE(!ec, "settings test plugin directory should be created");

    System::setPluginDirectory(pluginDir.string());
    const auto install = installPlugin(validPackagePath());
    ASSERT_TRUE(install.success, "settings fixture should install");

    {
        System system;

        const auto declaredPluginSettings =
            pluginSettings("org.libera.test-valid");
        ASSERT_TRUE(declaredPluginSettings.size() == 1,
                    "fixture should expose one plugin setting");
        if (!declaredPluginSettings.empty()) {
            ASSERT_STRING_EQ(declaredPluginSettings.front().value,
                             "standard",
                             "plugin setting should initially use its default");
        }

        auto discovered = system.discoverControllers();
        ASSERT_TRUE(discovered.size() == 1,
                    "fixture should discover one controller");
        if (discovered.empty()) {
            return;
        }
        ASSERT_STRING_EQ(discovered.front()->labelValue(),
                         "Test Valid Controller",
                         "default plugin setting should be applied before discovery");

        const auto offlineChange = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "gain", "8");
        ASSERT_TRUE(offlineChange.success,
                    "offline controller setting should be persisted");

        auto connectedBase = system.connectController(*discovered.front());
        auto controller =
            std::dynamic_pointer_cast<PluginController>(connectedBase);
        ASSERT_TRUE(controller != nullptr,
                    "fixture should connect through PluginController");
        if (!controller) {
            return;
        }

        const auto restoredGain = controller->getProperty("gain");
        ASSERT_TRUE(restoredGain.has_value(),
                    "fixture should report its applied controller gain");
        if (restoredGain) {
            ASSERT_STRING_EQ(*restoredGain,
                             "8",
                             "offline setting should apply before streaming starts");
        }

        const auto pluginChange = setPluginSetting(
            "org.libera.test-valid", "discovery_label", "alternate");
        ASSERT_TRUE(pluginChange.success,
                    "live plugin setting should be accepted");

        // One rescan came from the first discovery, one from the offline
        // controller change, and one must be triggered by the plugin change.
        const auto rescanCount = controller->getProperty("rescans");
        ASSERT_TRUE(rescanCount.has_value(),
                    "fixture should expose its rescan count");
        if (rescanCount) {
            ASSERT_STRING_EQ(*rescanCount,
                             "3",
                             "each successful setting change should trigger rescan");
        }

        discovered = system.discoverControllers();
        ASSERT_TRUE(!discovered.empty(),
                    "controller should remain discoverable after a live setting change");
        if (!discovered.empty()) {
            ASSERT_STRING_EQ(discovered.front()->labelValue(),
                             "Test Alternate Controller",
                             "live plugin setting should affect later discovery");
        }

        const auto liveControllerChange = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "gain", "9");
        ASSERT_TRUE(liveControllerChange.success,
                    "live controller setting should be accepted");
        const auto liveGain = controller->getProperty("gain");
        ASSERT_TRUE(liveGain.has_value(),
                    "live controller gain should remain readable");
        if (liveGain) {
            ASSERT_STRING_EQ(*liveGain,
                             "9",
                             "live controller setting should reach its opaque handle");
        }

        const auto disconnectRequest =
            controller->getProperty("request_disconnect");
        ASSERT_TRUE(disconnectRequest && *disconnectRequest == "requested",
                    "fixture should accept its test-only disconnect request");
        ASSERT_TRUE(waitForProperty(*controller,
                                    "connections",
                                    "2",
                                    std::chrono::seconds(2)),
                    "controller should reconnect with a new opaque handle");
        ASSERT_TRUE(waitForProperty(*controller,
                                    "gain",
                                    "9",
                                    std::chrono::seconds(2)),
                    "persisted controller setting should be restored after reconnect");

        const auto invalidChange = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "gain", "11");
        ASSERT_TRUE(!invalidChange.success,
                    "host should reject a controller value above its maximum");

        const auto invalidBool = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "invert_x", "1");
        ASSERT_TRUE(!invalidBool.success,
                    "host should require canonical boolean values");
        const auto validBool = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "invert_x", "true");
        ASSERT_TRUE(validBool.success,
                    "host should accept a canonical boolean value");

        const auto invalidFloat = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "scale", "2.5");
        ASSERT_TRUE(!invalidFloat.success,
                    "host should enforce floating-point bounds");
        const auto validFloat = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "scale", "1.25");
        ASSERT_TRUE(validFloat.success,
                    "host should accept a bounded floating-point value");

        const auto stringChange = setControllerSetting(
            "org.libera.test-valid", "test-valid-001", "nickname", "Test laser");
        ASSERT_TRUE(stringChange.success,
                    "host should accept a string setting");

        const auto savedControllerSettings = controllerSettings(
            "org.libera.test-valid", "test-valid-001");
        ASSERT_TRUE(savedControllerSettings.size() == 4,
                    "fixture should expose all primitive controller settings");
        const auto* savedGain = findSetting(savedControllerSettings, "gain");
        ASSERT_TRUE(savedGain != nullptr,
                    "fixture should still expose its integer gain");
        if (savedGain) {
            ASSERT_STRING_EQ(savedGain->value,
                             "9",
                             "rejected values should not replace persisted settings");
        }

        controller.reset();
        connectedBase.reset();
        system.shutdown();
    }

    ASSERT_TRUE(fs::is_regular_file(pluginSettingsFilePath(), ec),
                "successful changes should create the shared settings file");

    // Switch the configured path away and back so the store must reload from
    // disk rather than satisfying this check from its in-memory map.
    System::setPluginDirectory((pluginDir / "unused").string());
    (void)pluginSettings("org.libera.test-valid");
    System::setPluginDirectory(pluginDir.string());

    {
        System restartedSystem;
        auto discovered = restartedSystem.discoverControllers();
        ASSERT_TRUE(!discovered.empty(),
                    "fixture should remain registered in a restarted System");
        if (!discovered.empty()) {
            ASSERT_STRING_EQ(discovered.front()->labelValue(),
                             "Test Alternate Controller",
                             "plugin setting should reload from the shared file");

            auto connectedBase =
                restartedSystem.connectController(*discovered.front());
            auto controller =
                std::dynamic_pointer_cast<PluginController>(connectedBase);
            ASSERT_TRUE(controller != nullptr,
                        "fixture should reconnect after settings reload");
            if (controller) {
                const auto gain = controller->getProperty("gain");
                ASSERT_TRUE(gain.has_value(),
                            "reconnected fixture should expose gain");
                if (gain) {
                    ASSERT_STRING_EQ(*gain,
                                     "9",
                                     "controller setting should reload from disk");
                }
            }
            controller.reset();
            connectedBase.reset();
        }
        restartedSystem.shutdown();
    }

    // A cooperating Libera process may update the shared file after this
    // process cached it. Mutations must merge the latest on-disk state while
    // holding the interprocess store lock.
    {
        std::ofstream output(pluginSettingsFilePath(), std::ios::trunc);
        output << "{\"schemaVersion\":1,\"values\":[{"
                  "\"pluginId\":\"org.example.other\","
                  "\"controllerId\":\"\",\"key\":\"mode\","
                  "\"value\":\"external\"}]}\n";
    }
    const auto mergedChange = setPluginSetting(
        "org.libera.test-valid", "discovery_label", "standard");
    ASSERT_TRUE(mergedChange.success,
                "setting mutation should merge current shared file contents");
    {
        std::ifstream input(pluginSettingsFilePath());
        const std::string merged((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
        ASSERT_TRUE(containsText(merged, "org.example.other"),
                    "setting mutation should retain another process's values");
    }

    {
        std::ofstream output(pluginSettingsFilePath(), std::ios::trunc);
        output << "{\"schemaVersion\":1,\"values\":[]} trailing-data\n";
    }
    const auto malformedChange = setPluginSetting(
        "org.libera.test-valid", "discovery_label", "alternate");
    ASSERT_TRUE(!malformedChange.success,
                "setting mutation should refuse a malformed shared file");
    {
        std::ifstream input(pluginSettingsFilePath());
        const std::string preserved((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
        ASSERT_TRUE(containsText(preserved, "trailing-data"),
                    "failed setting mutation should preserve malformed data");
    }

#ifndef _WIN32
    // POSIX permits removal of a loaded shared-library file. Windows keeps the
    // test DLL locked until process exit, so its temporary directory is left
    // for the operating system's normal temp cleanup there.
    fs::remove_all(pluginDir, ec);
#endif
}

void testManagedPluginInstallListAndRemove() {
    namespace fs = std::filesystem;

    const fs::path pluginDir = uniqueTempDirectory();
    std::error_code ec;
    fs::remove_all(pluginDir, ec);
    fs::create_directories(pluginDir, ec);
    ASSERT_TRUE(!ec, "test plugin directory should be created");

    System::setPluginDirectory(pluginDir.string());
    ASSERT_STRING_EQ(userPluginDirectory(),
                     pluginDir.string(),
                     "management should use the configured user plugin directory");

    auto externalRemoval = removePlugin(validPackagePath());
    ASSERT_TRUE(!externalRemoval.success,
                "removePlugin should reject files outside the user plugin directory");

    auto install = installPlugin(validPackagePath());
    ASSERT_TRUE(install.success, "valid plugin should install");
    ASSERT_TRUE(containsText(normalizedPathString(install.installedPath),
                             normalizedPathString(pluginDir)),
                "plugin should install into the configured user store");

    const auto reinstall = installPlugin(validPackagePath());
    ASSERT_TRUE(reinstall.success,
                "installing a plugin already in the user directory should validate without copy failure");
    ASSERT_TRUE(reinstall.restartRequired,
                "a package not examined at startup should still require restart");
    ASSERT_STRING_EQ(normalizedPathString(reinstall.installedPath),
                     normalizedPathString(install.installedPath),
                     "same package revision should be reused");

    auto plugins = listManagedPlugins();
    const ManagedPluginInfo* pending =
        findManagedPlugin(plugins, install.installedPath);
    ASSERT_TRUE(pending != nullptr, "installed plugin should be listed");
    if (pending) {
        ASSERT_TRUE(pending->state == ManagedPluginState::PendingRestart,
                    "newly-installed plugin should require restart before load");
        ASSERT_TRUE(pending->source == ManagedPluginSource::UserPluginDirectory,
                    "newly-installed plugin should be from the user plugin directory");
        ASSERT_TRUE(pending->fileExists, "newly-installed plugin should exist");
        ASSERT_TRUE(pending->canRemove, "newly-installed plugin should be removable");
        ASSERT_TRUE(pending->restartRequired,
                    "newly-installed plugin should require restart");
    }

    std::string selectionError;
    ASSERT_TRUE(selectControllerDriver("TestValidPlugin",
                                       "org.libera.test-valid",
                                       &selectionError),
                "driver selection should persist in the package store");
    const auto selections = selectedControllerDrivers();
    const auto selected = selections.find("TestValidPlugin");
    ASSERT_TRUE(selected != selections.end() &&
                selected->second == "org.libera.test-valid",
                "persisted driver selection should round-trip");

    const auto pendingRemoval = removePlugin(install.installedPath);
    ASSERT_TRUE(pendingRemoval.success,
                "pending plugin file should be removable before restart");
    ASSERT_TRUE(!pendingRemoval.restartRequired,
                "removing a never-loaded pending plugin should not require restart");
    const auto removedSelections = selectedControllerDrivers();
    ASSERT_TRUE(removedSelections.find("TestValidPlugin") ==
                    removedSelections.end(),
                "removing a plugin should clear driver selections that name it");

    install = installPlugin(validPackagePath());
    ASSERT_TRUE(install.success, "valid plugin should reinstall after pending removal");

    PluginRegistry::instance().recordLoaded(normalizedPathString(install.installedPath),
                                            "org.libera.test-valid",
                                            "1.0.0",
                                            "TestValidPlugin",
                                            "Test Valid Plugin");

    const auto activeReinstall = installPlugin(validPackagePath());
    ASSERT_TRUE(activeReinstall.success,
                "reinstalling the active revision should succeed");
    ASSERT_TRUE(!activeReinstall.restartRequired,
                "reinstalling an already examined revision should not request restart");

    plugins = listManagedPlugins();
    const ManagedPluginInfo* loaded =
        findManagedPlugin(plugins, install.installedPath);
    ASSERT_TRUE(loaded != nullptr, "loaded plugin should be listed");
    if (loaded) {
        ASSERT_TRUE(loaded->state == ManagedPluginState::Loaded,
                    "registry-loaded plugin should be reported as loaded");
        ASSERT_TRUE(!loaded->restartRequired,
                    "loaded plugin should not require restart while its file exists");
        ASSERT_TRUE(loaded->canRemove,
                    "loaded user plugin file should still be removable");
    }

    const auto removal = removePlugin(install.installedPath);
    ASSERT_TRUE(removal.success, "installed plugin should be removed");
    ASSERT_TRUE(removal.restartRequired,
                "removing a plugin should require restart to unload native code");

    plugins = listManagedPlugins();
    const ManagedPluginInfo* removed =
        findManagedPlugin(plugins, install.installedPath);
    ASSERT_TRUE(removed != nullptr,
                "removed loaded plugin should remain visible until restart");
    if (removed) {
        ASSERT_TRUE(removed->state == ManagedPluginState::RemovedPendingRestart,
                    "removed loaded plugin should be marked pending restart");
        ASSERT_TRUE(!removed->fileExists, "removed plugin file should be gone");
        ASSERT_TRUE(!removed->canRemove,
                    "removed plugin should not offer another remove action");
        ASSERT_TRUE(removed->restartRequired,
                    "removed loaded plugin should require restart");
    }

    fs::remove_all(pluginDir, ec);
}

void testMalformedStateIsNotOverwritten() {
    namespace fs = std::filesystem;

    const fs::path pluginDir = uniqueTempDirectory();
    std::error_code ec;
    fs::create_directories(pluginDir, ec);
    ASSERT_TRUE(!ec, "state test plugin directory should be created");
    const fs::path statePath = pluginDir / "state.json";
    {
        std::ofstream output(statePath);
        output << "{\"schemaVersion\":1,\"activePackages\":[],"
                  "\"controllerDrivers\":{}}\n";
    }

    System::setPluginDirectory(pluginDir.string());
    const auto install = installPlugin(validPackagePath());
    ASSERT_TRUE(!install.success,
                "install should refuse a malformed existing state file");
    std::ifstream input(statePath);
    const std::string preserved((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
    ASSERT_TRUE(containsText(preserved, "\"activePackages\":[]"),
                "failed mutation should preserve the malformed state file");

    fs::remove_all(pluginDir, ec);
}

} // namespace

int main() {
    testValidationUsesRuntimeRules();
    testPluginAndControllerSettings();
    testManagedPluginInstallListAndRemove();
    testMalformedStateIsNotOverwritten();
    return g_failures == 0 ? 0 : 1;
}
