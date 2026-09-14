# Libera protocol requirements

This is a working draft for a first-party Libera network protocol. The goal is
to create a new standard for laser control over a network. 

## Goals

- A DAC or ingestion endpoint can advertise itself on the local network.
- Discovery uses UDP; the active control/data connection uses TCP.
- The active TCP connection carries a bidirectional stream of typed records.
- The sender can use the same stream as a true point stream or as a sequence of
  explicit source frames.
- Frame boundaries are records in the stream, not inferred from point geometry.
- A sender can ask the receiver to repeat the last accepted frame a specific
  number of times.
- Scanner sync is not baked into point samples. It is a separate receiver-side
  value so the ingestion end can offset colours against scanner position.
- Each frame can carry a target begin time.
- The DAC can send status, telemetry, and future hardware data back to the
  sender without changing the core point stream.
- The protocol has a reserved custom-message path so manufacturers can add
  private controls such as dichro adjustment without consuming standard message
  types.
- The base point format uses Ether Dream style granularity: signed 16-bit X/Y
  and unsigned 16-bit colour/intensity channels.
- User channels are negotiated during handshake. The protocol should support at
  least `u1`, `u2`, and `u3`; the exact channel count is not fixed globally.
- The wire format is versioned and starts with a handshake.

## Non-goals for the first version

- Hard real-time delivery over an unreliable network.
- Vendor-specific hardware management beyond identity, status, streaming, and
  basic capability negotiation.
- Compression. TCP message framing should be simple first.
- Freezing unreleased wire layouts. Until a release is tagged, fields, enum
  values, payload layouts, and negotiation rules can change freely. Senders and
  receivers should use the same `libera-protocol` revision.

## Terminology

- **Ingest endpoint**: the receiver. This is usually a DAC, a hardware bridge,
  or a Libera Link target that accepts incoming laser content.
- **Sender**: the producer of laser content. This may be Libera itself, Libera
  Link, a renderer, or another application.
- **Session**: one TCP connection after successful handshake.
- **Record**: one typed message inside the TCP stream.
- **Frame**: one logical source frame delimited by explicit frame-boundary
  records.
- **Point stream**: ordered point samples sent inside a session. Frame markers
  split the stream into frames; they are not guessed from geometry.
- **Session time**: receiver-defined monotonic time used for target begin times.

## Discovery requirements

Discovery should be UDP broadcast or multicast on a reserved Libera protocol
port.

Each ingest endpoint should periodically advertise:

- protocol magic and major/minor version
- stable device ID
- human-readable name
- TCP host/port for sessions
- endpoint type, such as `dac`, `bridge`, or `virtual`
- current availability, such as `available`, `busy`, `disabled`, or `fault`
- supported point-rate range
- maximum frame point count
- supported user-channel count range
- whether target begin times are supported
- whether repeat-last-frame commands are supported
- whether receiver-side scanner sync is supported
- advertised telemetry classes, such as temperature, shutter, interlock, and
  manufacturer-defined status
- manufacturer ID, if the endpoint supports private manufacturer messages

Senders should also be able to send a UDP query and receive an immediate unicast
advertisement. Periodic advertisements alone are too slow for manual refreshes
and UI discovery.

Discovery availability is advisory. It helps sender UIs avoid offering an
endpoint that is already owned, but TCP session negotiation is authoritative.
If two senders race an `available` advertisement, the receiver must accept only
one session and reject the other with a busy reason.

## TCP session requirements

The TCP stream should be length-prefixed binary records. Proposed convention:

- all multi-byte fields use network byte order
- the initial handshake carries protocol magic and version negotiation
- after handshake, each record starts with a compact fixed header:
  - 2 bytes record type
  - 2 bytes record flags
  - 4 bytes payload length
  - 4 bytes sequence number
- payload schemas are record-type specific

Both sides must reject malformed records and payloads that exceed negotiated
limits. Unsupported protocol versions should be rejected during discovery or
handshake rather than rediscovered on every record.

The session should include:

- handshake
- status/ack records
- ping/pong keepalive
- stream control records
- point records
- frame-boundary records
- telemetry records
- custom extension records
- orderly close

The first version should define broad record-type ranges so future additions do
not need to overload existing meanings:

- `0x0000..0x00ff`: session, handshake, ack, error, ping/pong, close
- `0x0100..0x01ff`: point stream and frame-boundary records
- `0x0200..0x02ff`: playback, timing, scanner-sync, and flow-control records
- `0x0300..0x03ff`: status and telemetry records
- `0x0400..0x04ff`: standard hardware-control records
- `0x7000..0x7fff`: experimental Libera records
- `0x8000..0xffff`: manufacturer/private extension records

## MVP wire format

The current MVP wire format is intentionally small and fixed-width where
possible. All multi-byte integer fields are unsigned big-endian unless the
field says otherwise. Variable strings are UTF-8 byte strings with explicit
16-bit byte lengths; they are not null-terminated.

Constants:

```text
PROTOCOL_MAGIC          uint32  0x4c494250  ("LIBP")
PROTOCOL_VERSION_MAJOR  uint8   0
PROTOCOL_VERSION_MINOR  uint8   1
DEFAULT_DISCOVERY_PORT  uint16  45425
DEFAULT_SESSION_PORT    uint16  45426
```

Every TCP record uses this 12-byte header:

```text
uint16  record_type
uint16  record_flags
uint32  payload_length
uint32  sequence_number
uint8   payload[payload_length]
```

Defined MVP record types:

```text
0x0001  HELLO
0x0002  ACCEPT
0x0003  REJECT
0x0004  READY
0x0005  ACK
0x0006  ERROR
0x0007  PING
0x0008  PONG
0x0009  CLOSE
0x0101  POINTS
0x0102  FRAME_MARKER
0x0201  STREAM_CONFIG
0x0202  SET_SCANNER_SYNC
0x0301  STATUS
0x8000  MANUFACTURER_PRIVATE
```

Defined MVP stream modes:

```text
0  raw point stream
1  raw point stream with optional markers
2  marked point stream
3  frame by count
4  frame by next marker
```

Defined MVP session feature flags:

```text
bit 0  target begin time
bit 1  repeat last frame
bit 2  scanner sync
bit 3  status
bit 4  manufacturer private records
```

### UDP discovery advertisement

Discovery advertisements are UDP datagrams. The current payload is:

```text
uint32  protocol_magic
uint8   version_major
uint8   version_minor
uint16  tcp_port
uint16  supported_stream_mode_mask
uint8   max_user_channel_count
uint8   availability
uint32  min_point_rate
uint32  max_point_rate
uint32  max_frame_point_count
uint32  feature_flags
uint16  endpoint_id_length
uint16  display_name_length
uint16  endpoint_type_length
uint16  address_length
uint8   endpoint_id[endpoint_id_length]
uint8   display_name[display_name_length]
uint8   endpoint_type[endpoint_type_length]
uint8   address[address_length]
```

`supported_stream_mode_mask` uses `1 << stream_mode`.

`availability` values:

```text
0  available
1  busy
2  disabled
3  fault
```

Availability is advisory because UDP can be stale. A sender may use it to grey
out endpoints in the UI, but TCP handshake rejection is authoritative.

If `address` is empty, the sender should use the source address of the UDP
datagram. `endpoint_id + address + tcp_port` identifies one advertised endpoint
for discovery de-duplication.

### HELLO

The sender opens TCP and sends `HELLO` first:

```text
uint32  protocol_magic
uint8   version_major
uint8   version_minor
uint16  requested_stream_mode
uint8   requested_user_channel_count
uint8   reserved
uint32  default_point_rate
uint16  sender_name_length
uint8   sender_name[sender_name_length]
```

### ACCEPT

The receiver accepts a session with:

```text
uint16  accepted_stream_mode
uint8   accepted_user_channel_count
uint8   reserved
uint32  default_point_rate
uint32  max_point_rate
uint32  max_frame_point_count
uint32  max_record_payload_size
uint64  session_id
uint32  feature_flags
```

After `ACCEPT`, the sender must send `READY` before data records.

### REJECT

The receiver rejects a session with:

```text
uint16  reject_code
uint16  message_length
uint8   message[message_length]
```

Defined MVP reject codes:

```text
1  unsupported version
2  busy
3  malformed HELLO
4  unsupported mode
5  internal error
```

`REJECT_BUSY` means a `REJECT` record with reject code `2`. It is not a
separate record type.

### STREAM_CONFIG

```text
uint32  default_point_rate
uint16  stream_mode
uint8   user_channel_count
uint8   reserved
uint16  flags
```

`STREAM_CONFIG` may update the active stream mode, point rate, and user-channel
count after handshake, within the receiver's accepted limits.

### FRAME_MARKER

```text
uint64  frame_id
uint64  target_begin_time_ns
uint32  point_rate
uint32  frame_point_count
uint32  flags
```

`target_begin_time_ns` is receiver session time. `0` means schedule as soon as
possible in order. `point_rate == 0` means use the active stream default.

In `frame by count` mode, `frame_point_count` must be non-zero and the receiver
commits the frame once that many points have arrived. In `frame by next marker`
mode, `frame_point_count` may be zero and the next marker closes the previous
frame.

### POINTS

The `POINTS` payload contains packed point samples. There is no point-count
field; the count is `payload_length / sample_size`.

```text
int16   x
int16   y
uint16  r
uint16  g
uint16  b
uint16  i
uint16  u[accepted_user_channel_count]
```

`sample_size = 12 + 2 * accepted_user_channel_count`.

### SET_SCANNER_SYNC

```text
int64   offset_ns
uint8   enabled
uint8   reserved[3]
```

`offset_ns` is signed nanoseconds. Positive values delay colour samples relative
to geometry. The receiver applies this after decoding points and before sending
to hardware.

### STATUS

```text
uint16  code
uint16  reserved
uint32  queued_points
uint32  queued_frames
uint32  feature_flags
uint16  message_length
uint8   message[message_length]
```

The MVP status record is deliberately small. Rich telemetry remains a future
extension.

### PING, PONG, CLOSE

`PING` and `PONG` carry one `uint64` timestamp payload. `CLOSE` has no payload.

## Handshake requirements

The sender opens TCP and sends `HELLO`.

`HELLO` should include:

- sender name and optional application ID
- supported protocol version range
- requested point format
- requested user-channel count
- requested stream mode
- desired point-rate range or default point rate
- desired maximum in-flight frames
- whether the sender will use target begin times
- whether the sender will use repeat-last-frame
- whether the sender wants receiver-side scanner sync
- requested telemetry subscriptions
- supported standard hardware-control groups
- supported manufacturer IDs for private extension records

The ingest endpoint responds with `ACCEPT` or `REJECT`.

`ACCEPT` should include:

- chosen protocol version
- accepted point format
- accepted user-channel count
- accepted stream mode
- maximum message size
- maximum frame point count
- maximum in-flight frames
- point-rate range
- session ID
- receiver clock information for session time
- scanner-sync limits and current scanner-sync value
- initial status
- accepted telemetry subscriptions
- accepted standard hardware-control groups
- accepted manufacturer/private extension capabilities

After `ACCEPT`, the sender sends `READY`. Data messages should not be accepted
before `READY`.

If an endpoint already has an exclusive active session, the ingest endpoint
should respond to `HELLO` with `REJECT` code `busy` and close the connection.
Rejected connections must not reset or otherwise disturb the active session.

## Endpoint ownership and multiple streams

The MVP does not multiplex multiple laser streams inside one TCP connection.
Each advertised ingest endpoint represents one controllable output target and
has its own TCP session endpoint.

For multiple DACs behind one computer, the bridge should advertise multiple
endpoints. They may share the same IP address, but they must be distinguishable
by endpoint ID and usually by TCP port:

```text
LL - Helios A  192.168.1.50:45426  available
LL - Helios B  192.168.1.50:45427  busy
LL - Helios C  192.168.1.50:45428  available
```

The sender treats these as separate controllers. It opens one TCP session per
selected endpoint.

For one DAC endpoint, the MVP ownership rule is exclusive:

```text
first accepted sender      -> ACCEPT, endpoint becomes busy
second simultaneous sender -> REJECT busy
accepted sender disconnects -> endpoint becomes available
```

There is no per-record stream ID. Multi-client mixing, handoff, priority, and
shared ownership are deferred protocol features. Until those exist, a receiver
must not accept two active data sessions for the same endpoint.

## Point format

The base point format should be compact and fixed after handshake:

```text
int16  x
int16  y
uint16 r
uint16 g
uint16 b
uint16 i
uint16 u[accepted_user_channel_count]
```

Coordinate mapping:

- `x` and `y` are signed scanner coordinates.
- `-32768` means full negative deflection.
- `0` means centre.
- `32767` means full positive deflection.

Channel mapping:

- `r`, `g`, `b`, `i`, and `uN` are unsigned channels.
- `0` means off/minimum.
- `65535` means full scale.

The first protocol version should avoid per-point control flags. Frame structure
belongs in boundary records, not inside every point.

## Point stream and frame boundaries

The protocol should be a point stream first. A sender can continuously write
`POINTS` records. When the negotiated stream mode uses source-frame boundaries,
the sender writes `FRAME_MARKER` records into the same stream. Pure streaming
mode does not require marker records and should not pay for them.

Pure point stream:

```text
STREAM_CONFIG(default_point_rate, flags)
POINTS(point_count, point_payload)
POINTS(point_count, point_payload)
POINTS(point_count, point_payload)
```

Marked point stream:

```text
STREAM_CONFIG(default_point_rate, flags)
FRAME_MARKER(frame_id, target_begin_time_ns, point_rate, flags[, frame_point_count])
POINTS(point_count, point_payload)
POINTS(point_count, point_payload)
FRAME_MARKER(next_frame_id, next_target_begin_time_ns, point_rate, flags[, frame_point_count])
POINTS(point_count, point_payload)
```

Requirements:

- `POINTS` carries only point samples in the negotiated point format.
- `FRAME_MARKER` is optional protocol metadata, not hidden point data.
- In pure point-stream mode the sender sends `POINTS` records without
  `FRAME_MARKER`.
- In marked/frame-aware modes, the sender sends `FRAME_MARKER` at each known
  source-frame boundary.
- `FRAME_MARKER` closes the previous marked frame, if any, and starts the next
  marked frame.
- `FRAME_MARKER` applies to the next point after the marker.
- Frame-mode capability is negotiated in the handshake. It is not repeated in
  every point or packet.
- The protocol does not need to send a version number on every record after the
  handshake has selected the session version.
- A frame point count is not part of every `POINTS` record. It is either omitted
  entirely and the frame closes at the next `FRAME_MARKER`, or it is sent once
  in the marker so the receiver knows the frame is complete as soon as that many
  points have arrived.
- `target_begin_time_ns` is in receiver session time. Zero means "as soon as the
  receiver can schedule it in order".
- `point_rate` may be zero to use the current stream default.
- `frame_id` is monotonically increasing within the session.
- A receiver with limited memory may stream points directly to hardware and use
  markers only for timing, status, repeat-cache, or frame-aware behaviour.
- A receiver with enough memory may assemble complete marked frames and feed a
  frame-ingester path.
- In frame-capable modes, the receiver must not submit a marked frame to a
  frame-ingester path until the whole frame has been received.

This model keeps true point-stream DACs viable while still giving frame-aware
receivers exact boundaries.

## Stream modes

The handshake should negotiate one of these receiver behaviours:

- **raw point stream**: the receiver accepts `POINTS` without requiring
  `FRAME_MARKER`. This is the lowest-overhead mode for DACs with limited memory
  or senders that do not know source-frame boundaries.
- **raw point stream with optional markers**: the receiver streams points
  directly, but the sender may include `FRAME_MARKER` as metadata when
  available.
- **marked point stream**: the receiver expects frame markers. If it is acting
  as a true point-stream DAC, it may output points as they arrive. If it is
  acting as a frame-capable receiver, it must wait for the complete marked frame
  before submitting it onward.
- **commit-by-count frames**: the receiver buffers points after a marker until
  the marker's frame point count is reached, then commits that frame.
- **commit-by-next-marker frames**: the receiver buffers points after a marker
  until the next marker closes the previous frame.

The sender should choose the strongest mode the receiver supports and its memory
budget allows.

## Repeat-last-frame

The sender should be able to avoid resending identical points:

```text
REPEAT_LAST_FRAME(target_begin_time_ns, repeat_count, flags)
```

Requirements:

- The command repeats the last fully accepted frame.
- `repeat_count` is the number of additional plays requested.
- For true point-stream receivers, this requires the receiver to have cached the
  last marked frame. If it has not, it must reject the command.
- A special maximum value may mean "repeat until replaced"; this must be
  negotiated and acknowledged explicitly.
- The receiver should acknowledge whether the repeat was accepted, queued, or
  rejected because no previous frame exists.
- The repeat should preserve the last frame's point data and point rate unless
  an override field is added and negotiated.

## Scanner-sync control

Scanner sync is a control value applied by the ingest endpoint after decoding
points and before sending to hardware.

`SET_SCANNER_SYNC` payload:

```text
int64  offset_ns
uint8  enabled
uint8  reserved[3]
```

Requirements:

- The offset is signed and expressed in nanoseconds, not points.
- The receiver converts the time offset to a point delay using the active frame
  or stream point rate.
- Positive values delay colour samples relative to geometry by `offset_ns`.
- Receivers that cannot apply negative offsets should clamp them to zero.
- The receiver reports the accepted/clamped value in status.
- Frames should not contain pre-shifted colour samples when scanner sync is
  active. The point stream should stay geometrically and chromatically aligned
  at source.

Libera currently stores scanner sync internally as a colour delay value. The
protocol should expose it as time so it is independent of point rate.

## Receiver-to-sender data

The protocol should be bidirectional. The receiver must be able to send status
and future hardware data back on the same TCP session.

Standard telemetry records should cover common DAC and projector state:

- playback state
- interlock and emergency-stop state
- shutter state
- output-enable state
- laser source temperature, when available
- scanner or amplifier temperature, when available
- supply voltage or fault state, when available
- fan speed or thermal throttling state, when available
- device-specific warning/fault codes

Telemetry should be subscription based. The sender requests classes of telemetry
during handshake or with a later subscription record, and the receiver sends:

- an immediate snapshot after subscription
- event records when values change materially
- periodic records for values that are useful as trends, such as temperature

Telemetry records must be optional. A minimal DAC should be able to implement
only basic status and still speak the protocol.

## Custom and manufacturer messages

The protocol should reserve a standard extension path rather than forcing
future hardware controls into point fields.

Two extension mechanisms are useful:

- **standard extension records** for controls that Libera may eventually define,
  such as dichro adjustment, calibration banks, projector profiles, or safety
  metadata.
- **manufacturer/private records** for vendor-specific data. This should be
  similar in spirit to MIDI SysEx: the standard defines the envelope, the
  manufacturer owns the private payload.

Manufacturer/private records should include:

```text
manufacturer_id
device_family_or_model
private_message_type
flags
payload
```

Requirements:

- Manufacturer/private records must be negotiated in the handshake.
- Unknown manufacturer/private records must be ignored or rejected cleanly; they
  must never alter point-stream parsing.
- The receiver should ack private control records with accepted/rejected/error.
- Private telemetry records may be sent only after the sender subscribes to that
  manufacturer capability.
- Standard record types must not be consumed for one-off vendor controls.

Possible future standard hardware-control groups:

- dichro adjustment
- colour calibration matrix
- source power limits
- scan geometry calibration
- safety zones and masks
- test pattern request
- persistent device naming

## Timing requirements

Each frame can carry a target begin time. This requires the sender to understand
the receiver's session time.

The first version should use a simple receiver-clock model:

- The receiver defines session time as monotonic nanoseconds from its own epoch.
- The handshake exposes enough clock data for the sender to estimate receiver
  time.
- Optional `TIME_SYNC` messages can refine clock offset during a session.
- The receiver decides what to do with late frames: play immediately, drop, or
  report late. The default should be play immediately and report lateness.

The protocol should not require wall-clock synchronization for basic operation.

## Status and flow control

The receiver should periodically send status and also reply to commands with
acks.

Status should include:

- playback state
- accepted frame ID
- currently scheduled frame ID
- last completed frame ID
- queued frame count
- queued point count or estimated buffered duration
- underrun count
- late-frame count
- current point rate
- current scanner-sync value
- available frame slots
- receiver session time
- active stream mode
- receiver memory pressure or point-buffer pressure
- last manufacturer/private error, if any

Flow control should be explicit:

- The handshake negotiates maximum in-flight frames and maximum message size.
- The handshake negotiates maximum buffered points and maximum cached frame
  points separately. This matters for DACs that can stream points but cannot
  retain whole frames.
- The sender should not exceed the receiver's advertised queue capacity.
- The receiver can reject frames when limits are exceeded.
- `REPEAT_LAST_FRAME` should count against queue capacity in time, but not
  require another point payload.

## Implementation notes

This protocol maps naturally onto Libera's frame-first path.

## MVP implementation

The MVP should prove one end-to-end path:

```text
Libera sender
    -> Libera protocol over TCP
    -> Libera Link virtual controller host
    -> linked Libera controller
```

The MVP should be deliberately small.

Protocol scope:

- UDP discovery with query/reply and periodic advertisements.
- One TCP client session per advertised endpoint.
- Endpoint availability in discovery and authoritative busy rejection during
  TCP handshake.
- Handshake with protocol version, point format, stream mode, point-rate limits,
  maximum message size, and maximum frame point count.
- Compact post-handshake record header with no per-record protocol version.
- Fixed MVP point format:
  - signed 16-bit `x`
  - signed 16-bit `y`
  - unsigned 16-bit `r`, `g`, `b`, `i`
  - negotiated user channels, initially `0..2` to match current `LaserPoint`
- Stream modes:
  - raw point stream
  - frame-marker mode with complete-frame delivery
- Records:
  - `HELLO`
  - `ACCEPT`
  - `REJECT`
  - `READY`
  - `STREAM_CONFIG`
  - `POINTS`
  - `FRAME_MARKER`
  - `STATUS`
  - `ACK`
  - `ERROR`
  - `PING`
  - `PONG`
  - `CLOSE`
- `SET_SCANNER_SYNC` if the receiver advertises scanner-sync support.
- A reserved manufacturer/private envelope that can be parsed and cleanly
  rejected, but no real manufacturer controls yet.

Deferred from MVP:

- `u3+` in Libera core point storage.
- Repeat-last-frame optimization.
- Rich telemetry such as laser temperature.
- Dichro adjustment or other hardware-control records.
- Authentication or encryption.
- Multi-client sessions for one endpoint.
- Compression.
- Low-latency partial-frame output in frame mode.

Libera-laser MVP work:

- Add a shared protocol module for constants, record structs, byte-order helpers,
  encoding, decoding, and validation.
- Add tests for handshake negotiation, record framing, point encode/decode,
  malformed payload rejection, and frame-marker assembly.
- Add a `LiberaProtocolManager` and controller backend.
- The backend should discover UDP advertisements, connect over TCP, handshake,
  and choose the best stream mode.
- If the receiver accepts frame-marker mode, the backend should call
  `requestFrame(...)`, send one `FRAME_MARKER`, then send that frame's `POINTS`.
- If the receiver accepts only raw point stream, the backend should call
  `requestPoints(...)` and send only `POINTS`.

Libera Link MVP work:

- Add a `LiberaProtocolVirtualControllerHost`.
- Advertise one endpoint per linked target over UDP.
- Accept one TCP client per endpoint.
- Negotiate raw point-stream or frame-marker mode during handshake.
- In raw point-stream mode, decode `POINTS` and submit them through the existing
  continuous target-sink path.
- In frame-marker mode, buffer points for the current marked frame and submit
  only complete frames.
- Add or adjust the target sink so frame-marker mode can use a real frame
  submission path instead of relying on point-stream boundary guessing.
- Send simple `STATUS` records with queue depth, underrun count, point rate,
  selected stream mode, and last accepted frame ID.

The MVP success test is: a Libera app can see a Libera Link advertised endpoint,
connect to it, negotiate frame-marker mode, send moving-start multi-shape
frames, and the linked target receives exact complete frames without
`PointStreamFramer` guessing.

## Repository and dependency layout

The protocol should live in its own public repository, but it should not become
a submodule of both `libera-laser` and `libera-link`.

Recommended layout:

```text
laser-dev/
  libera-protocol/
  libera-laser/
    libs/libera-protocol/      # submodule pointing at ../libera-protocol origin
  libera-link/
    extern/libera-laser/       # existing submodule
```

`libera-link` should consume the protocol through its existing
`extern/libera-laser` dependency. After `libera-link` calls
`add_subdirectory(extern/libera-laser)`, the `libera-protocol` CMake target is
available to Link as a normal target. Link can link its virtual controller host
against that target directly.

This keeps one checked-out protocol copy in a normal recursive checkout:

```text
libera-link
  -> extern/libera-laser
       -> libs/libera-protocol
```

It also avoids two different protocol revisions being present in one Link build.
If both `libera-link` and `libera-laser` had their own submodule pointers, they
could drift and produce duplicate CMake targets, duplicate symbols, or subtly
different wire-format code in the sender and receiver.

The standalone `libera-protocol` repository should avoid depending on
`libera-laser`. It should provide protocol data types, encode/decode logic,
discovery/session code, and callback-style sender/receiver primitives. The
Libera-specific mapping belongs in the consuming projects:

- `libera-laser` maps `LaserPoint` and controller scheduling into protocol
  sender calls.
- `libera-link` maps protocol receiver callbacks into target-sink submissions.

Suggested `libera-protocol` targets:

- `libera-protocol-core`: record types, constants, byte-order helpers,
  validation, point encode/decode, frame-marker assembly.
- `libera-protocol-net`: UDP discovery and TCP session client/server built on a
  small network dependency such as standalone Asio.
- `libera-protocol-tests`: codec, negotiation, parser, and loopback tests.

Alternative layouts:

- Use CMake `FetchContent` in both projects instead of a submodule. This avoids
  git submodule friction, but it makes offline/local development worse and needs
  careful target de-duplication when Link also builds `libera-laser`.
- Put the protocol inside `libera-laser` first and split it later. This is the
  least ceremony, but it works against the goal of a public, reusable protocol
  repo.
- Add the protocol as a top-level sibling only and require developers to set a
  CMake path. This is useful for development overrides, but not enough for a
  reliable fresh checkout.

Sender side:

- For Libera-backed senders that have exact frames, use `requestFrame(...)` and
  write a `FRAME_MARKER` before the frame's first point.
- For true streaming sources, write `POINTS` continuously and add
  `FRAME_MARKER` only when the source exposes a boundary.
- Encode `libera::core::LaserPoint` to the negotiated point format.
- Use `REPEAT_LAST_FRAME` when the frame content is unchanged.
- Subscribe to receiver telemetry when the UI or show-control layer can use it.

Receiver side:

- Decode TCP records into points, frame markers, control messages, telemetry,
  and private extension records.
- For frame-capable receivers, assemble marked frames and submit complete frames
  through the frame queue.
- For limited-memory point-stream receivers, stream `POINTS` directly while
  using `FRAME_MARKER` metadata for target timing and repeat cache when possible.
- Apply scanner sync through the existing controller-side post-processing path.
- Use target begin times to drive the frame scheduler's due-time gate.
- Report queue state and lateness through status messages.

Core changes likely needed:

- Add `u3` support or introduce a protocol-side point representation that can
  carry a negotiated user-channel vector before mapping into `LaserPoint`.
- Add a first-class network protocol module, for example
  `include/libera/protocol` and `src/protocol`.
- Add a discovery component similar in spirit to existing network DAC discovery,
  but using Libera-specific advertisements.
- Add a TCP session encoder/decoder with tests for message framing, handshake
  negotiation, malformed payload rejection, point-stream parsing, frame-marker
  parsing, telemetry, and private extension envelopes.
- Add a receiver implementation that can be used by hardware bridges or Libera
  Link targets.

## Open questions

- Protocol name and magic bytes.
- Default UDP discovery port and TCP session port.
- Whether the default frame completion rule should be commit-by-count,
  commit-by-next-marker, or negotiated per session.
- Whether target begin times should be required for all frames or optional per
  frame.
- Exact positive/negative scanner-sync semantics.
- Whether `repeat_count = 0` means no-op or infinite repeat.
- Whether user channels are purely numbered or typed by wavelength/function.
- Whether intensity `i` is mandatory or negotiable.
- Manufacturer ID format: ILDA-assigned, Libera registry, UUID, or reverse-DNS.
- Whether custom messages need checksums beyond TCP and the record length.
- Whether the first version needs authentication for shared show networks.
