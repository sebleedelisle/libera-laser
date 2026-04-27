// Verifies LaserController::queuedPointBudget()'s minimum-frames-buffered
// floor. The point-based latency formula (latency_ms × pps + frame_size)
// collapses on long-frame configurations: a 2400-pt frame at 30 kpps with
// 150 ms target latency only allows ~3 frames in the queue, and a 4000-pt
// frame only ~2. With advanceWhenAvailable on Helios USB, that makes any
// transient queue.size()==1 dip into a visible 1-frame replay (80–130 ms
// of frozen content). The floor keeps long-frame configs at a sensible
// 3-frame headroom while leaving short-frame configs unchanged.

#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
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

class BudgetFixture : public LaserController {
public:
    BudgetFixture() {
        setPointRate(30000);
    }

    // queuedPointBudget is private; isReadyForNewFrame() is the public hook
    // that consumes it. Test the floor by enqueueing frames of a known size
    // and observing when isReadyForNewFrame() flips false.
    std::size_t framesAcceptedBeforeRejection(std::size_t framePointCount) {
        startFrameMode();
        std::size_t accepted = 0;
        while (true) {
            if (!isReadyForNewFrame()) {
                return accepted;
            }
            Frame f;
            f.points.resize(framePointCount);
            for (auto& p : f.points) { p.r = 0.5f; }
            // Stamp epoch so the auto-stamp at sendFrame() doesn't push
            // these into the future for our budget probe.
            f.time = std::chrono::steady_clock::now();
            const bool sent = sendFrame(std::move(f));
            if (!sent) {
                return accepted;
            }
            ++accepted;
            // Hard cap to avoid runaway in case isReadyForNewFrame returns
            // true forever for some reason.
            if (accepted > 100) {
                return accepted;
            }
        }
    }

protected:
    void run() override {}
};

void testShortFrameBudgetUnchanged() {
    // 510pt frames at 30k pps × 150 ms latency: latency formula gives
    // 4500 + 510 = 5010 budget → ~9 frames in queue. The 3-frame floor
    // (3 × 510 = 1530) is well below, so no behaviour change.
    BudgetFixture controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(150));
    const std::size_t accepted = controller.framesAcceptedBeforeRejection(510);
    ASSERT_TRUE(accepted >= 9 && accepted <= 11,
                "short-frame queue depth roughly 9-10 frames (latency-bound)");
}

void testLongFrameBudgetHitsFloor() {
    // 2400pt frames at 30k pps × 150 ms latency:
    //   latency formula → 4500 + 2400 = 6900
    //   3-frame floor    → 3 × 2400 = 7200 (wins)
    // isReadyForNewFrame uses `count <= budget`, so the queue accepts up to
    // and including budget pts. 4 frames × 2400 = 9600 > 7200 → reject on
    // attempt 5. Result: 4 frames accepted, leaving 3 unconsumed after the
    // worker pops one.
    BudgetFixture controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(150));
    const std::size_t accepted = controller.framesAcceptedBeforeRejection(2400);
    ASSERT_EQ(accepted, static_cast<std::size_t>(4),
              "long-frame queue gets 4-frame depth (3-frame post-pop floor)");
}

void testVeryLongFrameBudgetHitsFloor() {
    // 4000pt frames: floor = 12000. 3 × 4000 ≤ 12000 (T,T,T,T), 4 × 4000 =
    // 16000 > 12000 (F). 4 accepted, 3 unconsumed post-pop.
    BudgetFixture controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(150));
    const std::size_t accepted = controller.framesAcceptedBeforeRejection(4000);
    ASSERT_EQ(accepted, static_cast<std::size_t>(4),
              "very long-frame queue gets 4-frame depth (3-frame post-pop floor)");
}

void testLowLatencyShortFramesStillRespected() {
    // With low latency target (20 ms) and short frames (300 pts):
    //   latency formula → 600 + 300 = 900
    //   3-frame floor    → 3 × 300 = 900
    // Equal — accept until count > 900, so 4 × 300 = 1200 rejects. 4
    // accepted.
    BudgetFixture controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(20));
    const std::size_t accepted = controller.framesAcceptedBeforeRejection(300);
    ASSERT_EQ(accepted, static_cast<std::size_t>(4),
              "low-latency short-frame respects floor");
}

void testZeroLatencySkipsFloor() {
    // At targetLatency=0 the floor must NOT apply: callers asking for
    // minimal queueing (point-mode test patterns, low-latency streaming)
    // still get tight backpressure. Budget = 0 + frameSize = frameSize.
    // `count <= budget` accepts the boundary, so a 10pt frame admits
    // 2 frames (queue 0→10 accept, 10→20 accept-at-boundary, 20>10 reject).
    // That's the same tight bound the existing frame-scheduler tests rely
    // on; the assertion here is "floor did not push us higher than that".
    BudgetFixture controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    const std::size_t accepted = controller.framesAcceptedBeforeRejection(10);
    ASSERT_EQ(accepted, static_cast<std::size_t>(2),
              "zero-latency yields tight 2-accept budget (= 1 + boundary), no floor lift");
}

} // namespace

int main() {
    testShortFrameBudgetUnchanged();
    testLongFrameBudgetHitsFloor();
    testVeryLongFrameBudgetHitsFloor();
    testLowLatencyShortFramesStillRespected();
    testZeroLatencySkipsFloor();

    if (g_failures == 0) {
        logInfo("test_core_queue_budget_floor: all tests passed");
        return 0;
    }
    logError("test_core_queue_budget_floor: failures =", g_failures);
    return 1;
}
