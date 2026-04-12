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
    // Set internally when the frame is first played; used to enforce maxFrameTime().
    std::chrono::steady_clock::time_point firstPlayTime{};
};

class LaserController : public LaserControllerStreaming {
public:
    enum class ContentSource {
        None,
        UserPoints,
        FrameQueue
    };

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

    /**
     * @brief Set the maximum time the last frame will be held/looped when no new frame arrives.
     *
     * If no new frame is submitted before this deadline expires, output goes
     * blank automatically. Set to zero to disable the limit (loop forever).
     * Default is 100 ms, which gives a generous window to replace a frame
     * while still going blank cleanly if the sender stops or pauses.
     */
    static void setMaxFrameHoldTime(std::chrono::milliseconds time);

    /**
     * @brief Get the current max frame hold time. Default is 100 ms.
     */
    static std::chrono::milliseconds maxFrameHoldTime();

    /**
     * @brief Install or clear the user point callback.
     *
     * Installing a callback switches the controller into streaming mode and
     * clears any queued frame-mode state. Queueing a frame later will switch
     * it back to frame mode automatically.
     */
    void setRequestPointsCallback(const RequestPointsCallback& callback);

    bool sendFrame(Frame&& frame);
    void startFrameMode();
    void stopFrameMode();
    bool isFrameModeEnabled() const;
    bool isReadyForNewFrame() const;
    std::size_t queuedFrameCount() const;

protected:
    struct FrameFillRequest {
        std::size_t maximumPointsRequired = 0;
        std::size_t blankFramePointCount = 0;
        std::chrono::steady_clock::time_point estimatedFirstPointRenderTime{};
        std::uint64_t currentPointIndex = 0;
    };

    bool requestPoints(const PointFillRequest& request);
    bool requestFrame(const FrameFillRequest& request, Frame& outputFrame);
    bool isUsingFrameQueueSource() const;

    void fillFromFrameQueue(const PointFillRequest& request,
                           std::vector<LaserPoint>& outputBuffer);
    void drainPendingFrames();

private:

    std::deque<std::unique_ptr<Frame>> pendingFrames;
    mutable std::mutex pendingFramesMutex;
    std::deque<std::unique_ptr<Frame>> frameQueue;
    bool frameModeActive = false;
    ContentSource activeSource = ContentSource::None;
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
    std::unique_ptr<Frame> pendingTransitionFrame;

    void generateTransitionPoints(const LaserPoint& from, const LaserPoint& to,
                                  std::vector<LaserPoint>& out);
    void drainPendingTransition(std::vector<LaserPoint>& outputBuffer,
                                std::size_t maxPoints);
};

} // namespace libera::core
