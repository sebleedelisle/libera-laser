#include "libera/core/LaserController.hpp"
#include <cassert>
#include <atomic>

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

std::atomic<std::int64_t>& targetRenderLatencyMsStorage() {
    // One shared frame scheduling latency for all LaserController instances.
    static std::atomic<std::int64_t> latencyMs{0};
    return latencyMs;
}

} // namespace

LaserController::LaserController() = default;
LaserController::~LaserController() = default;

void LaserController::setTargetRenderLatency(std::chrono::milliseconds latency) {
    const auto clamped = std::max<std::int64_t>(0, latency.count());
    targetRenderLatencyMsStorage().store(clamped, std::memory_order_relaxed);
}

std::chrono::milliseconds LaserController::targetRenderLatency() {
    return std::chrono::milliseconds(
        targetRenderLatencyMsStorage().load(std::memory_order_relaxed));
}

// returns false if the controller isn't ready for a new frame or if the frame is empty. 

bool LaserController::sendFrame(Frame&& frame) {
    if (!frameModeActive) {
        startFrameMode();
    }

    if(!isReadyForNewFrame()) return false; 

    // empty frame 
    assert(frame.points.size()>0 &&  "Empty frame!"); 
    if(frame.points.size()==0) return false; // if it's empty then don't add it and return false
    const auto framePointCount = frame.points.size();
    nominalFramePointCount.store(std::max<std::size_t>(framePointCount, 1), std::memory_order_relaxed);

    // Auto-stamp unscheduled frames to now + global target latency so callers
    // can queue frames without manually setting Frame::time each time.
    if (frame.time == std::chrono::steady_clock::time_point{}) {
        frame.time = std::chrono::steady_clock::now() + targetRenderLatency();
    }

    std::lock_guard<std::mutex> lock(pendingFramesMutex);

    pendingFrames.push_back(std::make_shared<Frame>(std::move(frame)));
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
        frameFillCallback(req, out);
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

bool LaserController::frameModeEnabled() const {
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

void LaserController::frameFillCallback(const PointFillRequest& request,
                                    std::vector<LaserPoint>& outputBuffer) {
    // Pull any frames provided by other threads into the local queue so the
    // remainder of this function can operate without locking.
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

    // Some backends can provide an estimate for when the first point in this
    // callback's batch will actually hit the mirrors. If unavailable, fall
    // back to "now" so scheduled frames can still advance.
    auto estimatedFirstRenderTime = request.estimatedFirstPointRenderTime;
    if (estimatedFirstRenderTime == std::chrono::steady_clock::time_point{}) {
        estimatedFirstRenderTime = std::chrono::steady_clock::now();
    }

    if (frameQueue.empty()) {
        // Nothing ready yet, so provide the minimum number of blank samples to
        // keep downstream controller logic satisfied.
        publishQueueMetrics();
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    // If newer frames are already due, we can skip stale queued frames, but we
    // only do that before playback of the current frame has started. Skipping a
    // frame mid-play causes visible "half-frame" truncation.
    while (frameQueue.size() > 1) {
        const auto& current = frameQueue.front();
        if (current->nextPoint != 0) {
            break;
        }
        const auto& queuedNext = frameQueue[1];
        if (!frameIsDueAt(*queuedNext, estimatedFirstRenderTime)) {
            break;
        }
        frameQueue.pop_front();
    }

    // If the front frame is not due yet and we have nothing earlier to show,
    // output blanks until its presentation time.
    if (!frameIsDueAt(*frameQueue.front(), estimatedFirstRenderTime)) {
        publishQueueMetrics();
        appendBlankPoints(outputBuffer, minPoints);
        return;
    }

    while(true) { 

        std::shared_ptr<Frame> currentFrame = frameQueue.front(); 
        if(currentFrame->playCount==0) currentFrame->playCount++; 
        
        assert(currentFrame->points.size()>0 &&  "Empty frame in buffer"); // should never happen

        // keep filling up the buffer from the current frame until the end of the frame or until we 
        // reach the maxPoints
        while((outputBuffer.size()<maxPoints) && (currentFrame->nextPoint<currentFrame->points.size())) { 
            // add the frame point to the buffer
            outputBuffer.push_back(currentFrame->points[currentFrame->nextPoint]); 
            // increment the nextPoint
            currentFrame->nextPoint++; 

        } 
        // if we're at the end of the frame but we're over minPoints, return
        if ((outputBuffer.size()>=minPoints) && (currentFrame->nextPoint>=currentFrame->points.size())) {
            publishQueueMetrics();
            return; 
        }
        // if we're at maxPoints, return
        if(outputBuffer.size()==maxPoints) {
            publishQueueMetrics();
            return; 
        }
        // else it means that we're below minPoints and we're at the end of the
        // current frame. If a queued frame is due, promote it, otherwise repeat
        // the current frame (hold-last-frame behavior).
        if (frameQueue.size() > 1 && frameIsDueAt(*frameQueue[1], estimatedFirstRenderTime)) {
            frameQueue.pop_front();
        } else {
            currentFrame->nextPoint=0; 
            currentFrame->playCount++; 
        }
    }

}

void LaserController::drainPendingFrames() {
    std::lock_guard<std::mutex> lock(pendingFramesMutex);
    while (!pendingFrames.empty()) {
        frameQueue.push_back(pendingFrames.front());
        pendingFrames.pop_front();
    }
    pendingFrameCount.store(pendingFrames.size(), std::memory_order_relaxed);
    pendingPointCount.store(0, std::memory_order_relaxed);
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
        std::max(0, millisToPoints(targetRenderLatency())));
    const auto nominalFramePoints =
        std::max<std::size_t>(nominalFramePointCount.load(std::memory_order_relaxed), 1);
    return latencyPoints + nominalFramePoints;
}



} // namespace libera::core
