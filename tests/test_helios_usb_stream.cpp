#include "libera/helios/HeliosManager.hpp"
#include "libera/helios/HeliosController.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace libera;
using namespace libera::helios;
using namespace libera::core;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define LOG_CHECK(cond, msg) \
    do { if (!(cond)) { logError("CHECK FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } \
         else { logInfo("  OK:", (msg)); } } while(0)

// ---------------------------------------------------------------------------
// Generate a repeating frame of laser points: a simple triangle with blanking
// moves between vertices and a blanking return to start. This is a realistic
// pattern that the PointStreamFramer should be able to detect as a natural
// loop boundary.
//
// Layout (all in normalised -1..1):
//   blank move to v0 -> lit line v0->v1 -> blank move to v1 ->
//   lit line v1->v2 -> blank move to v2 -> lit line v2->v0 ->
//   blank return to v0
// ---------------------------------------------------------------------------
static std::vector<LaserPoint> makeTriangleFrame(std::size_t pointsPerEdge,
                                                  std::size_t blankPoints) {
    struct Vertex { float x, y; };
    const Vertex v0{0.0f, 0.5f};
    const Vertex v1{-0.5f, -0.3f};
    const Vertex v2{0.5f, -0.3f};
    const Vertex verts[3] = {v0, v1, v2};

    std::vector<LaserPoint> frame;

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

    auto addBlankMove = [&](const Vertex& from, const Vertex& to) {
        for (std::size_t i = 0; i < blankPoints; ++i) {
            LaserPoint p{};
            float t = static_cast<float>(i) / static_cast<float>(blankPoints);
            p.x = lerp(from.x, to.x, t);
            p.y = lerp(from.y, to.y, t);
            // r=g=b=0 → blanked
            frame.push_back(p);
        }
    };

    auto addLitEdge = [&](const Vertex& from, const Vertex& to) {
        for (std::size_t i = 0; i < pointsPerEdge; ++i) {
            LaserPoint p{};
            float t = static_cast<float>(i) / static_cast<float>(pointsPerEdge);
            p.x = lerp(from.x, to.x, t);
            p.y = lerp(from.y, to.y, t);
            p.r = 0.02f;
            p.g = 0.0f;
            p.b = 0.0f;
            p.i = 0.02f;
            frame.push_back(p);
        }
    };

    // Initial blank move to first vertex
    Vertex origin{0.0f, 0.0f};
    addBlankMove(origin, v0);

    // Three lit edges with blank moves between them
    for (int edge = 0; edge < 3; ++edge) {
        addLitEdge(verts[edge], verts[(edge + 1) % 3]);
        if (edge < 2) {
            addBlankMove(verts[(edge + 1) % 3], verts[(edge + 1) % 3]);
        }
    }

    // Blank return to v0 (the start)
    addBlankMove(v2, v0);

    return frame;
}

int main() {
    logInfo("=== Helios USB Point Stream Test ===");
    logInfo("");

    // -----------------------------------------------------------------------
    // 1. Discover Helios USB DACs
    // -----------------------------------------------------------------------
    logInfo("--- Step 1: Discover ---");
    HeliosManager manager;
    auto discovered = manager.discover();

    if (discovered.empty()) {
        logError("No Helios USB DACs found. Plug one in and retry.");
        return 1;
    }

    logInfo("Found", discovered.size(), "Helios DAC(s):");
    for (const auto& info : discovered) {
        logInfo("  -", info->labelValue(), "(id:", info->idValue(), ")");
    }
    logInfo("");

    // -----------------------------------------------------------------------
    // 2. Connect to the first available DAC
    // -----------------------------------------------------------------------
    logInfo("--- Step 2: Connect ---");
    auto controller = manager.connectController(*discovered[0]);
    ASSERT_TRUE(controller != nullptr, "connectController returned non-null");
    if (!controller) {
        logError("Cannot continue without a controller.");
        return 1;
    }

    // Wait briefly for USB connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto* helios = dynamic_cast<HeliosController*>(controller.get());
    ASSERT_TRUE(helios != nullptr, "controller is a HeliosController");
    if (helios) {
        logInfo("Connected to:", helios->getDacName(),
                "firmware:", helios->getFirmwareVersion());
    }
    logInfo("");

    // -----------------------------------------------------------------------
    // 3. Configure and install point callback
    // -----------------------------------------------------------------------
    logInfo("--- Step 3: Stream points ---");

    constexpr std::uint32_t pointRate = 30000;
    controller->setPointRate(pointRate);
    controller->setArmed(true);

    // Build a repeating triangle frame (~300 points total with blanking)
    const auto triangleFrame = makeTriangleFrame(80, 10);
    const std::size_t frameLen = triangleFrame.size();
    logInfo("Triangle frame size:", frameLen, "points");

    std::atomic<std::uint64_t> callbackCalls{0};
    std::atomic<std::uint64_t> totalPointsEmitted{0};

    controller->setPointCallback(
        [&](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            callbackCalls.fetch_add(1, std::memory_order_relaxed);
            const std::size_t toWrite = req.maximumPointsRequired;
            const auto prevTotal = totalPointsEmitted.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < toWrite; ++i) {
                out.push_back(triangleFrame[(prevTotal + i) % frameLen]);
            }
            totalPointsEmitted.fetch_add(toWrite, std::memory_order_relaxed);
        });

    // -----------------------------------------------------------------------
    // 4. Monitor buffer state over time
    // -----------------------------------------------------------------------
    logInfo("--- Step 4: Monitor buffer (3 seconds) ---");
    logInfo("");

    constexpr int sampleCount = 30;
    constexpr auto sampleInterval = std::chrono::milliseconds(100);

    int risingSamples = 0;
    int previousBuffered = -1;
    int maxBuffered = 0;
    int minBuffered = std::numeric_limits<int>::max();
    int stableSamples = 0;
    bool hadValidBuffer = false;

    for (int i = 0; i < sampleCount; ++i) {
        std::this_thread::sleep_for(sampleInterval);

        auto bufState = controller->getBufferState();
        if (!bufState) {
            logInfo("  sample", i, "- no buffer state available");
            continue;
        }

        hadValidBuffer = true;
        const int buffered = bufState->pointsInBuffer;
        const int capacity = bufState->totalBufferPoints;
        const float fillPercent = capacity > 0
            ? 100.0f * static_cast<float>(buffered) / static_cast<float>(capacity)
            : 0.0f;

        const auto calls = callbackCalls.load(std::memory_order_relaxed);
        const auto emitted = totalPointsEmitted.load(std::memory_order_relaxed);

        logInfo("  sample", i,
                "- buffered:", buffered, "/", capacity,
                "(", static_cast<int>(fillPercent), "%)",
                "- callbacks:", calls,
                "- emitted:", emitted);

        if (buffered > maxBuffered) maxBuffered = buffered;
        if (buffered < minBuffered) minBuffered = buffered;

        if (previousBuffered >= 0) {
            if (buffered > previousBuffered + 50) {
                ++risingSamples;
            }
            if (std::abs(buffered - previousBuffered) < 100) {
                ++stableSamples;
            }
        }
        previousBuffered = buffered;
    }

    logInfo("");

    // -----------------------------------------------------------------------
    // 5. Check results
    // -----------------------------------------------------------------------
    logInfo("--- Step 5: Results ---");

    const auto totalEmitted = totalPointsEmitted.load(std::memory_order_relaxed);
    const auto totalCalls = callbackCalls.load(std::memory_order_relaxed);

    logInfo("Total callback calls:", totalCalls);
    logInfo("Total points emitted:", totalEmitted);
    logInfo("Buffer range:", minBuffered, "-", maxBuffered);
    logInfo("Rising samples:", risingSamples, "/", sampleCount);
    logInfo("Stable samples:", stableSamples, "/", sampleCount);
    logInfo("");

    // Callback should have been called many times
    LOG_CHECK(totalCalls > 10,
              "callback was called enough times (>10)");

    // Should have emitted a reasonable number of points (~3s * 30kpps = ~90k)
    LOG_CHECK(totalEmitted > 30000,
              "emitted enough points (>30k in 3 seconds)");

    // Buffer should have valid readings
    LOG_CHECK(hadValidBuffer,
              "buffer state was reported");

    // Buffer should not grow unboundedly - max should be reasonable
    // At 30kpps with 100ms latency target, buffer target is ~3300 points.
    // Allow generous headroom but flag if it's way too large.
    const int maxReasonableBuffer = static_cast<int>(pointRate); // 1 full second
    LOG_CHECK(maxBuffered < maxReasonableBuffer,
              "buffer did not grow excessively (< 1s worth of points)");

    // Buffer should stabilise, not keep rising
    LOG_CHECK(risingSamples < sampleCount / 2,
              "buffer is not continuously rising");

    // Most samples should be stable after initial fill
    LOG_CHECK(stableSamples > sampleCount / 3,
              "buffer is mostly stable after settling");

    logInfo("");

    // -----------------------------------------------------------------------
    // 6. Shutdown
    // -----------------------------------------------------------------------
    logInfo("--- Step 6: Shutdown ---");
    controller->clearPointCallback();
    controller->setArmed(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    manager.closeAll();
    logInfo("Closed.");

    logInfo("");
    if (g_failures) {
        logError("=== FAILED:", g_failures, "failure(s) ===");
        return 1;
    }

    logInfo("=== All checks passed ===");
    return 0;
}
