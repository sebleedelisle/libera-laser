#include "libera/core/GlobalDacManager.hpp"
#include "libera/core/LaserDevice.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/helios/HeliosManager.hpp"
#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace libera;

namespace {

constexpr std::size_t kCirclePoints = 500;
constexpr float kBrightness = 0.2f;

core::Frame makeCircleFrame(float phase) {
    core::Frame frame;
    frame.points.reserve(kCirclePoints);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < kCirclePoints; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kCirclePoints);
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
            r * kBrightness,
            g * kBrightness,
            b * kBrightness,
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

    logInfo("Waiting for DACs to be discovered...");
    constexpr auto discoveryTimeout = std::chrono::milliseconds(3000);
    constexpr auto discoveryPollInterval = std::chrono::milliseconds(100);
    const auto discoveryStart = std::chrono::steady_clock::now();

    std::vector<std::unique_ptr<core::DacInfo>> results;
    do {
        results = dacManager.discoverAll();
        if (!results.empty()) {
           // break;
        }
        std::this_thread::sleep_for(discoveryPollInterval);
    } while (std::chrono::steady_clock::now() - discoveryStart < discoveryTimeout);

    if (results.empty()) {
        logError("No devices discovered after timeout.");
        return 1;
    }

    logInfo("Discovered DACs:");
    for (std::size_t idx = 0; idx < results.size(); ++idx) {
        const auto& entry = results[idx];
        logInfo(idx, entry->labelValue(), "type", entry->type());
    }

    std::size_t choice = 0;
    //if (results.size() > 1) {
        logInfo("Select DAC index: ");
        if (!(std::cin >> choice) || choice >= results.size()) {
            logError("Invalid selection.");
            return 1;
        }
    //}

    std::shared_ptr<core::LaserDevice> dac = dacManager.getAndConnectToDac(*results[choice]);
    if (!dac) {
        logError("Failed to acquire DAC from manager.");
        return 1;
    }
    dac->setArmed(true); 
    constexpr float scannerSyncTestValue = 5.0f; // 0.5 ms expressed in 1/10,000 s units
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

        dac->setScannerSync(0);
       //dac->setScannerSync(scannerSyncTestValue * (std::sin(phase) + 1.0f));
        phase += phaseStep;
    }

    dacManager.close();
    logInfo("Done.");

    return 0;
}
