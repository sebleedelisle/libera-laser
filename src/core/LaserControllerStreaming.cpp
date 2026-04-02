#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/core/BufferEstimator.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/log/Log.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#include <windows.h>
#endif

namespace libera::core {
namespace {
void elevateWorkerThreadPriority() {
#if defined(__APPLE__)
    // macOS: use QoS to request a higher scheduling class without root.
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc != 0) {
        logInfo("[LaserControllerStreaming] pthread_set_qos_class_self_np failed", rc);
    }
#elif defined(__linux__)
    // Linux: try realtime scheduling; may require CAP_SYS_NICE or root.
    sched_param sch{};
    sch.sched_priority = 10; // modest priority within FIFO range
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch);
    if (rc != 0) {
        logInfo("[LaserControllerStreaming] pthread_setschedparam failed", rc, std::strerror(rc));
    }
#elif defined(_WIN32)
    // Windows: boost thread priority within the process.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        logInfo("[LaserControllerStreaming] SetThreadPriority failed", static_cast<int>(GetLastError()));
    }
#endif
}

double percentileFromSortedSamples(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }

    const double clampedPercentile = std::clamp(percentile, 0.0, 1.0);
    const double rawIndex = clampedPercentile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(rawIndex));
    const auto upper = static_cast<std::size_t>(std::ceil(rawIndex));
    if (lower == upper) {
        return sorted[lower];
    }

    const double weight = rawIndex - static_cast<double>(lower);
    return sorted[lower] + ((sorted[upper] - sorted[lower]) * weight);
}
} // namespace

LaserControllerStreaming::LaserControllerStreaming() {
    pointsToSend.reserve(30000);
}

LaserControllerStreaming::~LaserControllerStreaming() {
    stop();
}

void LaserControllerStreaming::setRequestPointsCallback(const RequestPointsCallback &callback) {
    // Store the callback (copied into the functor).
    requestPointsCallback = callback;
}

bool LaserControllerStreaming::requestPoints(const PointFillRequest &request) {
    if (!requestPointsCallback) {
        // No callback set, so there is no way to produce points.
        return false;
    }

    // Reset transmission buffer while retaining capacity.
    pointsToSend.clear();

    // Ask the user-supplied callback to append new points.
    requestPointsCallback(request, pointsToSend);

    // Debug-only: enforce the contract that the callback produced at least the requested minimum.
    assert(pointsToSend.size() >= request.minimumPointsRequired &&
           "Callback did not provide the minimum required number of points.");
  
    assert(pointsToSend.size() <= request.maximumPointsRequired &&
               "Callback produced more points than allowed by maximumPointsRequired.");
    if(pointsToSend.size()>request.maximumPointsRequired) { 
        // get rid of extra points
        logError("[LaserControllerStreaming::requestPoints] - too many points sent! Maximum :", request.maximumPointsRequired, " actual :", pointsToSend.size()); 
        logError("[LaserControllerStreaming::requestPoints] - removing additional points"); 
        pointsToSend.resize(request.maximumPointsRequired); 

    } else if(pointsToSend.size()<request.minimumPointsRequired) {
        // fill up the buffer with blanks
        const std::size_t missing = request.minimumPointsRequired - pointsToSend.size();
        const LaserPoint blankPoint{};
        pointsToSend.insert(pointsToSend.end(), missing, blankPoint);
    }



    // Apply startup blanking (first N points forced to black).
    // X/Y pass through so galvos can travel to content position while dark.
    // The delay line was cleared in resetStartupBlank() so it fills with
    // blank-RGB points, providing additional natural blanking during transition.
    int blankPointsRemaining = startupBlankPointsRemaining.load(std::memory_order_relaxed);
    if (blankPointsRemaining > 0) {
        for (auto &point : pointsToSend) {
            if (blankPointsRemaining <= 0) {
                break;
            }
            point.r = 0.0f;
            point.g = 0.0f;
            point.b = 0.0f;
            --blankPointsRemaining;
        }
        startupBlankPointsRemaining.store(blankPointsRemaining, std::memory_order_relaxed);
    }

    if(!armed.load(std::memory_order_relaxed)) {
        // Shutdown blanking: hold at last content position (dark) long enough
        // to flush the scanner sync colour delay line, then move to centre.
        int shutdownRemaining = shutdownBlankPointsRemaining.load(std::memory_order_relaxed);
        for (auto &point : pointsToSend) {
            point.r = 0.0f;
            point.g = 0.0f;
            point.b = 0.0f;
            if (shutdownRemaining > 0) {
                point.x = lastContentX;
                point.y = lastContentY;
                --shutdownRemaining;
            } else {
                point.x = 0.0f;
                point.y = 0.0f;
            }
        }
        shutdownBlankPointsRemaining.store(std::max(shutdownRemaining, 0), std::memory_order_relaxed);
    } else {
        // Track last content position for use during shutdown blanking.
        if (!pointsToSend.empty()) {
            const auto& lastPoint = pointsToSend.back();
            lastContentX = lastPoint.x;
            lastContentY = lastPoint.y;
        }
    } 

    // applies scanner sync
    const double syncTenThousandths =
        std::max(scannerSyncTime.load(std::memory_order_relaxed), 0.0);
    
    const auto shiftPointCount =
        static_cast<std::size_t>(millisToPoints(syncTenThousandths * 0.1));

    scannerSyncColourDelayLine.resize(shiftPointCount); 
    
    if(shiftPointCount>0) { 
        // Feed the FIFO with geometry points while reusing the delayed colour sample
        // from `scannerSyncColourDelayLine`. This keeps X/Y data contiguous but shifts
        // RGB by the requested number of points so colour modulation stays aligned
        // with the mirrors even when their propagation times differ.
        for (auto &point : pointsToSend) {
            scannerSyncColourDelayLine.push_back(point);
            const LaserPoint colourPoint = scannerSyncColourDelayLine.front();
            scannerSyncColourDelayLine.pop_front();
            point.r = colourPoint.r;
            point.g = colourPoint.g;
            point.b = colourPoint.b;
        }
    }
    
    return true;
}


void LaserControllerStreaming::start() {
    if (running) return; // Already running.
    workerFinished.store(false, std::memory_order_relaxed);
    running = true;
    worker = std::thread([this] {
        elevateWorkerThreadPriority();
        this->run();
        workerFinished.store(true, std::memory_order_release);
    });
}

void LaserControllerStreaming::stop() {
    logInfoVerbose("[LaserControllerStreaming] stop()");
    running = false;
    timedJoin(worker, workerFinished, std::chrono::milliseconds(5000),
              "LaserControllerStreaming::worker");
}

double LaserControllerStreaming::pointsToMillis(std::size_t pointCount) const {
    return pointsToMillis(pointCount, getPointRate());
}

double LaserControllerStreaming::pointsToMillis(std::size_t pointCount, std::uint32_t rate) const {
    if (rate == 0 || pointCount == 0) {
        return 0.0;
    }

    const double millis =
        (static_cast<double>(pointCount) * 1000.0) / static_cast<double>(rate);

    return std::max(millis, 0.0);
}

int LaserControllerStreaming::millisToPoints(double millis) const {
    const auto rate = getPointRate();
    if (rate == 0 || millis <= 0.0) {
        return 0;
    }

    const double seconds = millis / 1000.0;
    const double rawPoints = seconds * static_cast<double>(rate);
    const auto rounded = static_cast<long long>(std::llround(rawPoints));

    if (rounded <= 0) {
        return 0;
    }

    return static_cast<int>(std::min<long long>(rounded, std::numeric_limits<int>::max()));
}

bool LaserControllerStreaming::isArmed() const noexcept {
    return armed.load(std::memory_order_relaxed);
}

void LaserControllerStreaming::setArmed(bool state) {
    const bool wasArmed = armed.exchange(state, std::memory_order_relaxed);
    if (state && !wasArmed) {
        resetStartupBlank();
    }
    if (!state && wasArmed) {
        resetShutdownBlank();
    }
} 

void LaserControllerStreaming::setPointRate(std::uint32_t pointRateValue) {
    pointRate.store(pointRateValue, std::memory_order_relaxed);
}

std::uint32_t LaserControllerStreaming::getPointRate() const noexcept {
    return pointRate.load(std::memory_order_relaxed);
}

std::optional<BufferState> LaserControllerStreaming::getBufferState() const {
    const int totalBufferPoints =
        estimatedBufferCapacity.load(std::memory_order_relaxed);
    if (totalBufferPoints <= 0) {
        return std::nullopt;
    }

    if (!estimatedBufferAnchorValid.load(std::memory_order_relaxed)) {
        return buildBufferState(totalBufferPoints, 0);
    }

    const int anchorBufferFullness =
        estimatedBufferAnchorFullness.load(std::memory_order_relaxed);
    auto anchorPointRate =
        estimatedBufferAnchorPointRate.load(std::memory_order_relaxed);
    if (anchorPointRate == 0) {
        anchorPointRate = getPointRate();
    }

    const auto anchorTick = estimatedBufferAnchorTick.load(std::memory_order_relaxed);
    const auto anchorTime = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{anchorTick}};

    const int estimatedBufferFullness = calculateBufferFullnessFromAnchor(
        anchorBufferFullness,
        anchorTime,
        anchorPointRate,
        anchorBufferFullness);

    return buildBufferState(totalBufferPoints, estimatedBufferFullness);
}

std::optional<LatencyStats> LaserControllerStreaming::getLatencyStats() const {
    std::lock_guard<std::mutex> lock(latencySamplesMutex);
    if (latencySamplesMs.empty()) {
        return std::nullopt;
    }

    // Return cached result if the sample set hasn't changed since last computation.
    if (latencyMutationCount == cachedLatencyMutationCount) {
        return cachedLatencyStats;
    }

    std::vector<double> sortedSamples(latencySamplesMs.begin(), latencySamplesMs.end());
    std::sort(sortedSamples.begin(), sortedSamples.end());

    LatencyStats stats;
    stats.sampleCount = sortedSamples.size();
    stats.p50Ms = percentileFromSortedSamples(sortedSamples, 0.50);
    stats.p95Ms = percentileFromSortedSamples(sortedSamples, 0.95);
    stats.p99Ms = percentileFromSortedSamples(sortedSamples, 0.99);
    stats.maxMs = sortedSamples.back();

    cachedLatencyStats = stats;
    cachedLatencyMutationCount = latencyMutationCount;
    return stats;
}

ControllerStatus LaserControllerStreaming::getStatus() const noexcept {
    if (!controllerConnected.load(std::memory_order_relaxed)) {
        return ControllerStatus::Error;
    }
    if (hasIntermittentErrors.load(std::memory_order_relaxed)) {
        return ControllerStatus::Issues;
    }
    return ControllerStatus::Good;
}

std::vector<ControllerErrorInfo> LaserControllerStreaming::getErrors() const {
    std::vector<ControllerErrorInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(errorCountsMutex);
        snapshot.reserve(errorCounts.size());
        for (const auto& entry : errorCounts) {
            snapshot.push_back(ControllerErrorInfo{
                entry.first,
                std::string(error_types::labelFor(entry.first)),
                entry.second});
        }
    }

    std::sort(snapshot.begin(), snapshot.end(),
              [](const ControllerErrorInfo& a, const ControllerErrorInfo& b) {
                  if (a.count != b.count) {
                      return a.count > b.count;
                  }
                  return a.code < b.code;
              });
    return snapshot;
}

void LaserControllerStreaming::clearErrors() {
    hasIntermittentErrors.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(errorCountsMutex);
    errorCounts.clear();
}

void LaserControllerStreaming::recordLatencySample(std::chrono::steady_clock::duration sample) {
    const double sampleMs =
        std::chrono::duration<double, std::milli>(sample).count();
    if (sampleMs < 0.0 || !std::isfinite(sampleMs)) {
        return;
    }

    std::lock_guard<std::mutex> lock(latencySamplesMutex);
    latencySamplesMs.push_back(sampleMs);
    ++latencyMutationCount;
    while (latencySamplesMs.size() > latencySampleWindow) {
        latencySamplesMs.pop_front();
    }
}

void LaserControllerStreaming::setEstimatedBufferCapacity(int totalBufferPoints) {
    estimatedBufferCapacity.store(std::max(0, totalBufferPoints), std::memory_order_relaxed);
}

void LaserControllerStreaming::clearEstimatedBufferState() {
    estimatedBufferAnchorValid.store(false, std::memory_order_relaxed);
    estimatedBufferCapacity.store(0, std::memory_order_relaxed);
    estimatedBufferAnchorFullness.store(0, std::memory_order_relaxed);
    estimatedBufferAnchorPointRate.store(0, std::memory_order_relaxed);
    estimatedBufferAnchorTick.store(0, std::memory_order_relaxed);
}

void LaserControllerStreaming::updateEstimatedBufferAnchor(
    int anchorBufferFullness,
    std::chrono::steady_clock::time_point anchorTime,
    std::uint32_t pointRateValue) {
    estimatedBufferAnchorFullness.store(std::max(0, anchorBufferFullness), std::memory_order_relaxed);
    if (pointRateValue == 0) {
        pointRateValue = getPointRate();
    }
    estimatedBufferAnchorPointRate.store(pointRateValue, std::memory_order_relaxed);
    estimatedBufferAnchorTick.store(anchorTime.time_since_epoch().count(), std::memory_order_relaxed);
    estimatedBufferAnchorValid.store(true, std::memory_order_relaxed);
}

void LaserControllerStreaming::updateEstimatedBufferAnchorNow(
    int anchorBufferFullness,
    std::uint32_t pointRateValue) {
    updateEstimatedBufferAnchor(
        anchorBufferFullness,
        std::chrono::steady_clock::now(),
        pointRateValue);
}

int LaserControllerStreaming::calculateBufferFullnessFromAnchor(
    int anchorBufferFullness,
    std::chrono::steady_clock::time_point anchorTime,
    std::uint32_t rate,
    int fallbackBufferFullness,
    bool* projected) const {
    const auto estimate = BufferEstimator::estimateFromAnchor(
        anchorBufferFullness,
        anchorTime,
        rate);

    if (projected) {
        *projected = estimate.projected;
    }

    if (!estimate.projected) {
        return std::max(0, fallbackBufferFullness);
    }
    return std::max(0, estimate.bufferFullness);
}

std::uint16_t LaserControllerStreaming::encodeUnsigned16FromSignedUnit(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
}

std::uint16_t LaserControllerStreaming::encodeUnsigned16FromUnit(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * 65535.0f));
}

std::uint16_t LaserControllerStreaming::encodeUnsigned12FromSignedUnit(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(std::lround(normalized * 4095.0f));
}

std::uint16_t LaserControllerStreaming::encodeUnsigned12FromUnit(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * 4095.0f));
}

std::uint8_t LaserControllerStreaming::encodeUnsigned8FromUnit(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

int LaserControllerStreaming::clampBufferFullnessToCapacity(
    int pointsInBuffer,
    int totalBufferPoints) {
    if (totalBufferPoints <= 0) {
        return 0;
    }
    return std::clamp(pointsInBuffer, 0, totalBufferPoints);
}

std::optional<BufferState> LaserControllerStreaming::buildBufferState(
    int totalBufferPoints,
    int pointsInBuffer) {
    if (totalBufferPoints <= 0) {
        return std::nullopt;
    }

    BufferState state;
    state.totalBufferPoints = totalBufferPoints;
    state.pointsInBuffer = clampBufferFullnessToCapacity(pointsInBuffer, totalBufferPoints);
    return state;
}

void LaserControllerStreaming::setConnectionState(bool connected) noexcept {
    controllerConnected.store(connected, std::memory_order_relaxed);
}

void LaserControllerStreaming::recordIntermittentError(std::string_view errorType) {
    incrementErrorCount(errorType);
    hasIntermittentErrors.store(true, std::memory_order_relaxed);
}

void LaserControllerStreaming::recordConnectionError(std::string_view errorType) {
    incrementErrorCount(errorType);
    setConnectionState(false);
}

void LaserControllerStreaming::incrementErrorCount(std::string_view errorType) {
    if (errorType.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(errorCountsMutex);
    ++errorCounts[std::string(errorType)];
}

void LaserControllerStreaming::resetStartupBlank() {
    const int blankPoints = millisToPoints(1.0f);
    startupBlankPointsRemaining.store(blankPoints, std::memory_order_relaxed);
    scannerSyncColourDelayLine.clear();
}

void LaserControllerStreaming::resetShutdownBlank() {
    // Hold at last content position (dark) long enough to flush the scanner
    // sync colour delay line plus 1 ms dwell, so no stale colours leak through
    // as galvos travel back to centre.
    const double syncTenThousandths =
        std::max(scannerSyncTime.load(std::memory_order_relaxed), 0.0);
    const int syncPoints = static_cast<int>(millisToPoints(syncTenThousandths * 0.1));
    const int dwellPoints = millisToPoints(1.0);
    shutdownBlankPointsRemaining.store(syncPoints + dwellPoints, std::memory_order_relaxed);
    scannerSyncColourDelayLine.clear();
}

void LaserControllerStreaming::setVerbose(bool enabled) {
    verbose.store(enabled, std::memory_order_relaxed);
}

bool LaserControllerStreaming::isVerbose() const noexcept {
    return verbose.load(std::memory_order_relaxed);
}

void LaserControllerStreaming::setScannerSync(double offsetTenThousandths) {
    const double clamped = std::max(offsetTenThousandths, 0.0);
    scannerSyncTime.store(clamped, std::memory_order_relaxed);
}

double LaserControllerStreaming::getScannerSync() const noexcept {
    return scannerSyncTime.load(std::memory_order_relaxed);
}

} // namespace libera::core
