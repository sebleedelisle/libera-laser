#pragma once

// Default public umbrella include:
// - pulls in the core API
// - registers all built-in controller managers via their static registrars
//
// For selective registration, include `libera/System.hpp`
// and only the specific `*Manager.hpp` headers you want.

#include "libera/System.hpp"
#include "libera/avb/AvbManager.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosManager.hpp"
#include "libera/idn/IdnManager.hpp"
#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"
