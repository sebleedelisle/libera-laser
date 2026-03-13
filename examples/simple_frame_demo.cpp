#include "libera.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace libera;

namespace {

core::Frame makeWhiteCircleFrame() {
    // A frame is just a list of laser points. In frame mode the library turns
    // this list into the streaming callback internally, so application code can
    // think in whole drawings instead of "fill whatever buffer space is free".
    core::Frame frame;

    // Keep the geometry modest and the brightness conservative so the example
    // is easy on most scanners. The library works with normalised coordinates:
    // -1..1 is the full scan area, 0 is the centre.
    constexpr std::size_t pointCount = 360;
    constexpr float radius = 0.7f;
    constexpr float brightness = 0.2f;

    frame.points.reserve(pointCount);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(pointCount);
        const float angle = t * tau;

        // LaserPoint is the library's common sample format:
        // x/y  = mirror position
        // r/g/b = colour channels
        // u1/u2 = spare extension fields that this example does not use
        frame.points.emplace_back(core::LaserPoint{
            radius * std::cos(angle),
            radius * std::sin(angle),
            brightness, // red
            brightness, // green
            brightness, // blue
            0.0f,       // u1 unused here
            0.0f        // u2 unused here
        });
    }

    // Leaving frame.time at its default means "play as soon as possible".
    // LaserController::sendFrame() will stamp it using the global target render
    // latency. By default that latency is zero, so this example starts
    // immediately after the controller accepts the frame.
    return frame;
}

std::shared_ptr<core::LaserController> waitForFirstDac(core::GlobalDacManager& dacManager) {
    // GlobalDacManager owns one manager per supported DAC family. Including
    // libera.h above registers all built-in managers, so discoverAll() fans out
    // to every backend and returns one merged list of DacInfo objects.
    constexpr auto discoveryPollInterval = std::chrono::milliseconds(250);

    logInfo("Searching for DACs...");

    while (true) {
        std::vector<std::unique_ptr<core::DacInfo>> discovered = dacManager.discoverAll();
        if (!discovered.empty()) {
            const core::DacInfo& firstDevice = *discovered.front();

            logInfo("Found DAC:",
                    firstDevice.labelValue(),
                    "type",
                    firstDevice.type());

            // getAndConnectToDac() asks the matching backend manager to return a
            // controller for this DacInfo and start it if needed. The returned
            // LaserController gives us the common frame/streaming API regardless
            // of which concrete DAC type was discovered.
            return dacManager.getAndConnectToDac(firstDevice);
        }

        std::this_thread::sleep_for(discoveryPollInterval);
    }
}

} // namespace

int main() {
    // This is the single top-level object most applications start with. It
    // handles discovery across all registered DAC backends and also gives us a
    // single place to close every controller cleanly at the end.
    core::GlobalDacManager dacManager;

    std::shared_ptr<core::LaserController> dac = waitForFirstDac(dacManager);
    if (!dac) {
        logError("A device was discovered, but the library could not create a controller.");
        dacManager.close();
        return 1;
    }

    // Controllers stay dark until armed. This gives the application an
    // explicit "yes, it is safe to emit light now" step after discovery and
    // connection have completed.
    dac->setArmed(true);

    // We only need to queue one frame. The frame-mode scheduler built into
    // LaserController keeps replaying the current frame until a newer frame is
    // due, so one circle frame is enough to hold a static image on the output.
    core::Frame frame = makeWhiteCircleFrame();

    // sendFrame() may return false if the controller is temporarily not ready
    // for another frame yet. This small wait loop keeps the example reliable
    // without introducing any extra concepts.
    while (!dac->sendFrame(std::move(frame))) {
        if (!dac->isReadyForNewFrame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        logError("Failed to queue the circle frame.");
        dacManager.close();
        return 1;
    }

    logInfo("Sending white circle for 10 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // close() tells every backend manager to stop and tear down its active
    // controllers. That stops the worker threads and closes USB/network
    // connections in one place.
    dacManager.close();
    logInfo("Done.");
    return 0;
}
