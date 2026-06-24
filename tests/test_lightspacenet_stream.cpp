#include "libera/lightspacenet/LightSpaceNetManager.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace libera;
using namespace libera::core;
using namespace libera::lightspacenet;

namespace {

std::vector<LaserPoint> makeDimSquareFrame() {
    constexpr std::size_t pointsPerEdge = 80;
    constexpr std::size_t blankPoints = 16;
    constexpr float brightness = 0.03f;

    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
    };

    const Vertex vertices[] = {
        {-0.35f, -0.35f},
        { 0.35f, -0.35f},
        { 0.35f,  0.35f},
        {-0.35f,  0.35f},
    };

    std::vector<LaserPoint> frame;
    frame.reserve((pointsPerEdge + blankPoints) * 4 + blankPoints);

    auto addLine = [&](Vertex from, Vertex to, bool lit) {
        const std::size_t count = lit ? pointsPerEdge : blankPoints;
        for (std::size_t i = 0; i < count; ++i) {
            const float t = count > 1
                ? static_cast<float>(i) / static_cast<float>(count - 1)
                : 0.0f;
            LaserPoint point{};
            point.x = from.x + ((to.x - from.x) * t);
            point.y = from.y + ((to.y - from.y) * t);
            if (lit) {
                point.r = brightness;
                point.g = brightness;
                point.b = brightness;
                point.i = brightness;
            }
            frame.push_back(point);
        }
    };

    addLine({0.0f, 0.0f}, vertices[0], false);
    for (std::size_t i = 0; i < 4; ++i) {
        addLine(vertices[i], vertices[(i + 1) % 4], true);
        addLine(vertices[(i + 1) % 4], vertices[(i + 1) % 4], false);
    }

    return frame;
}

std::shared_ptr<LaserController> waitForController(LightSpaceNetManager& manager) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        auto discovered = manager.discover();
        if (!discovered.empty()) {
            for (const auto& info : discovered) {
                logInfo("Found LightSpace Net controller:",
                        info->labelValue(),
                        "id",
                        info->idValue());
            }
            return manager.connectController(*discovered.front());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return nullptr;
}

} // namespace

int main() {
    logInfo("=== LightSpace Net hardware stream test ===");
    logInfo("This sends a small dim square for 3 seconds after discovery.");

    LightSpaceNetManager manager;
    auto controller = waitForController(manager);
    if (!controller) {
        logError("No LightSpace Net controller found on UDP port 25555.");
        manager.closeAll();
        return 1;
    }

    constexpr std::uint32_t pointRate = 30000;
    controller->setPointRate(pointRate);

    const auto frame = makeDimSquareFrame();
    std::atomic<std::uint64_t> totalPoints{0};
    std::atomic<std::uint64_t> callbackCalls{0};

    controller->setPointCallback(
        [&](const PointFillRequest& request, std::vector<LaserPoint>& output) {
            callbackCalls.fetch_add(1, std::memory_order_relaxed);
            const auto start = totalPoints.load(std::memory_order_relaxed);
            const auto count = request.maximumPointsRequired;
            for (std::size_t i = 0; i < count; ++i) {
                output.push_back(frame[(start + i) % frame.size()]);
            }
            totalPoints.fetch_add(count, std::memory_order_relaxed);
        });

    controller->setArmed(true);

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto status = controller->getStatus();
        const auto buffer = controller->getBufferState();
        if (buffer) {
            logInfo("status",
                    static_cast<int>(status),
                    "buffer",
                    buffer->pointsInBuffer,
                    "/",
                    buffer->totalBufferPoints,
                    "callbacks",
                    callbackCalls.load(std::memory_order_relaxed),
                    "points",
                    totalPoints.load(std::memory_order_relaxed));
        } else {
            logInfo("status",
                    static_cast<int>(status),
                    "callbacks",
                    callbackCalls.load(std::memory_order_relaxed),
                    "points",
                    totalPoints.load(std::memory_order_relaxed));
        }
    }

    controller->setArmed(false);
    controller->clearPointCallback();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    manager.closeAll();

    const auto calls = callbackCalls.load(std::memory_order_relaxed);
    const auto points = totalPoints.load(std::memory_order_relaxed);
    if (calls == 0 || points == 0) {
        logError("Stream callback was not consumed by the backend.");
        return 1;
    }

    logInfo("LightSpace Net stream test finished.",
            "callbacks", calls,
            "points", points);
    return 0;
}
