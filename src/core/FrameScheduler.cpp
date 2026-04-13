#include "libera/core/FrameScheduler.hpp"

#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>

namespace libera::core {
namespace {

constexpr float blankTransitionDistanceThreshold = 0.2f;
constexpr float blankPointsPerUnitDistance = 20.0f;
constexpr std::size_t minBlankPointsPerEnd = 2;

bool frameIsDueAt(const Frame& frame,
                  std::chrono::steady_clock::time_point estimatedFirstRenderTime) {
    // Unscheduled frames (default time point) are treated as "play immediately".
    if (frame.time == std::chrono::steady_clock::time_point{}) {
        return true;
    }
    return frame.time <= estimatedFirstRenderTime;
}

double pointsToMillis(std::size_t pointCount, std::uint32_t pointRateValue) {
    if (pointRateValue == 0 || pointCount == 0) {
        return 0.0;
    }

    const double millis =
        (static_cast<double>(pointCount) * 1000.0) / static_cast<double>(pointRateValue);

    return std::max(millis, 0.0);
}

template <typename... Args>
void logSchedulerVerbose(bool verbose, Args&&... args) {
    if (verbose) {
        libera::log::logInfo(std::forward<Args>(args)...);
    }
}

} // namespace

struct FrameScheduler::State {
    std::deque<std::unique_ptr<Frame>> pendingFrames;
    std::deque<std::unique_ptr<Frame>> frameQueue;
    std::vector<LaserPoint> pendingTransitionPoints;
    std::size_t nominalFramePointCount = 1;
    mutable std::mutex mutex;
};

FrameScheduler::FrameScheduler()
    : state(std::make_unique<State>()) {}

FrameScheduler::~FrameScheduler() = default;

bool FrameScheduler::enqueueFrame(Frame&& frame) {
    if (frame.points.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->nominalFramePointCount = std::max<std::size_t>(frame.points.size(), 1);
    state->pendingFrames.push_back(std::make_unique<Frame>(std::move(frame)));
    return true;
}

void FrameScheduler::reset() {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingFrames.clear();
    state->frameQueue.clear();
    state->pendingTransitionPoints.clear();
    state->nominalFramePointCount = 1;
}

bool FrameScheduler::isReadyForNewFrame(std::size_t queuedPointBudget) const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return queuedPointCountUnsafe() <= queuedPointBudget;
}

std::size_t FrameScheduler::queuedFrameCount() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->pendingFrames.size() + state->frameQueue.size();
}

std::size_t FrameScheduler::nominalFramePointCount() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->nominalFramePointCount;
}

void FrameScheduler::fillPoints(const PointFillRequest& request,
                                std::uint32_t pointRateValue,
                                std::chrono::milliseconds maxFrameHoldTime,
                                std::vector<LaserPoint>& outputBuffer,
                                bool verbose) {
    std::lock_guard<std::mutex> lock(state->mutex);
    drainPendingFramesUnsafe();

    if (request.maximumPointsRequired == 0) {
        return;
    }

    const std::size_t minPoints = request.minimumPointsRequired;
    const std::size_t maxPoints = request.maximumPointsRequired;

    // Drain any leftover transition points from a previous fill call.
    drainPendingTransitionUnsafe(outputBuffer, maxPoints);
    if (outputBuffer.size() == maxPoints) {
        return;
    }

    auto estimatedFirstRenderTime = request.estimatedFirstPointRenderTime;
    if (estimatedFirstRenderTime == std::chrono::steady_clock::time_point{}) {
        estimatedFirstRenderTime = std::chrono::steady_clock::now();
    }

    const auto renderTimeForBufferedPoints =
        [estimatedFirstRenderTime, pointRateValue](std::size_t bufferedPointCount) {
            return estimatedFirstRenderTime +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double, std::milli>(
                           pointsToMillis(bufferedPointCount, pointRateValue)));
        };

    if (state->frameQueue.empty()) {
        logSchedulerVerbose(verbose,
                            "[FrameScheduler] fillPoints: queue empty, blanking",
                            minPoints,
                            "pts");
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    // Skip stale frames: only drop a front frame that has never been played.
    {
        std::size_t skipped = 0;
        while (state->frameQueue.size() > 1) {
            if (state->frameQueue.front()->playCount != 0) {
                break;
            }
            if (!frameIsDueAt(*state->frameQueue[1], estimatedFirstRenderTime)) {
                break;
            }
            state->frameQueue.pop_front();
            ++skipped;
        }
        if (skipped > 0) {
            logSchedulerVerbose(verbose,
                                "[FrameScheduler] fillPoints: skipped",
                                skipped,
                                "stale frames");
        }
    }

    // The scheduled time only gates the first play of a frame.
    if (state->frameQueue.front()->playCount == 0 &&
        !frameIsDueAt(*state->frameQueue.front(), estimatedFirstRenderTime)) {
        const auto now = std::chrono::steady_clock::now();
        const auto frameDueIn = std::chrono::duration<double, std::milli>(
            state->frameQueue.front()->time - estimatedFirstRenderTime).count();
        const auto renderLeadMs = std::chrono::duration<double, std::milli>(
            estimatedFirstRenderTime - now).count();
        logSchedulerVerbose(verbose,
                            "[FrameScheduler] fillPoints: frame not due, blanking",
                            minPoints,
                            "pts",
                            "frameDueInMs",
                            frameDueIn,
                            "renderLeadMs",
                            renderLeadMs,
                            "queueSize",
                            state->frameQueue.size());
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    while (true) {
        Frame* currentFrame = state->frameQueue.front().get();
        assert(currentFrame != nullptr && "Null frame in queue");

        if (currentFrame->playCount == 0) {
            currentFrame->firstPlayTime = std::chrono::steady_clock::now();
            ++currentFrame->playCount;
        }

        assert(!currentFrame->points.empty() && "Empty frame in queue");

        {
            const std::size_t available = currentFrame->points.size() - currentFrame->nextPoint;
            const std::size_t spaceLeft = maxPoints - outputBuffer.size();
            const std::size_t toAdd = std::min(available, spaceLeft);
            outputBuffer.insert(outputBuffer.end(),
                                currentFrame->points.begin() +
                                    static_cast<std::ptrdiff_t>(currentFrame->nextPoint),
                                currentFrame->points.begin() +
                                    static_cast<std::ptrdiff_t>(currentFrame->nextPoint + toAdd));
            currentFrame->nextPoint += toAdd;
        }

        if ((outputBuffer.size() >= minPoints) &&
            (currentFrame->nextPoint >= currentFrame->points.size())) {
            return;
        }
        if (outputBuffer.size() == maxPoints) {
            return;
        }

        const LaserPoint lastPoint = currentFrame->points.back();

        if (state->frameQueue.size() > 1 &&
            frameIsDueAt(*state->frameQueue[1], renderTimeForBufferedPoints(outputBuffer.size()))) {
            state->frameQueue.pop_front();
        } else {
            if (maxFrameHoldTime.count() > 0) {
                const auto elapsed = std::chrono::steady_clock::now() - currentFrame->firstPlayTime;
                if (elapsed >= maxFrameHoldTime) {
                    state->frameQueue.pop_front();
                    const std::size_t needed =
                        minPoints > outputBuffer.size() ? minPoints - outputBuffer.size() : 0;
                    appendBlankPoints(outputBuffer, needed);
                    return;
                }
            }
            currentFrame->nextPoint = 0;
            ++currentFrame->playCount;
        }

        Frame* nextFrame = state->frameQueue.front().get();
        const LaserPoint& firstPoint = nextFrame->points[nextFrame->nextPoint];
        const float dx = firstPoint.x - lastPoint.x;
        const float dy = firstPoint.y - lastPoint.y;
        const float distance = std::sqrt(dx * dx + dy * dy);

        if (distance > blankTransitionDistanceThreshold) {
            generateTransitionPoints(lastPoint, firstPoint, state->pendingTransitionPoints);
            drainPendingTransitionUnsafe(outputBuffer, maxPoints);
            if (outputBuffer.size() == maxPoints) {
                return;
            }
        }
    }
}

void FrameScheduler::fillFrame(const FramePullRequest& request,
                               std::chrono::milliseconds maxFrameHoldTime,
                               Frame& outputFrame,
                               bool verbose) {
    std::lock_guard<std::mutex> lock(state->mutex);
    outputFrame = Frame{};
    drainPendingFramesUnsafe();

    auto estimatedFirstRenderTime = request.estimatedFirstPointRenderTime;
    if (estimatedFirstRenderTime == std::chrono::steady_clock::time_point{}) {
        estimatedFirstRenderTime = std::chrono::steady_clock::now();
    }

    const std::size_t maximumPointsRequired = request.maximumPointsRequired;
    if (maximumPointsRequired == 0) {
        return;
    }

    const auto loadBlankFrame = [this,
                                 &outputFrame,
                                 blankCount = request.blankFramePointCount,
                                 maximumPointsRequired]() {
        outputFrame = Frame{};
        appendBlankPoints(outputFrame.points, std::min(blankCount, maximumPointsRequired));
    };

    // Transition blanking is chunked just like oversized content frames. If a
    // previous call could not fit the entire transition, finish draining it
    // before returning any more content.
    if (!state->pendingTransitionPoints.empty()) {
        drainPendingTransitionUnsafe(outputFrame.points, maximumPointsRequired);
        return;
    }

    if (state->frameQueue.empty()) {
        loadBlankFrame();
        return;
    }

    {
        std::size_t skipped = 0;
        while (state->frameQueue.size() > 1) {
            if (state->frameQueue.front()->playCount != 0) {
                break;
            }
            if (!frameIsDueAt(*state->frameQueue[1], estimatedFirstRenderTime)) {
                break;
            }
            state->frameQueue.pop_front();
            ++skipped;
        }
        if (skipped > 0) {
            logSchedulerVerbose(verbose,
                                "[FrameScheduler] fillFrame: skipped",
                                skipped,
                                "stale frames");
        }
    }

    while (true) {
        if (state->frameQueue.empty()) {
            loadBlankFrame();
            return;
        }

        Frame* currentFrame = state->frameQueue.front().get();
        assert(currentFrame != nullptr && "Null frame in queue");
        assert(!currentFrame->points.empty() && "Empty frame in queue");

        if (currentFrame->playCount == 0 &&
            currentFrame->nextPoint == 0 &&
            !frameIsDueAt(*currentFrame, estimatedFirstRenderTime)) {
            loadBlankFrame();
            return;
        }

        if (currentFrame->nextPoint >= currentFrame->points.size()) {
            if (state->frameQueue.size() > 1 &&
                frameIsDueAt(*state->frameQueue[1], estimatedFirstRenderTime)) {
                const LaserPoint lastPoint = currentFrame->points.back();
                state->frameQueue.pop_front();

                Frame* nextFrame = state->frameQueue.front().get();
                assert(nextFrame != nullptr && !nextFrame->points.empty() &&
                       "Next frame must contain points");

                const LaserPoint& firstPoint = nextFrame->points.front();
                const float dx = firstPoint.x - lastPoint.x;
                const float dy = firstPoint.y - lastPoint.y;
                const float distance = std::sqrt(dx * dx + dy * dy);

                if (distance > blankTransitionDistanceThreshold) {
                    generateTransitionPoints(lastPoint, firstPoint, state->pendingTransitionPoints);
                    drainPendingTransitionUnsafe(outputFrame.points, maximumPointsRequired);
                    return;
                }

                continue;
            }

            if (currentFrame->playCount > 0 && maxFrameHoldTime.count() > 0) {
                const auto elapsed = std::chrono::steady_clock::now() - currentFrame->firstPlayTime;
                if (elapsed >= maxFrameHoldTime) {
                    state->frameQueue.pop_front();
                    loadBlankFrame();
                    return;
                }
            }

            // No replacement frame is ready, so start a new logical loop of the
            // same frame on the next iteration. Oversized frames therefore span
            // multiple transport submissions, but short frames still pass through
            // unchanged one loop at a time.
            currentFrame->nextPoint = 0;
            continue;
        }

        if (currentFrame->nextPoint == 0) {
            // firstPlayTime measures the age of the logical frame being held,
            // not the age of an individual transport chunk. We only switch
            // away from an oversized frame once this logical loop finishes.
            if (currentFrame->playCount == 0) {
                currentFrame->firstPlayTime = std::chrono::steady_clock::now();
            }
            ++currentFrame->playCount;
        }

        const std::size_t available = currentFrame->points.size() - currentFrame->nextPoint;
        const std::size_t toCopy = std::min(available, maximumPointsRequired);

        if (currentFrame->points.size() > maximumPointsRequired) {
            logSchedulerVerbose(verbose,
                                "[FrameScheduler] fillFrame: chunking oversized frame",
                                "framePoints",
                                currentFrame->points.size(),
                                "chunkStart",
                                currentFrame->nextPoint,
                                "chunkSize",
                                toCopy,
                                "max",
                                maximumPointsRequired);
        }

        outputFrame = Frame{};
        outputFrame.time = currentFrame->time;
        outputFrame.points.insert(outputFrame.points.end(),
                                  currentFrame->points.begin() +
                                      static_cast<std::ptrdiff_t>(currentFrame->nextPoint),
                                  currentFrame->points.begin() +
                                      static_cast<std::ptrdiff_t>(currentFrame->nextPoint + toCopy));
        currentFrame->nextPoint += toCopy;
        return;
    }
}

std::size_t FrameScheduler::queuedPointCountUnsafe() const {
    std::size_t queuedPoints = 0;

    for (const auto& frame : state->pendingFrames) {
        if (frame) {
            queuedPoints += frame->points.size();
        }
    }

    for (const auto& frame : state->frameQueue) {
        if (!frame) {
            continue;
        }
        const auto nextPoint = std::min(frame->nextPoint, frame->points.size());
        queuedPoints += (frame->points.size() - nextPoint);
    }

    return queuedPoints;
}

void FrameScheduler::drainPendingFramesUnsafe() {
    while (!state->pendingFrames.empty()) {
        state->frameQueue.push_back(std::move(state->pendingFrames.front()));
        state->pendingFrames.pop_front();
    }
}

void FrameScheduler::appendBlankPoints(std::vector<LaserPoint>& buffer, std::size_t count) const {
    if (count == 0) {
        return;
    }

    LaserPoint blankPoint;
    // Keep scheduler-generated blanks dark on controllers that still honour
    // the legacy intensity channel as well as RGB.
    blankPoint.i = 0.0f;
    buffer.insert(buffer.end(), count, blankPoint);
}

void FrameScheduler::generateTransitionPoints(const LaserPoint& from,
                                              const LaserPoint& to,
                                              std::vector<LaserPoint>& out) const {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const auto count = std::max(
        minBlankPointsPerEnd,
        static_cast<std::size_t>(distance * blankPointsPerUnitDistance));

    out.clear();
    out.reserve(count * 2);

    LaserPoint blankFrom = from;
    blankFrom.r = 0.0f;
    blankFrom.g = 0.0f;
    blankFrom.b = 0.0f;
    blankFrom.i = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(blankFrom);
    }

    LaserPoint blankTo = to;
    blankTo.r = 0.0f;
    blankTo.g = 0.0f;
    blankTo.b = 0.0f;
    blankTo.i = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(blankTo);
    }
}

void FrameScheduler::drainPendingTransitionUnsafe(std::vector<LaserPoint>& outputBuffer,
                                                  std::size_t maxPoints) {
    if (state->pendingTransitionPoints.empty()) {
        return;
    }

    const std::size_t spaceLeft = maxPoints - outputBuffer.size();
    const std::size_t toDrain = std::min(spaceLeft, state->pendingTransitionPoints.size());

    outputBuffer.insert(outputBuffer.end(),
                        state->pendingTransitionPoints.begin(),
                        state->pendingTransitionPoints.begin() +
                            static_cast<std::ptrdiff_t>(toDrain));

    state->pendingTransitionPoints.erase(
        state->pendingTransitionPoints.begin(),
        state->pendingTransitionPoints.begin() + static_cast<std::ptrdiff_t>(toDrain));
}

} // namespace libera::core
