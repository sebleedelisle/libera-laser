#include "libera/core/LaserController.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
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

#define LOG_CHECK(cond, msg) \
    do { if (!(cond)) { logError("CHECK FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } \
         else { logInfo("  OK:", (msg)); } } while(0)

// ---------------------------------------------------------------------------
// Test controller that exposes requestFrame for direct testing.
// ---------------------------------------------------------------------------
class FramerTestController : public LaserController {
public:
    bool requestFrameNow(std::size_t maximumPointsRequired,
                         std::size_t preferredPointCount,
                         Frame& outputFrame) {
        FrameFillRequest request{};
        request.maximumPointsRequired = maximumPointsRequired;
        request.preferredPointCount = preferredPointCount;
        request.blankFramePointCount = preferredPointCount;
        request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();
        return LaserController::requestFrame(request, outputFrame);
    }

    void noteSubmitted(std::size_t pointCount, std::uint32_t rate) {
        noteFrameTransportSubmission(pointCount,
                                     std::chrono::steady_clock::now(),
                                     rate);
    }

protected:
    void run() override {}
};

// ---------------------------------------------------------------------------
// Frame-building helpers
// ---------------------------------------------------------------------------

// Lerp between two positions.
static LaserPoint makeLitPoint(float x, float y, float r = 1.0f, float g = 0.0f, float b = 0.0f) {
    LaserPoint p{};
    p.x = x; p.y = y;
    p.r = r; p.g = g; p.b = b;
    p.i = 1.0f;
    return p;
}

static LaserPoint makeBlankPoint(float x, float y) {
    LaserPoint p{};
    p.x = x; p.y = y;
    return p;
}

// Blank move: N points interpolating from (x0,y0) to (x1,y1), all dark.
static void addBlankMove(std::vector<LaserPoint>& frame,
                         float x0, float y0, float x1, float y1,
                         std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        frame.push_back(makeBlankPoint(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
}

// Lit edge: N points interpolating from (x0,y0) to (x1,y1), coloured.
static void addLitEdge(std::vector<LaserPoint>& frame,
                       float x0, float y0, float x1, float y1,
                       std::size_t count,
                       float r = 1.0f, float g = 0.0f, float b = 0.0f) {
    for (std::size_t i = 0; i < count; ++i) {
        float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        frame.push_back(makeLitPoint(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r, g, b));
    }
}

// Circle: N lit points around a circle centred at (cx,cy) with radius r.
static void addLitCircle(std::vector<LaserPoint>& frame,
                         float cx, float cy, float radius,
                         std::size_t count,
                         float r = 0.0f, float g = 1.0f, float b = 0.0f) {
    const float pi2 = 6.2831853f;
    for (std::size_t i = 0; i < count; ++i) {
        float angle = pi2 * static_cast<float>(i) / static_cast<float>(count);
        float x = cx + radius * std::cos(angle);
        float y = cy + radius * std::sin(angle);
        frame.push_back(makeLitPoint(x, y, r, g, b));
    }
}

// Build a repeating point stream from a single frame pattern.
// The callback cycles through the pattern endlessly.
struct RepeatingStreamSource {
    std::vector<LaserPoint> pattern;
    std::size_t cursor = 0;

    void reset() { cursor = 0; }

    RequestPointsCallback callback() {
        return [this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                out.push_back(pattern[cursor % pattern.size()]);
                ++cursor;
            }
        };
    }
};

// ---------------------------------------------------------------------------
// Extract multiple frames and return their sizes.
// ---------------------------------------------------------------------------
static std::vector<std::size_t> extractFrames(FramerTestController& controller,
                                               std::size_t maxFramePoints,
                                               std::size_t preferredPoints,
                                               int count) {
    std::vector<std::size_t> sizes;
    for (int i = 0; i < count; ++i) {
        Frame output;
        if (controller.requestFrameNow(maxFramePoints, preferredPoints, output)) {
            sizes.push_back(output.points.size());
            controller.noteSubmitted(output.points.size(), controller.getPointRate());
        }
    }
    return sizes;
}

// Check whether most frames in a run match the expected pattern size.
// skipInitial: ignore the first N frames (the framer may need time to lock on).
// The last frame is also skipped since it may be a partial remainder.
static bool mostFramesMatchPatternSize(const std::vector<std::size_t>& sizes,
                                        std::size_t patternSize,
                                        std::size_t tolerance,
                                        int skipInitial = 2) {
    const int last = static_cast<int>(sizes.size()) - 1;
    if (last <= skipInitial) return false;

    int matchCount = 0;
    int total = 0;
    for (int i = skipInitial; i < last; ++i) {
        ++total;
        std::size_t diff = (sizes[i] > patternSize)
            ? sizes[i] - patternSize
            : patternSize - sizes[i];
        if (diff <= tolerance) {
            ++matchCount;
        }
    }
    return total > 0 && matchCount > total / 2;
}

// ---------------------------------------------------------------------------
// Test 1: Simple triangle with blanking return
//
// A basic frame with 3 lit edges, blanking between shapes, and a blanking
// return to the start position. The framer should find the boundary at the
// blank return point.
// ---------------------------------------------------------------------------
void testSimpleTriangleFrameDetection() {
    logInfo("--- Test: Simple triangle frame detection ---");

    std::vector<LaserPoint> frame;
    // Blank move to first vertex (0, 0.5)
    addBlankMove(frame, 0.0f, 0.0f, 0.0f, 0.5f, 8);
    // Edge 1: (0,0.5) -> (-0.5,-0.3)
    addLitEdge(frame, 0.0f, 0.5f, -0.5f, -0.3f, 60);
    // Blank dwell at vertex
    addBlankMove(frame, -0.5f, -0.3f, -0.5f, -0.3f, 5);
    // Edge 2: (-0.5,-0.3) -> (0.5,-0.3)
    addLitEdge(frame, -0.5f, -0.3f, 0.5f, -0.3f, 60);
    // Blank dwell at vertex
    addBlankMove(frame, 0.5f, -0.3f, 0.5f, -0.3f, 5);
    // Edge 3: (0.5,-0.3) -> (0,0.5)
    addLitEdge(frame, 0.5f, -0.3f, 0.0f, 0.5f, 60);
    // Blank return to start
    addBlankMove(frame, 0.0f, 0.5f, 0.0f, 0.0f, 8);

    logInfo("  Pattern size:", frame.size(), "points");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    auto sizes = extractFrames(controller, 4095, 300, 30);

    logInfo("  Extracted", sizes.size(), "frames");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        logInfo("    frame", i, ":", sizes[i], "points");
    }

    LOG_CHECK(!sizes.empty(), "extracted at least one frame");
    LOG_CHECK(mostFramesMatchPatternSize(sizes, frame.size(), 5),
              "most frames match pattern size (simple triangle)");
}

// ---------------------------------------------------------------------------
// Test 2: Multi-shape frame (triangle + square)
//
// Two separate shapes in one frame with blanking moves between them.
// The framer should find the boundary at the end of the full frame, not
// at the mid-frame blanking between shapes.
// ---------------------------------------------------------------------------
void testMultiShapeFrameDetection() {
    logInfo("--- Test: Multi-shape frame detection ---");

    std::vector<LaserPoint> frame;

    // Shape 1: Triangle at (-0.4, 0.2)
    float tx = -0.4f, ty = 0.2f;
    addBlankMove(frame, 0.0f, 0.0f, tx, ty + 0.2f, 8);
    addLitEdge(frame, tx, ty + 0.2f, tx - 0.2f, ty - 0.1f, 40, 1.0f, 0.0f, 0.0f);
    addLitEdge(frame, tx - 0.2f, ty - 0.1f, tx + 0.2f, ty - 0.1f, 40, 1.0f, 0.0f, 0.0f);
    addLitEdge(frame, tx + 0.2f, ty - 0.1f, tx, ty + 0.2f, 40, 1.0f, 0.0f, 0.0f);

    // Blank move to shape 2
    float sx = 0.4f, sy = -0.2f, sh = 0.15f;
    addBlankMove(frame, tx, ty + 0.2f, sx - sh, sy - sh, 10);

    // Shape 2: Square at (0.4, -0.2)
    addLitEdge(frame, sx - sh, sy - sh, sx + sh, sy - sh, 40, 0.0f, 0.0f, 1.0f);
    addLitEdge(frame, sx + sh, sy - sh, sx + sh, sy + sh, 40, 0.0f, 0.0f, 1.0f);
    addLitEdge(frame, sx + sh, sy + sh, sx - sh, sy + sh, 40, 0.0f, 0.0f, 1.0f);
    addLitEdge(frame, sx - sh, sy + sh, sx - sh, sy - sh, 40, 0.0f, 0.0f, 1.0f);

    // Blank return to origin
    addBlankMove(frame, sx - sh, sy - sh, 0.0f, 0.0f, 10);

    logInfo("  Pattern size:", frame.size(), "points");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    auto sizes = extractFrames(controller, 4095, 300, 30);

    logInfo("  Extracted", sizes.size(), "frames");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        logInfo("    frame", i, ":", sizes[i], "points");
    }

    LOG_CHECK(!sizes.empty(), "extracted at least one frame");
    LOG_CHECK(mostFramesMatchPatternSize(sizes, frame.size(), 10),
              "most frames match full pattern size (not mid-frame split)");
}

// ---------------------------------------------------------------------------
// Test 3: Large frame (bigger than nominal)
//
// A frame significantly larger than the nominal 300-point chunk size.
// This is the scenario that was broken before widening the search window.
// ---------------------------------------------------------------------------
void testLargeFrameDetection() {
    logInfo("--- Test: Large frame detection (> nominal) ---");

    std::vector<LaserPoint> frame;

    // 5-pointed star with blanking - creates a large frame (~700 points)
    const float pi2 = 6.2831853f;
    const int numPoints = 5;
    const float outerR = 0.6f;
    const float innerR = 0.25f;

    // Blank move to first outer vertex
    float startX = outerR * std::cos(-pi2 / 4.0f);
    float startY = outerR * std::sin(-pi2 / 4.0f);
    addBlankMove(frame, 0.0f, 0.0f, startX, startY, 10);

    // Star outline: outer->inner->outer->inner...
    for (int i = 0; i < numPoints; ++i) {
        float outerAngle = pi2 * static_cast<float>(i) / static_cast<float>(numPoints) - pi2 / 4.0f;
        float innerAngle = outerAngle + pi2 / (2.0f * static_cast<float>(numPoints));
        float nextOuterAngle = pi2 * static_cast<float>(i + 1) / static_cast<float>(numPoints) - pi2 / 4.0f;

        float ox = outerR * std::cos(outerAngle);
        float oy = outerR * std::sin(outerAngle);
        float ix = innerR * std::cos(innerAngle);
        float iy = innerR * std::sin(innerAngle);
        float nox = outerR * std::cos(nextOuterAngle);
        float noy = outerR * std::sin(nextOuterAngle);

        addLitEdge(frame, ox, oy, ix, iy, 60, 1.0f, 1.0f, 0.0f);
        addLitEdge(frame, ix, iy, nox, noy, 60, 1.0f, 1.0f, 0.0f);
    }

    // Blank return to origin
    float endX = outerR * std::cos(-pi2 / 4.0f);
    float endY = outerR * std::sin(-pi2 / 4.0f);
    addBlankMove(frame, endX, endY, 0.0f, 0.0f, 10);

    logInfo("  Pattern size:", frame.size(), "points (nominal: 300)");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    auto sizes = extractFrames(controller, 4095, 300, 30);

    logInfo("  Extracted", sizes.size(), "frames");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        logInfo("    frame", i, ":", sizes[i], "points");
    }

    LOG_CHECK(!sizes.empty(), "extracted at least one frame");
    LOG_CHECK(mostFramesMatchPatternSize(sizes, frame.size(), 10),
              "most frames match large pattern size (star)");
}

// ---------------------------------------------------------------------------
// Test 4: Circle + text-like shapes (many small shapes with blanking)
//
// Simulates a more complex scene: a circle and several small line segments
// scattered around, similar to text or abstract graphics. Many blanking
// gaps at different positions - the framer should not be confused by
// mid-frame blanking.
// ---------------------------------------------------------------------------
void testComplexMultiShapeFrameDetection() {
    logInfo("--- Test: Complex multi-shape frame detection ---");

    std::vector<LaserPoint> frame;

    // Blank to circle start
    float circleStartX = 0.3f;
    float circleStartY = 0.0f;
    addBlankMove(frame, 0.0f, 0.0f, circleStartX, circleStartY, 8);

    // Circle
    addLitCircle(frame, 0.0f, 0.0f, 0.3f, 120, 0.0f, 1.0f, 0.0f);

    // 4 small line segments at various positions (like text strokes)
    struct Segment { float x0, y0, x1, y1; };
    Segment segments[] = {
        {-0.7f, 0.4f, -0.5f, 0.4f},
        {-0.6f, 0.4f, -0.6f, 0.2f},
        {-0.7f, -0.3f, -0.5f, -0.5f},
        { 0.6f, 0.3f,  0.8f, 0.5f},
    };

    float prevX = circleStartX, prevY = circleStartY;
    for (const auto& seg : segments) {
        addBlankMove(frame, prevX, prevY, seg.x0, seg.y0, 8);
        addLitEdge(frame, seg.x0, seg.y0, seg.x1, seg.y1, 30, 1.0f, 1.0f, 1.0f);
        prevX = seg.x1;
        prevY = seg.y1;
    }

    // Blank return to origin
    addBlankMove(frame, prevX, prevY, 0.0f, 0.0f, 10);

    logInfo("  Pattern size:", frame.size(), "points");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    auto sizes = extractFrames(controller, 4095, 300, 30);

    logInfo("  Extracted", sizes.size(), "frames");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        logInfo("    frame", i, ":", sizes[i], "points");
    }

    LOG_CHECK(!sizes.empty(), "extracted at least one frame");
    LOG_CHECK(mostFramesMatchPatternSize(sizes, frame.size(), 10),
              "most frames match complex pattern size");
}

// ---------------------------------------------------------------------------
// Test 5: Frame size exactly matches nominal
//
// A frame that happens to be the same size as the nominal chunk (300 points).
// The framer should find the boundary cleanly without force-emitting.
// ---------------------------------------------------------------------------
void testNominalSizeFrameDetection() {
    logInfo("--- Test: Nominal-size frame detection ---");

    std::vector<LaserPoint> frame;

    // Build a frame that's exactly ~300 points
    addBlankMove(frame, 0.0f, 0.0f, -0.5f, 0.5f, 10);
    addLitEdge(frame, -0.5f, 0.5f, 0.5f, 0.5f, 90);
    addLitEdge(frame, 0.5f, 0.5f, 0.5f, -0.5f, 90);
    addLitEdge(frame, 0.5f, -0.5f, -0.5f, -0.5f, 90);
    // Blank return
    addBlankMove(frame, -0.5f, -0.5f, 0.0f, 0.0f, 10);
    // Pad to exactly 300 if needed
    while (frame.size() < 300) {
        frame.push_back(makeBlankPoint(0.0f, 0.0f));
    }

    logInfo("  Pattern size:", frame.size(), "points (= nominal)");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    auto sizes = extractFrames(controller, 4095, 300, 30);

    logInfo("  Extracted", sizes.size(), "frames");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        logInfo("    frame", i, ":", sizes[i], "points");
    }

    LOG_CHECK(!sizes.empty(), "extracted at least one frame");
    LOG_CHECK(mostFramesMatchPatternSize(sizes, frame.size(), 5),
              "most frames match nominal-size pattern");
}

// ---------------------------------------------------------------------------
// Test 6: Buffer stability check
//
// Run many frame extractions and verify the reported buffer state doesn't
// grow unboundedly.
// ---------------------------------------------------------------------------
void testBufferDoesNotGrowUnbounded() {
    logInfo("--- Test: Buffer stability ---");

    std::vector<LaserPoint> frame;
    addBlankMove(frame, 0.0f, 0.0f, 0.0f, 0.5f, 8);
    addLitEdge(frame, 0.0f, 0.5f, -0.5f, -0.3f, 80);
    addLitEdge(frame, -0.5f, -0.3f, 0.5f, -0.3f, 80);
    addLitEdge(frame, 0.5f, -0.3f, 0.0f, 0.5f, 80);
    addBlankMove(frame, 0.0f, 0.5f, 0.0f, 0.0f, 8);

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(100));
    controller.setPointRate(30000);
    controller.setArmed(true);

    RepeatingStreamSource source;
    source.pattern = frame;
    controller.setPointCallback(source.callback());

    int maxBuffered = 0;
    int lastBuffered = 0;
    int risingCount = 0;

    // Simulate real-time frame extraction: sleep between calls so the
    // transport submission estimate drains naturally, just as the real
    // Helios run loop does (~10ms per frame at 30kpps / 300 points).
    for (int i = 0; i < 50; ++i) {
        Frame output;
        if (controller.requestFrameNow(4095, 300, output)) {
            controller.noteSubmitted(output.points.size(), 30000);
        }

        // Let the transport estimate drain for one frame period.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto buf = controller.getBufferState();
        if (buf) {
            if (buf->pointsInBuffer > maxBuffered) {
                maxBuffered = buf->pointsInBuffer;
            }
            if (i > 5 && buf->pointsInBuffer > lastBuffered + 50) {
                ++risingCount;
            }
            lastBuffered = buf->pointsInBuffer;
        }
    }

    logInfo("  Max buffer:", maxBuffered, "- rising count:", risingCount, "/ 50");

    // At 30kpps with 100ms latency: target is ~3300 points.
    // Allow generous headroom but it should not be unbounded.
    LOG_CHECK(maxBuffered < 10000,
              "buffer stays under 10k points");
    LOG_CHECK(risingCount < 15,
              "buffer is not continuously rising after settling");
}

// ---------------------------------------------------------------------------
// Test 7: Keep one prepared frame in reserve
//
// The framer should not wait until the next transport-ready poll to start
// building the following frame. After serving one frame it should keep another
// complete frame prefetched when possible.
// ---------------------------------------------------------------------------
void testKeepsPreparedFrameReserve() {
    logInfo("--- Test: Prepared frame reserve ---");

    FramerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setPointRate(30000);
    controller.setArmed(true);

    std::vector<LaserPoint> frame;
    frame.push_back(makeBlankPoint(0.0f, 0.0f));
    for (std::size_t i = 0; i < 78; ++i) {
        frame.push_back(makeLitPoint(0.6f, 0.0f));
    }
    frame.push_back(makeBlankPoint(0.0f, 0.0f));

    std::size_t callbackCalls = 0;
    std::size_t cursor = 0;
    controller.setPointCallback(
        [&](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            ++callbackCalls;
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                out.push_back(frame[cursor % frame.size()]);
                ++cursor;
            }
        });

    Frame output;
    ASSERT_TRUE(controller.requestFrameNow(frame.size(), frame.size(), output),
                "first requestFrame succeeds");
    ASSERT_EQ(output.points.size(), frame.size(),
              "first extracted frame matches the full natural boundary");
    ASSERT_EQ(callbackCalls, static_cast<std::size_t>(2),
              "framer pulled a second frame ahead while serving the first");

    const auto breakdown = controller.getPointCallbackBufferBreakdown();
    ASSERT_TRUE(breakdown.has_value(),
                "prepared reserve should be visible in point-callback breakdown");
    ASSERT_EQ(breakdown->prefetchedPoints, frame.size(),
              "one full frame remains prefetched after the first extraction");
}

// ---------------------------------------------------------------------------
int main() {
    testSimpleTriangleFrameDetection();
    logInfo("");
    testMultiShapeFrameDetection();
    logInfo("");
    testLargeFrameDetection();
    logInfo("");
    testComplexMultiShapeFrameDetection();
    logInfo("");
    testNominalSizeFrameDetection();
    logInfo("");
    testBufferDoesNotGrowUnbounded();
    logInfo("");
    testKeepsPreparedFrameReserve();
    logInfo("");

    if (g_failures) {
        logError("=== FAILED:", g_failures, "failure(s) ===");
        return 1;
    }

    logInfo("=== All point stream framer tests passed ===");
    return 0;
}
