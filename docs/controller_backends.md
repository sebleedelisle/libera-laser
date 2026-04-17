# Implementing a Libera controller backend

This note is for people adding a built-in controller backend to libera itself.
If you are writing an out-of-tree plugin instead, see
[Writing a Libera plugin](plugins.md).

## The mental model

When you implement a backend, there are two separate questions:

1. Where does content come from?
2. What kind of payload does the transport want next?

Today the content sources are:

- `PointCallback`
  A callback installed with `setPointCallback(...)`. Each time the
  transport asks for more output, libera calls the callback to append the next
  batch of points.
- `FrameQueue`
  Frames pushed with `sendFrame(...)`. Libera owns the queueing, scheduling,
  hold-last-frame behaviour, stale-frame skipping, and transition blanking.

The important rule for backend authors is: **the backend should describe what
the transport wants, not reimplement scheduler policy itself.**

## Point-ingester backend

This is the common case for transports that want "some more points now". The
backend polls the hardware, works out how many points it can accept, asks
libera for that many points, then packs and sends them.

```cpp
class MyPointController : public LaserController {
public:
    void run() override {
        while (threadShouldRun()) {
            DeviceState state = pollHardware();

            if (!state.connected || !state.readyForMorePoints) {
                sleepShort();
                continue;
            }

            PointFillRequest request;
            request.minimumPointsRequired = computeMinPoints(state);
            request.maximumPointsRequired = computeMaxPoints(state);
            request.estimatedFirstPointRenderTime = estimateRenderStart(state);

            // requestPoints() handles either:
            // - PointCallback directly
            // - FrameQueue adapted into points by the shared scheduler
            if (!requestPoints(request) || pointsToSend.empty()) {
                sleepShort();
                continue;
            }

            HardwarePacket packet = encodePoints(pointsToSend);
            sendPacket(packet);
        }
    }
};
```

This is already close to how Ether Dream, LaserCube USB, LaserCube Net, AVB,
IDN, and point-ingester plugins currently behave.

## Frame-ingester backend

This is the shape for transports that want one whole frame submission at a
time. The backend waits for the transport to become ready for a new frame,
asks libera for one frame, then packs and sends it.

```cpp
class MyFrameController : public LaserController {
public:
    void run() override {
        while (threadShouldRun()) {
            DeviceState state = pollHardware();

            if (!state.connected || !state.readyForNextFrame) {
                sleepShort();
                continue;
            }

            FrameFillRequest request;
            request.maximumPointsRequired = computeMaxFramePoints(state);
            request.preferredPointCount = computePreferredFramePoints(state);
            request.blankFramePointCount = computeBlankFramePoints(state);
            request.estimatedFirstPointRenderTime = estimateFrameStart(state);

            Frame frame;

            // requestFrame() handles either:
            // - FrameQueue directly
            // - PointCallback adapted into one transport frame
            if (!requestFrame(request, frame) || frame.points.empty()) {
                sleepShort();
                continue;
            }

            HardwareFrame payload = encodeFrame(frame);
            sendFrameToHardware(payload);
        }
    }
};
```

Today this is the right mental model for native frame transports such as the
direct Helios USB path.

Out-of-tree plugins can now mirror this frame-ingester shape too via the ABI
v2 `get_frame_requirements()` + `send_frame()` callbacks.

## Discovery example

Built-in backends implement discovery on the manager, not on the controller
thread. In practice the pattern is:

- define a `ControllerInfo` subtype that keeps the exact metadata
  `connectController()` will need later
- make `discover()` enumerate hardware and return lightweight info objects
  without permanently claiming the device
- let the manager reuse one live controller per stable discovery key

```cpp
class MyControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "MyDac";
    }

    MyControllerInfo(std::string id,
                     std::string label,
                     std::string transportPath)
    : ControllerInfo(controllerType(), std::move(id), std::move(label))
    , transportPathValue(std::move(transportPath)) {}

    const std::string& transportPath() const { return transportPathValue; }

private:
    std::string transportPathValue;
};

class MyManager
    : public core::ControllerManagerBase<MyControllerInfo,
                                         MyController> {
public:
    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override {
        std::vector<std::unique_ptr<core::ControllerInfo>> results;

        for (const VendorDeviceSummary& device : vendorEnumerateDevices()) {
            auto info = std::make_unique<MyControllerInfo>(
                device.stableId,
                device.friendlyName,
                device.transportPath);

            // Discovery should describe the device and then get out of the way.
            info->setMaxPointRate(device.maxPointRate);
            info->setUsageState(probeUsageState(device));

            results.emplace_back(std::move(info));
        }

        return results;
    }
    std::shared_ptr<MyController>
    createController(const MyControllerInfo& info) override {
        if (!sdkReady()) {
            return nullptr;
        }
        return std::make_shared<MyController>();
    }

    core::ControllerManagerBase<MyControllerInfo, MyController>
        ::NewControllerDisposition
    prepareNewController(MyController& controller,
                         const MyControllerInfo& info) override {
        // Connect using the exact path discovered earlier rather than trying
        // to find the device again by a friendly label.
        if (!controller.connect(info.transportPath())) {
            return NewControllerDisposition::DropController;
        }

        controller.startThread();
        return NewControllerDisposition::KeepController;
    }

    void closeController(const std::string& key,
                         MyController& controller) override {
        (void)key;
        controller.close();
    }
};
```

`ControllerManagerBase` now owns the repetitive parts:

- typed `ControllerInfo` casting
- `managedType()` routing for fixed-type backends
- one-live-controller-per-key caching
- dropping a failed first connection from the cache
- shutdown snapshots and the default `stopThread()` loop

That means most built-in backends only need to implement:

- `discover()`
- `createController(...)`
- `prepareNewController(...)`
- optionally `prepareExistingController(...)`
- optionally `controllerKey(...)` if the reconnect key is not `info.idValue()`
- optionally `closeController(...)` / `beforeCloseControllers()` /
  `afterCloseControllers()` for backend-specific teardown

If the backend has a more unusual shape such as shared multi-controller device
runtimes, then deriving directly from `AbstractControllerManager` can still make
sense. `AVB` is the current example of that. It keeps its shared
runtime/configuration state in a dedicated backend helper and still reuses
`ControllerCache` directly for one-live-controller-per-id bookkeeping.

The important parts are:

- register the manager with `ControllerManagerRegistry` or `AddControllerManager(...)`
  so `System` will construct it and call `discover()`
- `id` should be stable across rescans. Prefer a serial number, MAC address,
  port path, or another physical identity instead of "device 0" style ordering.
- fixed-type backends should expose one `Info::controllerType()` helper and
  pass that into `ControllerInfo(...)`. `ControllerManagerBase` reuses the same
  value for `managedType()` automatically.
- if the live-controller cache should be keyed by something else such as a USB
  port path or a protocol unit id, override `controllerKey(...)`
- if discovery learns something needed to reconnect to the exact device later
  such as a USB port path, backend device id, or resolved network port, store
  it on the `ControllerInfo` subtype instead of repeating another fuzzy lookup
  during connect.
- if the backend is network-based, you can also fill the base
  `ControllerInfo::NetworkInfo` with the discovered IP and port.

## What the backend should own

The backend should own:

- hardware discovery/connection
- buffer or slot polling
- transport-specific request sizing
- payload encoding
- send timing and error handling

The backend should not own:

- frame queue scheduling
- hold-last-frame policy
- stale-frame dropping
- transition blanking between queued frames
- shared point post-processing such as startup blanking, shutdown blanking, or
  scanner-sync colour delay

## Current implementation

Both backend shapes now fit the shared content-source model in libera:

- `FrameQueue -> points` exists through `FrameScheduler`
- `FrameQueue -> frame` exists through `requestFrame(...)`
- `PointCallback -> points` exists through `LaserControllerStreaming`
- `PointCallback -> frame` exists through `LaserController::requestFrame(...)`

That means:

- a point-ingester backend can call `requestPoints(...)` and let the shared
  scheduler adapt either content source into point batches
- a frame-ingester backend can call `requestFrame(...)` and let the shared
  scheduler adapt either content source into whole-frame submissions, as long
  as it provides the transport's preferred frame size in `FrameFillRequest`
