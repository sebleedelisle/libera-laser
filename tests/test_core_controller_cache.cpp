#include "libera/core/ControllerCache.hpp"
#include "libera/log/Log.hpp"

#include <string>

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

struct DummyController {
    explicit DummyController(int tokenValue)
    : token(tokenValue) {}

    int token = 0;
    bool connected = true;
};

void testGetOrCreateReusesExistingController() {
    ControllerCache<std::string, DummyController> cache;
    int createCount = 0;

    const auto first = cache.getOrCreate("dev-a", [&] {
        ++createCount;
        return std::make_shared<DummyController>(1);
    });
    const auto second = cache.getOrCreate("dev-a", [&] {
        ++createCount;
        return std::make_shared<DummyController>(2);
    });

    ASSERT_TRUE(first.controller != nullptr, "first controller created");
    ASSERT_TRUE(first.created, "first acquisition reports created");
    ASSERT_TRUE(second.controller == first.controller, "second acquisition reuses live controller");
    ASSERT_TRUE(!second.created, "second acquisition reports reused controller");
    ASSERT_EQ(createCount, 1, "factory runs once for one live key");
}

void testExpiredControllerIsPrunedAndRecreated() {
    ControllerCache<std::string, DummyController> cache;
    int createCount = 0;

    {
        const auto first = cache.getOrCreate("dev-a", [&] {
            ++createCount;
            return std::make_shared<DummyController>(1);
        });
        ASSERT_TRUE(first.controller != nullptr, "initial controller created");
    }

    const auto recreated = cache.getOrCreate("dev-a", [&] {
        ++createCount;
        return std::make_shared<DummyController>(2);
    });

    ASSERT_TRUE(recreated.controller != nullptr, "expired controller recreated");
    ASSERT_TRUE(recreated.created, "recreated controller reports created");
    ASSERT_EQ(createCount, 2, "factory runs again after cached weak pointer expires");
    ASSERT_EQ(recreated.controller->token, 2, "new controller instance is returned");
}

void testSnapshotKeepsOnlyLiveEntries() {
    ControllerCache<std::string, DummyController> cache;
    auto kept = cache.getOrCreate("dev-a", [] {
        return std::make_shared<DummyController>(10);
    }).controller;

    {
        auto dropped = cache.getOrCreate("dev-b", [] {
            return std::make_shared<DummyController>(20);
        }).controller;
        ASSERT_TRUE(dropped != nullptr, "temporary controller created");
    }

    const auto snapshot = cache.snapshot();
    ASSERT_EQ(snapshot.size(), static_cast<std::size_t>(1), "snapshot prunes expired entries");
    ASSERT_TRUE(snapshot.find("dev-a") != snapshot.end(), "snapshot keeps live controller");
    ASSERT_TRUE(snapshot.find("dev-b") == snapshot.end(), "snapshot removes expired controller");
    ASSERT_TRUE(snapshot.at("dev-a") == kept, "snapshot keeps the original live instance");
}

void testSnapshotAndClearDetachesCacheFromLiveInstances() {
    ControllerCache<std::string, DummyController> cache;
    auto first = cache.getOrCreate("dev-a", [] {
        return std::make_shared<DummyController>(30);
    }).controller;

    auto snapshot = cache.snapshotAndClear();
    ASSERT_EQ(snapshot.size(), static_cast<std::size_t>(1), "snapshotAndClear returns the live controller");
    ASSERT_TRUE(snapshot.at("dev-a") == first, "snapshotAndClear keeps the live instance alive");

    const auto second = cache.getOrCreate("dev-a", [] {
        return std::make_shared<DummyController>(40);
    });
    ASSERT_TRUE(second.created, "cache creates a fresh controller after clear");
    ASSERT_TRUE(second.controller != first, "cleared cache no longer reuses old controller");
    ASSERT_EQ(second.controller->token, 40, "new controller uses the replacement instance");
}

void testGetOrCreateIfReplacesRejectedController() {
    ControllerCache<std::string, DummyController> cache;
    auto first = cache.getOrCreate("dev-a", [] {
        return std::make_shared<DummyController>(50);
    }).controller;
    first->connected = false;

    const auto replacement = cache.getOrCreateIf(
        "dev-a",
        [](const std::shared_ptr<DummyController>& controller) {
            return controller->connected;
        },
        [] {
            return std::make_shared<DummyController>(60);
        });

    ASSERT_TRUE(replacement.created, "rejected controller is replaced");
    ASSERT_TRUE(replacement.controller != first, "replacement controller differs from rejected instance");
    ASSERT_EQ(replacement.controller->token, 60, "replacement controller comes from fallback factory");
}

} // namespace

int main() {
    testGetOrCreateReusesExistingController();
    testExpiredControllerIsPrunedAndRecreated();
    testSnapshotKeepsOnlyLiveEntries();
    testSnapshotAndClearDetachesCacheFromLiveInstances();
    testGetOrCreateIfReplacesRejectedController();

    if (g_failures == 0) {
        logInfo("test_core_controller_cache: OK");
        return 0;
    }

    logError("test_core_controller_cache: FAIL", g_failures);
    return 1;
}
