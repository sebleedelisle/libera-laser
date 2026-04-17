#include "libera/core/LaserController.hpp"
#include "libera/core/FrameScheduler.hpp"
#include "libera/core/PointStreamFramer.hpp"
#include "libera/log/Log.hpp"

#include <cassert>
#include <atomic>

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
    LaserControllerStreaming::setRequestPointsCallback(callback);
    activeSource = ContentSource::PointCallback;
}

void LaserController::clearPointCallback() {
    std::lock_guard<std::mutex> lock(contentSourceMutex);
    LaserControllerStreaming::setRequestPointsCallback({});
    if (activeSource == ContentSource::PointCallback) {
        if (pointStreamFramer) pointStreamFramer->reset();
        activeSource = ContentSource::None;
    }
}

void LaserController::setRequestPointsCallback(const RequestPointsCallback& callback) {
    setPointCallback(callback);
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
    if (pointStreamFramer) pointStreamFramer->reset();
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

        if (!pointStreamFramer) {
            pointStreamFramer = std::make_unique<PointStreamFramer>();
        }
        pointStreamFramer->setNominalFrameSize(preferredPointCount);
        pointStreamFramer->setMaxFrameSize(
            request.maximumPointsRequired > 0 ? request.maximumPointsRequired : preferredPointCount);

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

        if (!pointStreamFramer->extractFrame(callback, templateReq, outputFrame)) {
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

} // namespace libera::core
