#include "libera/core/LaserController.hpp"
#include "libera/core/BufferEstimator.hpp"
#include "libera/core/FrameScheduler.hpp"
#include "libera/core/PointStreamFramer.hpp"
#include "libera/log/Log.hpp"

#include <cassert>
#include <atomic>
#include <limits>

namespace libera::core {
namespace {

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

LaserController::LaserController()
    : frameScheduler(std::make_unique<FrameScheduler>()) {}

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

void LaserController::setPointCallback(const PointCallback& callback) {
    if (!callback) {
        clearPointCallback();
        return;
    }

    std::lock_guard<std::mutex> lock(contentSourceMutex);

    // Keep "last writer wins" behaviour, but serialize the source switch so
    // the worker cannot observe the queue being torn down mid-request.
    frameScheduler->reset();
    resetPointCallbackAdapterState();
    LaserControllerStreaming::setRequestPointsCallback(callback);
    activeSource = ContentSource::PointCallback;
}

void LaserController::clearPointCallback() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    LaserControllerStreaming::setRequestPointsCallback({});
    if (activeSource == ContentSource::PointCallback) {
        resetPointCallbackAdapterState();
        activeSource = ContentSource::None;
    }
}

void LaserController::setRequestPointsCallback(const RequestPointsCallback& callback) {
    setPointCallback(callback);
}

void LaserController::clearPointCallbackPrefetch() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    if (activeSource != ContentSource::PointCallback) {
        return;
    }
    resetPointCallbackAdapterState();
}

// returns false if the controller isn't ready for a new frame or if the frame is empty. 

bool LaserController::sendFrame(Frame&& frame) {
    if (frame.points.empty()) {
        return false;
    }

    // Auto-stamp unscheduled frames to now + global target latency so callers
    // can queue frames without manually setting Frame::time each time.
    if (frame.time == std::chrono::steady_clock::time_point{}) {
        frame.time = std::chrono::steady_clock::now() + targetLatency();
    }

    std::lock_guard<std::mutex> lock(contentSourceMutex);
    if (activeSource != ContentSource::FrameQueue) {
        // Frame mode owns scheduling internally, so it must not reuse the user
        // point callback slot in the streaming base class.
        resetPointCallbackAdapterState();
        LaserControllerStreaming::setRequestPointsCallback({});
        activeSource = ContentSource::FrameQueue;
    }

    if (!frameScheduler->isReadyForNewFrame(queuedPointBudget())) {
        return false;
    }

    return frameScheduler->enqueueFrame(std::move(frame));
}

void LaserController::useFrameQueue() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    if (activeSource == ContentSource::FrameQueue) {
        return;
    }

    // Frame mode owns scheduling internally, so it must not reuse the user
    // point callback slot in the streaming base class.
    resetPointCallbackAdapterState();
    LaserControllerStreaming::setRequestPointsCallback({});
    activeSource = ContentSource::FrameQueue;
}

void LaserController::clearFrameQueue() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    frameScheduler->reset();
    if (activeSource == ContentSource::FrameQueue) {
        activeSource = ContentSource::None;
    }
}

void LaserController::clearContentSource() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    LaserControllerStreaming::setRequestPointsCallback({});
    frameScheduler->reset();
    resetPointCallbackAdapterState();
    activeSource = ContentSource::None;
}

LaserController::ContentSource LaserController::contentSource() const {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    return activeSource;
}

void LaserController::startFrameMode() {
    useFrameQueue();
}

void LaserController::stopFrameMode() {
    clearFrameQueue();
}

bool LaserController::isFrameModeEnabled() const {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    return activeSource == ContentSource::FrameQueue;
}

bool LaserController::isReadyForNewFrame() const {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    return frameScheduler->isReadyForNewFrame(queuedPointBudget());
}

std::size_t LaserController::queuedFrameCount() const {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    return frameScheduler->queuedFrameCount();
}

std::optional<BufferState> LaserController::getBufferState() const {
    const auto transportState = LaserControllerStreaming::getBufferState();

    ContentSource source = ContentSource::None;
    {
        std::lock_guard<std::mutex> lock(contentSourceMutex);
        source = activeSource;
    }

    if (source != ContentSource::PointCallback) {
        return transportState;
    }

    std::size_t framerBufferedPoints = 0;
    {
        std::lock_guard<std::mutex> lock(pointStreamFramerMutex);
        if (pointStreamFramer) {
            framerBufferedPoints = pointStreamFramer->bufferedPointCount();
        }
    }
    if (framerBufferedPoints == 0) {
        return transportState;
    }

    const std::size_t transportBufferedPoints =
        transportState ? static_cast<std::size_t>(std::max(transportState->pointsInBuffer, 0)) : 0;
    const std::size_t virtualBufferedPoints = transportBufferedPoints + framerBufferedPoints;
    const std::size_t virtualTargetPoints = std::max(
        lastPointCallbackVirtualBufferTarget.load(std::memory_order_relaxed),
        virtualBufferedPoints);

    return buildBufferState(
        clampPointCountToInt(virtualTargetPoints),
        clampPointCountToInt(virtualBufferedPoints));
}

std::optional<LaserController::PointCallbackBufferBreakdown>
LaserController::getPointCallbackBufferBreakdown() const {
    const auto transportState = LaserControllerStreaming::getBufferState();
    const std::size_t transportBufferedPoints =
        transportState ? static_cast<std::size_t>(std::max(transportState->pointsInBuffer, 0)) : 0;

    ContentSource source = ContentSource::None;
    {
        std::lock_guard<std::mutex> lock(contentSourceMutex);
        source = activeSource;
    }

    std::size_t prefetchedPoints = 0;
    if (source == ContentSource::PointCallback) {
        std::lock_guard<std::mutex> lock(pointStreamFramerMutex);
        if (pointStreamFramer) {
            prefetchedPoints = pointStreamFramer->bufferedPointCount();
        }
    }

    if (!transportState && prefetchedPoints == 0) {
        return std::nullopt;
    }

    PointCallbackBufferBreakdown breakdown;
    breakdown.transportBufferedPoints = transportBufferedPoints;
    breakdown.prefetchedPoints = prefetchedPoints;
    breakdown.totalBufferedPoints = transportBufferedPoints + prefetchedPoints;
    return breakdown;
}

bool LaserController::requestPoints(const PointFillRequest& request) {
    ContentSource source = ContentSource::None;
    {
        std::lock_guard<std::mutex> lock(contentSourceMutex);
        // Snapshot the source choice quickly, then let the actual point pull run
        // without holding the mode-switch mutex for the whole callback/schedule step.
        source = activeSource;
    }

    if (source == ContentSource::PointCallback) {
        return LaserControllerStreaming::requestPoints(request);
    }

    if (source != ContentSource::FrameQueue) {
        return false;
    }

    pointsToSend.clear();
    frameScheduler->fillPoints(request,
                               getPointRate(),
                               maxFrameHoldTime(),
                               pointsToSend,
                               isVerbose());

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
    ContentSource source = ContentSource::None;
    {
        std::lock_guard<std::mutex> lock(contentSourceMutex);
        source = activeSource;
    }

    if (source == ContentSource::PointCallback) {
        std::size_t preferredPointCount = request.preferredPointCount;
        if (preferredPointCount == 0) {
            preferredPointCount = request.blankFramePointCount;
        }
        if (preferredPointCount == 0) {
            preferredPointCount = request.maximumPointsRequired;
        }
        if (request.maximumPointsRequired > 0) {
            preferredPointCount = std::min(preferredPointCount, request.maximumPointsRequired);
        }

        if (preferredPointCount == 0) {
            outputFrame = Frame{};
            return false;
        }

        RequestPointsCallback callback;
        {
            std::lock_guard<std::mutex> lock(requestPointsCallbackMutex);
            callback = requestPointsCallback;
        }
        if (!callback) {
            outputFrame = Frame{};
            return false;
        }

        PointFillRequest templateReq{};
        templateReq.estimatedFirstPointRenderTime = request.estimatedFirstPointRenderTime;
        templateReq.currentPointIndex = request.currentPointIndex;

        const std::size_t transportBufferedPoints = currentFrameTransportBufferedPoints();
        const std::size_t virtualBufferTarget =
            pointCallbackVirtualBufferTarget(preferredPointCount);

        lastPointCallbackVirtualBufferTarget.store(
            virtualBufferTarget,
            std::memory_order_relaxed);

        bool extractedFrame = false;
        {
            std::lock_guard<std::mutex> lock(pointStreamFramerMutex);
            if (!pointStreamFramer) {
                pointStreamFramer = std::make_unique<PointStreamFramer>();
            }
            pointStreamFramer->setNominalFrameSize(preferredPointCount);
            pointStreamFramer->setMaxFrameSize(
                request.maximumPointsRequired > 0 ? request.maximumPointsRequired : preferredPointCount);
            pointStreamFramer->setVirtualBufferTarget(virtualBufferTarget);
            pointStreamFramer->setTransportBufferedPoints(transportBufferedPoints);
            extractedFrame = pointStreamFramer->extractFrame(callback, templateReq, outputFrame);
        }

        if (!extractedFrame) {
            outputFrame = Frame{};
            return false;
        }
        postProcessOutputPoints(outputFrame.points);
        return true;
    }

    if (source != ContentSource::FrameQueue) {
        outputFrame = Frame{};
        return false;
    }

    FramePullRequest schedulerRequest;
    schedulerRequest.maximumPointsRequired = request.maximumPointsRequired;
    schedulerRequest.blankFramePointCount = request.blankFramePointCount;
    schedulerRequest.estimatedFirstPointRenderTime = request.estimatedFirstPointRenderTime;
    schedulerRequest.currentPointIndex = request.currentPointIndex;
    frameScheduler->fillFrame(
        schedulerRequest,
        maxFrameHoldTime(),
        outputFrame,
        isVerbose());
    postProcessOutputPoints(outputFrame.points);
    return true;
}

bool LaserController::isUsingFrameQueueSource() const {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    return activeSource == ContentSource::FrameQueue;
}

std::size_t LaserController::queuedPointBudget() const {
    const auto latencyPoints = static_cast<std::size_t>(
        std::max(0, millisToPoints(targetLatency())));
    const auto nominalFramePoints =
        std::max<std::size_t>(frameScheduler->nominalFramePointCount(), 1);
    return latencyPoints + nominalFramePoints;
}

void LaserController::noteFrameTransportSubmission(
    std::size_t pointCount,
    std::chrono::steady_clock::time_point estimatedFirstPointRenderTime,
    std::uint32_t pointRateValue) {
    noteFrameTransportSubmissionBounded(
        pointCount,
        estimatedFirstPointRenderTime,
        pointRateValue,
        std::numeric_limits<std::size_t>::max());
}

void LaserController::noteFrameTransportSubmissionBounded(
    std::size_t pointCount,
    std::chrono::steady_clock::time_point estimatedFirstPointRenderTime,
    std::uint32_t pointRateValue,
    std::size_t maxCarryOverPoints) {
    if (pointCount == 0) {
        return;
    }

    if (pointRateValue == 0) {
        pointRateValue = getPointRate();
    }

    const auto now = std::chrono::steady_clock::now();
    const auto requestedStartTime =
        estimatedFirstPointRenderTime == std::chrono::steady_clock::time_point{}
            ? now
            : estimatedFirstPointRenderTime;

    std::size_t totalBufferedPoints = pointCount;
    std::chrono::steady_clock::time_point snapshotTime = std::max(requestedStartTime, now);

    {
        std::lock_guard<std::mutex> lock(frameTransportEstimateMutex);
        if (frameTransportEstimate.valid) {
            // Project remaining backlog forward to the *new* snapshotTime, not
            // to now. The previous backlog drains continuously while the new
            // frame waits for its render slot to open; by snapshotTime, only
            // the portion that hasn't drained by then carries over. Without
            // this, a frame-first transport with a small firmware buffer (like
            // Helios USB) double-counts the currently-playing frame on every
            // submission, and the projection of "when the next write will
            // play" drifts further into the future on each iteration —
            // breaking the latency target and making FrameScheduler's
            // stale-skipping uneven.
            const auto carryEstimate = BufferEstimator::estimateFromSnapshot(
                clampPointCountToInt(frameTransportEstimate.snapshotPoints),
                frameTransportEstimate.snapshotTime,
                frameTransportEstimate.pointRate,
                /*now=*/snapshotTime);
            const int remainingPoints = carryEstimate.projected
                ? std::max(0, carryEstimate.bufferFullness)
                : clampPointCountToInt(frameTransportEstimate.snapshotPoints);

            if (remainingPoints > 0) {
                const auto carryOverPoints = std::min<std::size_t>(
                    static_cast<std::size_t>(remainingPoints),
                    maxCarryOverPoints);
                totalBufferedPoints += carryOverPoints;
            }
        }

        frameTransportEstimate.snapshotPoints = totalBufferedPoints;
        frameTransportEstimate.snapshotTime = snapshotTime;
        frameTransportEstimate.pointRate = pointRateValue;
        frameTransportEstimate.valid = true;
    }

    const std::size_t bufferedTarget = std::max({
        lastPointCallbackVirtualBufferTarget.load(std::memory_order_relaxed),
        pointCallbackVirtualBufferTarget(pointCount),
        totalBufferedPoints});
    setEstimatedBufferCapacity(clampPointCountToInt(bufferedTarget));
    updateEstimatedBufferSnapshot(
        clampPointCountToInt(totalBufferedPoints),
        snapshotTime,
        pointRateValue);
}

void LaserController::clearFrameTransportSubmissionEstimate() {
    {
        std::lock_guard<std::mutex> lock(frameTransportEstimateMutex);
        frameTransportEstimate = FrameTransportEstimate{};
    }
    clearEstimatedBufferState();
}

std::chrono::steady_clock::time_point
LaserController::projectedNextWriteRenderTime(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration writeLead) const {
    // Floor: a write physically can't land in the past. Even with a stale or
    // empty submission estimate the next write still needs at least writeLead
    // to clear the bus.
    const auto floorTime = now + writeLead;

    std::lock_guard<std::mutex> lock(frameTransportEstimateMutex);
    if (!frameTransportEstimate.valid || frameTransportEstimate.pointRate == 0) {
        return floorTime;
    }

    // Project drain of the most recent submission snapshot. snapshotPoints is
    // the total points buffered downstream as of snapshotTime (the just-written
    // frame plus any carry-over from previous in-flight frames). When that
    // backlog drains, the slot freed up is the one our next write will play in.
    const auto playoutDuration = std::chrono::microseconds(
        (static_cast<std::int64_t>(frameTransportEstimate.snapshotPoints) * 1'000'000)
        / static_cast<std::int64_t>(frameTransportEstimate.pointRate));
    const auto drainCompleteTime = frameTransportEstimate.snapshotTime + playoutDuration;

    return std::max(drainCompleteTime, floorTime);
}

void LaserController::resetPointCallbackAdapterState() {
    std::lock_guard<std::mutex> lock(pointStreamFramerMutex);
    if (pointStreamFramer) {
        pointStreamFramer->reset();
    }
    lastPointCallbackVirtualBufferTarget.store(0, std::memory_order_relaxed);
}

std::size_t LaserController::pointCallbackVirtualBufferTarget(std::size_t nominalFramePoints) const {
    const auto latencyPoints = static_cast<std::size_t>(
        std::max(0, millisToPoints(targetLatency())));
    return latencyPoints + std::max<std::size_t>(nominalFramePoints, 1);
}

std::size_t LaserController::currentFrameTransportBufferedPoints() const {
    const auto transportState = LaserControllerStreaming::getBufferState();
    if (!transportState) {
        return 0;
    }
    return static_cast<std::size_t>(std::max(transportState->pointsInBuffer, 0));
}

int LaserController::clampPointCountToInt(std::size_t pointCount) {
    return static_cast<int>(std::min<std::size_t>(
        pointCount,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

} // namespace libera::core
