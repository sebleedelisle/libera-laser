#include "libera/core/LaserDeviceBase.hpp"
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
#include <windows.h>
#endif

namespace libera::core {
namespace {
void elevateWorkerThreadPriority() {
#if defined(__APPLE__)
    // macOS: use QoS to request a higher scheduling class without root.
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc != 0) {
        logInfo("[LaserDeviceBase] pthread_set_qos_class_self_np failed", rc);
    }
#elif defined(__linux__)
    // Linux: try realtime scheduling; may require CAP_SYS_NICE or root.
    sched_param sch{};
    sch.sched_priority = 10; // modest priority within FIFO range
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch);
    if (rc != 0) {
        logInfo("[LaserDeviceBase] pthread_setschedparam failed", rc, std::strerror(rc));
    }
#elif defined(_WIN32)
    // Windows: boost thread priority within the process.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        logInfo("[LaserDeviceBase] SetThreadPriority failed", static_cast<int>(GetLastError()));
    }
#endif
}
} // namespace

LaserDeviceBase::LaserDeviceBase() {
    pointsToSend.reserve(30000);
}

LaserDeviceBase::~LaserDeviceBase() {
    stop();
}

void LaserDeviceBase::setRequestPointsCallback(const RequestPointsCallback &callback) {
    // Store the callback (copied into the functor).
    requestPointsCallback = callback;
}

bool LaserDeviceBase::requestPoints(const PointFillRequest &request) {
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
        logError("[LaserDeviceBase::requestPoints] - too many points sent! Maximum :", request.maximumPointsRequired, " actual :", pointsToSend.size()); 
        logError("[LaserDeviceBase::requestPoints] - removing additional points"); 
        pointsToSend.resize(request.maximumPointsRequired); 

    } else if(pointsToSend.size()<request.minimumPointsRequired) { 
        // fill up the buffer with blanks
        const std::size_t missing = request.minimumPointsRequired - pointsToSend.size();
        const LaserPoint blankPoint{};
        pointsToSend.insert(pointsToSend.end(), missing, blankPoint);
    }



    // Apply startup blanking (first N points forced to black).
    int blankPointsRemaining = startupBlankPointsRemaining.load(std::memory_order_relaxed);
    if (blankPointsRemaining > 0) {
        for (auto &point : pointsToSend) {
            if (blankPointsRemaining <= 0) {
                break;
            }
            point.r = 0.0f;
            point.g = 0.0f;
            point.b = 0.0f;
            point.i = 0.0f;
            --blankPointsRemaining;
        }
        startupBlankPointsRemaining.store(blankPointsRemaining, std::memory_order_relaxed);
    }

    if(!armed.load(std::memory_order_relaxed) ) { 
        for (auto &point : pointsToSend) {
            point.r = 0.0f;
            point.g = 0.0f;
            point.b = 0.0f;
            point.i = 0.0f;
            point.x = 0; 
            point.y = 0; 
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
        // RGB/I intensity by the requested number of points so colour modulation stays
        // aligned with the mirrors even when their propagation times differ.
        for (auto &point : pointsToSend) {
            scannerSyncColourDelayLine.push_back(point);
            const LaserPoint colourPoint = scannerSyncColourDelayLine.front();
            scannerSyncColourDelayLine.pop_front();
            point.r = colourPoint.r;
            point.g = colourPoint.g;
            point.b = colourPoint.b;
            point.i = colourPoint.i;
        }
    }
    
    return true;
}


void LaserDeviceBase::start() {
    if (running) return; // Already running.
    running = true;
    worker = std::thread([this] {
        elevateWorkerThreadPriority();
        this->run();
    });
}

void LaserDeviceBase::stop() {
    logInfoVerbose("[LaserDeviceBase] stop()");
    running = false;
    if (worker.joinable()) {
        worker.join();
    }
}

double LaserDeviceBase::pointsToMillis(std::size_t pointCount) const {
    return pointsToMillis(pointCount, getPointRate());
}

double LaserDeviceBase::pointsToMillis(std::size_t pointCount, std::uint32_t rate) const {
    if (rate == 0 || pointCount == 0) {
        return 0.0;
    }

    const double millis =
        (static_cast<double>(pointCount) * 1000.0) / static_cast<double>(rate);

    return std::max(millis, 0.0);
}

int LaserDeviceBase::millisToPoints(double millis) const {
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

bool LaserDeviceBase::getArmed() const noexcept { 
    return armed.load(std::memory_order_relaxed);
}

void LaserDeviceBase::setArmed(bool state) { 
    armed.store(state, std::memory_order_relaxed);
} 

void LaserDeviceBase::setPointRate(std::uint32_t pointRateValue) {
    pointRate.store(pointRateValue, std::memory_order_relaxed);
}

std::uint32_t LaserDeviceBase::getPointRate() const noexcept {
    return pointRate.load(std::memory_order_relaxed);
}

std::optional<DacBufferState> LaserDeviceBase::getBufferState() const {
    return std::nullopt;
}

std::optional<DacLatencyStats> LaserDeviceBase::getLatencyStats() const {
    return std::nullopt;
}

void LaserDeviceBase::resetStartupBlank() {
    const int blankPoints = millisToPoints(1.0f);
    startupBlankPointsRemaining.store(blankPoints, std::memory_order_relaxed);
    scannerSyncColourDelayLine.clear();
}

void LaserDeviceBase::setVerbose(bool enabled) {
    verbose.store(enabled, std::memory_order_relaxed);
}

bool LaserDeviceBase::isVerbose() const noexcept {
    return verbose.load(std::memory_order_relaxed);
}

void LaserDeviceBase::setScannerSync(double offsetTenThousandths) {
   // logInfo("[LaserDeviceBase::setScannerSync]", offsetTenThousandths); 
    const double clamped = std::max(offsetTenThousandths, 0.0);
    scannerSyncTime.store(clamped, std::memory_order_relaxed);
}

double LaserDeviceBase::getScannerSync() const noexcept {
    return scannerSyncTime.load(std::memory_order_relaxed);
}

} // namespace libera::core
