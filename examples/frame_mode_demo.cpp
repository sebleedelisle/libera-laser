// you only need to include libera.h if you want to use all available laser
// controller types. If you want to be selective about which ones you support, 
// include each one manually. See libera.h for the other files you need. 

#include "libera.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace libera;

namespace {


// function that creates a frame of points to draw a rainbow circle. The phase
// value changes the colour shift. 

core::Frame makeCircleFrame(float phase) {

    constexpr std::size_t circlePoints = 500;
    constexpr float brightness = 0.2f;

    core::Frame frame;
    frame.points.reserve(circlePoints);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < circlePoints; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(circlePoints);
        const float angle = t * tau;
        const float x = std::cos(angle);
        const float y = std::sin(angle);

        const float colourPhase = phase + t * tau;
        const float r = (std::sin(colourPhase) + 1.0f) * 0.5f;
        const float g = (std::sin(colourPhase + tau / 3.0f) + 1.0f) * 0.5f;
        const float b = (std::sin(colourPhase + (2.0f * tau / 3.0f)) + 1.0f) * 0.5f;

        frame.points.emplace_back(core::LaserPoint{
            x,
            y,
            r * brightness,
            g * brightness,
            b * brightness,
            1.0f,
            0.0f,
            0.0f
        });
    }

    return frame;
}


} // namespace


int main() {

    core::GlobalDacManager dacManager;

    libera::logInfo("Waiting for DACs to be discovered...");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    // get all the discovered controllers from the dacManager
    std::vector<std::unique_ptr<core::DacInfo>> results = dacManager.discoverAll();

    if (results.empty()) {
        libera::logInfo("No devices discovered.");
        return 1;
    }

    // display list and offer selection to user
    libera::logInfo("Discovered DACs: ");
    for (std::size_t idx = 0; idx < results.size(); ++idx) {
        const auto& entry = results[idx];
        libera::logInfo(idx, entry->labelValue(), "type", entry->type());
    }

    std::size_t choice = 0;
    libera::logInfo("Select DAC index: ");
    if (!(std::cin >> choice) || choice >= results.size()) {
        libera::logError("Invalid selection.");
        return 1;
    }


    std::shared_ptr<core::LaserDevice> dac = dacManager.getAndConnectToDac(*results[choice]);
    if (!dac) {
        libera :: logError("Failed to acquire DAC from manager.");
        return 1;
    }

    dac->setArmed(true); 
    
    const float phaseStep = 0.05f;
    float phase = 0.0f;

    const int totalFrames = 3000;
    for (int i = 0; i < totalFrames; ++i) {
        while (!dac->isReadyForNewFrame()){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        core::Frame frame = makeCircleFrame(phase);
        if (!dac->sendFrame(std::move(frame))) {
            logError("Failed to queue frame ", i);
        }

        phase += phaseStep;
    }

    dacManager.close();
    libera::logInfo("Done.");

    return 0;
}
