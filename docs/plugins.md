# Writing a Libera plugin

Libera plugins are shared libraries that add controller support without having
to fork or rebuild Libera itself.

The plugin API is plain C, but it is deliberately shaped like Libera's built-in
backend model:

- one plugin-wide **backend** object
- `discover()` on that backend
- `connect_controller()` returning one **controller handle** per live connection
- controller methods such as `send_points()`, `set_armed()`, and
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

- built-in `ControllerManagerBase` ~= plugin backend callbacks
- built-in `LaserController` ~= plugin controller handle plus controller callbacks

## Loading plugin libraries

Configure plugin search paths before you construct `libera::System`:

```cpp
libera::System::setPluginDirectory("plugins");
libera::System::addPluginDirectory("/absolute/path/to/more/plugins");

libera::System liberaSystem;
```

If you never call `setPluginDirectory()`, Libera uses `LIBERA_PLUGIN_DIR` when
that environment variable is set, otherwise it looks for a `plugins/`
directory.

## Required callbacks

These fields in `libera_plugin_api_t` are required:

- `type_name`
- `display_name`
- `discover`
- `connect_controller`
- `destroy_controller`
- `send_points`

Everything else is optional.

## Optional callbacks

These can be omitted by setting the field to `NULL`:

- `create_backend`
- `destroy_backend`
- `rescan`
- `set_point_rate`
- `set_armed`
- `get_buffer_state`
- `read_property`

If `get_buffer_state` is omitted, the host falls back to a fixed batch size.

If `properties` / `read_property` are omitted, the plugin exposes no
properties.

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

## Minimal example

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
    libera_controller_info_init(&info, "dac-1", "My DAC", 30000);
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
    .type_name = "MyDac",
    .display_name = "My DAC",
    .discover = discover,
    .connect_controller = connect_controller,
    .destroy_controller = destroy_controller,
    .send_points = send_points,
};

LIBERA_PLUGIN_EXPORT(pluginApi)
```

See the full working example here:

- [example_plugin.cpp](../examples/example_plugin.cpp)

## Lifecycle

For a plugin that supports two devices, the call flow is:

```text
load .dylib
├── libera_plugin_get_api()
├── create_backend(host)                 optional
├── rescan(backend)                      optional
├── discover(backend, emit, ctx)         emits "dev-A", "dev-B"
│
├── connect_controller(backend, dev-A, host_ctx_A) -> controller_A
│   ├── set_point_rate(controller_A, 30000)         optional
│   ├── set_armed(controller_A, true)               optional
│   ├── send_points(controller_A, pts, n)           repeated
│   ├── get_buffer_state(controller_A, &bs)         optional
│   └── destroy_controller(controller_A)
│
├── connect_controller(backend, dev-B, host_ctx_B) -> controller_B
│   └── ...
│
└── destroy_backend(backend)             optional
```

`discover()` may be called repeatedly while the app is running.

## Streaming model

The host still owns the high-level streaming loop.

That means the plugin does **not** implement Libera's scheduler policy itself.
The plugin's job is just to:

- discover devices
- open/close controller connections
- accept point batches in `send_points()`
- optionally expose buffer state so the host can pace itself accurately

Whether the application is using queued frames or a live point callback, the
host adapts that content source into point batches before calling the plugin.

If your vendor SDK is internally frame-based, it is fine for the plugin to
buffer points and feed the SDK from its own worker thread.

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
