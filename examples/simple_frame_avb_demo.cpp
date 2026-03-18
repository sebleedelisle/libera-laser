#include "libera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace libera;

namespace {

core::Frame makeWhiteCircleFrame(int frameNumber) {
    // A frame is just a list of laser points. The library manages frame queuing
    // and buffering automatically so you do not have to run the transport
    // yourself.
    core::Frame frame;

    constexpr std::size_t pointCount = 360;
    constexpr float radius = 0.7f;
    constexpr float brightness = 0.2f;

    frame.points.reserve(pointCount);

    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    for (std::size_t i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(pointCount);
        const float angle = t * tau;
        const float size = 1.0f * std::cos(static_cast<float>(frameNumber) * 0.05f);

        frame.points.emplace_back(core::LaserPoint{
            radius * std::cos(angle) * size,
            radius * std::sin(angle) * size,
            brightness,
            brightness,
            brightness
        });
    }

    return frame;
}

void printAvailableAvbDevices(const std::vector<avb::AvbAudioDeviceInfo>& devices) {
    logInfo("Available AVB-capable audio interfaces:");
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];

        std::ostringstream rates;
        for (std::size_t rateIndex = 0; rateIndex < device.supportedPointRates.size(); ++rateIndex) {
            if (rateIndex > 0) {
                rates << ", ";
            }
            rates << device.supportedPointRates[rateIndex];
        }

        logInfo("[", index, "]",
                device.label,
                "uid", device.uid,
                "channels", device.outputChannels,
                "default_pps", device.defaultPointRate,
                "rates", rates.str());
    }
}

std::vector<std::size_t> promptForDeviceIndices(std::size_t deviceCount) {
    while (true) {
        logInfo("Select one or more AVB audio interfaces by index (space separated), or type 'all':");

        std::string line;
        if (!std::getline(std::cin >> std::ws, line)) {
            return {};
        }

        if (line == "all" || line == "ALL") {
            std::vector<std::size_t> indices(deviceCount);
            for (std::size_t index = 0; index < deviceCount; ++index) {
                indices[index] = index;
            }
            return indices;
        }

        std::istringstream input(line);
        std::vector<std::size_t> indices;
        std::size_t index = 0;
        bool valid = true;
        while (input >> index) {
            if (index >= deviceCount) {
                valid = false;
                break;
            }

            if (std::find(indices.begin(), indices.end(), index) == indices.end()) {
                indices.push_back(index);
            }
        }

        if (valid && !indices.empty()) {
            return indices;
        }

        logError("Invalid selection. Please enter valid device indices.");
    }
}

std::vector<std::unique_ptr<core::ControllerInfo>> discoverSelectedAvbControllers(System& liberaSystem) {
    std::vector<std::unique_ptr<core::ControllerInfo>> avbControllers;

    while (avbControllers.empty()) {
        auto discovered = liberaSystem.discoverControllers();
        for (auto& info : discovered) {
            if (info && info->type() == "AVB") {
                avbControllers.emplace_back(std::move(info));
            }
        }

        if (!avbControllers.empty()) {
            break;
        }

        logInfo("Waiting for AVB controllers to appear...");
        std::this_thread::sleep_for(250ms);
    }

    return avbControllers;
}

std::size_t promptForControllerIndex(const std::vector<std::unique_ptr<core::ControllerInfo>>& controllers) {
    logInfo("Discovered AVB controllers:");
    for (std::size_t index = 0; index < controllers.size(); ++index) {
        const auto& controller = controllers[index];
        logInfo("[", index, "]",
                controller->labelValue(),
                "id", controller->idValue(),
                "type", controller->type());
    }

    while (true) {
        logInfo("Select controller index:");

        std::size_t choice = 0;
        if ((std::cin >> choice) && choice < controllers.size()) {
            return choice;
        }

        logError("Invalid controller selection.");
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
}

} // namespace

int main() {
    avb::AvbManager::setConfiguredDevices({});

    const auto audioDevices = avb::AvbManager::availableDevices();
    if (audioDevices.empty()) {
        logError("No AVB-capable audio interfaces were found.");
        return 1;
    }

    printAvailableAvbDevices(audioDevices);
    const auto selectedDeviceIndices = promptForDeviceIndices(audioDevices.size());
    if (selectedDeviceIndices.empty()) {
        logError("No AVB audio interfaces were selected.");
        return 1;
    }

    std::vector<avb::AvbDeviceConfiguration> avbConfigs;
    avbConfigs.reserve(selectedDeviceIndices.size());
    for (const auto index : selectedDeviceIndices) {
        const auto& device = audioDevices[index];
        avbConfigs.push_back(avb::AvbDeviceConfiguration{
            device.uid,
            device.defaultPointRate});
    }
    avb::AvbManager::setConfiguredDevices(avbConfigs);

    System liberaSystem;

    logInfo("Searching for AVB controllers...");
    auto discoveredControllers = discoverSelectedAvbControllers(liberaSystem);
    const auto controllerIndex = promptForControllerIndex(discoveredControllers);

    std::shared_ptr<core::LaserController> controller =
        liberaSystem.connectController(*discoveredControllers[controllerIndex]);
    if (!controller) {
        logError("A controller was discovered, but the library could not create it.");
        liberaSystem.shutdown();
        return 1;
    }

    controller->setArmed(true);

    std::size_t submittedFrameCount = 0;
    logInfo("Sending white circle frames...");

    while (submittedFrameCount < 1000) {
        if (!controller->isReadyForNewFrame()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        core::Frame frame = makeWhiteCircleFrame(static_cast<int>(submittedFrameCount));
        if (controller->sendFrame(std::move(frame))) {
            logInfo("Sent frame number", submittedFrameCount);
            ++submittedFrameCount;
        }
    }

    liberaSystem.shutdown();

    logInfo("Done. Submitted frames:", submittedFrameCount);
    return 0;
}
