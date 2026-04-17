# Integrating Libera into your laser app

This guide walks through adding Libera to an application that needs to send points
to a laser. If you have never worked with laser control software before, don't
worry — the terms are explained as they come up.

## What Libera does for you

Libera is a C++ library for anyone that makes laser software. It handles all
of the discovery and communication for a wide variety of laser **controllers** -
the hardware boxes (or network devices) that take point data from your computer
and turn it into the signals that drive a display laser's colours, brightness and
scanning mirrors. Libera gives you:

- **Discovery** — a single call that finds every supported controller currently
  visible on your machine and network, regardless of brand.
- **A common API** — once you've connected to a controller, the rest of your
  code looks the same whether you're talking to an Ether Dream, a Helios, a
  LaserCube, or anything else Libera supports.
- **Streaming and frame buffering** — it handles the timing-sensitive work of
  keeping each controller's buffer topped up so the laser output stays smooth.

> **A note on terminology.** You may have heard these devices called *DACs*
> (digital-to-analogue converters) elsewhere. Libera calls them **controllers**,
> because modern devices do more than just convert signals — and some newer
> ones don't convert to analogue at all. Similarly, the scanning show laser
> itself is just called a **laser**, not a "projector".

## The minimum you need to know

Everything flows through one top-level object:

```cpp
#include "libera.h"

using namespace libera;

System liberaSystem;
```

Creating a `System` registers all the built-in controller types and gets
discovery ready to go. When your app shuts down, call `liberaSystem.shutdown()`
to stop background threads and close every open controller cleanly.

### Include patterns

The simplest include pulls everything in:

```cpp
#include "libera.h"
```

If you like, you can instead include just the controllers you want to support:

```cpp
#include "libera/System.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosManager.hpp"
```

Only the built-in controller types you include will be discovered.

Plugins are loaded separately from a few default search locations, so in the
normal case you can just construct `System` and drop plugin libraries into a
`plugins/` folder next to your app:

```cpp
libera::System liberaSystem;
```

Only configure plugin search paths if you want to override those defaults:

```cpp
libera::System::setPluginDirectory("plugins");
libera::System::addPluginDirectory("/absolute/path/to/more/plugins");

libera::System liberaSystem;
```

## Step 1 — discover controllers

`discoverControllers()` returns a list of `ControllerInfo` objects describing
what Libera can currently see:

```cpp
auto discovered = liberaSystem.discoverControllers();

for (const auto& info : discovered) {
    std::cout << info->labelValue()        // e.g. "Ether Dream 1234"
              << " (" << info->type() << ")\n";
}
```

Discovery is cheap to call repeatedly. In a real app you'd typically call it on
a timer (once per second or so) or while the user is on a "select device" screen,
because USB devices come and go and network devices take a moment to appear.

Each `ControllerInfo` is just a description — no hardware connection has been
opened yet.

## Step 2 — connect to a controller

Pass the `ControllerInfo` you want to `connectController()`:

```cpp
std::shared_ptr<core::LaserController> controller =
    liberaSystem.connectController(*discovered.front());

if (!controller) {
    // Connection failed — log and move on.
    return;
}
```
This will create and connect to a `LaserController` instance, and return a 
`shared_ptr` to that instance. If the controller is already connected, 
you'll get back the existing instance.

You can hold onto the `shared_ptr` for the lifetime of your laser output.

Once connected, the controller exposes the same identity fields that were on
the `ControllerInfo`, so you don't need to keep the info object around:

```cpp
std::cout << controller->getName() << "\n";  // e.g. "Ether Dream 1234"
std::cout << controller->getID()   << "\n";  // stable unique identifier
```

## Step 3 — arm the controller

Controllers start in an **unarmed** state and will not emit any light until you
explicitly arm them. This is a deliberate safety step:

```cpp
controller->setArmed(true);
```

Call `setArmed(false)` any time you want output to stop immediately (e.g. when
the user presses an e-stop button, the window loses focus, or the app is
closing).

The idea is that you surface the arming mechanism to the user but it can also act 
as an laser enable / disable for simpler apps. 

## The point format

Both of the ways you'll send data to the laser (described in the next section)
are built on the same building block: the `LaserPoint`. One `LaserPoint` is a
single sample — a single position with a single colour — that the scanner will
visit for a tiny fraction of a second.

```cpp
struct LaserPoint {
    float x  = 0.0f;   // horizontal position, -1..+1
    float y  = 0.0f;   // vertical position,   -1..+1
    float r  = 0.0f;   // red,   0..1
    float g  = 0.0f;   // green, 0..1
    float b  = 0.0f;   // blue,  0..1
    float i  = 1.0f;   // legacy master intensity, 0..1
    float u1 = 0.0f;   // user field (extensions: waveforms, safety masks, ...)
    float u2 = 0.0f;   // user field
};
```

- **`x`, `y`** use **normalised coordinates**: `-1..+1` spans the full scan
  area, with `0, 0` at the centre. This is independent of the hardware — you
  never need to know the device's native resolution.
- **`r`, `g`, `b`** are colour channels in the range `0..1`. `0, 0, 0` is a
  **blanked** point (the laser is dark but the scanner still moves to that
  position), which is how you draw separate shapes without a line between them.
- **`i`** is a legacy master-intensity channel kept around for controllers
  that still require it. Leave it at `1.0` unless you know you need it.
- **`u1`, `u2`** are user fields reserved for extensions — things like
  waveform synthesis or per-point safety masks. Ignore them for normal use.

A couple of example points:

```cpp
core::LaserPoint red  { 0.5f,  0.5f, 1.0f, 0.0f, 0.0f };   // top-right, red
core::LaserPoint dark {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f };   // top-left,  blanked
core::LaserPoint green{-0.5f, -0.5f, 0.0f, 1.0f, 0.0f };   // bottom-left, green
```

> **Safety tip for testing.** Scanning lasers are bright. While you're
> developing, keep your colour values low (say `0.1`–`0.2`) until you're
> confident your geometry is doing what you expect.

## Step 4 — send points

Libera gives you two ways to feed points to a controller. Both are supported on
every controller type; pick whichever suits your app.

Only one content source is active at a time. Installing a point callback clears
any queued frames; queueing a frame later switches the controller back to the
shared frame queue automatically.

### Frame mode (easier)

A **frame** is simply a list of points that make up one drawing — one circle,
one line of text, one animated shape at one instant in time. In frame mode you
build a whole frame, hand it to Libera, and Libera worries about how fast to
stream it to the hardware.

```cpp
core::Frame frame;
frame.points.push_back(core::LaserPoint{ 0.5f, 0.5f, 1.0f, 0.0f, 0.0f });  // red
frame.points.push_back(core::LaserPoint{-0.5f, 0.5f, 0.0f, 1.0f, 0.0f });  // green
// ...add more points...

if (controller->isReadyForNewFrame()) {
    controller->sendFrame(std::move(frame));
}
```

Typical loop:

```cpp
while (running) {
    if (controller->isReadyForNewFrame()) {
        controller->sendFrame(buildFrameForThisMoment());
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

`isReadyForNewFrame()` returns `true` when the internal queue has room.
`sendFrame()` hands ownership of the frame over to Libera, which then feeds it
to the hardware at the right pace.

**Hold time and auto-blanking.** By default, if no new frame arrives within
100 ms the last frame stops looping and output goes blank automatically. This
means your laser turns off cleanly if your app pauses or stops sending — no
extra code required. You can adjust the window with `setMaxFrameHoldTime()`,
or set it to zero to loop the last frame indefinitely:

```cpp
// Blank after 200 ms of no new frames (e.g. for slower frame rates):
core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(200));

// Loop the last frame forever (legacy behaviour):
core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));
```

### Streaming mode (more control)

In streaming mode, instead of pushing whole frames, you register a **callback**
that Libera calls whenever a controller needs more points. This is closer to
how audio engines work and is a good fit if your source material is naturally
continuous (a running oscillator, a generative pattern, a live input).

```cpp
controller->setPointCallback(
    [](const core::PointFillRequest& req,
       std::vector<core::LaserPoint>& out) {

        // Produce at least req.minimumPointsRequired points,
        // and no more than req.maximumPointsRequired.
        // Append them to `out`.
    });
```

The callback runs on a Libera-owned thread, so keep it quick and don't block.
If you share state between the callback and your main thread, protect it with a
lock or a lock-free structure.

## Step 5 — shut down cleanly

When you're done:

```cpp
controller->setArmed(false);   // stop emitting light
liberaSystem.shutdown();       // stop backends, close every controller
```

`shutdown()` is safe to call from your app's normal exit path; it joins
background threads and releases USB and network resources.

## Monitoring controller health

Every controller exposes a small status API you can poll from your UI:

```cpp
auto status = controller->getStatus();   // Good / Issues / Error
auto errors = controller->getErrors();   // list of {code, label, count}
controller->clearErrors();               // reset counters
```

Error codes are grouped by transport:

- Network controllers report `network.*` codes (`network.timeout`,
  `network.packet_loss`, `network.buffer_underflow`, ...).
- USB controllers report `usb.*` codes (`usb.timeout`, `usb.transfer_failed`,
  `usb.connection_lost`, ...).

A quick way to surface problems in your UI is to show a green/yellow/red dot
based on `getStatus()` and reveal the error list if the user clicks it.

## Putting it all together

The shortest useful program looks like this:

```cpp
#include "libera.h"

using namespace libera;

int main() {
    System liberaSystem;

    auto discovered = liberaSystem.discoverControllers();
    if (discovered.empty()) return 1;

    auto controller = liberaSystem.connectController(*discovered.front());
    if (!controller) return 1;

    controller->setArmed(true);

    while (/* running */) {
        if (controller->isReadyForNewFrame()) {
            controller->sendFrame(buildNextFrame());
        }
    }

    liberaSystem.shutdown();
}
```

For fuller examples — including a streaming-callback demo and a multi-device
selector — see the `examples/` directory in this repository.
