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
};

class FrameScheduler {
public:
    FrameScheduler();
    ~FrameScheduler();

    bool enqueueFrame(Frame&& frame);
    void reset();

    bool isReadyForNewFrame(std::size_t queuedPointBudget) const;
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

    std::size_t queuedPointCountUnsafe() const;
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
