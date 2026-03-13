#pragma once

#include "libera/System.hpp"
#include "libera/idn/IdnControllerInfo.hpp"
#include "libera/idn/IdnController.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace libera::idn {

class IdnManager : public core::ControllerManagerBase {
public:
    IdnManager();
    ~IdnManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static inline core::ControllerManagerRegistry registrar{
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
