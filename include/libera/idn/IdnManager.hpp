#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/idn/IdnControllerInfo.hpp"
#include "libera/idn/IdnController.hpp"

#include <memory>
#include <unordered_map>

namespace libera::idn {

class IdnManager
    : public core::ControllerManagerBase<IdnControllerInfo,
                                         IdnController> {
public:
    IdnManager();
    ~IdnManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    void openIfNeeded();
    std::size_t refreshControllerCount(bool allowRescan);

    std::shared_ptr<HeliosDac> sdk;
    bool opened = false;
    std::size_t controllerCount = 0;
    std::unordered_map<unsigned int, std::string> stableUnitIdByIndex;

    std::string controllerKey(const IdnControllerInfo& info) const override;
    ControllerPtr createController(const IdnControllerInfo& info) override;
    NewControllerDisposition prepareNewController(IdnController& controller,
                                                  const IdnControllerInfo& info) override;
    void prepareExistingController(IdnController& controller,
                                   const IdnControllerInfo& info) override;
    void closeController(const std::string& key, IdnController& controller) override;
    void afterCloseControllers() override;
};

inline core::ControllerManagerRegistry IdnManager::registrar{
    [] { return std::make_unique<IdnManager>(); }
};

} // namespace libera::idn
