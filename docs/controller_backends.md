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
the plugin host path, and IDN currently behave.

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
