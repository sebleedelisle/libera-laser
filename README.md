# libera-core

This repository implements reusable building blocks for streaming laser control data. The EtherDream integration highlights how runtime control, networking, and wire serialization are separated for clarity and testability.

## EtherDream Control Loop
- `EtherDreamDevice` owns the worker thread that polls device status, gathers points from user callbacks, and streams frames using the configuration in `libera/etherdream/EtherDreamConfig.hpp`.
- Every command uses `waitForResponse()` to synchronously await the EtherDream ACK and capture the decoded `dac_status` payload (buffer fullness, playback state, point rate, etc.).
- The streaming loop keeps at least `ETHERDREAM_MIN_PACKET_POINTS` points in flight and estimates how many additional samples are required based on the latest ACK’s buffer fullness.

## Networking Setup
- TCP connectivity is managed by `libera::net::TcpClient`; `EtherDreamDevice::connect()` resolves the configured port (`ETHERDREAM_DAC_PORT_DEFAULT`) and enables low-latency options (TCP_NODELAY + keepalive) as soon as the socket comes up.
- Networking calls share the same timeout budget as the streaming loop, so point refills and command/ACK sequences stay in lockstep.
- The worker loop starts with a protocol ping (`?`) to confirm the DAC is alive before entering the cadence-driven run loop, reusing the shared timeout policy.
- `waitForResponse()` blocks for the matching EtherDream ACK and returns the parsed status payload, so every command path picks up fresh device telemetry or surfaces transport errors immediately.

## Serialization Contract
- `EtherDreamCommand` constructs packets in the EtherDream wire format using a simple little-endian `ByteBuffer`. The streaming loop asks it to build `d`, `b`, and `q` messages as required.
- Coordinates and colour channels are scaled to the documented 16-bit ranges before serialisation, so the DAC receives full-scale values regardless of the user-space units.
- `EtherDreamResponse` decodes ACK frames and surfaces the parsed status fields, keeping the worker loop focused solely on scheduling and IO.
- Time-sensitive behaviour is tuned through `ETHERDREAM_MIN_PACKET_POINTS` and the sleep bounds (`ETHERDREAM_MIN_SLEEP` / `ETHERDREAM_MAX_SLEEP`), with the stream loop calculating how many samples to queue based on the last ACK’s buffer fullness and point rate.

## Coding Conventions
- Compile-time constants use ALL_CAPS snake case (for example `ETHERDREAM_MIN_POINTS_PER_TICK`) to make immutability obvious in hot code paths.
- File headers provide a brief summary of responsibilities and cross-component touch points to reduce orientation time for new contributors.
