// Verifies FrameFillRequest::advanceWhenAvailable. Frame-first transports
// (Helios USB) opt in so the scheduler drains the queue in submitted order
// even when queue[1].time is still "in the future" relative to the projected
// render time. Without this opt-in, the auto-stamped 150ms latency target
// makes queue[1] look not-yet-due whenever Liberation's submission interval
// slips past one DAC playback period, and the scheduler loops the current
// frame — which the user perceives as "frames replaying multiple times".

#include "libera/core/FrameScheduler.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <vector>

using namespace libera;
using namespace libera::core;
using namespace std::chrono_literals;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

Frame makeMarkerFrame(float marker, std::size_t count = 4) {
    Frame f;
    f.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LaserPoint p{};
        p.r = marker;
        f.points.push_back(p);
    }
    return f;
}

void testAdvanceWhenAvailableDrainsQueueInOrder() {
    FrameScheduler scheduler;
    const auto base = std::chrono::steady_clock::now();

    // Queue three frames, all with `time` 200 ms in the future relative to
    // base. Without advanceWhenAvailable, fillFrame would loop frame 1
    // forever because queue[1].time > projection. With advanceWhenAvailable,
    // the scheduler should pop and switch to the next queued frame as soon
    // as the current one is fully consumed.
    Frame f1 = makeMarkerFrame(0.1f);
    Frame f2 = makeMarkerFrame(0.2f);
    Frame f3 = makeMarkerFrame(0.3f);
    f1.time = base + 200ms;
    f2.time = base + 200ms;
    f3.time = base + 200ms;
    ASSERT_TRUE(scheduler.enqueueFrame(std::move(f1)), "f1 enqueued");
    ASSERT_TRUE(scheduler.enqueueFrame(std::move(f2)), "f2 enqueued");
    ASSERT_TRUE(scheduler.enqueueFrame(std::move(f3)), "f3 enqueued");

    FramePullRequest req;
    req.maximumPointsRequired = 64;
    req.blankFramePointCount = 4;
    req.estimatedFirstPointRenderTime = base; // earlier than every frame.time
    req.advanceWhenAvailable = true;

    Frame out;
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(static_cast<int>(out.points.size()), 4, "first call returns f1");
    ASSERT_EQ(out.points.front().r, 0.1f, "first call delivers f1 marker");

    out = Frame{};
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(static_cast<int>(out.points.size()), 4, "second call returns f2");
    ASSERT_EQ(out.points.front().r, 0.2f, "second call advances to f2 even though f2.time > projection");

    out = Frame{};
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(out.points.front().r, 0.3f, "third call advances to f3");
}

void testAdvanceFalseStillRespectsTimeGate() {
    // Sanity: with advanceWhenAvailable=false (the default), the existing
    // hold-current-frame-until-next-due semantics must be preserved.
    FrameScheduler scheduler;
    const auto base = std::chrono::steady_clock::now();

    Frame f1 = makeMarkerFrame(0.1f);
    Frame f2 = makeMarkerFrame(0.2f);
    f1.time = base; // due immediately
    f2.time = base + 200ms; // not due yet
    ASSERT_TRUE(scheduler.enqueueFrame(std::move(f1)), "f1 enqueued");
    ASSERT_TRUE(scheduler.enqueueFrame(std::move(f2)), "f2 enqueued");

    FramePullRequest req;
    req.maximumPointsRequired = 64;
    req.blankFramePointCount = 4;
    req.estimatedFirstPointRenderTime = base; // matches f1.time, not f2.time
    req.advanceWhenAvailable = false;

    Frame out;
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(out.points.front().r, 0.1f, "first call delivers f1");

    out = Frame{};
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(out.points.front().r, 0.1f, "second call still delivers f1: f2 is not yet due");
}

void testAdvanceWhenAvailableBypassesFrontDueGateOnPoppedSuccessor() {
    // Mirrors the steady-state Helios case: front frame just consumed,
    // queue[1].time is in the future. With advanceWhenAvailable, the popped
    // successor must NOT then trigger the front-due check (which would
    // blank). It must play immediately.
    FrameScheduler scheduler;
    const auto base = std::chrono::steady_clock::now();

    Frame f1 = makeMarkerFrame(0.1f);
    Frame f2 = makeMarkerFrame(0.2f);
    f1.time = base; // due
    f2.time = base + 500ms; // far in the future
    scheduler.enqueueFrame(std::move(f1));
    scheduler.enqueueFrame(std::move(f2));

    FramePullRequest req;
    req.maximumPointsRequired = 64;
    req.blankFramePointCount = 4;
    req.estimatedFirstPointRenderTime = base;
    req.advanceWhenAvailable = true;

    Frame out;
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(out.points.front().r, 0.1f, "first call returns f1");

    out = Frame{};
    scheduler.fillFrame(req, std::chrono::milliseconds(0), out, /*verbose=*/false);
    ASSERT_EQ(out.points.front().r, 0.2f, "second call must return f2 content, not blank");
    ASSERT_TRUE(out.points.front().r != 0.0f, "popped successor must not blank under advanceWhenAvailable");
}

} // namespace

int main() {
    testAdvanceWhenAvailableDrainsQueueInOrder();
    testAdvanceFalseStillRespectsTimeGate();
    testAdvanceWhenAvailableBypassesFrontDueGateOnPoppedSuccessor();

    if (g_failures == 0) {
        logInfo("test_core_frame_advance: all tests passed");
        return 0;
    }
    logError("test_core_frame_advance: failures =", g_failures);
    return 1;
}
