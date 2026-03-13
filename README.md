# libera - cross-platform laser control

Libera aims to be a de facto standard for laser control, with support for a growing ecosystem of open-protocol hardware:
- Ether Dream
- Helios USB
- Helios Pro
- IDN (ILDA Digital Network)
- Laser Cube USB (LaserDock)
- Laser Cube Network (for Wifi laser cubes - wired network strongly recommended!)
- AVB (LA Sollinger lasers) (coming soon)

The project uses a permissive license and is intended for broad adoption in laser control software.

## Overview

The library discovers laser controllers on the system and provides a list of available controllers. You can then instantiate one or more controllers and stream points in either streaming or frame mode.

## Include patterns

Default (all built-in controller managers registered):

```cpp
#include "libera.h"
```

Selective (register only chosen managers):

```cpp
#include "libera/System.hpp"
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

### Controller health/status
Every controller now exposes a shared status/error API:
- `getStatus()` returns `Good`, `Issues`, or `Error`.
- `getErrors()` returns `{code, label, count}` entries.
- `clearErrors()` resets error counters and intermittent warning state.

Error type keys are shared by transport class:
- Network controllers use `network.*` types (for example `network.timeout`, `network.packet_loss`, `network.buffer_underflow`).
- USB controllers use `usb.*` types (for example `usb.timeout`, `usb.transfer_failed`, `usb.connection_lost`).

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

## A note on terminology

Throughout this library (and in Liberation) I generally avoid the term **“DAC”** (Digital-to-Analogue Converter), which is the traditional name for a laser control interface.

Today, most laser controllers do indeed perform digital-to-analogue conversion in order to drive the analogue ILDA interface used by scanning lasers. However, the role of these devices is broader than simply converting signals - they also implement protocols, manage streaming, and translate data between software and hardware.

Looking forward, this distinction will become even more relevant as laser systems increasingly move towards fully digital communication between controllers and drivers. In those cases there may be no analogue conversion at all, and the device functions purely as a controller and protocol bridge.

For this reason Libera generally refers to these devices as **controllers** rather than DACs.

Similarly, I avoid the term **“projector”** when referring to scanning show lasers. The term has become ambiguous as pixel-based video projectors with laser light sources are now widely marketed as “laser projectors”. In Libera and Liberation these devices are simply referred to as **lasers**.

## Licence

Libera is licensed under the MIT License.

This means you are free to use it in commercial and non-commercial projects,
including proprietary software and hardware, as long as the copyright notice
and licence are included.

See LICENSE for details.
