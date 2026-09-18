#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libera::plugin {

struct PluginEntrypoint {
    std::string os;
    std::string arch;
    std::string path;
};

struct PluginDocuments {
    std::optional<std::string> readme;
    std::optional<std::string> license;
};

struct PluginManifest {
    std::uint32_t schemaVersion = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string vendor;
    std::string description;
    std::string controllerType;
    std::uint32_t abiVersion = 0;
    std::vector<PluginEntrypoint> entrypoints;
    PluginDocuments documents;
};

struct PluginPackageInspection {
    bool success = false;
    PluginManifest manifest;
    std::string selectedEntrypoint;
    std::string packageSha256;
    std::string message;
};

/* Inspect and fully validate a .liberaplugin ZIP without loading native code. */
PluginPackageInspection inspectPluginPackage(const std::string& packagePath);

/*
 * Extract a validated package into an empty destination directory.
 * The archive is inspected again immediately before extraction so callers do
 * not need to retain parser state across an installation transaction.
 */
bool extractPluginPackage(const std::string& packagePath,
                          const std::string& destinationDirectory,
                          PluginPackageInspection* inspection,
                          std::string* error = nullptr);

/* File-picker extension without a leading dot. */
const char* pluginPackageExtension();

} // namespace libera::plugin
