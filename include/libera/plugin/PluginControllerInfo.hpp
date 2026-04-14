#pragma once

#include "libera/System.hpp"
#include "libera/plugin/libera_plugin.h"

#include <string>
#include <utility>

namespace libera::plugin {

class PluginControllerInfo : public core::ControllerInfo {
public:
    PluginControllerInfo(const libera_controller_info_t& pluginInfo,
                         std::string pluginTypeName)
    : ControllerInfo(pluginInfo.id,
                     pluginInfo.label,
                     pluginInfo.max_point_rate,
                     makeNetworkInfo(pluginInfo))
    , info(pluginInfo)
    , pluginType(std::move(pluginTypeName)) {}

    const std::string& type() const override { return pluginType; }

    const libera_controller_info_t& pluginInfo() const { return info; }

private:
    static std::optional<core::ControllerInfo::NetworkInfo>
    makeNetworkInfo(const libera_controller_info_t& pluginInfo) {
        if (!pluginInfo.network.has_value) {
            return std::nullopt;
        }

        return core::ControllerInfo::NetworkInfo{
            pluginInfo.network.ip,
            pluginInfo.network.port
        };
    }

    libera_controller_info_t info{};
    std::string pluginType;
};

} // namespace libera::plugin
