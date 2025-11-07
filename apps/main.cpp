#include "libera/core/DacDiscovery.hpp"
#include "libera/etherdream/EtherDreamDevice.hpp"
#include "libera/etherdream/EtherDreamDiscoverer.hpp"
#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/log/Log.hpp"
#include <chrono>
#include <thread>
#include <cmath>
#include <limits>

using namespace libera;

namespace {

void installCirclePointsCallback(etherdream::EtherDreamDevice& device) {
    device.setRequestPointsCallback(
        [](const core::PointFillRequest& req, std::vector<core::LaserPoint>& out) {
            static const std::vector<core::LaserPoint> circle = []{
                constexpr std::size_t kCirclePoints = 500;
                std::vector<core::LaserPoint> pts;
                pts.reserve(kCirclePoints);
                const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
                for (std::size_t i = 0; i < kCirclePoints; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(kCirclePoints);
                    const float angle = t * tau;
                    const float x = std::cos(angle);
                    const float y = std::sin(angle);

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

                    constexpr float brightness = 0.2f;
                    pts.emplace_back(core::LaserPoint{
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
                return pts;
            }();

            static std::size_t cursor = 0;

            if (circle.empty()) {
                return;
            }

            std::size_t minNeeded = req.minimumPointsRequired;
            if (minNeeded == 0) {
                minNeeded = circle.size(); // default to a full revolution
            }

            const std::size_t maxAllowed =
                req.maximumPointsRequired == 0
                    ? std::numeric_limits<std::size_t>::max()
                    : req.maximumPointsRequired;

            if (req.maximumPointsRequired > 0 && minNeeded > req.maximumPointsRequired) {
                minNeeded = req.maximumPointsRequired;
            }

            std::size_t target = std::max(minNeeded, circle.size());
            target = std::min(target, maxAllowed);

            if (target == 0) {
                return;
            }

            std::size_t produced = 0;
            while (produced < target) {
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
    etherdream::EtherDreamDevice etherdream;
    installCirclePointsCallback(etherdream);

    core::DacDiscoveryManager discoveryManager;
    const auto discoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<std::unique_ptr<core::DiscoveredDac>> results;
    while (std::chrono::steady_clock::now() < discoveryDeadline) {
        results = discoveryManager.discoverAll();
        if (!results.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (results.empty()) {
        logError("No EtherDream devices discovered after timeout.\n");
        return 1;
    }

    auto* info = dynamic_cast<etherdream::EtherDreamDeviceInfo*>(results.front().get());
    if (!info) {
        logError("First discovery result is not an EtherDream device.\n");
        return 1;
    }

    if (auto result = etherdream.connect(*info); !result) {
        const auto err = result.error();
        logError("Connect failed: ", err.message(),
                                " (", err.category().name(), ":", err.value(), ")\n");
    } else { 
        // Step 5: Start the device worker thread (calls EtherDreamDevice::run()).
        logInfo("Starting dummy run...\n");
        etherdream.start();

        // Keep main alive long enough for the worker to do a few ticks.
        std::this_thread::sleep_for(std::chrono::seconds(30));

        // Step 6: Stop the device worker and close the socket if you connected.
        etherdream.stop();
        etherdream.close();
        logInfo("Done.\n");
    }
    

    return 0;
}
