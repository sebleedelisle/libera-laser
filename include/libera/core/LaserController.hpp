#pragma once

#include "libera/core/LaserControllerStreaming.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>
#include <memory>

namespace libera::core {

struct Frame {
    std::vector<LaserPoint> points;
    // Desired first-point presentation time. If left as default (epoch), the
    // frame will be auto-stamped in sendFrame() using targetLatency().
    std::chrono::steady_clock::time_point time{};
    std::size_t playCount = 0;
    std::size_t nextPoint = 0; // cursor of the next sample to emit
};

class LaserController : public LaserControllerStreaming {
public:
    LaserController();
    virtual ~LaserController();

    /**
     * @brief Set the global target latency used by frame-mode controllers.
     *
     * When a frame has an empty timestamp (`Frame::time{}`), sendFrame() will
     * stamp it to now + this latency so all frame-mode controllers can share
     * one scheduling target. Buffer-aware controllers may also use the same
     * latency target when deciding how much transport lead to keep queued.
     */
    static void setTargetLatency(std::chrono::milliseconds latency);

    /**
     * @brief Get the global target latency used by frame-mode controllers.
     */
    static std::chrono::milliseconds targetLatency();

    bool sendFrame(Frame&& frame);
    void startFrameMode();
    void stopFrameMode();
    bool isFrameModeEnabled() const;
    bool isReadyForNewFrame() const;
    std::size_t queuedFrameCount() const;

protected:
    void fillFromFrameQueue(const PointFillRequest& request,
                           std::vector<LaserPoint>& outputBuffer);
    void drainPendingFrames();

private:

    std::deque<std::unique_ptr<Frame>> pendingFrames;
    mutable std::mutex pendingFramesMutex;
    std::deque<std::unique_ptr<Frame>> frameQueue;
    bool frameModeActive = false;
    std::atomic<std::size_t> pendingFrameCount{0};
    std::atomic<std::size_t> pendingPointCount{0};
    std::atomic<std::size_t> frameQueueCountEstimate{0};
    std::atomic<std::size_t> frameQueuePointCountEstimate{0};
    std::atomic<std::size_t> nominalFramePointCount{1};

    void appendBlankPoints(std::vector<LaserPoint>& buffer, std::size_t count);
    void updateFrameQueueMetricsUnsafe();
    std::size_t queuedPointBudget() const;

    // Automatic blanking for frame transitions / wraps.
    static constexpr float BLANK_TRANSITION_DISTANCE_THRESHOLD = 0.2f;
    static constexpr float BLANK_POINTS_PER_UNIT_DISTANCE = 20.0f;
    static constexpr std::size_t MIN_BLANK_POINTS_PER_END = 2;
    std::vector<LaserPoint> pendingTransitionPoints;

    void generateTransitionPoints(const LaserPoint& from, const LaserPoint& to,
                                  std::vector<LaserPoint>& out);
    void drainPendingTransition(std::vector<LaserPoint>& outputBuffer,
                                std::size_t maxPoints);
};

} // namespace libera::core
