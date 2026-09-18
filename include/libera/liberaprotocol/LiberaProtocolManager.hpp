#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/liberaprotocol/LiberaProtocolController.hpp"
#include "libera/liberaprotocol/LiberaProtocolControllerInfo.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace libera::liberaprotocol {

class LiberaProtocolManager
    : public core::ControllerManagerBase<LiberaProtocolControllerInfo,
                                         LiberaProtocolController> {
public:
    LiberaProtocolManager();
    ~LiberaProtocolManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    ControllerPtr createController(const LiberaProtocolControllerInfo& info) override;
    NewControllerDisposition prepareNewController(LiberaProtocolController& controller,
                                                  const LiberaProtocolControllerInfo& info) override;
    void prepareExistingController(LiberaProtocolController& controller,
                                   const LiberaProtocolControllerInfo& info) override;
    void closeController(const std::string& key, LiberaProtocolController& controller) override;
};

inline core::ControllerManagerRegistry LiberaProtocolManager::registrar{
    core::ControllerManagerRegistration{
        core::ControllerManagerInfo{
            "libera.builtin.libera-protocol",
            std::string(LiberaProtocolControllerInfo::controllerType()),
            "Libera Protocol",
            "Libera protocol receivers discovered by UDP advertisement.",
        },
        [] { return std::make_unique<LiberaProtocolManager>(); },
    }
};

} // namespace libera::liberaprotocol
