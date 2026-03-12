#pragma once

#include "libera/core/GlobalDacManager.hpp"
#include "libera/idn/IdnControllerInfo.hpp"
#include "libera/idn/IdnController.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace libera::idn {

class IdnManager : public core::DacManagerBase {
public:
    IdnManager();
    ~IdnManager() override;

    std::vector<std::unique_ptr<core::DacInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> getAndConnectToDac(const core::DacInfo& info) override;
    void closeAll() override;

    static inline core::DacManagerRegistry registrar{
        [] { return std::make_unique<IdnManager>(); }
    };

private:
    static constexpr std::string_view typeName{"IDN"};

    void openIfNeeded();
    std::size_t refreshControllerCount(bool allowRescan);

    std::shared_ptr<HeliosDac> sdk;
    bool opened = false;
    std::size_t controllerCount = 0;

    std::mutex activeMutex;
    std::unordered_map<unsigned int, std::weak_ptr<IdnController>> activeControllers;
};

} // namespace libera::idn
