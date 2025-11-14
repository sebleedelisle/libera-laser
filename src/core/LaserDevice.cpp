#include "libera/core/LaserDevice.hpp"

namespace libera::core {

LaserDevice::LaserDevice() = default;
LaserDevice::~LaserDevice() = default;

bool LaserDevice::sendFrame(Frame&& frame) {
    if (!frameModeActive) {
        startFrameMode();
    }

    if(!isReadyForNewFrame()) return false; 

    std::lock_guard<std::mutex> lock(pendingFramesMutex);
    
    pendingFrames.push_back(std::move(frame));
    return true;
}

void LaserDevice::startFrameMode() {
    if (frameModeActive) {
        return;
    }
    frameModeActive = true;
    setRequestPointsCallback([this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
        frameFillCallback(req, out);
    });
}

void LaserDevice::stopFrameMode() {
    if (!frameModeActive) {
        return;
    }
    setRequestPointsCallback({});
    {
        std::lock_guard<std::mutex> lock(pendingFramesMutex);
        pendingFrames.clear();
    }
    frameModeActive = false;
}

bool LaserDevice::frameModeEnabled() const {
    return frameModeActive;
}

bool LaserDevice::isReadyForNewFrame() const {

    return true; 
}

void LaserDevice::frameFillCallback(const PointFillRequest& request,
                                    std::vector<LaserPoint>& outputBuffer) {
    drainPendingFrames();
    (void)request;
    (void)outputBuffer;
}

void LaserDevice::drainPendingFrames() {
    std::lock_guard<std::mutex> lock(pendingFramesMutex);
    while (!pendingFrames.empty()) {
        frameQueue.push_back(std::move(pendingFrames.front()));
        pendingFrames.pop_front();
    }
}

} // namespace libera::core
