#pragma once

#include "libera/core/LaserControllerStreaming.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <memory>
#include <vector>

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

class FrameScheduler;

class LaserController : public LaserControllerStreaming {
public:
    enum class ContentSource {
        None,
        PointCallback,
        FrameQueue
    };

    using PointCallback = RequestPointsCallback;

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
     * @brief Install or clear the callback that produces point batches on demand.
     *
     * Installing a callback switches the controller to the point-callback
     * content source and clears any queued frames. Queueing a frame later will
     * switch the active content source back to the frame queue automatically.
     */
    void setPointCallback(const PointCallback& callback);

    /**
     * @brief Clear the currently installed point callback.
     *
     * This only switches the content source back to None if the callback was
     * the active source. Clearing the callback does not tear down frame-queue
     * state because queued frames are a separate source.
     */
    void clearPointCallback();

    /**
     * @brief Compatibility wrapper around setPointCallback().
     *
     * The old name is still kept so existing applications continue to build
     * while the codebase moves toward the clearer "PointCallback" language.
     */
    void setRequestPointsCallback(const RequestPointsCallback& callback);

    bool sendFrame(Frame&& frame);
    void useFrameQueue();
    void clearFrameQueue();
    void clearContentSource();
    ContentSource contentSource() const;
    void startFrameMode();
    void stopFrameMode();
    bool isFrameModeEnabled() const;
    bool isReadyForNewFrame() const;
    std::size_t queuedFrameCount() const;

protected:
    struct FrameFillRequest {
        std::size_t maximumPointsRequired = 0;
        // Preferred output size when a frame-ingester backend adapts the point
        // callback into one transport frame. When left at 0, requestFrame()
        // falls back to blankFramePointCount, then maximumPointsRequired.
        std::size_t preferredPointCount = 0;
        // Idle blank size used when the frame queue has nothing due yet. Frame
        // backends often set this to their natural frame size so idle output
        // keeps a stable cadence.
        std::size_t blankFramePointCount = 0;
        std::chrono::steady_clock::time_point estimatedFirstPointRenderTime{};
        std::uint64_t currentPointIndex = 0;
    };

    bool requestPoints(const PointFillRequest& request);
    bool requestFrame(const FrameFillRequest& request, Frame& outputFrame);
    bool isUsingFrameQueueSource() const;

private:
    mutable std::mutex contentSourceMutex;
    std::unique_ptr<FrameScheduler> frameScheduler;
    ContentSource activeSource = ContentSource::None;
    std::size_t queuedPointBudget() const;
};

} // namespace libera::core
