# Writing a Libera plugin

Libera supports **plugins**: shared libraries that add support for new laser
controllers without having to fork or recompile Libera itself. If you make a
controller and you'd like Libera apps to talk to it, a plugin is how you do it.

This guide explains what a plugin is, the small C interface you need to
implement, and how the host app loads and drives your plugin at runtime.

## What is a plugin?

A Libera plugin is a shared library — a `.dylib` on macOS, a `.so` on Linux,
or a `.dll` on Windows — that exports a specific set of C functions. When a
Libera-based application starts, it scans its plugins directory, loads each
library it finds, and asks each one to discover and connect to its own
devices.

From the application's point of view, a plugin-provided controller looks and
behaves exactly like a built-in one.

## Why a plain C interface?

The plugin API is deliberately pure C — no C++ classes, no STL types cross
the boundary. That means:

- You can write a plugin in any language that can produce a shared library
  with C linkage (C, C++, Rust, Zig, D, Object Pascal, ...).
- You can ship a plugin built against one compiler and load it into a host
  built with a different one.
- The interface is small and explicit, which makes versioning manageable.

The header you need is [libera_plugin.h](../include/libera/plugin/libera_plugin.h).

## The streaming model

A plugin does not handle "frames". The host drives a streaming loop for every
connected controller and pushes batches of points to the plugin at whatever
cadence it decides. Your plugin's job is to:

1. Accept each batch in `libera_plugin_send_points()`.
2. Get those points out to the hardware — over USB, network, or whatever
   transport your controller uses.
3. Report how full your internal buffer is, so the host can pace itself.

If your controller's native SDK is frame-based (it expects whole drawings at
once rather than streams), you'll need to buffer points internally in your
plugin and flush them from your own worker thread. That's completely fine —
it just means the plugin does a bit more work.

### Back-pressure

The host calls `libera_plugin_get_buffer_state()` on every streaming cycle
and throttles when your buffer is nearly full — specifically, when the
remaining headroom is less than half the batch it was about to send. If you
don't report buffer state (return non-zero, or fill `-1`s), the host falls
back to a fixed cadence of roughly 10 ms worth of points per call.

Accurate buffer reporting is the main knob you have for keeping the device
running at the edge of its buffer without under-running.

## The functions you must export

Every plugin must export these C functions (see
[libera_plugin.h](../include/libera/plugin/libera_plugin.h) for full signatures
and docs):

| Function | Purpose |
|---|---|
| `libera_plugin_api_version` | Return `LIBERA_PLUGIN_API_VERSION` so the host can check compatibility. |
| `libera_plugin_type` | A unique short tag (e.g. `"MyBrandDAC"`) used to route connect calls. |
| `libera_plugin_name` | Human-readable name shown in the host UI. |
| `libera_plugin_init` | Called once after load. Receives the host services struct. |
| `libera_plugin_shutdown` | Called once before unload. Tear down global state. |
| `libera_plugin_discover` | Emit one `libera_controller_info_t` per device you can see. |
| `libera_plugin_connect` | Open a connection and return an opaque handle. |
| `libera_plugin_set_point_rate` | Told what point rate (points/sec) the host wants. |
| `libera_plugin_send_points` | Accept a batch of points for output. |
| `libera_plugin_get_buffer_state` | Report your internal buffer fullness. |
| `libera_plugin_set_armed` | Enable or disable light output. |
| `libera_plugin_disconnect` | Close a connection and free per-connection state. |

## Optional exports

These functions are resolved at load time but their absence is not an
error. Export them only if you have something to contribute.

| Function | Purpose |
|---|---|
| `libera_plugin_rescan` | Hint to network-discovery plugins: drop cached state and refresh. Called before `discover()`. |
| `libera_plugin_list_properties` | Per-device: enumerate `(key, label)` pairs for things like firmware version, serial number, IP. |
| `libera_plugin_get_property` | Per-device: read one property as a string, snprintf-style. |

Properties are per-connection: different devices managed by the same
plugin can legitimately expose different property sets. Values are
always strings — format numbers, versions, timestamps as text inside
your plugin.

## Per-device state and the handle

This is the central model and the one thing worth internalising up front:

**Your plugin exports one set of C functions, shared across every device it
manages.** There is no "one plugin instance per device" — the host loads
your `.dylib` once, and calls the same `send_points`, `set_armed`,
`disconnect` (etc.) functions for every controller.

**The `void*` handle is how those functions know which device a call is
about.** When the host opens a device it calls:

```c
void* handle = libera_plugin_connect("my-dac-abc", host_ctx);
```

You return any non-NULL pointer — typically a pointer to a struct you
allocate that holds everything this connection needs (USB handle, socket,
worker thread, queue, current rate, armed flag, `host_ctx`, ...). The host
treats it as opaque and hands it back on every later call:

```c
libera_plugin_send_points(handle_A, points, 1000);  /* device A */
libera_plugin_send_points(handle_B, points, 1000);  /* device B */
libera_plugin_set_armed  (handle_A, true);          /* device A */
libera_plugin_disconnect (handle_B);                /* device B */
```

Inside each function you cast the `void*` back to your own struct and use
whatever's in it to route the call to the correct hardware.

Consequences of this design:

- **You do not need a global list of open controllers.** Each handle is
  self-contained.
- **Per-device state goes on the handle struct**, not in globals.
- **The global state in your plugin is minimal**: loaded SDK function
  pointers, the host services pointer, and any shared discovery state.
- **The host never inspects your handle.** Its internals are entirely
  your business.

See the example's `ExampleHandle` struct for the typical shape.

## Lifecycle

For a plugin that manages two devices, the call sequence looks like:

```
load .dylib
├── libera_plugin_api_version()              → version check
├── libera_plugin_init(host)                 → global setup
├── libera_plugin_discover(emit, ctx)        → emits "dev-A", "dev-B"
│
├── libera_plugin_connect("dev-A", ctx_A)    → returns handle_A
│   ├── libera_plugin_set_point_rate(handle_A, 30000)
│   ├── libera_plugin_set_armed(handle_A, true)
│   ├── libera_plugin_send_points(handle_A, pts, n)   ┐
│   ├── libera_plugin_get_buffer_state(handle_A, &bs) │ repeated
│   ├── libera_plugin_send_points(handle_A, pts, n)   │ continuously
│   ├── libera_plugin_get_buffer_state(handle_A, &bs) ┘
│   ├── libera_plugin_set_armed(handle_A, false)
│   └── libera_plugin_disconnect(handle_A)
│
├── libera_plugin_connect("dev-B", ctx_B)    → returns handle_B
│   └── ... same streaming sequence ...
│
└── libera_plugin_shutdown()                 → global teardown
unload .dylib
```

`discover()` may also be called repeatedly during the run, not just once at
startup — the host may ask for a refreshed list at any time.

## Error status codes

`libera_plugin_send_points()` returns a `libera_status_t`:

| Code                            | Meaning | Host behaviour |
|---|---|---|
| `LIBERA_OK`                     | Batch accepted. | Normal streaming. |
| `LIBERA_ERR_DISCONNECTED`       | Device is gone (unplugged, socket closed). | Records a **connection error**, marks the controller disconnected, and tries to reconnect. |
| `LIBERA_ERR_TIMEOUT`            | Transport timed out. | Records an intermittent error and keeps streaming. |
| `LIBERA_ERR_BUSY`               | Device can't accept more points right now. | Records an intermittent error and keeps streaming. |
| `LIBERA_ERR_PROTOCOL`           | Protocol-level failure (bad response, decode error). | Records an intermittent error and keeps streaming. |
| `LIBERA_ERR_INVALID_ARGUMENT`   | Bad args (null pointer, nonsense count). | Records an intermittent error. |
| `LIBERA_ERR_INTERNAL`           | Anything else you want to flag. | Records an intermittent error. |

Use `LIBERA_ERR_DISCONNECTED` only for true connection loss — it's the
error code that drives the host's reconnect logic. Everything else is
treated as transient noise that contributes to per-controller error
statistics but doesn't tear the connection down.

For errors outside the hot path (e.g. from a worker thread, or a hotplug
notification), call `host->report_error(host_ctx, code, label)` directly
instead — it's the async equivalent.

## Host services (what you get from the host)

When the host calls `libera_plugin_init()`, it passes you a struct of
function pointers — services the host provides to you:

- `log(level, message)` — write to the host's logging system.
- `record_latency(host_ctx, nanoseconds)` — report a transport latency
  sample, fed into the host's rolling percentile statistics.
- `report_error(host_ctx, code, label)` — raise a structured error, using
  the same `network.*` / `usb.*` style codes the built-in controllers use.

Any of these pointers can be NULL, so always null-check before calling.

The `host_ctx` value is a per-connection token the host gives you at
`libera_plugin_connect()` time. You must pass it back to the host services
so the host can attribute the call to the right controller. Stop using
`host_ctx` before `libera_plugin_disconnect()` returns.

## The minimum useful plugin

The repository ships with [example_plugin.cpp](../examples/example_plugin.cpp),
a stub plugin that simulates a single controller and accepts (and discards)
every point it is sent. It's the smallest thing that implements the whole
interface end-to-end, so it's the right starting point for your own plugin —
copy it, rename the strings, and replace the stubs with real transport code.
Every exported function is documented inline with the expected behaviour,
threading, and common pitfalls.

## The point format

Points cross the plugin boundary as fixed-size integers, not floats:

```c
typedef struct {
    int16_t  x, y;      // -32768 .. 32767, full scan range
    uint16_t r, g, b;   // 0 .. 65535
    uint16_t i;         // master intensity, 0 .. 65535
    uint16_t u1, u2;    // user channels, 0 .. 65535
} libera_point_t;
```

`u1` and `u2` are extra per-point channels libera uses for things like
safety masks or auxiliary waveforms. Forward them to your hardware if it
has the wires for it; otherwise just ignore them.

If your hardware expects a different range or scaling, convert inside your
plugin.

## Building your plugin

The example's build comments cover the typical commands:

```sh
# macOS
c++ -shared -fPIC -std=c++17 -o my-plugin.dylib my_plugin.cpp -I <path-to>/include

# Linux
c++ -shared -fPIC -std=c++17 -o my-plugin.so    my_plugin.cpp -I <path-to>/include

# Windows (MSVC)
cl /LD /std:c++17 /I <path-to>\include my_plugin.cpp /Fe:my-plugin.dll
```

Drop the resulting file into the host app's plugins directory and start the
app — your plugin's `discover()` will be called and any devices it emits will
show up alongside the built-in ones.

## Threading notes

- `libera_plugin_init` and `libera_plugin_shutdown` are called once, on the
  host's setup/teardown thread.
- `libera_plugin_discover` may be called repeatedly from the host's
  discovery thread. Keep it quick and non-blocking.
- `libera_plugin_send_points` runs on a per-controller streaming thread.
  This is the hot path — avoid allocations where you can.
- The host services (`log`, `record_latency`, `report_error`) may be called
  from any plugin thread.

If your plugin needs its own worker thread (for example, to pace a
frame-based SDK), start it in `libera_plugin_connect` and join it in
`libera_plugin_disconnect`.

## Versioning

`LIBERA_PLUGIN_API_VERSION` is a single integer that the host checks at
load time. If the host sees a mismatch it will refuse to load the plugin
rather than risking an ABI crash. The host services struct has its own
`abi_version` field and is designed so new fields can be appended without
breaking older plugins — plugins should simply ignore service fields they
don't recognise.
