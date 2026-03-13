// you only need to include libera.h if you want to use all available laser
// controller types. If you want to be selective about which ones you support, 
// include each one manually. See libera.h for the other files you need. 

#include "libera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace libera;

namespace {


// function that creates a frame of points to draw a rainbow circle. The phase
// value changes the colour shift. 

core::Frame makeCircleFrame(float phase, float bufferFillFraction) {

    constexpr std::size_t circlePoints = 500;
    constexpr float brightness = 0.2f;
    constexpr std::size_t dottedOnPoints = 3;
    constexpr std::size_t dottedOffPoints = 3;
    constexpr std::size_t dottedCyclePoints = dottedOnPoints + dottedOffPoints;
    const float clampedFill = std::clamp(bufferFillFraction, 0.0f, 1.0f);
    const std::size_t whitePointCount = static_cast<std::size_t>(
        std::lround(clampedFill * static_cast<float>(circlePoints)));

    core::Frame frame;
    frame.points.reserve(circlePoints);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < circlePoints; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(circlePoints);
        const float angle = t * tau;
        const float x = std::cos(angle);
        const float y = std::sin(angle);

        const float colourPhase = phase + t * tau;
        float r = (std::sin(colourPhase) + 1.0f) * 0.5f;
        float g = (std::sin(colourPhase + tau / 3.0f) + 1.0f) * 0.5f;
        float b = (std::sin(colourPhase + (2.0f * tau / 3.0f)) + 1.0f) * 0.5f;

        if (i < whitePointCount) {
            r = 1.0f;
            g = 1.0f;
            b = 1.0f;
        } else {
            // Keep the non-white segment dotted with a fixed 3-on / 3-off mask.
            // This uses the absolute point index so dots stay spatially static.
            const bool dotIsOn = (i % dottedCyclePoints) < dottedOnPoints;
            if (!dotIsOn) {
                r = 0.0f;
                g = 0.0f;
                b = 0.0f;
            }
        }

        frame.points.emplace_back(core::LaserPoint{
            x,
            y,
            r * brightness,
            g * brightness,
            b * brightness,
            0.0f,
            0.0f
        });
    }

    return frame;
}

float getBufferFillFraction(const std::optional<core::DacBufferState>& bufferState) {
    if (!bufferState || bufferState->totalBufferPoints <= 0) {
        return 0.0f;
    }
    return std::clamp(
        static_cast<float>(bufferState->pointsInBuffer) /
            static_cast<float>(bufferState->totalBufferPoints),
        0.0f,
        1.0f);
}

void printBufferState(const std::shared_ptr<core::LaserController>& dac,
                      const std::optional<core::DacBufferState>& bufferState,
                      int frameIndex,
                      int totalFrames) {
    const std::size_t queuedFrames = dac->queuedFrameCount();
    const bool readyForNewFrame = dac->isReadyForNewFrame();

    constexpr std::size_t queueBarSize = 2;
    const std::size_t filled =
        std::min<std::size_t>(queuedFrames, queueBarSize);

    std::string bar(queueBarSize, '-');
    for (std::size_t i = 0; i < filled; ++i) {
        bar[i] = '#';
    }

    // Keep output on one line: return to column zero and clear the line before
    // printing the latest status snapshot.
    std::cout << "\r\033[2K"
              << "frame " << (frameIndex + 1) << "/" << totalFrames
              << " dac_buffer ";
    if (bufferState && bufferState->totalBufferPoints > 0) {
        const int pointsInBuffer = std::clamp(
            bufferState->pointsInBuffer,
            0,
            bufferState->totalBufferPoints);
        const int percent = static_cast<int>(std::lround(
            (100.0 * static_cast<double>(pointsInBuffer)) /
            static_cast<double>(bufferState->totalBufferPoints)));
        std::cout << pointsInBuffer << "/" << bufferState->totalBufferPoints
                  << " (" << percent << "%)";
    } else {
        std::cout << "n/a";
    }
    std::cout << " queue[" << bar << "]"
              << " queued=" << queuedFrames
              << " state=" << (readyForNewFrame ? "ready" : "busy");

    const auto latencyStats = dac->getLatencyStats();
    if (latencyStats) {
        std::cout << std::fixed << std::setprecision(2)
                  << " latency_ms p50/p95/p99 "
                  << latencyStats->p50Ms << "/"
                  << latencyStats->p95Ms << "/"
                  << latencyStats->p99Ms
                  << " n=" << latencyStats->sampleCount
                  << std::defaultfloat;
    } else {
        std::cout << " latency_ms n/a";
    }

        std::cout
                  << "   "
                  << std::flush;
}

std::optional<std::error_code> getFatalTransportError(
    const std::shared_ptr<core::LaserController>& dac) {
    auto etherDream = std::dynamic_pointer_cast<etherdream::EtherDreamController>(dac);
    if (!etherDream || etherDream->hasActiveConnection()) {
        return std::nullopt;
    }
    return etherDream->networkError();
}


} // namespace


int main() {

    core::GlobalDacManager dacManager;

    libera::logInfo("Waiting for DACs to be discovered...");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    // get all the discovered controllers from the dacManager
    std::vector<std::unique_ptr<core::DacInfo>> results = dacManager.discoverAll();

    if (results.empty()) {
        libera::logInfo("No controllers discovered.");
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


    std::shared_ptr<core::LaserController> dac = dacManager.getAndConnectToDac(*results[choice]);
    if (!dac) {
        libera :: logError("Failed to acquire DAC from manager.");
        return 1;
    }

    dac->setArmed(true); 
    
    const float phaseStep = 0.05f;
    float phase = 0.0f;
    float lastKnownBufferFillFraction = 0.0f;
    auto lastStatusPrint = std::chrono::steady_clock::now();

    const int totalFrames = 3000;
    for (int i = 0; i < totalFrames; ++i) {
        if (auto fatalError = getFatalTransportError(dac)) {
            std::cout << std::endl;
            libera::logError("Selected DAC connection failed.", fatalError->message());
            dacManager.close();
            return 1;
        }

        while (!dac->isReadyForNewFrame()){
            if (auto fatalError = getFatalTransportError(dac)) {
                std::cout << std::endl;
                libera::logError("Selected DAC connection failed.", fatalError->message());
                dacManager.close();
                return 1;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - lastStatusPrint >= std::chrono::milliseconds(50)) {
                const auto bufferState = dac->getBufferState();
                if (bufferState) {
                    lastKnownBufferFillFraction = getBufferFillFraction(bufferState);
                }
                printBufferState(dac, bufferState, i, totalFrames);
                lastStatusPrint = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const auto bufferState = dac->getBufferState();
        if (bufferState) {
            lastKnownBufferFillFraction = getBufferFillFraction(bufferState);
        }

        core::Frame frame = makeCircleFrame(phase, lastKnownBufferFillFraction);
        if (!dac->sendFrame(std::move(frame))) {
            logError("Failed to queue frame ", i);
        }

        printBufferState(dac, bufferState, i, totalFrames);

        phase += phaseStep;
    }
    std::cout << std::endl;

    dacManager.close();
    libera::logInfo("Done.");

    return 0;
}
