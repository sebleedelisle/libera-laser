#pragma once

#include "libera/plugin/PluginPackage.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace libera::plugin {

struct InstalledPluginRevision {
    PluginManifest manifest;
    std::string revision;
    std::string packageRoot;
    std::string entrypointPath;
    std::string archivePath;
    std::string packageSha256;
};

std::vector<InstalledPluginRevision> activePluginRevisions(
    const std::string& storeRoot);

std::unordered_map<std::string, std::string> controllerDriverSelections(
    const std::string& storeRoot);

} // namespace libera::plugin
