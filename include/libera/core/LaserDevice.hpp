#pragma once

#include "libera/core/LaserDeviceBase.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

namespace libera::core {

struct Frame {
    std::vector<LaserPoint> points;
    std::chrono::steady_clock::time_point time{};
    std::size_t playCount = 0;
};

class LaserDevice : public LaserDeviceBase {
public:
    LaserDevice();
    virtual ~LaserDevice();

    bool sendFrame(Frame&& frame);
    void startFrameMode();
    void stopFrameMode();
    bool frameModeEnabled() const;
    bool isReadyForNewFrame() const;

protected:
    void frameFillCallback(const PointFillRequest& request,
                           std::vector<LaserPoint>& outputBuffer);
    void drainPendingFrames();

private:

    std::deque<Frame> pendingFrames;
    mutable std::mutex pendingFramesMutex;
    std::deque<Frame> frameQueue;
    bool frameModeActive = false;
};

} // namespace libera::core
