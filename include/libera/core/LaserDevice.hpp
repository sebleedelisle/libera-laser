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
    // Desired first-point presentation time. If left as default (epoch), the
    // frame will be auto-stamped in sendFrame() using targetRenderLatency().
    std::chrono::steady_clock::time_point time{};
    std::size_t playCount = 0;
    std::size_t nextPoint = 0; // cursor of the next sample to emit
};

class LaserDevice : public LaserDeviceBase {
public:
    LaserDevice();
    virtual ~LaserDevice();

    /**
     * @brief Set global frame presentation latency used by sendFrame().
     *
     * When a frame has an empty timestamp (`Frame::time{}`), sendFrame() will
     * stamp it to now + this latency so all frame-mode DACs can share one
     * scheduling target.
     */
    static void setTargetRenderLatency(std::chrono::milliseconds latency);

    /**
     * @brief Get global frame presentation latency used by sendFrame().
     */
    static std::chrono::milliseconds targetRenderLatency();

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
