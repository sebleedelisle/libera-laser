#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/log/Log.hpp"

#include <cstddef>
#include <vector>

using namespace libera;
using namespace libera::core;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

class ScannerSyncHarness : public LaserControllerStreaming {
public:
    void run() override {}

    const std::vector<LaserPoint>& lastBatch() const {
        return pointsToSend;
    }
};

// ── Scanner sync default ─────────────────────────────────────────────

void testDefaultScannerSync() {
    ScannerSyncHarness controller;
    // Default scanner sync is 2.0 (in 1/10,000th of a second units).
    ASSERT_EQ(controller.getScannerSync(), 2.0, "default scanner sync should be 2.0");
}

void testSetGetScannerSync() {
    ScannerSyncHarness controller;
    controller.setScannerSync(5.0);
    ASSERT_EQ(controller.getScannerSync(), 5.0, "scanner sync round-trips");
}

void testNegativeScannerSyncClamps() {
    ScannerSyncHarness controller;
    controller.setScannerSync(-10.0);
    ASSERT_EQ(controller.getScannerSync(), 0.0, "negative scanner sync clamps to 0");
}

// ── Colour delay when sync > 0 ──────────────────────────────────────

void testColourDelayShiftsRGB() {
    ScannerSyncHarness controller;
    controller.setPointRate(10000); // 10 kpps
    controller.setArmed(true);

    // Set a large sync offset so the delay is clearly visible.
    // 10.0 units * 0.1ms = 1.0ms. At 10kpps that's 10 points of delay.
    controller.setScannerSync(10.0);

    // First, consume the startup blank (1ms at 10kpps = 10 points).
    controller.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            LaserPoint blank{};
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i)
                out.push_back(blank);
        });
    PointFillRequest drain{};
    drain.minimumPointsRequired = 20;
    drain.maximumPointsRequired = 20;
    controller.requestPoints(drain);

    // Now install the real callback: first point is red, rest are green.
    static int callCount = 0;
    callCount = 0;
    controller.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                LaserPoint p{};
                p.x = static_cast<float>(callCount * static_cast<int>(req.maximumPointsRequired) + static_cast<int>(i)) * 0.1f;
                p.y = 0.0f;
                if (callCount == 0 && i == 0) {
                    p.r = 1.0f;
                } else {
                    p.g = 1.0f;
                }
                out.push_back(p);
            }
            callCount++;
        });

    PointFillRequest request{};
    request.minimumPointsRequired = 20;
    request.maximumPointsRequired = 20;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    // With a 10-point colour delay, the XY coordinates should be immediate
    // but the RGB should be delayed. Point 0 should have the delay line's
    // initial value (from the drain call, which was all black).
    ASSERT_EQ(batch[0].r, 0.0f, "colour at point 0 should be from delay line (black)");

    // The red colour should appear ~10 points later.
    bool foundDelayedRed = false;
    for (std::size_t i = 8; i < 14 && i < batch.size(); ++i) {
        if (batch[i].r == 1.0f) {
            foundDelayedRed = true;
            break;
        }
    }
    ASSERT_TRUE(foundDelayedRed, "red colour should appear delayed by ~10 points");
}

// ── No colour delay when sync = 0 ───────────────────────────────────

void testZeroSyncNoDelay() {
    ScannerSyncHarness controller;
    controller.setPointRate(10000);
    controller.setArmed(true);
    controller.setScannerSync(0.0);

    controller.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                LaserPoint p{};
                p.x = static_cast<float>(i);
                p.r = static_cast<float>(i) * 0.1f;
                out.push_back(p);
            }
        });

    PointFillRequest request{};
    request.minimumPointsRequired = 5;
    request.maximumPointsRequired = 5;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    // With zero sync, colour should pass through unmodified (after startup blank).
    // But point 0 is blanked by the startup blank (1ms = 10 points at 10kpps).
    // At this point rate, startup blank is 10 points, so all 5 points are blanked.
    // Let's request again after startup blank has been consumed.
    ASSERT_TRUE(controller.requestPoints(request), "second requestPoints succeeds");
    const auto& batch2 = controller.lastBatch();

    // Now the XY and colour should match exactly.
    for (std::size_t i = 0; i < batch2.size(); ++i) {
        ASSERT_EQ(batch2[i].x, static_cast<float>(i), "x matches callback output");
    }
}

// ── Disarmed forces all black ────────────────────────────────────────

void testDisarmedForcesBlack() {
    ScannerSyncHarness controller;
    controller.setPointRate(1000);
    controller.setArmed(false);

    controller.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                LaserPoint p{};
                p.x = 0.5f;
                p.y = 0.5f;
                p.r = 1.0f;
                p.g = 1.0f;
                p.b = 1.0f;
                out.push_back(p);
            }
        });

    PointFillRequest request{};
    request.minimumPointsRequired = 5;
    request.maximumPointsRequired = 5;

    ASSERT_TRUE(controller.requestPoints(request), "disarmed requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    for (std::size_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(batch[i].r, 0.0f, "disarmed should blank r");
        ASSERT_EQ(batch[i].g, 0.0f, "disarmed should blank g");
        ASSERT_EQ(batch[i].b, 0.0f, "disarmed should blank b");
        ASSERT_EQ(batch[i].x, 0.0f, "disarmed should zero x");
        ASSERT_EQ(batch[i].y, 0.0f, "disarmed should zero y");
    }
}

} // namespace

int main() {
    testDefaultScannerSync();
    testSetGetScannerSync();
    testNegativeScannerSyncClamps();
    testColourDelayShiftsRGB();
    testZeroSyncNoDelay();
    testDisarmedForcesBlack();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Scanner sync tests passed");
    return 0;
}
