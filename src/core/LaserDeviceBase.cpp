#include "libera/core/LaserDeviceBase.hpp"
#include "libera/log/Log.hpp"
#include <cassert>

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
    if (request.maximumPointsRequired > 0) {
        assert(pointsToSend.size() <= request.maximumPointsRequired &&
               "Callback produced more points than allowed by maximumPointsRequired.");
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
} // namespace libera::core
