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
        return;
    }
    frameModeActive = true;
    setRequestPointsCallback([this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
        fillFromFrameQueue(req, out);
    });
}

void LaserController::stopFrameMode() {
    if (!frameModeActive) {
        return;
    }
    setRequestPointsCallback({});
    {
        std::lock_guard<std::mutex> lock(pendingFramesMutex);
        pendingFrames.clear();
        pendingFrameCount.store(0, std::memory_order_relaxed);
        pendingPointCount.store(0, std::memory_order_relaxed);
    }
    frameModeActive = false;
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

    // Skip stale frames: only skip if playback of the current frame hasn't started.
    {
        std::size_t skipped = 0;
        while (frameQueue.size() > 1) {
            if (frameQueue.front()->nextPoint != 0) {
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

    if (!frameIsDueAt(*frameQueue.front(), estimatedFirstRenderTime)) {
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
        if(currentFrame->playCount==0) currentFrame->playCount++;

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
