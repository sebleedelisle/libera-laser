#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"
#include <cassert>
#include <atomic>
#include <cmath>

namespace libera::core {
namespace {

bool frameIsDueAt(const Frame& frame,
                  std::chrono::steady_clock::time_point estimatedFirstRenderTime) {
    // Unscheduled frames (default time point) are treated as "play immediately".
    if (frame.time == std::chrono::steady_clock::time_point{}) {
        return true;
    }
    return frame.time <= estimatedFirstRenderTime;
}

std::atomic<std::int64_t>& targetLatencyMsStorage() {
    // One shared target latency for all LaserController instances.
    // Match ofxLaser's long-standing default so standalone libera examples do
    // not behave very differently from the wrapper path by default.
    static std::atomic<std::int64_t> latencyMs{100};
    return latencyMs;
}

std::atomic<std::int64_t>& maxFrameHoldTimeMsStorage() {
    // Default 100 ms: generous window to replace a frame, but output goes
    // blank automatically if the sender stops or pauses.
    // Set to 0 to disable (loop the last frame forever).
    static std::atomic<std::int64_t> ms{100};
    return ms;
}

} // namespace

LaserController::LaserController() = default;
LaserController::~LaserController() = default;

void LaserController::setTargetLatency(std::chrono::milliseconds latency) {
    const auto clamped = std::max<std::int64_t>(0, latency.count());
    targetLatencyMsStorage().store(clamped, std::memory_order_relaxed);
}

std::chrono::milliseconds LaserController::targetLatency() {
    return std::chrono::milliseconds(
        targetLatencyMsStorage().load(std::memory_order_relaxed));
}

void LaserController::setMaxFrameHoldTime(std::chrono::milliseconds time) {
    const auto clamped = std::max<std::int64_t>(0, time.count());
    maxFrameHoldTimeMsStorage().store(clamped, std::memory_order_relaxed);
}

std::chrono::milliseconds LaserController::maxFrameHoldTime() {
    return std::chrono::milliseconds(
        maxFrameHoldTimeMsStorage().load(std::memory_order_relaxed));
}

void LaserController::setRequestPointsCallback(const RequestPointsCallback& callback) {
    if (callback) {
        // The public API keeps "last writer wins" behaviour: installing a user
        // point callback abandons any queued frame-mode state immediately.
        stopFrameMode();
        LaserControllerStreaming::setRequestPointsCallback(callback);
        activeSource = ContentSource::UserPoints;
        return;
    }

    LaserControllerStreaming::setRequestPointsCallback({});
    if (activeSource == ContentSource::UserPoints) {
        activeSource = ContentSource::None;
    }
}

// returns false if the controller isn't ready for a new frame or if the frame is empty. 

bool LaserController::sendFrame(Frame&& frame) {
    if (!frameModeActive) {
        startFrameMode();
    }

    if(!isReadyForNewFrame()) return false; 

    if(frame.points.size()==0) return false;
    const auto framePointCount = frame.points.size();
    nominalFramePointCount.store(std::max<std::size_t>(framePointCount, 1), std::memory_order_relaxed);

    // Auto-stamp unscheduled frames to now + global target latency so callers
    // can queue frames without manually setting Frame::time each time.
    if (frame.time == std::chrono::steady_clock::time_point{}) {
        frame.time = std::chrono::steady_clock::now() + targetLatency();
    }

    std::lock_guard<std::mutex> lock(pendingFramesMutex);

    pendingFrames.push_back(std::make_unique<Frame>(std::move(frame)));
    pendingFrameCount.fetch_add(1, std::memory_order_relaxed);
    pendingPointCount.fetch_add(framePointCount, std::memory_order_relaxed);
    return true;
}

void LaserController::startFrameMode() {
    if (frameModeActive) {
        activeSource = ContentSource::FrameQueue;
        return;
    }

    // Frame mode owns scheduling internally, so it must not reuse the user
    // point callback slot in the streaming base class.
    LaserControllerStreaming::setRequestPointsCallback({});
    frameModeActive = true;
    activeSource = ContentSource::FrameQueue;
}

void LaserController::stopFrameMode() {
    if (!frameModeActive) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingFramesMutex);
        pendingFrames.clear();
        pendingFrameCount.store(0, std::memory_order_relaxed);
        pendingPointCount.store(0, std::memory_order_relaxed);
    }
    frameQueue.clear();
    pendingTransitionPoints.clear();
    pendingTransitionFrame.reset();
    updateFrameQueueMetricsUnsafe();
    nominalFramePointCount.store(1, std::memory_order_relaxed);
    frameModeActive = false;
    if (activeSource == ContentSource::FrameQueue) {
        activeSource = ContentSource::None;
    }
}

bool LaserController::isFrameModeEnabled() const {
    return frameModeActive;
}

bool LaserController::isReadyForNewFrame() const {
    const auto queuedPoints =
        frameQueuePointCountEstimate.load(std::memory_order_relaxed) +
        pendingPointCount.load(std::memory_order_relaxed);
    return queuedPoints <= queuedPointBudget();
}

std::size_t LaserController::queuedFrameCount() const {
    return frameQueueCountEstimate.load(std::memory_order_relaxed) +
           pendingFrameCount.load(std::memory_order_relaxed);
}

bool LaserController::requestPoints(const PointFillRequest& request) {
    if (activeSource == ContentSource::UserPoints) {
        return LaserControllerStreaming::requestPoints(request);
    }

    if (activeSource != ContentSource::FrameQueue) {
        return false;
    }

    pointsToSend.clear();
    fillFromFrameQueue(request, pointsToSend);

    // Keep the point-stream contract identical to LaserControllerStreaming so
    // existing controllers and tests continue to see the same behaviour.
    assert(pointsToSend.size() >= request.minimumPointsRequired &&
           "Frame queue did not provide the minimum required number of points.");
    assert(pointsToSend.size() <= request.maximumPointsRequired &&
           "Frame queue produced more points than allowed by maximumPointsRequired.");

    if (pointsToSend.size() > request.maximumPointsRequired) {
        logError("[LaserController::requestPoints] - too many points sent! Maximum :",
                 request.maximumPointsRequired,
                 " actual :",
                 pointsToSend.size());
        logError("[LaserController::requestPoints] - removing additional points");
        pointsToSend.resize(request.maximumPointsRequired);
    } else if (pointsToSend.size() < request.minimumPointsRequired) {
        const std::size_t missing = request.minimumPointsRequired - pointsToSend.size();
        const LaserPoint blankPoint{};
        pointsToSend.insert(pointsToSend.end(), missing, blankPoint);
    }

    postProcessOutputPoints(pointsToSend);
    return true;
}

bool LaserController::requestFrame(const FrameFillRequest& request, Frame& outputFrame) {
    if (activeSource != ContentSource::FrameQueue) {
        return false;
    }

    outputFrame = Frame{};
    drainPendingFrames();

    auto estimatedFirstRenderTime = request.estimatedFirstPointRenderTime;
    if (estimatedFirstRenderTime == std::chrono::steady_clock::time_point{}) {
        estimatedFirstRenderTime = std::chrono::steady_clock::now();
    }

    const auto publishQueueMetrics = [this]() {
        updateFrameQueueMetricsUnsafe();
    };

    const auto clampFrameToMaximum = [&request, this](Frame& frame) {
        if (request.maximumPointsRequired == 0) {
            frame.points.clear();
            return;
        }

        if (frame.points.size() <= request.maximumPointsRequired) {
            return;
        }

        logInfoVerbose("[LaserController] requestFrame: truncating frame from",
                       frame.points.size(),
                       "to",
                       request.maximumPointsRequired,
                       "points");
        frame.points.resize(request.maximumPointsRequired);
    };

    const auto finishFrame = [&]() {
        clampFrameToMaximum(outputFrame);
        postProcessOutputPoints(outputFrame.points);
        publishQueueMetrics();
        return true;
    };

    const auto loadBlankFrame = [&]() {
        outputFrame = Frame{};
        appendBlankPoints(outputFrame.points, request.blankFramePointCount);
    };

    if (pendingTransitionFrame) {
        outputFrame = *pendingTransitionFrame;
        pendingTransitionFrame.reset();
        return finishFrame();
    }

    if (frameQueue.empty()) {
        loadBlankFrame();
        return finishFrame();
    }

    // Skip stale frames that have never been sent. This matches the existing
    // point-stream scheduler, which only discards a frame if a newer frame is
    // already due and the older one has not started yet.
    {
        std::size_t skipped = 0;
        while (frameQueue.size() > 1) {
            if (frameQueue.front()->playCount != 0) {
                break;
            }
            if (!frameIsDueAt(*frameQueue[1], estimatedFirstRenderTime)) {
                break;
            }
            frameQueue.pop_front();
            ++skipped;
        }
        if (skipped > 0) {
            logInfoVerbose("[LaserController] requestFrame: skipped", skipped, "stale frames");
        }
    }

    while (true) {
        if (frameQueue.empty()) {
            loadBlankFrame();
            return finishFrame();
        }

        Frame* currentFrame = frameQueue.front().get();
        assert(currentFrame != nullptr && "Null frame in queue");
        assert(!currentFrame->points.empty() && "Empty frame in queue");

        // The timestamp only gates the very first play. Once a frame has
        // started, we either hold it, replace it with the next due frame, or
        // blank when the hold timeout expires.
        if (currentFrame->playCount == 0 &&
            !frameIsDueAt(*currentFrame, estimatedFirstRenderTime)) {
            loadBlankFrame();
            return finishFrame();
        }

        if (currentFrame->playCount > 0) {
            if (frameQueue.size() > 1 &&
                frameIsDueAt(*frameQueue[1], estimatedFirstRenderTime)) {
                const LaserPoint lastPoint = currentFrame->points.back();
                frameQueue.pop_front();

                Frame* nextFrame = frameQueue.front().get();
                assert(nextFrame != nullptr && !nextFrame->points.empty() &&
                       "Next frame must contain points");

                const LaserPoint& firstPoint = nextFrame->points.front();
                const float dx = firstPoint.x - lastPoint.x;
                const float dy = firstPoint.y - lastPoint.y;
                const float distance = std::sqrt(dx * dx + dy * dy);

                if (distance > BLANK_TRANSITION_DISTANCE_THRESHOLD) {
                    pendingTransitionFrame = std::make_unique<Frame>();
                    generateTransitionPoints(lastPoint,
                                             firstPoint,
                                             pendingTransitionFrame->points);
                    outputFrame = *pendingTransitionFrame;
                    pendingTransitionFrame.reset();
                    return finishFrame();
                }

                continue;
            }

            const auto maxHold = maxFrameHoldTime();
            if (maxHold.count() > 0) {
                const auto elapsed = std::chrono::steady_clock::now() - currentFrame->firstPlayTime;
                if (elapsed >= maxHold) {
                    frameQueue.pop_front();
                    loadBlankFrame();
                    return finishFrame();
                }
            }
        }

        if (currentFrame->playCount == 0) {
            currentFrame->firstPlayTime = std::chrono::steady_clock::now();
        }

        // Native-frame transports should consume whole frames. If a frame is
        // too large for the backend, truncate the queued copy so the dropped
        // tail does not leak into future hold/repeat submissions.
        clampFrameToMaximum(*currentFrame);
        currentFrame->nextPoint = 0;
        ++currentFrame->playCount;
        outputFrame = *currentFrame;
        return finishFrame();
    }
}

bool LaserController::isUsingFrameQueueSource() const {
    return activeSource == ContentSource::FrameQueue;
}

void LaserController::fillFromFrameQueue(const PointFillRequest& request,
                                    std::vector<LaserPoint>& outputBuffer) {
    drainPendingFrames();
    const auto publishQueueMetrics = [this]() {
        updateFrameQueueMetricsUnsafe();
    };

    if (request.maximumPointsRequired == 0) {
        publishQueueMetrics();
        return;
    }

    const std::size_t minPoints = request.minimumPointsRequired;
    const std::size_t maxPoints = request.maximumPointsRequired;

    // Drain any leftover transition points from a previous fill call.
    drainPendingTransition(outputBuffer, maxPoints);
    if (outputBuffer.size() == maxPoints) {
        publishQueueMetrics();
        return;
    }

    auto estimatedFirstRenderTime = request.estimatedFirstPointRenderTime;
    if (estimatedFirstRenderTime == std::chrono::steady_clock::time_point{}) {
        estimatedFirstRenderTime = std::chrono::steady_clock::now();
    }
    const auto renderTimeForBufferedPoints = [this, estimatedFirstRenderTime](std::size_t bufferedPointCount) {
        return estimatedFirstRenderTime +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::duration<double, std::milli>(pointsToMillis(bufferedPointCount)));
    };

    if (frameQueue.empty()) {
        logInfoVerbose("[LaserController] fillFromFrameQueue: queue empty, blanking", minPoints, "pts");
        publishQueueMetrics();
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    // Skip stale frames: only drop a front frame that has NEVER been played.
    // `nextPoint == 0` alone is not enough — a frame that is being held-and-
    // repeated (hold-last-frame behaviour) is also at nextPoint=0 between
    // iterations, and we must not discard it.  `playCount == 0` is the
    // unique marker for "queued but never touched".
    {
        std::size_t skipped = 0;
        while (frameQueue.size() > 1) {
            if (frameQueue.front()->playCount != 0) {
                break;
            }
            if (!frameIsDueAt(*frameQueue[1], estimatedFirstRenderTime)) {
                break;
            }
            frameQueue.pop_front();
            ++skipped;
        }
        if (skipped > 0) {
            logInfoVerbose("[LaserController] fillFromFrameQueue: skipped", skipped, "stale frames");
        }
    }

    // The scheduled `time` is the release target for a frame's FIRST play.
    // Once a frame has been played at least once (playCount > 0) it is being
    // held/repeated, so its original time is historical and we should keep
    // feeding from it rather than blanking.
    if (frameQueue.front()->playCount == 0 &&
        !frameIsDueAt(*frameQueue.front(), estimatedFirstRenderTime)) {
        const auto now = std::chrono::steady_clock::now();
        const auto frameDueIn = std::chrono::duration<double, std::milli>(
            frameQueue.front()->time - estimatedFirstRenderTime).count();
        const auto renderLeadMs = std::chrono::duration<double, std::milli>(
            estimatedFirstRenderTime - now).count();
        logInfoVerbose("[LaserController] fillFromFrameQueue: frame not due, blanking",
                       minPoints, "pts",
                       "frameDueInMs", frameDueIn,
                       "renderLeadMs", renderLeadMs,
                       "queueSize", frameQueue.size());
        publishQueueMetrics();
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    while(true) {
        Frame* currentFrame = frameQueue.front().get();
        if(currentFrame->playCount==0) {
            currentFrame->firstPlayTime = std::chrono::steady_clock::now();
            currentFrame->playCount++;
        }

        assert(currentFrame->points.size()>0 && "Empty frame in buffer");

        // Bulk-copy as many points as possible from the current frame.
        {
            const std::size_t available = currentFrame->points.size() - currentFrame->nextPoint;
            const std::size_t spaceLeft = maxPoints - outputBuffer.size();
            const std::size_t toAdd = std::min(available, spaceLeft);
            outputBuffer.insert(outputBuffer.end(),
                currentFrame->points.begin() + static_cast<std::ptrdiff_t>(currentFrame->nextPoint),
                currentFrame->points.begin() + static_cast<std::ptrdiff_t>(currentFrame->nextPoint + toAdd));
            currentFrame->nextPoint += toAdd;
        }

        if ((outputBuffer.size() >= minPoints) && (currentFrame->nextPoint >= currentFrame->points.size())) {
            publishQueueMetrics();
            return;
        }
        if (outputBuffer.size() == maxPoints) {
            publishQueueMetrics();
            return;
        }

        // Below minPoints and current frame is exhausted. Promote next frame if due,
        // otherwise repeat the current frame (hold-last-frame behaviour).
        // Copy by value — pop_front() below may destroy the frame this came from.
        const LaserPoint lastPoint = currentFrame->points.back();

        if (frameQueue.size() > 1 &&
            frameIsDueAt(*frameQueue[1], renderTimeForBufferedPoints(outputBuffer.size()))) {
            frameQueue.pop_front();
            // currentFrame pointer is now dangling — do not access it.
        } else {
            // Hold-last-frame: check max frame time before looping.
            const auto maxFT = maxFrameHoldTime();
            if (maxFT.count() > 0) {
                const auto elapsed = std::chrono::steady_clock::now() - currentFrame->firstPlayTime;
                if (elapsed >= maxFT) {
                    frameQueue.pop_front();
                    const std::size_t needed = minPoints > outputBuffer.size() ? minPoints - outputBuffer.size() : 0;
                    appendBlankPoints(outputBuffer, needed);
                    publishQueueMetrics();
                    return;
                }
            }
            currentFrame->nextPoint = 0;
            currentFrame->playCount++;
        }

        // Insert transition blanking if the jump between the end of the
        // previous frame and the start of the next content is large enough.
        Frame* nextFrame = frameQueue.front().get();
        const LaserPoint& firstPoint = nextFrame->points[nextFrame->nextPoint];
        const float dx = firstPoint.x - lastPoint.x;
        const float dy = firstPoint.y - lastPoint.y;
        const float distance = std::sqrt(dx * dx + dy * dy);

        if (distance > BLANK_TRANSITION_DISTANCE_THRESHOLD) {
            generateTransitionPoints(lastPoint, firstPoint, pendingTransitionPoints);
            drainPendingTransition(outputBuffer, maxPoints);
            if (outputBuffer.size() == maxPoints) {
                publishQueueMetrics();
                return;
            }
        }
    }
}

void LaserController::drainPendingFrames() {
    {
        std::lock_guard<std::mutex> lock(pendingFramesMutex);
        while (!pendingFrames.empty()) {
            frameQueue.push_back(std::move(pendingFrames.front()));
            pendingFrames.pop_front();
        }
        pendingFrameCount.store(0, std::memory_order_relaxed);
        pendingPointCount.store(0, std::memory_order_relaxed);
    }
    updateFrameQueueMetricsUnsafe();
}

void LaserController::appendBlankPoints(std::vector<LaserPoint>& buffer, std::size_t count) {
    if (count == 0) {
        return;
    }
    LaserPoint blankPoint;
    // Keep library-generated blanks dark on controllers that honour the legacy
    // intensity channel as well as RGB.
    blankPoint.i = 0.0f;
    buffer.insert(buffer.end(), count, blankPoint);
}

void LaserController::updateFrameQueueMetricsUnsafe() {
    std::size_t remainingPoints = 0;
    for (const auto& frame : frameQueue) {
        if (!frame) {
            continue;
        }
        const auto nextPoint = std::min(frame->nextPoint, frame->points.size());
        remainingPoints += (frame->points.size() - nextPoint);
    }

    frameQueueCountEstimate.store(frameQueue.size(), std::memory_order_relaxed);
    frameQueuePointCountEstimate.store(remainingPoints, std::memory_order_relaxed);
}

std::size_t LaserController::queuedPointBudget() const {
    const auto latencyPoints = static_cast<std::size_t>(
        std::max(0, millisToPoints(targetLatency())));
    const auto nominalFramePoints =
        std::max<std::size_t>(nominalFramePointCount.load(std::memory_order_relaxed), 1);
    return latencyPoints + nominalFramePoints;
}



void LaserController::generateTransitionPoints(const LaserPoint& from, const LaserPoint& to,
                                                std::vector<LaserPoint>& out) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const auto count = std::max(MIN_BLANK_POINTS_PER_END,
        static_cast<std::size_t>(distance * BLANK_POINTS_PER_UNIT_DISTANCE));

    out.clear();
    out.reserve(count * 2);

    // Dwell at the old position with laser off.
    LaserPoint blankFrom = from;
    blankFrom.r = 0.0f;
    blankFrom.g = 0.0f;
    blankFrom.b = 0.0f;
    blankFrom.i = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(blankFrom);
    }

    // Dwell at the new position with laser off.
    LaserPoint blankTo = to;
    blankTo.r = 0.0f;
    blankTo.g = 0.0f;
    blankTo.b = 0.0f;
    blankTo.i = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(blankTo);
    }
}

void LaserController::drainPendingTransition(std::vector<LaserPoint>& outputBuffer,
                                              std::size_t maxPoints) {
    if (pendingTransitionPoints.empty()) {
        return;
    }

    const std::size_t spaceLeft = maxPoints - outputBuffer.size();
    const std::size_t toDrain = std::min(spaceLeft, pendingTransitionPoints.size());

    outputBuffer.insert(outputBuffer.end(),
        pendingTransitionPoints.begin(),
        pendingTransitionPoints.begin() + static_cast<std::ptrdiff_t>(toDrain));

    pendingTransitionPoints.erase(
        pendingTransitionPoints.begin(),
        pendingTransitionPoints.begin() + static_cast<std::ptrdiff_t>(toDrain));
}

} // namespace libera::core
