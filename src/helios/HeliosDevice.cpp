#include "libera/helios/HeliosDevice.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace libera::helios {
namespace {

constexpr std::size_t kDefaultFramePoints = 1000; // maximum 4096 points
constexpr std::size_t kMinFramePoints = 20;

constexpr unsigned int kHeliosFlags = HELIOS_FLAGS_DEFAULT;

std::uint16_t clampU16FromUnit(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * 65535.0f));
}

std::uint16_t clampU16FromSigned(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
}

} // namespace

HeliosDevice::HeliosDevice(std::shared_ptr<HeliosDac> sdkInstance, unsigned int deviceIndex)
: sdk(std::move(sdkInstance))
, index(deviceIndex) {
    targetFramePoints.store(kDefaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(kDefaultFramePoints);
}

HeliosDevice::~HeliosDevice() {
    stop();
    close();
}

void HeliosDevice::close() {
    if (!sdk) {
        return;
    }
    sdk->Stop(index);
}

bool HeliosDevice::isConnected() const {
    if (!sdk) {
        return false;
    }
    return sdk->GetIsClosed(index) == 0;
}

void HeliosDevice::setPointRate(std::uint32_t pointRateValue) {
    LaserDeviceBase::setPointRate(pointRateValue);
}

void HeliosDevice::setFramePointCount(std::size_t points) {
    const auto clamped = std::max(points, kMinFramePoints);
    targetFramePoints.store(clamped, std::memory_order_relaxed);
    frameBuffer.reserve(clamped);
}

std::size_t HeliosDevice::framePointCount() const {
    return targetFramePoints.load(std::memory_order_relaxed);
}

void HeliosDevice::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    while (running) {
        if (!sdk) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        if (sdk->GetIsClosed(index) != 0) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        const int status = sdk->GetStatus(index);
        if (status < 0) {
            // -5007 is a libusb timeout from status polling. Treat as transient.
            if (status == -5007) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            logError("[HeliosDevice] status error", status);
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (status == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const std::size_t framePoints = targetFramePoints.load(std::memory_order_relaxed);

        core::PointFillRequest req;
        req.minimumPointsRequired = framePoints;
        req.maximumPointsRequired = framePoints;
        req.estimatedFirstPointRenderTime =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(pointsToMillis(framePoints)));
        req.currentPointIndex = currentPointIndex.load(std::memory_order_relaxed);

        if (!requestPoints(req)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (pointsToSend.empty()) {
            continue;
        }

        frameBuffer.resize(pointsToSend.size());
        for (std::size_t i = 0; i < pointsToSend.size(); ++i) {
            const auto& p = pointsToSend[i];
            auto& out = frameBuffer[i];
            out.x = clampU16FromSigned(p.x);
            out.y = clampU16FromSigned(p.y);
            out.r = clampU16FromUnit(p.r);
            out.g = clampU16FromUnit(p.g);
            out.b = clampU16FromUnit(p.b);
            out.i = clampU16FromUnit(p.i);
            out.user1 = clampU16FromUnit(p.u1);
            out.user2 = clampU16FromUnit(p.u2);
            out.user3 = 0;
            out.user4 = 0;
        }

        const unsigned int pps = getPointRate();
        const int result = sdk->WriteFrameExtended(
            index,
            pps,
            kHeliosFlags,
            frameBuffer.data(),
            static_cast<unsigned int>(frameBuffer.size()));

        if (result < 0) {
            logError("[HeliosDevice] WriteFrameExtended failed", result);
        } else {
            currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
        }
    }
}

} // namespace libera::helios
