#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::plugin {

class PluginControllerInfo : public core::ControllerInfo {
public:
    PluginControllerInfo(std::string id,
                         std::string label,
                         std::uint32_t maxPointRateValue,
                         std::string pluginTypeName)
    : ControllerInfo(std::move(id), std::move(label), maxPointRateValue)
    , pluginType(std::move(pluginTypeName)) {}

    const std::string& type() const override { return pluginType; }

private:
    std::string pluginType;
};

} // namespace libera::plugin
