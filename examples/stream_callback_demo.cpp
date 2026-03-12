#include "libera.h"
#include <chrono>
#include <thread>
#include <cmath>
#include <limits>
#include <iostream>
#include <memory>
#include <vector>

using namespace libera;

namespace {

void installCirclePointsCallback(const std::shared_ptr<core::LaserController>& device) {
    // Register a callback that continuously feeds a coloured circle to the DAC.
    // The lambda keeps all state internally (static precomputed points + cursor)
    // so the outer application only has to install it once.
    device->setRequestPointsCallback(
        [](const core::PointFillRequest& req, std::vector<core::LaserPoint>& out) {

            // Precompute the actual circle once and reuse it for every invocation.
            // This keeps the hot path allocation-free and guarantees identical
            // geometry for each revolution.
            static const std::vector<core::LaserPoint> circle = [] {
                constexpr std::size_t circlePoints = 500;
                std::vector<core::LaserPoint> pts;
                pts.reserve(circlePoints);

                const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
                for (std::size_t i = 0; i < circlePoints; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(circlePoints);
                    const float angle = t * tau;
                    const float x = std::cos(angle);
                    const float y = std::sin(angle);

                    // Colour-code the quadrants so you can see orientation on the wall.
                    float r = 0.0f;
                    float g = 0.0f;
                    float b = 0.0f;
                    if (x >= 0.0f && y >= 0.0f) {
                        r = g = b = 1.0f; // Quadrant I - white.
                    } else if (x < 0.0f && y >= 0.0f) {
                        r = 1.0f;         // Quadrant II - red.
                    } else if (x < 0.0f && y < 0.0f) {
                        g = 1.0f;         // Quadrant III - green.
                    } else {
                        b = 1.0f;         // Quadrant IV - blue.
                    }

                    constexpr float brightness = 0.2f; // keep scanners happy.
                    pts.emplace_back(core::LaserPoint{
                        x,
                        y,
                        r * brightness,
                        g * brightness,
                        b * brightness,
                        1.0f,  // intensity
                        0.0f,  // u (unused)
                        0.0f   // v (unused)
                    });
                }
                return pts;
            }();

            static std::size_t cursor = 0; // remembers where the last callback stopped.

            if (circle.empty()) {
                return;
            }

            // Honour the scheduler contract:
            //  * produce at least minimumPointsRequired (or a whole circle if it asked for zero)
            //  * never exceed maximumPointsRequired

            if (req.maximumPointsRequired == 0) {
                return; // no room to push additional samples yet
            }
            const std::size_t requestedMin =
                req.minimumPointsRequired == 0 ? circle.size() : req.minimumPointsRequired;
            const std::size_t minBatch = std::max(requestedMin, circle.size());

            const std::size_t maxAllowed = req.maximumPointsRequired;

            const std::size_t target = std::min(minBatch, maxAllowed);
            if (target == 0) {
                return;
            }

            std::size_t produced = 0;
            while (produced < target) {
                // Copy as large a chunk as possible without wrapping, then loop
                // around to the head of the circle buffer if needed.
                const std::size_t remaining = target - produced;
                const std::size_t available = circle.size() - cursor;
                const std::size_t chunk = std::min(remaining, available);
                if (chunk == 0) {
                    cursor = 0;
                    continue;
                }
                out.insert(out.end(),
                           circle.begin() + cursor,
                           circle.begin() + cursor + chunk);

                cursor = (cursor + chunk) % circle.size();
                produced += chunk;
            }
        });
}

} // namespace

int main() {

    core::GlobalDacManager dacManager;

    logInfo("Waiting for DACs to be discovered..."); 
    constexpr auto discoveryTimeout = std::chrono::milliseconds(1500);
    constexpr auto discoveryPollInterval = std::chrono::milliseconds(100);
    const auto discoveryStart = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<core::DacInfo>> results;
    do {
        results = dacManager.discoverAll();
        if (!results.empty()) {
            break; // stop waiting as soon as at least one DAC is found
        }
        std::this_thread::sleep_for(discoveryPollInterval);
    } while (std::chrono::steady_clock::now() - discoveryStart < discoveryTimeout);

    if (results.empty()) {
        logError("No  devices discovered after timeout.");
        return 1;
    }

    logInfo("Discovered DACs:");
    for (std::size_t idx = 0; idx < results.size(); ++idx) {
        const auto& entry = results[idx];
        logInfo(idx, entry->labelValue(), "type", entry->type());
    }

    std::size_t choice = 0;
    if(results.size()>1) { 
        
        logInfo("Select DAC index: ");
        if (!(std::cin >> choice) || choice >= results.size()) {
            logError("Invalid selection.");
            return 1;
        }
    }

    // the discovery manager getAndConnectToDac should create and connect the dac if it hasn't already. 
    // if it has already it just returns the existing dac

    std::shared_ptr<core::LaserController> dac = dacManager.getAndConnectToDac(*results[choice]); 
    if (!dac) {
        logError("Failed to acquire DAC from manager.");
        return 1;
    }
    
    installCirclePointsCallback(dac); 

    const float phaseStep = 0.05f;          // smaller => slower change
    constexpr float scannerSyncBaseUnits = 5.0f; // 0.5 ms expressed in 1/10,000 s units
    float phase = 0.f;

    for (int i = 0; i < 300; ++i) {
        dac->setScannerSync(scannerSyncBaseUnits * (std::sin(phase) + 1.f));

        phase += phaseStep;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // or duration<double>(0.1)
    }


    

    // dacManager closes down all dacs safely
    dacManager.close(); 

    logInfo("Done.");
    

    return 0;
}
