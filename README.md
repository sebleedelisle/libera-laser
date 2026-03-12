# libera - cross-platform laser control

Libera aims to be a de facto standard for laser control, with support for a growing ecosystem of open-protocol hardware:
- Ether Dream
- Helios USB
- Helios Pro
- IDN (ILDA Digital Network)
- Laser Cube USB (LaserDock)
- Laser Cube Network
- AVB (LA Sollinger lasers)

The project uses a permissive license and is intended for broad adoption in laser control software.

## Overview

The library discovers laser controllers on the system and provides a list of available controllers. You can then instantiate one or more controllers and stream points in either streaming or frame mode.

## Include patterns

Default (all built-in DAC managers registered):

```cpp
#include "libera.h"
```

Selective (register only chosen managers):

```cpp
#include "libera/core/GlobalDacManager.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosManager.hpp"
```

### Streaming mode
Streaming mode is inspired by audio engines: you provide a callback, and the controller requests points when it needs more data. The callback receives:
- A minimum number of points required to keep playback smooth.
- A maximum number of points the controller can accept.
- An estimated timestamp for when the first point will be rendered.

### Frame mode
Frame mode is built into the `LaserController` base class. You can enqueue frames, query whether the controller is ready for another frame, and let the frame management system feed the streaming callback internally.

## Build

This project uses CMake presets. Typical flow:

```sh
cmake --preset debug
cmake --build --preset debug
```

Release build:

```sh
cmake --preset release
cmake --build --preset release
```

### Options

Examples and tests are off by default except for unit tests in debug:

```sh
cmake --preset debug -DLIBERA_BUILD_EXAMPLES=ON
cmake --build --preset debug
```

Other options:
- `LIBERA_BUILD_TESTS` (default ON for debug presets)
- `LIBERA_BUILD_INTEGRATION_TESTS` (default OFF)
- `LIBERA_BUILD_HARDWARE_TESTS` (default OFF)
- `LIBERA_BUILD_EXAMPLES` (default OFF)
- `LIBERA_USE_BUNDLED_LIBUSB` (default OFF)

### Tests

```sh
ctest --preset debug
```

Integration/hardware tests require enabling their options and may need actual controllers on the network.



## Coding Conventions
- Compile-time constants use ALL_CAPS snake case (for example `ETHERDREAM_MIN_POINTS_PER_TICK`) to make immutability obvious in hot code paths.
- File headers provide a brief summary of responsibilities and cross-component touch points to reduce orientation time for new contributors.

## Licence

Libera is licensed under the MIT License.

This means you are free to use it in commercial and non-commercial projects,
including proprietary software and hardware, as long as the copyright notice
and licence are included.

See LICENSE for details.
