#include "libera/helios/HeliosController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>

using namespace libera;
using namespace libera::helios;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

void testReadyFrameRenderLeadUsesMeasuredWriteTime() {
    const auto zeroLead = detail::requestRenderLead(std::chrono::microseconds(0));
    ASSERT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(zeroLead).count(),
              0LL,
              "zero write lead stays at zero");

    const auto measuredLead = detail::requestRenderLead(std::chrono::microseconds(4200));
    ASSERT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(measuredLead).count(),
              4200LL,
              "render lead follows measured write duration");
}

void testWriteLeadSmoothingClampsAndBlends() {
    ASSERT_EQ(detail::smoothWriteLeadMicros(0, 3000),
              3000LL,
              "first sample seeds the estimate");
    ASSERT_EQ(detail::smoothWriteLeadMicros(4000, 12000),
              6000LL,
              "subsequent samples are smoothed");
    ASSERT_EQ(detail::smoothWriteLeadMicros(4000, -1000),
              3000LL,
              "negative samples are clamped away");
}

void testDefaultFramePointCountTracksPointRate() {
    ASSERT_EQ(static_cast<long long>(detail::defaultFramePointCount(30000)),
              300LL,
              "default Helios chunk is about 10ms at 30kpps");
    ASSERT_EQ(static_cast<long long>(detail::defaultFramePointCount(100000)),
              1000LL,
              "default Helios chunk scales with point rate");
}

void testExplicitFramePointCountStopsAutomaticRetuning() {
    HeliosController controller(nullptr, 0);
    ASSERT_EQ(static_cast<long long>(controller.framePointCount()),
              300LL,
              "constructor seeds automatic frame point count from default pps");

    controller.setPointRate(40000);
    ASSERT_EQ(static_cast<long long>(controller.framePointCount()),
              400LL,
              "automatic frame point count follows point rate changes");

    controller.setFramePointCount(900);
    controller.setPointRate(20000);
    ASSERT_EQ(static_cast<long long>(controller.framePointCount()),
              900LL,
              "explicit frame point count override persists across rate changes");
}

void testMinimumRequestSizeIsNoLongerFixedToWholeChunk() {
    ASSERT_EQ(static_cast<long long>(detail::minimumRequestPoints(300)),
              20LL,
              "Helios minimum request stays small so frames can end naturally");
    ASSERT_EQ(static_cast<long long>(detail::minimumRequestPoints(10)),
              10LL,
              "minimum request never exceeds available chunk size");
}

} // namespace

int main() {
    testReadyFrameRenderLeadUsesMeasuredWriteTime();
    testWriteLeadSmoothingClampsAndBlends();
    testDefaultFramePointCountTracksPointRate();
    testExplicitFramePointCountStopsAutomaticRetuning();
    testMinimumRequestSizeIsNoLongerFixedToWholeChunk();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Helios timing tests passed");
    return 0;
}
