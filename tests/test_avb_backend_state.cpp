#include "libera/avb/AvbController.hpp"
#include "libera/avb/AvbManager.hpp"
#include "libera/log/Log.hpp"

#include "../src/avb/AvbBackendState.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace libera;
using namespace libera::avb;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

class FakeOutputStream : public detail::AudioOutputStream {
public:
    FakeOutputStream(std::uint32_t pointRateValue,
                     std::uint32_t channelCountValue,
                     bool startSucceedsValue,
                     int* startCountValue,
                     int* stopCountValue)
    : pointRateValue(pointRateValue)
    , channelCountValue(channelCountValue)
    , startSucceeds(startSucceedsValue)
    , startCount(startCountValue)
    , stopCount(stopCountValue) {}

    bool start() override {
        if (startCount != nullptr) {
            ++(*startCount);
        }
        return startSucceeds;
    }

    void stop() override {
        if (stopCount != nullptr) {
            ++(*stopCount);
        }
    }

    std::uint32_t pointRate() const override {
        return pointRateValue;
    }

    std::uint32_t channelCount() const override {
        return channelCountValue;
    }

private:
    std::uint32_t pointRateValue = 0;
    std::uint32_t channelCountValue = 0;
    bool startSucceeds = true;
    int* startCount = nullptr;
    int* stopCount = nullptr;
};

class FakeAudioHost : public detail::AudioHost {
public:
    std::vector<detail::AudioOutputDeviceInfo> devices;
    bool streamStartSucceeds = true;
    int openCount = 0;
    int startCount = 0;
    int stopCount = 0;
    std::vector<std::string> openedDeviceUids;
    std::vector<std::uint32_t> openedPointRates;

    std::vector<detail::AudioOutputDeviceInfo> listOutputDevices() override {
        return devices;
    }

    std::unique_ptr<detail::AudioOutputStream> openOutputStream(
        const detail::AudioOutputDeviceInfo& device,
        std::uint32_t pointRateValue,
        detail::AudioOutputCallback callback) override {
        (void)callback;
        ++openCount;
        openedDeviceUids.push_back(device.uid);
        openedPointRates.push_back(pointRateValue);
        return std::make_unique<FakeOutputStream>(
            pointRateValue,
            device.outputChannels,
            streamStartSucceeds,
            &startCount,
            &stopCount);
    }
};

std::shared_ptr<FakeAudioHost> installFakeAudioHost() {
    auto host = std::make_shared<FakeAudioHost>();

    // Reset any previous singleton state so each test starts from one clean
    // fake host and no shared AVB runtime/configuration leftovers.
    detail::AvbBackendState::setAudioHostFactoryForTesting([host] {
        return host;
    });
    return host;
}

void testInjectedAudioHostControlsDiscoveryAndConfiguration() {
    auto host = installFakeAudioHost();
    host->devices = {
        detail::AudioOutputDeviceInfo{1, "dev-b", "Device B", 16, 48000, true, {48000, 96000}},
        detail::AudioOutputDeviceInfo{2, "", "Missing Id", 16, 48000, true, {48000}},
        detail::AudioOutputDeviceInfo{3, "too-small", "Too Small", 6, 30000, false, {}},
        detail::AudioOutputDeviceInfo{4, "dev-a", "Device A", 8, 30000, false, {}},
    };

    const auto devices = AvbManager::availableDevices();
    ASSERT_EQ(devices.size(), static_cast<std::size_t>(2),
              "AVB discovery filters ineligible devices");
    ASSERT_TRUE(devices[0].uid == "dev-a",
                "available devices stay sorted by label and uid");
    ASSERT_TRUE(devices[1].uid == "dev-b",
                "second available device is the 16-channel fake host");

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"dev-b", 12345},
        AvbDeviceConfiguration{"dev-a", 0},
    });

    const auto configs = AvbManager::configuredDevices();
    ASSERT_EQ(configs.size(), static_cast<std::size_t>(2),
              "configuredDevices keeps one normalized config per device");
    ASSERT_TRUE(configs[0].deviceUid == "dev-a",
                "configured devices stay sorted by uid");
    ASSERT_EQ(configs[0].preferredPointRate, 30000u,
              "device without explicit supported rates falls back to default");
    ASSERT_TRUE(configs[1].deviceUid == "dev-b",
                "second configured device is the 16-channel fake host");
    ASSERT_EQ(configs[1].preferredPointRate, 48000u,
              "unsupported preferred point rate normalizes to device default");

    const auto controllers = AvbManager::configuredControllers();
    ASSERT_EQ(controllers.size(), static_cast<std::size_t>(3),
              "configured 8-channel banks are generated from channel count");
    ASSERT_TRUE(controllers[0].idValue() == "dev-a::ch-0",
                "single-bank device keeps one stable controller id");
    ASSERT_TRUE(controllers[1].idValue() == "dev-b::ch-0",
                "first bank of 16-channel device starts at channel 0");
    ASSERT_TRUE(controllers[2].idValue() == "dev-b::ch-8",
                "second bank of 16-channel device starts at channel 8");
}

void testInjectedAudioHostPreservesSharedRuntimeReuse() {
    auto host = installFakeAudioHost();
    host->devices = {
        detail::AudioOutputDeviceInfo{1, "shared-dev", "Shared Device", 16, 48000, true, {48000}},
    };

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"shared-dev", 48000},
    });

    const auto controllers = AvbManager::configuredControllers();
    ASSERT_EQ(controllers.size(), static_cast<std::size_t>(2),
              "16-channel device exposes two AVB controller banks");

    AvbManager manager;
    auto first = std::dynamic_pointer_cast<AvbController>(
        manager.connectController(controllers[0]));
    auto firstAgain = std::dynamic_pointer_cast<AvbController>(
        manager.connectController(controllers[0]));
    auto second = std::dynamic_pointer_cast<AvbController>(
        manager.connectController(controllers[1]));

    ASSERT_TRUE(first != nullptr, "first AVB controller connects");
    ASSERT_TRUE(firstAgain == first,
                "reconnecting the same bank reuses the existing controller");
    ASSERT_TRUE(second != nullptr, "second AVB controller connects");
    ASSERT_TRUE(second != first,
                "different banks still produce distinct controllers");
    ASSERT_EQ(host->openCount, 1,
              "all banks on one device share a single runtime open");
    ASSERT_EQ(host->startCount, 1,
              "shared runtime starts its output stream once");

    manager.closeAll();
    ASSERT_EQ(host->stopCount, 1,
              "shared runtime stops its output stream once during shutdown");
}

void testSetConfiguredDevicesReopensLiveRuntimeWhenPointRateChanges() {
    auto host = installFakeAudioHost();
    host->devices = {
        detail::AudioOutputDeviceInfo{1, "reopen-dev", "Reopen Device", 8, 48000, true, {48000, 96000}},
    };

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"reopen-dev", 48000},
    });

    AvbManager manager;
    const auto controllers = AvbManager::configuredControllers();
    auto controller = std::dynamic_pointer_cast<AvbController>(
        manager.connectController(controllers[0]));

    ASSERT_TRUE(controller != nullptr, "AVB controller connects before reopen");
    ASSERT_EQ(host->openCount, 1, "initial connect opens one runtime stream");
    ASSERT_EQ(host->startCount, 1, "initial connect starts one runtime stream");
    ASSERT_EQ(host->stopCount, 0, "runtime has not been stopped yet");

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"reopen-dev", 96000},
    });

    ASSERT_TRUE(controller->isConnected(),
                "controller remains connected after runtime reopen");
    ASSERT_EQ(host->openCount, 2,
              "changing the configured point rate reopens the live runtime");
    ASSERT_EQ(host->startCount, 2,
              "runtime reopen starts a fresh output stream");
    ASSERT_EQ(host->stopCount, 1,
              "runtime reopen stops the previous stream first");
    ASSERT_EQ(host->openedPointRates.size(), static_cast<std::size_t>(2),
              "fake host records both open attempts");
    ASSERT_EQ(host->openedPointRates[0], 48000u,
              "first open uses the original configured point rate");
    ASSERT_EQ(host->openedPointRates[1], 96000u,
              "second open uses the updated configured point rate");

    manager.closeAll();
    ASSERT_EQ(host->stopCount, 2,
              "shutdown stops the reopened runtime stream once");
}

void testSetConfiguredDevicesSkipsReopenWhenPointRateStaysTheSame() {
    auto host = installFakeAudioHost();
    host->devices = {
        detail::AudioOutputDeviceInfo{1, "steady-dev", "Steady Device", 8, 48000, true, {48000, 96000}},
    };

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"steady-dev", 48000},
    });

    AvbManager manager;
    const auto controllers = AvbManager::configuredControllers();
    auto controller = std::dynamic_pointer_cast<AvbController>(
        manager.connectController(controllers[0]));

    ASSERT_TRUE(controller != nullptr, "AVB controller connects before no-op config update");
    ASSERT_EQ(host->openCount, 1, "initial connect opens one runtime stream");

    AvbManager::setConfiguredDevices({
        AvbDeviceConfiguration{"steady-dev", 48000},
    });

    ASSERT_TRUE(controller->isConnected(),
                "controller stays connected when point rate does not change");
    ASSERT_EQ(host->openCount, 1,
              "same configured point rate does not reopen the runtime");
    ASSERT_EQ(host->startCount, 1,
              "same configured point rate does not start another stream");
    ASSERT_EQ(host->stopCount, 0,
              "same configured point rate does not stop the live stream");

    manager.closeAll();
    ASSERT_EQ(host->stopCount, 1,
              "shutdown still stops the original runtime stream");
}

} // namespace

int main() {
    testInjectedAudioHostControlsDiscoveryAndConfiguration();
    testInjectedAudioHostPreservesSharedRuntimeReuse();
    testSetConfiguredDevicesReopensLiveRuntimeWhenPointRateChanges();
    testSetConfiguredDevicesSkipsReopenWhenPointRateStaysTheSame();

    detail::AvbBackendState::setAudioHostFactoryForTesting({});

    if (g_failures == 0) {
        logInfo("test_avb_backend_state: OK");
        return 0;
    }

    logError("test_avb_backend_state: FAIL", g_failures);
    return 1;
}
