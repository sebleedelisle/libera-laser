#include "AvbDeviceRuntime.hpp"

#include <algorithm>

namespace libera::avb::detail {

AvbDeviceRuntime::AvbDeviceRuntime(std::shared_ptr<AudioHost> audioHostValue,
                                   AudioOutputDeviceInfo deviceInfo)
: audioHost(std::move(audioHostValue))
, deviceInfoValue(std::move(deviceInfo)) {}

AvbDeviceRuntime::~AvbDeviceRuntime() {
    close();
}

bool AvbDeviceRuntime::open(std::uint32_t pointRateValue) {
    if (!audioHost || pointRateValue == 0) {
        return false;
    }

    if (stream && currentPointRate() == pointRateValue) {
        return true;
    }

    close();

    stream = audioHost->openOutputStream(
        deviceInfoValue,
        pointRateValue,
        [this](float* output,
               std::uint32_t frameCount,
               std::uint32_t channelCount,
               std::chrono::steady_clock::duration outputLeadTime) {
            handleAudioCallback(output, frameCount, channelCount, outputLeadTime);
        });

    if (!stream) {
        return false;
    }

    if (!stream->start()) {
        stream.reset();
        return false;
    }

    const auto actualPointRateValue = stream->pointRate() > 0
        ? stream->pointRate()
        : pointRateValue;
    currentPointRateValue.store(actualPointRateValue, std::memory_order_relaxed);
    notifyControllersAttached(actualPointRateValue);
    return true;
}

bool AvbDeviceRuntime::reopen(std::uint32_t pointRateValue) {
    return open(pointRateValue);
}

void AvbDeviceRuntime::close() {
    const bool hadStream = static_cast<bool>(stream);

    if (stream) {
        stream->stop();
        stream.reset();
    }

    currentPointRateValue.store(0, std::memory_order_relaxed);
    if (hadStream) {
        notifyControllersDetached();
    }
}

void AvbDeviceRuntime::attachController(std::uint32_t channelOffset,
                                        const std::shared_ptr<AvbController>& controller) {
    if (!controller) {
        return;
    }

    {
        std::lock_guard lock(banksMutex);
        banksByOffset.insert_or_assign(channelOffset, BankState{controller});
    }

    const auto pointRateValue = currentPointRate();
    if (pointRateValue > 0) {
        controller->attachToRuntime(pointRateValue);
    }
}

void AvbDeviceRuntime::detachController(std::uint32_t channelOffset) {
    std::shared_ptr<AvbController> controller;
    {
        std::lock_guard lock(banksMutex);
        auto it = banksByOffset.find(channelOffset);
        if (it != banksByOffset.end()) {
            controller = it->second.controller.lock();
            banksByOffset.erase(it);
        }
    }

    if (controller) {
        controller->detachFromRuntime();
    }
}

void AvbDeviceRuntime::handleAudioCallback(
    float* output,
    std::uint32_t frameCount,
    std::uint32_t channelCount,
    std::chrono::steady_clock::duration outputLeadTime) {
    if (output == nullptr || frameCount == 0 || channelCount == 0) {
        return;
    }

    std::fill_n(output, static_cast<std::size_t>(frameCount) * channelCount, 0.0f);

    std::vector<std::pair<std::uint32_t, std::shared_ptr<AvbController>>> banks;
    {
        std::lock_guard lock(banksMutex);
        banks.reserve(banksByOffset.size());
        for (auto it = banksByOffset.begin(); it != banksByOffset.end();) {
            auto controller = it->second.controller.lock();
            if (!controller) {
                it = banksByOffset.erase(it);
                continue;
            }
            banks.emplace_back(it->first, std::move(controller));
            ++it;
        }
    }

    const auto estimatedFirstRenderTime = std::chrono::steady_clock::now() + outputLeadTime;
    for (auto& [channelOffset, controller] : banks) {
        (void)channelOffset;
        controller->renderInterleavedBlock(
            output,
            frameCount,
            channelCount,
            estimatedFirstRenderTime);
    }
}

void AvbDeviceRuntime::notifyControllersAttached(std::uint32_t pointRateValue) {
    std::vector<std::shared_ptr<AvbController>> controllers;
    {
        std::lock_guard lock(banksMutex);
        controllers.reserve(banksByOffset.size());
        for (auto& [channelOffset, state] : banksByOffset) {
            (void)channelOffset;
            if (auto controller = state.controller.lock()) {
                controllers.push_back(std::move(controller));
            }
        }
    }

    for (auto& controller : controllers) {
        controller->attachToRuntime(pointRateValue);
    }
}

void AvbDeviceRuntime::notifyControllersDetached() {
    std::vector<std::shared_ptr<AvbController>> controllers;
    {
        std::lock_guard lock(banksMutex);
        controllers.reserve(banksByOffset.size());
        for (auto& [channelOffset, state] : banksByOffset) {
            (void)channelOffset;
            if (auto controller = state.controller.lock()) {
                controllers.push_back(std::move(controller));
            }
        }
    }

    for (auto& controller : controllers) {
        controller->detachFromRuntime();
    }
}

} // namespace libera::avb::detail
