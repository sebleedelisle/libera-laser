#pragma once

#include "libera/core/LaserControllerStreaming.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace libera::core {

struct Frame;

struct FramePullRequest {
    std::size_t maximumPointsRequired = 0;
    std::size_t blankFramePointCount = 0;
    std::chrono::steady_clock::time_point estimatedFirstPointRenderTime{};
    std::uint64_t currentPointIndex = 0;

    // Frame-first transports (Helios USB, etc.) drain the queue in submitted
    // order; per-frame `time` gating is already enforced as queue depth via
    // isReadyForNewFrame(). When this flag is true:
    //   - end-of-frame advance is unconditional whenever queue.size() > 1
    //   - the front-due check (which would otherwise blank a freshly-popped
    //     frame whose `time` is still in the future) is bypassed
    // For streaming/scheduled-time transports leave this false.
    bool advanceWhenAvailable = false;

    // Most frame transports need the host to resend the current frame while
    // waiting for a replacement because the hardware consumes each submission.
    // Current-pattern transports retain and replay the last accepted frame
    // internally, so they can opt out and receive only real frame changes plus
    // the eventual safety blank when maxFrameHoldTime() expires.
    bool repeatCurrentFrameWhenIdle = true;
};

class FrameScheduler {
public:
    FrameScheduler();
    ~FrameScheduler();

    bool enqueueFrame(Frame&& frame);
    bool tryEnqueueFrameIfReady(Frame&& frame,
                                std::size_t queuedPointBudget,
                                std::size_t minimumQueuedPointsPerFrame = 1);
    void reset();

    bool isReadyForNewFrame(std::size_t queuedPointBudget,
                            std::size_t minimumQueuedPointsPerFrame = 1) const;
    bool tryIsReadyForNewFrame(std::size_t queuedPointBudget,
                               std::size_t minimumQueuedPointsPerFrame = 1) const;
    std::size_t queuedFrameCount() const;
    std::size_t nominalFramePointCount() const;

    void fillPoints(const PointFillRequest& request,
                    std::uint32_t pointRateValue,
                    std::chrono::milliseconds maxFrameHoldTime,
                    std::vector<LaserPoint>& outputBuffer,
                    bool verbose);

    void fillFrame(const FramePullRequest& request,
                   std::chrono::milliseconds maxFrameHoldTime,
                   Frame& outputFrame,
                   bool verbose);

private:
    struct State;

    std::size_t queuedPointCountUnsafe(std::size_t minimumQueuedPointsPerFrame = 1) const;
    void drainPendingFramesUnsafe();
    void appendBlankPoints(std::vector<LaserPoint>& buffer, std::size_t count) const;
    void generateTransitionPoints(const LaserPoint& from,
                                  const LaserPoint& to,
                                  std::vector<LaserPoint>& out) const;
    void drainPendingTransitionUnsafe(std::vector<LaserPoint>& outputBuffer,
                                      std::size_t maxPoints);

    std::unique_ptr<State> state;
};

} // namespace libera::core
