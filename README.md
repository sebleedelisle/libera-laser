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

The library discovers laser controllers on the system and provides a list of available devices. You can then instantiate one or more devices and stream points in either streaming or frame mode.

### Streaming mode
Streaming mode is inspired by audio engines: you provide a callback, and the device requests points when it needs more data. The callback receives:
- A minimum number of points required to keep playback smooth.
- A maximum number of points the device can accept.
- An estimated timestamp for when the first point will be rendered.

### Frame mode
Frame mode is built into the `LaserDevice` base class. You can enqueue frames, query whether the device is ready for another frame, and let the frame management system feed the streaming callback internally.



## Coding Conventions
- Compile-time constants use ALL_CAPS snake case (for example `ETHERDREAM_MIN_POINTS_PER_TICK`) to make immutability obvious in hot code paths.
- File headers provide a brief summary of responsibilities and cross-component touch points to reduce orientation time for new contributors.
