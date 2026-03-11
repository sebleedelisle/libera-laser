#pragma once

#include "libera/core/LaserDeviceBase.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>
#include <memory>

namespace libera::core {

struct Frame {
    std::vector<LaserPoint> points;
    std::chrono::steady_clock::time_point time{};
    std::size_t playCount = 0;
    std::size_t nextPoint = 0; // cursor of the next sample to emit
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
    std::size_t queuedFrameCount() const;

protected:
    void frameFillCallback(const PointFillRequest& request,
                           std::vector<LaserPoint>& outputBuffer);
    void drainPendingFrames();

private:

    std::deque<std::shared_ptr<Frame>> pendingFrames;
    mutable std::mutex pendingFramesMutex;
    std::deque<std::shared_ptr<Frame>> frameQueue;
    bool frameModeActive = false;

    void appendBlankPoints(std::vector<LaserPoint>& buffer, std::size_t count);
};

} // namespace libera::core
