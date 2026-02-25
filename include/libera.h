#pragma once

// Default public umbrella include:
// - pulls in the core API
// - registers all built-in DAC managers via their static registrars
//
// For selective registration, include `libera/core/GlobalDacManager.hpp`
// and only the specific `*Manager.hpp` headers you want.

#include "libera/core/GlobalDacManager.hpp"
#include "libera/core/LaserDevice.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosManager.hpp"
#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"
