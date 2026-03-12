#pragma once

#include "libera/helios/HeliosController.hpp"

namespace libera::idn {

class IdnController : public helios::HeliosController {
public:
    explicit IdnController(std::shared_ptr<HeliosDac> sdk, unsigned int controllerIndex)
    : helios::HeliosController(std::move(sdk), controllerIndex) {}
};

} // namespace libera::idn
