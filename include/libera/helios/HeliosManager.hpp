#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/helios/HeliosController.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct libusb_context;

namespace libera::helios {

class HeliosManager
    : public core::ControllerManagerBase<HeliosControllerInfo,
                                         HeliosController> {
public:
    HeliosManager();
    ~HeliosManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    struct ActiveControllerSnapshot {
        bool hasActive = false;
        // Track which physical USB DAC transport paths this process already owns.
        //
        // Why this exists:
        // discovery probes intentionally interpret claim/open failures as
        // "busy elsewhere". Without this set, a Helios DAC that we ourselves
        // already connected would look externally busy during the next scan.
        std::unordered_set<std::string> connectedPortPaths;
    };

    ActiveControllerSnapshot snapshotActiveControllers();
    std::vector<HeliosControllerInfo> collectDiscoveredControllers(
        const ActiveControllerSnapshot& activeSnapshot);

    // Shared process-lifetime libusb context for Helios USB.
    //
    // We avoid explicit libusb_exit() during manager teardown because the Helios
    // USB path has historically been crash-prone around shutdown. Matching the
    // existing LaserCube USB strategy, we keep the context alive for the life of
    // the process and let the OS reclaim it on exit.
    std::shared_ptr<libusb_context> usbContext;

    // Keep labels stable across transient direct USB name-read failures so a
    // briefly unhealthy control channel doesn't churn device identity.
    std::unordered_map<std::string, std::string> stableLabelByPortPath;

    std::string controllerKey(const HeliosControllerInfo& info) const override;
    ControllerPtr createController(const HeliosControllerInfo& info) override;
    bool shouldReuseController(const HeliosController& controller,
                               const HeliosControllerInfo& info) const override;
    NewControllerDisposition prepareNewController(HeliosController& controller,
                                                  const HeliosControllerInfo& info) override;
    void prepareExistingController(HeliosController& controller,
                                   const HeliosControllerInfo& info) override;
    void closeController(const std::string& key, HeliosController& controller) override;
    void afterCloseControllers() override;
};

inline core::ControllerManagerRegistry HeliosManager::registrar{
    core::ControllerManagerRegistration{
        core::ControllerManagerInfo{
            std::string(HeliosControllerInfo::controllerType()),
            "Helios",
            "Helios USB controllers.",
        },
        [] { return std::make_unique<HeliosManager>(); },
    }
};

} // namespace libera::helios
