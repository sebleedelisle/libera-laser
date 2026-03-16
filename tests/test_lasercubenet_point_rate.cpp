#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/log/Log.hpp"

using namespace libera;
using namespace libera::lasercubenet;

static int g_failures = 0;

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

void testClampKeepsDefaultWhenWithinMax() {
    ASSERT_EQ(
        LaserCubeNetConfig::clampPointRate(30000, 60000),
        30000u,
        "configured point rate should be preserved when within the device max");
}

void testClampRespectsAdvertisedMax() {
    ASSERT_EQ(
        LaserCubeNetConfig::clampPointRate(30000, 20000),
        20000u,
        "configured point rate should clamp to the device max");
}

void testClampIgnoresUnknownMax() {
    ASSERT_EQ(
        LaserCubeNetConfig::clampPointRate(30000, 0),
        30000u,
        "unknown device max should not zero the configured point rate");
}

} // namespace

int main() {
    testClampKeepsDefaultWhenWithinMax();
    testClampRespectsAdvertisedMax();
    testClampIgnoresUnknownMax();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("LaserCubeNet point-rate tests passed");
    return 0;
}
