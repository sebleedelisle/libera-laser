# Writing a Libera plugin

Libera plugins are shared libraries that add controller support without having
to fork or rebuild Libera itself.

The plugin API is plain C, but it is deliberately shaped like Libera's built-in
backend model:

- one plugin-wide **backend** object
- `discover()` on that backend
- `connect_controller()` returning one **controller handle** per live connection
- controller methods such as `send_points()`, `send_frame()`, `set_armed()`, and
  `get_buffer_state()`

That keeps translating an in-tree backend into a plugin mostly mechanical.

## The one export

Every plugin exports exactly one symbol:

```c
const libera_plugin_api_t* libera_plugin_get_api(void);
```

That function returns a static `libera_plugin_api_t` describing the plugin and
the callbacks Libera should use.

The public header is:

- [libera_plugin.h](../include/libera/plugin/libera_plugin.h)

## Mental model

Think of the plugin as two layers:

1. **Backend**
   Owns plugin-wide state such as a vendor SDK context, USB/libusb setup, or a
   cached discovery list.
2. **Controller**
   Owns one live connection to one discovered device.

That maps directly onto Libera's built-in structure:

- built-in `AbstractControllerManager` / `ControllerManagerBase` ~= plugin backend callbacks
- built-in `LaserController` ~= plugin controller handle plus controller callbacks

## Loading plugin libraries

In the normal case you do not need to configure plugin directories at all.
Libera loads plugins from one shared user plugin folder:

- Windows: `%LOCALAPPDATA%\Libera\Plugins`
- macOS: `~/Library/Application Support/Libera/Plugins`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/libera/plugins`

So the simplest setup is just:

```cpp
libera::System liberaSystem;
```

Only configure plugin search paths if you deliberately want a custom plugin set,
for example in a test, development tool, or portable host app:

```cpp
libera::System::setPluginDirectory("plugins");
libera::System::addPluginDirectory("/absolute/path/to/more/plugins");

libera::System liberaSystem;
```

Passing an empty string to `setPluginDirectory("")` disables plugin loading
entirely for that process.

## Managing installed plugins

GUI apps should use the small backend API in
[PluginManagement.hpp](../include/libera/plugin/PluginManagement.hpp) rather
than each app reimplementing plugin install/remove/list logic.

The main functions are:

- `libera::plugin::userPluginDirectory()`
- `libera::plugin::listManagedPlugins()`
- `libera::plugin::installPlugin(path)`
- `libera::plugin::removePlugin(path)`
- `libera::plugin::platformPluginExtension()`
- `libera::plugin::pluginSettings(type)`
- `libera::plugin::controllerSettings(type, controllerId)`
- `libera::plugin::setPluginSetting(type, key, value)`
- `libera::plugin::setControllerSetting(type, controllerId, key, value)`

`listManagedPlugins()` merges the runtime `PluginRegistry` with shared-library
files found in the user plugin folder. That gives apps enough state to render:

- loaded plugins
- validation/load failures
- plugin files copied after startup, reported as `PendingRestart`
- loaded plugin files removed from disk, reported as `RemovedPendingRestart`
- runtime errors reported by a plugin while controllers are active

Plugin setting values are stored in `libera-plugin-settings.conf` beside the
shared plugin libraries. Libera owns this small per-user store so the same
plugin configuration is restored in each Libera-enabled application. Values
are keyed by the stable plugin `type_name`; controller values also include the
stable discovery `id`.

`installPlugin()` validates the plugin using the same callback and ABI rules as
the runtime loader before copying it into `userPluginDirectory()`. New installs
need an app restart before `System` can load them, because plugin libraries are
only scanned during startup.

`removePlugin()` only removes files from `userPluginDirectory()`. Removing a
loaded plugin does not unload native code from the current process, so the
result reports `restartRequired=true`.

### Optional ImGui panel

Apps that use Dear ImGui can also opt into Libera's reusable plugin panel:

```cmake
include("path/to/libera-core/cmake/LiberaImguiWidgets.cmake")
libera_add_imgui_plugin_ui(libera-plugin-ui-imgui imgui_lib)
target_link_libraries(my-app PRIVATE libera-plugin-ui-imgui)
```

The panel entry point is:

```cpp
libera::gui::imgui::DrawPluginManagementPanel(state, callbacks, options);
```

The caller still owns the window, native file picker, restart behavior, and
styling. The shared panel owns the install/remove/list/status UI and renders
plugin-wide settings. Controller UIs can use:

```cpp
libera::gui::imgui::DrawPluginControllerSettings(
    pluginType, controllerId, panelState);
```

## Required callbacks

These fields in `libera_plugin_api_t` are required:

- `type_name`
- `display_name`
- `discover`
- `connect_controller`
- `destroy_controller`
- transport callback(s): `send_points`, or `get_frame_requirements` + `send_frame`

Everything else is optional.

Set `.abi_version = LIBERA_PLUGIN_API_VERSION` and
`.struct_size = sizeof(libera_plugin_api_t)`. The current unreleased plugin API
version remains `1`; `struct_size` permits future append-only additions without
forcing a version change.

## Optional callbacks

These can be omitted by setting the field to `NULL`:

- `create_backend`
- `destroy_backend`
- `rescan`
- `set_point_rate`
- `set_armed`
- `get_buffer_state`
- `read_property`
- `get_frame_requirements`
- `send_frame`
- `get_setting_count`
- `get_setting_definition`
- `set_plugin_setting`
- `set_controller_setting`

If `get_buffer_state` is provided, the host can pace point submissions against
the device's reported fill level.

If `get_buffer_state` is omitted, the host cannot maintain a specific
device-side buffer target. In that case it falls back to an automatic
point-ingester cadence derived from the current point rate and submits fixed
batches on that schedule.

If `properties` / `read_property` are omitted, the plugin exposes no
properties.

If you use the frame-ingester path, `get_frame_requirements` and `send_frame`
must be provided together.

The two setting-definition callbacks must be provided together. A setter is
required only for a scope whose setting count is non-zero.

## Host services

If you implement `create_backend()`, Libera passes a
`const libera_host_services_t*` into it. That host table is versioned
separately from the main plugin ABI and currently exposes:

- `log(level, message)`
- `record_latency(host_ctx, nanoseconds)`
- `report_error(host_ctx, code, label)`

The host-services table also carries `struct_size`. Plugins must check both its
version and size before using fields added by a later host.

`host_ctx` is the opaque token Libera passes into `connect_controller()`. Keep
that token on the controller side if you want to report transport latency or
device-specific errors back into the host later from `send_points()` or
`send_frame()`.

## Discovery and connect

`discover()` emits `libera_controller_info_t` structs. Each one contains:

- `id`
- `label`
- `max_point_rate`
- optional `usage_state`
- optional `network` info
- optional `connect_cookie`

The `connect_cookie` is important: the host copies it back into
`connect_controller()`, so a plugin can keep a tiny transport-specific token
from discovery time instead of re-looking everything up by string id.

If the plugin provides `rescan()`, Libera calls it before each `discover()`
pass so network or USB plugins can refresh cached state.

For discovery-oriented plugins, the recommended lifecycle is:

- `rescan()` does one bounded probe/listen pass
- the backend updates a cached discovery list
- any temporary discovery sockets are closed before `rescan()` returns
- `discover()` just emits the cached list

That keeps idle plugin instances from holding discovery ports or background
scan threads indefinitely while still fitting the current plugin ABI.

## Properties

Plugins do not need to implement both "list properties" and "get by key".

Instead they provide:

- a static property table: `properties` + `property_count`
- one reader callback: `read_property(controller, property_index, ...)`

The host handles:

- listing properties
- key lookup
- `get_property(key)`

That keeps property boilerplate small.

## Settings

Settings are persistent writable configuration. They are separate from
read-only properties and from operational state such as point rate or arming.
Libera supports two scopes:

- `LIBERA_SETTING_SCOPE_PLUGIN` for one value shared by the backend and all of
  its controllers
- `LIBERA_SETTING_SCOPE_CONTROLLER` for a value stored against one stable
  discovered controller `id`

A plugin exposes definitions through:

```c
uint32_t get_setting_count(libera_setting_scope_t scope);

const libera_setting_def_t* get_setting_definition(
    libera_setting_scope_t scope,
    uint32_t setting_index);
```

Each returned definition must remain valid for the lifetime of the loaded
library and set `.struct_size = sizeof(libera_setting_def_t)`. Definitions are
returned one at a time so their structures can grow without changing array
stride. Keys must be unique within their scope. This first version deliberately
uses one static schema for every controller exposed by a plugin; model-specific
or dynamically generated schemas can be added later if a real plugin needs
them.

Supported setting types are:

- `LIBERA_SETTING_BOOL`
- `LIBERA_SETTING_INT`
- `LIBERA_SETTING_FLOAT`
- `LIBERA_SETTING_STRING`
- `LIBERA_SETTING_ENUM`

Values cross the ABI as canonical UTF-8 strings. Booleans use `"true"` and
`"false"`; integers use base-10 notation; floating-point values use a decimal
point; enums use their stable choice value rather than their display label.
Numeric definitions can declare optional minimum, maximum, and UI step values.

Setters are separated by scope:

```c
libera_status_t set_plugin_setting(void* backend,
                                   const char* key,
                                   const char* value);

libera_status_t set_controller_setting(void* controller,
                                       const char* key,
                                       const char* value);
```

The host validates values before calling a setter and persists a change only
after the setter returns `LIBERA_OK`. Controller-setting callbacks are
serialized with host calls on that controller's opaque handle, so changes land
between complete submissions. A plugin setting can affect several controllers,
so the plugin must synchronize that callback with any shared transport work it
owns. After every successful change, Libera calls `rescan()` when the plugin
provides it. Applications will observe the refreshed discovery state on their
next normal discovery pass.

Startup order is deliberately deterministic:

```text
create_backend()
├── apply saved/default plugin settings
├── rescan() and discover()
└── connect_controller()
    ├── apply saved/default controller settings
    └── start host streaming thread
```

Controller settings are persisted using `type_name + controller id + setting
key`. Plugins should therefore use serial numbers, stable unit identifiers, or
another persistent identity for discovery IDs. An offline controller setting
can be saved before connection and is applied immediately after its next
`connect_controller()` call. Settings are also restored after an automatic
transport reconnect creates a fresh controller handle.

Host applications can inspect and update settings without handling the C ABI:

```cpp
auto sharedSettings = libera::plugin::pluginSettings("ExamplePlugin");
auto deviceSettings = libera::plugin::controllerSettings(
    "ExamplePlugin", "device-001");

auto result = libera::plugin::setPluginSetting(
    "ExamplePlugin", "transport_mode", "low_latency");

auto controllerResult = libera::plugin::setControllerSetting(
    "ExamplePlugin", "device-001", "invert_x", "true");
```

## Choosing a transport shape

Plugins should mirror the same two backend shapes that built-in controllers use:

- `Point-ingester`
  The transport wants "some more points now". Implement `send_points()`. The
  host asks Libera's shared scheduler for a point batch, converts it to
  `libera_point_t`, and forwards it to the plugin.
  Point-ingester plugins can run in two modes:
  with `get_buffer_state()`, the host tries to maintain a target buffer level;
  without it, the host just keeps points flowing automatically based on point
  rate.
- `Frame-ingester`
  The transport wants one whole frame submission at a time. Implement the
  paired `get_frame_requirements()` + `send_frame()` callbacks. The host asks Libera's
  shared scheduler for one frame, converts it to `libera_point_t`, and forwards
  it to the plugin.

If both shapes are present in one plugin, the host prefers the
frame-ingester path because it most closely matches a built-in frame backend.

## Readiness and backpressure

There is no separate generic `is_ready()` callback in the plugin ABI. The host
uses different readiness signals depending on the transport shape:

- `Point-ingester`
  Backpressure is reported indirectly through `get_buffer_state()`. The plugin
  reports `points_in_buffer` and `total_buffer_points`, and the host uses that
  telemetry to decide whether to submit more points now or wait a little longer.
  That is the mode to use when the host should actively maintain a device-side
  buffer level.
- `Frame-ingester`
  Readiness is reported explicitly through `get_frame_requirements()`. Return
  `LIBERA_OK` and fill out the requirements when the transport can accept one
  more frame now. Return `LIBERA_ERR_BUSY` when the transport is not ready yet;
  the host waits briefly and asks again.

  When the active content source is a live point callback, the host also keeps
  a shared virtual point backlog for the callback-to-frame adapter. That
  backlog includes points already accepted through successful `send_frame()`
  calls plus any extra points currently staged inside the adapter while it
  searches for natural frame boundaries. The host uses that shared backlog to
  decide how many more callback points to request, so frame-ingester plugins do
  not need to invent their own point-side pacing policy.

Two details are important:

- `send_points()` / `send_frame()` are submission calls, not readiness polls.
  A non-`LIBERA_OK` return from those callbacks is treated as a send failure,
  not as normal backpressure.
- If a point-ingester plugin omits `get_buffer_state()`, the host falls back to
  automatic point-rate-based feeding. In that mode the host does not try to
  maintain a specific device-side buffer level; it derives a fixed batch size
  from the current point rate and keeps submitting on its own cadence.
- A frame-ingester plugin does not need `get_buffer_state()` for that callback
  adaptation path to work. The host can synthesize a virtual backlog from
  accepted `send_frame()` submissions. If the plugin does expose
  `get_buffer_state()`, the host uses that as the transport-side truth and
  still adds the adapter's own staged points on top when reporting the shared
  buffered-point view.

## Helper utilities

`libera_plugin.h` also ships a few small helpers so plugins do not need to
rebuild the same safety boilerplate:

- `libera_controller_info_init()` to zero and populate discovery records
- `libera_controller_info_set_network()` to attach IP/port metadata
- `libera_controller_info_set_cookie()` to copy a small discovery token into
  `connect_cookie`
- `libera_frame_requirements_init()` to populate frame requirements
- `libera_copy_string()` for fixed-size string fields and property output
- `LIBERA_PLUGIN_EXPORT(pluginApi)` to export `libera_plugin_get_api()`

## Minimal example

In the examples below:

- `type_name` identifies the controller family or backend type
- `display_name` is the human-readable family name
- `id` is the stable identifier for one discovered device
- `label` is the human-readable per-device name

### Point-ingester

```c
typedef struct {
    VendorSdk* sdk;
} MyBackend;

typedef struct {
    VendorDevice* device;
    bool armed;
} MyController;

static void discover(void* rawBackend,
                     libera_emit_controller_fn emit,
                     void* ctx) {
    libera_controller_info_t info;
    libera_controller_info_init(&info,
                                "acme-usb-001",
                                "Acme USB DAC #1",
                                30000);
    emit(ctx, &info);
}

static void* connect_controller(void* rawBackend,
                                const libera_controller_info_t* info,
                                libera_host_ctx_t hostCtx) {
    MyBackend* backend = rawBackend;
    MyController* controller = calloc(1, sizeof(MyController));
    controller->device = vendor_open(backend->sdk, info->id);
    controller->armed = false;
    (void)hostCtx;
    return controller;
}

static void destroy_controller(void* rawController) {
    MyController* controller = rawController;
    vendor_close(controller->device);
    free(controller);
}

static libera_status_t send_points(void* rawController,
                                   const libera_point_t* points,
                                   uint32_t count) {
    MyController* controller = rawController;
    return vendor_send_points(controller->device, points, count)
        ? LIBERA_OK
        : LIBERA_ERR_INTERNAL;
}

static const libera_plugin_api_t pluginApi = {
    .abi_version = LIBERA_PLUGIN_API_VERSION,
    .struct_size = sizeof(libera_plugin_api_t),
    .type_name = "AcmeUsbDac",
    .display_name = "Acme USB DAC",
    .discover = discover,
    .connect_controller = connect_controller,
    .destroy_controller = destroy_controller,
    .send_points = send_points,
};

LIBERA_PLUGIN_EXPORT(pluginApi)
```

### Frame-ingester

A frame-ingester plugin replaces `send_points()` with the paired frame
callbacks:

```c
static libera_status_t get_frame_requirements(void* rawController,
                                              libera_frame_requirements_t* out) {
    MyController* controller = rawController;
    if (!controller || !out) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }

    libera_frame_requirements_init(out,
                                   /* maximumPointsRequired */ 1000,
                                   /* preferredPointCount   */ 1000,
                                   /* blankFramePointCount  */ 1000);
    out->estimated_first_point_render_delay_ns = 5ull * 1000ull * 1000ull;
    return LIBERA_OK;
}

static libera_status_t send_frame(void* rawController,
                                  const libera_point_t* points,
                                  uint32_t count) {
    MyController* controller = rawController;
    return vendor_send_frame(controller->device, points, count)
        ? LIBERA_OK
        : LIBERA_ERR_INTERNAL;
}
```

Then wire those fields into `libera_plugin_api_t`:

```c
static const libera_plugin_api_t pluginApi = {
    .abi_version = LIBERA_PLUGIN_API_VERSION,
    .struct_size = sizeof(libera_plugin_api_t),
    .type_name = "AcmeFrameDac",
    .display_name = "Acme Frame DAC",
    .discover = discover,
    .connect_controller = connect_controller,
    .destroy_controller = destroy_controller,
    .get_frame_requirements = get_frame_requirements,
    .send_frame = send_frame,
};
```

See the full working example here:

- [example_plugin.cpp](../examples/example_plugin.cpp)

## Lifecycle

For a point-ingester plugin that supports two devices, the call flow is:

```text
load .dylib
├── libera_plugin_get_api()
├── create_backend(host)                 optional
├── set_plugin_setting(...)              zero or more saved/default values
├── rescan(backend)                      optional
├── discover(backend, emit, ctx)         emits "dev-A", "dev-B"
│
├── connect_controller(backend, dev-A, host_ctx_A) -> controller_A
│   ├── set_point_rate(controller_A, 30000)         optional
│   ├── set_armed(controller_A, true)               optional
│   ├── set_controller_setting(...)                 zero or more values
│   ├── get_buffer_state(controller_A, &bs)         optional
│   ├── send_points(controller_A, pts, n)           repeated
│   └── destroy_controller(controller_A)
│
├── connect_controller(backend, dev-B, host_ctx_B) -> controller_B
│   └── ...
│
└── destroy_backend(backend)             optional
```

For a frame-ingester plugin, the per-controller loop becomes:

```text
load .dylib
├── libera_plugin_get_api()
├── create_backend(host)                           optional
├── set_plugin_setting(...)                        zero or more values
├── discover(backend, emit, ctx)
│
├── connect_controller(backend, dev-A, host_ctx_A) -> controller_A
│   ├── set_point_rate(controller_A, 30000)           optional
│   ├── set_armed(controller_A, true)                 optional
│   ├── set_controller_setting(...)                   zero or more values
│   ├── get_frame_requirements(controller_A, &req)    repeated
│   ├── send_frame(controller_A, frame_pts, n)        repeated
│   └── destroy_controller(controller_A)
│
└── destroy_backend(backend)                       optional
```

`discover()` may be called repeatedly while the app is running.

## Streaming model

The host still owns the high-level streaming loop.

That means the plugin does **not** implement Libera's scheduler policy itself.
The plugin's job is just to:

- discover devices
- open/close controller connections
- describe what kind of transport payload it wants next
- accept that payload through `send_points()` or `send_frame()`
- optionally expose buffer state so the host can pace or report status

Whether the application is using queued frames or a live point callback, the
host adapts that content source into the payload shape the plugin asked for.

For frame-ingester plugins, that host-owned adaptation now also includes the
virtual point backlog used to throttle live callbacks. The plugin still only
needs to expose frame readiness and accept frames; the host keeps the callback
side from overfilling the transport by accounting for both accepted frames and
any points staged in the shared framer.

If your vendor SDK is internally frame-based, prefer the
`get_frame_requirements()` + `send_frame()` path so the plugin stays aligned
with built-in frame-ingester backends. Only re-buffer point batches inside the
plugin if you deliberately want the transport to behave like a point-ingester.

## Building

Typical commands:

```sh
# macOS
c++ -shared -fPIC -std=c++17 -o my-plugin.dylib my_plugin.cpp -I <path-to>/include

# Linux
c++ -shared -fPIC -std=c++17 -o my-plugin.so my_plugin.cpp -I <path-to>/include

# Windows (MSVC)
cl /LD /std:c++17 /I <path-to>\include my_plugin.cpp /Fe:my-plugin.dll
```
