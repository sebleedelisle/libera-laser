#include "libera/System.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace libera;
using namespace libera::core;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { ++g_failures; } } while (0)

namespace {

int builtInConnects = 0;
int pluginConnects = 0;

class TestManager final : public AbstractControllerManager {
public:
    TestManager(std::string typeValue,
                std::string labelValue,
                int* connectCountValue)
    : type(std::move(typeValue))
    , label(std::move(labelValue))
    , connectCount(connectCountValue) {}

    std::vector<std::unique_ptr<ControllerInfo>> discover() override {
        std::vector<std::unique_ptr<ControllerInfo>> result;
        result.push_back(std::make_unique<ControllerInfo>(
            type, "test-controller", label));
        return result;
    }

    std::string_view managedType() const override { return type; }

    std::shared_ptr<LaserController>
    connectController(const ControllerInfo&) override {
        ++*connectCount;
        return nullptr;
    }

    void closeAll() override {}

private:
    std::string type;
    std::string label;
    int* connectCount = nullptr;
};

void registerTestDrivers() {
    AddControllerManager({
        {"libera.builtin.test-shared",
         "TestShared",
         "Built-in test driver",
         "Used to verify default selection.",
         true},
        [] {
            return std::make_unique<TestManager>(
                "TestShared", "built-in", &builtInConnects);
        },
    });

    AddControllerManager({
        {"com.example.test-shared",
         "TestShared",
         "Plugin test driver",
         "Used to verify explicit plugin selection.",
         false},
        [] {
            return std::make_unique<TestManager>(
                "TestShared", "plugin", &pluginConnects);
        },
    });

    AddControllerManager({
        {"com.example.ambiguous-a",
         "TestAmbiguous",
         "Ambiguous A",
         "First plugin-only implementation.",
         false},
        [] {
            return std::make_unique<TestManager>(
                "TestAmbiguous", "ambiguous-a", &pluginConnects);
        },
    });

    AddControllerManager({
        {"com.example.ambiguous-b",
         "TestAmbiguous",
         "Ambiguous B",
         "Second plugin-only implementation.",
         false},
        [] {
            return std::make_unique<TestManager>(
                "TestAmbiguous", "ambiguous-b", &pluginConnects);
        },
    });
}

SystemOptions testOptions() {
    SystemOptions options;
    for (const auto& manager : registeredControllerManagers()) {
        if (manager.type != "TestShared" &&
            manager.type != "TestAmbiguous") {
            options.disabledControllerTypes.insert(manager.type);
        }
    }
    return options;
}

void testBuiltInWinsByDefault() {
    auto options = testOptions();
    System system(options);
    auto discovered = system.discoverControllers();

    ASSERT_TRUE(discovered.size() == 1,
                "only the default built-in driver should discover");
    if (!discovered.empty()) {
        ASSERT_TRUE(discovered.front()->labelValue() == "built-in",
                    "the built-in implementation should be selected");
        ASSERT_TRUE(discovered.front()->driverId() ==
                        "libera.builtin.test-shared",
                    "System should stamp the concrete driver ID");
        system.connectController(*discovered.front());
    }
    ASSERT_TRUE(builtInConnects == 1,
                "connect should route back to the built-in manager");
    ASSERT_TRUE(pluginConnects == 0,
                "the unselected plugin must not receive connect calls");
}

void testExplicitPluginSelection() {
    auto options = testOptions();
    options.selectedControllerDrivers["TestShared"] =
        "com.example.test-shared";
    System system(options);
    auto discovered = system.discoverControllers();

    ASSERT_TRUE(discovered.size() == 1,
                "one explicitly selected implementation should discover");
    if (!discovered.empty()) {
        ASSERT_TRUE(discovered.front()->labelValue() == "plugin",
                    "the requested plugin implementation should be selected");
        ASSERT_TRUE(discovered.front()->driverId() ==
                        "com.example.test-shared",
                    "discovery should retain the selected plugin ID");
        system.connectController(*discovered.front());
    }
    ASSERT_TRUE(pluginConnects == 1,
                "connect should route back to the selected plugin manager");
}

} // namespace

int main() {
    System::setPluginDirectory("");
    registerTestDrivers();
    testBuiltInWinsByDefault();
    testExplicitPluginSelection();
    return g_failures == 0 ? 0 : 1;
}
