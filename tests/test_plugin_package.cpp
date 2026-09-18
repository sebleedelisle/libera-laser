#include "libera/plugin/PluginPackage.hpp"
#include "libera/plugin/libera_plugin.h"

#include <miniz.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace libera::plugin;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { ++g_failures; } } while (0)

namespace {

namespace fs = std::filesystem;

struct Entry {
    std::string path;
    std::string contents;
};

fs::path uniqueTempDirectory() {
    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    return fs::temp_directory_path() /
           ("libera-plugin-package-test-" + std::to_string(suffix));
}

std::string testOs() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string testArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string libraryExtension(const std::string& os) {
    if (os == "windows") return ".dll";
    if (os == "macos") return ".dylib";
    return ".so";
}

std::string defaultEntrypoint() {
    return "bin/plugin" + libraryExtension(testOs());
}

std::string manifest(const std::string& entrypoint = {},
                     const std::string& pluginId = "com.example.test-plugin",
                     const std::string& version = "1.2.3",
                     const std::string& additionalEntrypoints = {}) {
    const std::string selectedEntrypoint = entrypoint.empty()
        ? defaultEntrypoint()
        : entrypoint;
    return std::string("{") +
        "\"schemaVersion\":1,"
        "\"id\":\"" + pluginId + "\","
        "\"version\":\"" + version + "\","
        "\"name\":\"Test Plugin\","
        "\"vendor\":\"Example\","
        "\"description\":\"Package parser fixture\","
        "\"controllerType\":\"TestController\","
        "\"libera\":{\"abiVersion\":" +
        std::to_string(LIBERA_PLUGIN_API_VERSION) + "},"
        "\"entrypoints\":[{\"os\":\"" + testOs() +
        "\",\"arch\":\"" + testArch() +
        "\",\"path\":\"" + selectedEntrypoint + "\"}" +
        additionalEntrypoints + "],"
        "\"documents\":{\"readme\":\"docs/README.md\","
        "\"license\":\"docs/LICENSE.txt\"}"
        "}";
}

bool writePackage(const fs::path& path, const std::vector<Entry>& entries) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0)) {
        return false;
    }
    for (const auto& entry : entries) {
        if (!mz_zip_writer_add_mem(&archive,
                                   entry.path.c_str(),
                                   entry.contents.data(),
                                   entry.contents.size(),
                                   MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return false;
        }
    }
    const bool finalized = mz_zip_writer_finalize_archive(&archive) != 0;
    const bool ended = mz_zip_writer_end(&archive) != 0;
    return finalized && ended;
}

std::vector<Entry> validEntries() {
    return {
        {"manifest.json", manifest()},
        {defaultEntrypoint(), "not-a-real-library"},
        {"docs/README.md", "# Test plugin\n"},
        {"docs/LICENSE.txt", "Test licence\n"},
    };
}

void testValidPackage(const fs::path& root) {
    const fs::path package = root / "valid.liberaplugin";
    ASSERT_TRUE(writePackage(package, validEntries()),
                "test package should be created");

    const auto inspected = inspectPluginPackage(package.string());
    ASSERT_TRUE(inspected.success, "valid package should pass inspection");
    ASSERT_TRUE(inspected.manifest.id == "com.example.test-plugin",
                "manifest package ID should be parsed");
    ASSERT_TRUE(inspected.manifest.version == "1.2.3",
                "manifest version should be parsed");
    ASSERT_TRUE(inspected.selectedEntrypoint == defaultEntrypoint(),
                "current platform entrypoint should be selected");
    ASSERT_TRUE(inspected.packageSha256.size() == 64,
                "package should receive a SHA-256 content identity");

    PluginPackageInspection extractedInspection;
    std::string error;
    const fs::path extracted = root / "extracted";
    ASSERT_TRUE(extractPluginPackage(package.string(),
                                     extracted.string(),
                                     &extractedInspection,
                                     &error),
                "valid package should extract");
    ASSERT_TRUE(fs::is_regular_file(extracted / "manifest.json"),
                "manifest should be extracted");
    ASSERT_TRUE(fs::is_regular_file(extracted / "docs" / "README.md"),
                "package documents should be extracted");
}

void testTraversalRejected(const fs::path& root) {
    auto entries = validEntries();
    entries.push_back({"../escape.txt", "escape"});
    const fs::path package = root / "traversal.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "traversal fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "parent traversal should be rejected");
}

void testCaseCollisionRejected(const fs::path& root) {
    auto entries = validEntries();
    entries.push_back({"DOCS/readme.md", "collision"});
    const fs::path package = root / "collision.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "collision fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "case-insensitive path collisions should be rejected");
}

void testMissingEntrypointRejected(const fs::path& root) {
    auto entries = validEntries();
    entries[0].contents = manifest(
        "bin/missing" + libraryExtension(testOs()));
    const fs::path package = root / "missing-entrypoint.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "missing entrypoint fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "manifest entrypoint must exist in the archive");
}

void testMissingUnselectedEntrypointRejected(const fs::path& root) {
    auto entries = validEntries();
    const std::string otherOs = testOs() == "windows" ? "linux" : "windows";
    entries[0].contents = manifest(
        {},
        "com.example.test-plugin",
        "1.2.3",
        ",{\"os\":\"" + otherOs +
            "\",\"arch\":\"x86_64\",\"path\":\"bin/missing" +
            libraryExtension(otherOs) + "\"}");
    const fs::path package = root / "missing-other-entrypoint.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "cross-platform fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "every manifest entrypoint must exist in the archive");
}

void testManifestIdentityRules(const fs::path& root) {
    auto entries = validEntries();
    entries[0].contents = manifest({}, "libera.builtin.fake");
    const fs::path reserved = root / "reserved-id.liberaplugin";
    ASSERT_TRUE(writePackage(reserved, entries),
                "reserved ID fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(reserved.string()).success,
                "plugins must not impersonate built-in driver IDs");

    entries = validEntries();
    entries[0].contents = manifest({}, "com.example.test-plugin", "1.2.3-01");
    const fs::path invalidVersion = root / "invalid-version.liberaplugin";
    ASSERT_TRUE(writePackage(invalidVersion, entries),
                "invalid version fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(invalidVersion.string()).success,
                "semantic versions must reject numeric prerelease leading zeros");

    entries = validEntries();
    entries[0].contents = manifest("bin/plugin.txt");
    entries.push_back({"bin/plugin.txt", "not-a-real-library"});
    const fs::path wrongExtension = root / "wrong-extension.liberaplugin";
    ASSERT_TRUE(writePackage(wrongExtension, entries),
                "wrong extension fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(wrongExtension.string()).success,
                "entrypoint extensions must match their operating system");
}

void testFileDirectoryCollisionRejected(const fs::path& root) {
    auto entries = validEntries();
    entries.push_back({"resources", "ordinary file"});
    entries.push_back({"resources/device.json", "{}"});
    const fs::path package = root / "file-directory-collision.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "file/directory collision fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "a file must not also act as a parent directory");
}

void testInstallerMetadataCollisionRejected(const fs::path& root) {
    auto entries = validEntries();
    entries.push_back({"archive.liberaplugin", "conflicting package data"});
    const fs::path package = root / "installer-metadata-collision.liberaplugin";
    ASSERT_TRUE(writePackage(package, entries),
                "installer metadata collision fixture should be created");
    ASSERT_TRUE(!inspectPluginPackage(package.string()).success,
                "package content must not replace the preserved source archive");
}

} // namespace

int main() {
    const fs::path root = uniqueTempDirectory();
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_TRUE(!ec, "temporary test directory should be created");

    testValidPackage(root);
    testTraversalRejected(root);
    testCaseCollisionRejected(root);
    testMissingEntrypointRejected(root);
    testMissingUnselectedEntrypointRejected(root);
    testManifestIdentityRules(root);
    testFileDirectoryCollisionRejected(root);
    testInstallerMetadataCollisionRejected(root);

    fs::remove_all(root, ec);
    return g_failures == 0 ? 0 : 1;
}
