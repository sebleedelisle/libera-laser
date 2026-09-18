# Writing and packaging a Libera plugin

Libera plugins are unsigned native-code packages. A `.liberaplugin` file is an
ordinary ZIP containing a root `manifest.json`, one or more platform native
libraries, optional adjacent dependencies/resources, and user-facing documents.
The package format is schema version 1; its native interface is ABI version 1.

Installing a plugin gives its code the same operating-system access as the host
application. The manifest publisher is descriptive and is not verified. Hosts
must make this clear, and users should install packages only from sources they
trust.

## Package layout

```text
manifest.json
bin/
  macos/arm64/acme-usb-dac.dylib
  windows/x86_64/acme-usb-dac.dll
  linux/x86_64/acme-usb-dac.so
docs/
  README.md
  LICENSE.txt
resources/
  device-definitions.json
```

See [the complete example manifest](../examples/plugin-package/manifest.json).
The required shape is:

```json
{
  "schemaVersion": 1,
  "id": "com.acme.usb-dac",
  "version": "1.0.0",
  "name": "Acme USB DAC",
  "vendor": "Acme Laser Systems",
  "description": "Driver for Acme USB laser controllers.",
  "controllerType": "AcmeUsbDac",
  "libera": { "abiVersion": 1 },
  "entrypoints": [
    {
      "os": "macos",
      "arch": "arm64",
      "path": "bin/macos/arm64/acme-usb-dac.dylib"
    }
  ],
  "documents": {
    "readme": "docs/README.md",
    "license": "docs/LICENSE.txt"
  }
}
```

`id` identifies this driver/package and must be a stable lowercase dotted or
hyphenated identifier. The `libera.builtin.*` namespace is reserved for host
drivers. `controllerType` identifies the hardware family. Multiple plugin or
built-in drivers may implement one type; Libera runs only the implementation
explicitly selected by the user, and a newly installed plugin never silently
replaces a built-in driver. If multiple configured stores claim the same plugin
ID, the first store wins and later copies are rejected without loading them.

Versions use semantic versioning. Supported `os` values are `macos`, `windows`,
and `linux`; supported architectures are `arm64`, `x86_64`, `x86`, and
`universal` as an OS-specific fallback. Entrypoints use `.dylib`, `.dll`, and
`.so` respectively, and every entrypoint named by the manifest must be present
even when it targets a different platform.

Package paths use `/`, are relative, case-insensitively unique, and portable to
Windows. The installer rejects traversal, absolute paths, symlinks/special
files, encrypted entries, suspicious compression ratios, oversized archives,
malformed ZIP/CRC data, and the reserved root path `archive.liberaplugin` used
to retain the source package. Packages have no install or lifecycle scripts.

## Building a package

Place `manifest.json` and all referenced files in one directory, then run:

```sh
python3 tools/package_plugin.py path/to/package-root dist/acme-usb-dac.liberaplugin
```

The tool checks the manifest, references, symlinks, and portable path rules and
writes a deterministic ZIP. A whole-archive SHA-256 identifies the exact
installed revision. It prevents accidental revision/name collisions; because
plugins are intentionally unsigned, it is not publisher authentication.

Bundle private runtime dependencies with the entrypoint. Resolve them relative
to the native library:

- macOS: `@loader_path`
- Linux: `$ORIGIN`
- Windows: place DLLs beside the entrypoint; the host uses
  `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | ...)`

## Installing and loading

By default all Libera hosts share one user store:

- Windows: `%LOCALAPPDATA%\Libera\Plugins`
- macOS: `~/Library/Application Support/Libera/Plugins`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/libera/plugins`

Use `libera::plugin::installPlugin(packagePath)` rather than copying files.
Installation does not execute the incoming native library. It validates the
ZIP and manifest, extracts to staging, commits an immutable directory named
`<version>-<sha256-prefix>`, preserves the source archive, and atomically updates
`state.json`. Native ABI validation happens at the next process start. Updating
and removing packages also activate on restart, so loaded code is never
overwritten in place. Store and settings mutations are locked across processes,
so multiple running Libera applications cannot overwrite one another's updates.

`listManagedPlugins()` exposes install/load state, metadata, README/license
paths, failures, and runtime errors. `removePlugin(pluginId)` deactivates a
package without attempting to unload live native code. Driver choices are
stored with `selectControllerDriver(controllerType, driverId)`.

Tests and embedding applications can replace or add stores before constructing
`System`:

```cpp
libera::System::setPluginDirectory("test-plugin-store");
libera::System::addPluginDirectory("another-store");
libera::System system;
```

Relative store paths are resolved from the process working directory. Passing
an empty path disables plugin loading.

## Native ABI v1

Include [libera_plugin.h](../include/libera/plugin/libera_plugin.h) and export:

```c
const libera_plugin_api_t* libera_plugin_get_api(void);
```

The returned static table must set:

- `abi_version = LIBERA_PLUGIN_API_VERSION`
- `struct_size = sizeof(libera_plugin_api_t)`
- `plugin_id`, `plugin_version`, `controller_type`, and `display_name`
- `discover`, `connect_controller`, and `destroy_controller`
- `send_points`, or both `get_frame_requirements` and `send_frame`

The four identity strings must exactly match the package manifest. This catches
packaging mistakes and prevents an archive from presenting one driver in the UI
while loading another native backend.

`create_backend`, when supplied, receives both host services and an immutable
package environment:

```c
void* create_backend(
    const libera_host_services_t* host,
    const libera_plugin_environment_t* environment);
```

`environment->package_root` is the extracted revision directory and
`entrypoint_path` is the absolute library path. Use the package root for bundled
resources instead of the process working directory. The environment structure
is call-scoped, while both pointed-to strings remain valid until
`destroy_backend()` returns.

The optional backend callbacks are `create_backend`, `destroy_backend`, and
`rescan`. Optional controller functionality includes point rate, armed state,
buffer telemetry, static properties, frame transport, and typed settings. See
[example_plugin.cpp](../examples/example_plugin.cpp) for a complete point/frame
implementation.

## Lifecycle

The plugin owns one backend and any controller handles created from it:

1. `create_backend()` creates vendor SDK or shared discovery state.
2. `rescan()` performs a bounded probe and refreshes cached devices.
3. `discover()` emits `libera_controller_info_t` values.
4. `connect_controller()` opens one discovered device.
5. The host submits points or frames and calls optional controls.
6. `destroy_controller()` closes each live handle.
7. `destroy_backend()` releases shared state.

The host owns scheduling and worker threads. A discovery result contains stable
`id`/`label`, maximum point rate, optional usage/network information, and a
small `connect_cookie` copied back to `connect_controller()`.

## Host services, properties, and settings

Host services currently provide logging, latency reporting, and controller
error reporting. Check their `abi_version`, `struct_size`, and the
`LIBERA_PLUGIN_HOST_SERVICES_HAS_FIELD` macro before using fields introduced by
a newer host. `host_ctx` is the opaque value received by
`connect_controller()` and must be returned with controller-specific reports.

Read-only properties use a static `properties` table plus
`read_property(controller, index, ...)`.

Writable settings use `get_setting_count` and `get_setting_definition` for
plugin or controller scope, plus the corresponding setter. Values cross the ABI
as canonical UTF-8 strings. Libera validates and persists a value only after the
setter succeeds. Settings are keyed by `plugin_id` (and stable controller ID for
controller scope), so competing drivers for one hardware type do not share
configuration accidentally.

## macOS distribution

A hardened, signed Libera host that intentionally supports unsigned plugins
must carry `com.apple.security.cs.disable-library-validation`. This disables
Apple's same-team library-signature restriction for the process; it does not
turn off Gatekeeper's download/quarantine assessment of the host application.
The Libera Link release signing workflow applies this entitlement. Plugin
authors do not have to sign their dylib, though signed/notarized distribution
can still improve user trust and download handling.
