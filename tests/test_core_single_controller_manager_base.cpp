#include "libera/core/ControllerManagerBase.hpp"
#include "libera/log/Log.hpp"

#include <memory>
#include <string>
#include <utility>
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

class DummyInfo : public ControllerInfo {
public:
    DummyInfo(std::string id, std::string label, std::string reconnectKey)
    : ControllerInfo(std::move(id), std::move(label))
    , reconnectKeyValue(std::move(reconnectKey)) {}

    const std::string& type() const override { return typeName; }
    const std::string& reconnectKey() const { return reconnectKeyValue; }

private:
    static inline const std::string typeName{"Dummy"};
    std::string reconnectKeyValue;
};

class DummyController : public LaserController {
public:
    explicit DummyController(int tokenValue)
    : token(tokenValue) {}

    int token = 0;
    bool reusable = true;
    int prepareNewCount = 0;
    int prepareExistingCount = 0;
    int stopCount = 0;
    int closeCount = 0;

protected:
    void run() override {}
};

class DummyManager : public ControllerManagerBase<DummyInfo, DummyController> {
public:
    std::vector<std::unique_ptr<ControllerInfo>> discover() override {
        return {};
    }

    std::string_view managedType() const override {
        return "Dummy";
    }

    int createCount = 0;
    bool dropNewController = false;
    int beforeCloseCount = 0;
    int afterCloseCount = 0;
    std::vector<std::string> closedKeys;

protected:
    std::string controllerKey(const DummyInfo& info) const override {
        return info.reconnectKey();
    }

    ControllerPtr createController(const DummyInfo& info) override {
        (void)info;
        ++createCount;
        return std::make_shared<DummyController>(createCount);
    }

    bool shouldReuseController(const DummyController& controller,
                               const DummyInfo& info) const override {
        (void)info;
        return controller.reusable;
    }

    NewControllerDisposition prepareNewController(DummyController& controller,
                                                  const DummyInfo& info) override {
        (void)info;
        ++controller.prepareNewCount;
        return dropNewController
            ? NewControllerDisposition::DropController
            : NewControllerDisposition::KeepController;
    }

    void prepareExistingController(DummyController& controller,
                                   const DummyInfo& info) override {
        (void)info;
        ++controller.prepareExistingCount;
    }

    void beforeCloseControllers() override {
        ++beforeCloseCount;
    }

    void afterCloseControllers() override {
        ++afterCloseCount;
    }

    void stopController(DummyController& controller) override {
        ++controller.stopCount;
    }

    void closeController(const std::string& key,
                         DummyController& controller) override {
        closedKeys.push_back(key);
        ++controller.closeCount;
    }
};

std::shared_ptr<DummyController> asDummy(const std::shared_ptr<LaserController>& controller) {
    return std::dynamic_pointer_cast<DummyController>(controller);
}

void testConnectReusesExistingControllerForSameKey() {
    DummyManager manager;
    DummyInfo firstInfo("id-a", "Dummy A", "key-a");
    DummyInfo secondInfo("id-b", "Dummy B", "key-a");

    auto first = asDummy(manager.connectController(firstInfo));
    auto second = asDummy(manager.connectController(secondInfo));

    ASSERT_TRUE(first != nullptr, "first controller created");
    ASSERT_TRUE(second == first, "second connect reuses live controller for same key");
    ASSERT_EQ(manager.createCount, 1, "factory runs once for reused key");
    ASSERT_EQ(first->prepareNewCount, 1, "new-controller hook runs once");
    ASSERT_EQ(first->prepareExistingCount, 1, "existing-controller hook runs on reuse");
}

void testCustomControllerKeyOverridesIdValue() {
    DummyManager manager;
    DummyInfo firstInfo("same-id", "Dummy A", "key-a");
    DummyInfo secondInfo("same-id", "Dummy B", "key-b");

    auto first = asDummy(manager.connectController(firstInfo));
    auto second = asDummy(manager.connectController(secondInfo));

    ASSERT_TRUE(first != nullptr, "first custom-key controller created");
    ASSERT_TRUE(second != nullptr, "second custom-key controller created");
    ASSERT_TRUE(second != first, "different reconnect keys create different controllers");
    ASSERT_EQ(manager.createCount, 2, "factory runs once per unique reconnect key");
}

void testDroppedNewControllerIsRemovedFromCache() {
    DummyManager manager;
    DummyInfo info("id-a", "Dummy A", "key-a");

    manager.dropNewController = true;
    auto dropped = manager.connectController(info);
    ASSERT_TRUE(!dropped, "dropped first-acquire returns nullptr");

    manager.dropNewController = false;
    auto recreated = asDummy(manager.connectController(info));
    ASSERT_TRUE(recreated != nullptr, "second attempt recreates controller after drop");
    ASSERT_EQ(manager.createCount, 2, "failed first-acquire does not poison the cache");
    ASSERT_EQ(recreated->prepareNewCount, 1, "recreated controller sees one successful first-acquire");
}

void testRejectedReuseCreatesReplacementController() {
    DummyManager manager;
    DummyInfo info("id-a", "Dummy A", "key-a");

    auto first = asDummy(manager.connectController(info));
    ASSERT_TRUE(first != nullptr, "initial controller created");
    first->reusable = false;

    auto replacement = asDummy(manager.connectController(info));
    ASSERT_TRUE(replacement != nullptr, "replacement controller created");
    ASSERT_TRUE(replacement != first, "rejected reuse path creates a new controller");
    ASSERT_EQ(manager.createCount, 2, "factory reruns after reuse predicate rejects old controller");
}

void testCloseAllStopsAndClosesLiveControllers() {
    DummyManager manager;
    DummyInfo firstInfo("id-a", "Dummy A", "key-a");
    DummyInfo secondInfo("id-b", "Dummy B", "key-b");

    auto first = asDummy(manager.connectController(firstInfo));
    auto second = asDummy(manager.connectController(secondInfo));
    ASSERT_TRUE(first != nullptr, "first controller exists before closeAll");
    ASSERT_TRUE(second != nullptr, "second controller exists before closeAll");

    manager.closeAll();

    ASSERT_EQ(manager.beforeCloseCount, 1, "beforeCloseControllers runs once");
    ASSERT_EQ(manager.afterCloseCount, 1, "afterCloseControllers runs once");
    ASSERT_EQ(first->stopCount, 1, "first controller stopped once");
    ASSERT_EQ(first->closeCount, 1, "first controller closed once");
    ASSERT_EQ(second->stopCount, 1, "second controller stopped once");
    ASSERT_EQ(second->closeCount, 1, "second controller closed once");
    ASSERT_EQ(manager.closedKeys.size(), static_cast<std::size_t>(2), "all live keys are passed to closeController");

    auto fresh = asDummy(manager.connectController(firstInfo));
    ASSERT_TRUE(fresh != nullptr, "closeAll clears the cache for future connects");
    ASSERT_TRUE(fresh != first, "future connect after closeAll creates a fresh controller");
}

} // namespace

int main() {
    testConnectReusesExistingControllerForSameKey();
    testCustomControllerKeyOverridesIdValue();
    testDroppedNewControllerIsRemovedFromCache();
    testRejectedReuseCreatesReplacementController();
    testCloseAllStopsAndClosesLiveControllers();

    if (g_failures == 0) {
        logInfo("test_core_single_controller_manager_base: OK");
        return 0;
    }

    logError("test_core_single_controller_manager_base: FAIL", g_failures);
    return 1;
}
