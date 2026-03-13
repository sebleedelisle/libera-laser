#pragma once

#include "libera/core/GlobalDacManager.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/helios/HeliosController.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace libera::helios {

class HeliosManager : public core::DacManagerBase {
public:
    HeliosManager();
    ~HeliosManager() override;

    std::vector<std::unique_ptr<core::DacInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> getAndConnectToDac(const core::DacInfo& info) override;
    void closeAll() override;

    static inline core::DacManagerRegistry registrar{
        [] { return std::make_unique<HeliosManager>(); }
    };

private:
    static constexpr std::string_view typeName{"Helios"};

    void openIfNeeded();
    std::size_t refreshControllerCount(bool allowRescan);

    std::shared_ptr<HeliosDac> sdk;
    bool opened = false;
    std::size_t controllerCount = 0;

    std::mutex activeMutex;
    // Active controller wrappers are keyed by the persisted Helios device name,
    // not the transient SDK index.
    std::unordered_map<std::string, std::weak_ptr<HeliosController>> activeControllers;

    // Keep IDs/labels stable across transient SDK name-read failures so a
    // briefly unhealthy USB control channel doesn't churn device identity.
    std::unordered_map<unsigned int, std::string> stableIdByIndex;
    std::unordered_map<unsigned int, std::string> stableLabelByIndex;
};

} // namespace libera::helios
