#pragma once

#include "libera/System.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/helios/HeliosController.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct libusb_context;

namespace libera::helios {

class HeliosManager : public core::ControllerManagerBase {
public:
    HeliosManager();
    ~HeliosManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static core::ControllerManagerRegistry registrar;

private:
    static constexpr std::string_view typeName{"Helios"};

    struct ActiveControllerSnapshot {
        bool hasActive = false;
        // Track which physical USB DACs this process already owns.
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

    std::mutex activeMutex;
    // Active controller wrappers are keyed by the USB port path so one process
    // only ever claims the selected DAC.
    std::unordered_map<std::string, std::weak_ptr<HeliosController>> activeControllers;

    // Keep labels stable across transient direct USB name-read failures so a
    // briefly unhealthy control channel doesn't churn device identity.
    std::unordered_map<std::string, std::string> stableLabelByPortPath;
};

inline core::ControllerManagerRegistry HeliosManager::registrar{
    [] { return std::make_unique<HeliosManager>(); }
};

} // namespace libera::helios
