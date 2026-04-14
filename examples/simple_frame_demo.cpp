#include "libera.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
using namespace libera;

namespace {

core::Frame makeWhiteCircleFrame(int framenum) {
    // A frame is just a list of laser points. The library manages frame queuing 
    // and buffering automatically so you don't have to worry about managing a 
    // stream. 
    core::Frame frame;

    // Makes a simple circle of point with brightness at 20% for safety.  
    // The library works with normalised coordinates:
    // -1..1 is the full scan area, 0 is the centre.
    constexpr std::size_t pointCount = 360;
    constexpr float radius = 0.7f;
    constexpr float brightness = 0.2f;

    frame.points.reserve(pointCount);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(pointCount);
        const float angle = t * tau;
        const float size = 1.0f * std::cos((float)framenum*0.05); 

        // LaserPoint is the library's common sample format:
        // x/y   = scanner position in the normalised range -1..+1
        // r/g/b = colour channels, in the range 0..1

        frame.points.emplace_back(core::LaserPoint{
            radius * std::cos(angle) * size, // x
            radius * std::sin(angle) * size, // y
            brightness, // red
            brightness, // green
            brightness  // blue
        });
    }

    return frame;
}


} // namespace

int main() {
    // This is the single top-level object. It
    // handles discovery across all controller types, creation and clean destruction
    // of controller objects.
    System liberaSystem;

    logInfo("Searching for controllers...");

    std::shared_ptr<core::LaserController> controller;

    while (!controller) {

        // discoverControllers returns all controllers currently visible through
        // the registered backend managers.
        std::vector<std::unique_ptr<core::ControllerInfo>> discovered =
            liberaSystem.discoverControllers();
        if (!discovered.empty()) {
            const core::ControllerInfo& firstDevice = *discovered.front();
            
            logInfo("Found controller:",
                    firstDevice.labelValue(),
                    "type",
                    firstDevice.type());

            // connectController() asks the matching backend manager to return a
            // controller for this ControllerInfo and start it if needed. The returned
            // LaserController gives us the common frame/streaming API regardless
            // of which concrete controller type was discovered.
            controller = liberaSystem.connectController(firstDevice);
            if (!controller) {
                logError("A device was discovered, but the library could not create a controller.");
                liberaSystem.shutdown();
                return 1;
            }
            
            break; 
        }

        std::this_thread::sleep_for(250ms);
    }



   

    // Controllers stay dark until armed. This gives the application an
    // explicit "yes, it is safe to emit light now" step after discovery and
    // connection have completed.
    controller->setArmed(true);

    // This loop shows the frame-mode usage pattern:
    // 1. Ask the controller whether it is ready for another frame.
    // 2. If it is, build one frame and submit it.
    // 3. If it is not, wait and check again.
    //
    // In this demo the artwork is always the same white circle, so we submit
    // the same drawing 100 times. Real applications usually
    // change the frame contents each time around this loop.


    std::size_t submittedFrameCount = 0;
    logInfo("Sending white circle frames...");

    while (submittedFrameCount < 1000) {
        if (!controller->isReadyForNewFrame()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        core::Frame frame = makeWhiteCircleFrame(submittedFrameCount);

        // sendFrame() hands the frame over to the library's internal scheduler.
        // The concrete backend then pulls either point batches or whole frames
        // from that shared queue depending on what the transport expects.
        if (controller->sendFrame(std::move(frame))) {
            logInfo("Sent frame number ", submittedFrameCount);
            submittedFrameCount++;
        }

        
    }

    // shutdown() stops the backend managers and cleanly destroys all controllers.
    liberaSystem.shutdown();

    logInfo("Done. Submitted frames:", submittedFrameCount);
    return 0;
}
