#include "libera/core/LaserDeviceBase.hpp"
#include "libera/log/Log.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <limits>

namespace libera::core {

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

    

    return true;
}


void LaserDeviceBase::start() {
    if (running) return; // Already running.
    running = true;
    worker = std::thread([this] {
        this->run();
    });
}

void LaserDeviceBase::stop() {
    logInfo("[EtherDreamDevice] stop()\n");
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

void LaserDeviceBase::setPointRate(std::uint32_t pointRateValue) {
    pointRate.store(pointRateValue, std::memory_order_relaxed);
}

std::uint32_t LaserDeviceBase::getPointRate() const {
    return pointRate.load(std::memory_order_relaxed);
}

} // namespace libera::core
