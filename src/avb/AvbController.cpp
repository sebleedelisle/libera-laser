#include "libera/avb/AvbController.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace libera::avb {
namespace {

float clampSignedSample(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

float clampUnitSample(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

AvbController::AvbController(std::string stableId,
                             std::string deviceUid,
                             std::uint32_t channelOffset,
                             std::uint32_t channelCount)
: stableControllerId(std::move(stableId))
, deviceUidValue(std::move(deviceUid))
, channelOffsetValue(channelOffset)
, channelCountValue(channelCount) {
    setConnectionState(false);
}

AvbController::~AvbController() {
    stopThread();
    close();
}

bool AvbController::isConnected() const {
    return getStatus() != core::ControllerStatus::Error;
}

void AvbController::close() {
    detachFromRuntime();
}

void AvbController::setPointRate(std::uint32_t pointRateValue) {
    (void)pointRateValue;
    // AVB point rate is configured at the shared device runtime level.
    // Controller-local rate changes are intentionally ignored for now so one
    // laser cannot silently reopen the audio device out from under siblings.
}

void AvbController::setHalfXYOutputEnabled(bool enabled) {
    halfXYOutputEnabled.store(enabled, std::memory_order_relaxed);
}

bool AvbController::isHalfXYOutputEnabled() const {
    return halfXYOutputEnabled.load(std::memory_order_relaxed);
}

void AvbController::run() {
    using namespace std::chrono_literals;

    // AVB output is driven from the shared audio callback, not this worker
    // thread. Keep the thread idle in case existing manager code starts it.
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(100ms);
    }
}

void AvbController::attachToRuntime(std::uint32_t pointRateValue) {
    if (pointRateValue > 0) {
        LaserControllerStreaming::setPointRate(pointRateValue);
    }
    setConnectionState(true);
}

void AvbController::detachFromRuntime() {
    setConnectionState(false);
}

void AvbController::renderInterleavedBlock(
    float* output,
    std::size_t frameCount,
    std::size_t totalChannelCount,
    std::chrono::steady_clock::time_point estimatedFirstRenderTime) {
    if (output == nullptr || frameCount == 0 || totalChannelCount == 0) {
        return;
    }

    const auto endChannel = channelOffsetValue + channelCountValue;
    if (endChannel > totalChannelCount || channelCountValue < 8) {
        return;
    }

    core::PointFillRequest request;
    request.minimumPointsRequired = frameCount;
    request.maximumPointsRequired = frameCount;
    request.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
    request.currentPointIndex = currentPointIndex.load(std::memory_order_relaxed);

    if (!requestPoints(request)) {
        return;
    }

    const std::size_t pointCount = std::min<std::size_t>(pointsToSend.size(), frameCount);
    const float xyScale = isHalfXYOutputEnabled() ? 0.5f : 1.0f;
    for (std::size_t frameIndex = 0; frameIndex < pointCount; ++frameIndex) {
        const auto& point = pointsToSend[frameIndex];
        float* frameOut = output + (frameIndex * totalChannelCount) + channelOffsetValue;
        // Half X/Y Output compensates for AVB-to-ILDA chains where both ends
        // fake a differential pair by grounding one side. In those setups the
        // effective scan size doubles unless we halve the outgoing X/Y signal.
        frameOut[0] = clampSignedSample(point.x * xyScale);
        frameOut[1] = clampSignedSample(point.y * xyScale);
        frameOut[2] = clampUnitSample(point.r);
        frameOut[3] = clampUnitSample(point.g);
        frameOut[4] = clampUnitSample(point.b);
        frameOut[5] = clampUnitSample(point.i);
        frameOut[6] = clampUnitSample(point.u1);
        frameOut[7] = clampUnitSample(point.u2);
    }

    currentPointIndex.fetch_add(pointCount, std::memory_order_relaxed);
}

} // namespace libera::avb
