#include "libera/core/LaserDevice.hpp"
#include <cassert>

namespace libera::core {

LaserDevice::LaserDevice() = default;
LaserDevice::~LaserDevice() = default;

bool LaserDevice::sendFrame(Frame&& frame) {
    if (!frameModeActive) {
        startFrameMode();
    }

    if(!isReadyForNewFrame()) return false; 

    // empty frame ? 
    assert(frame.points.size()>0 &&  "Empty frame!"); // should never happen
    if(frame.points.size()==0) frame.points.push_back(LaserPoint());

    std::lock_guard<std::mutex> lock(pendingFramesMutex);

    pendingFrames.push_back(std::make_shared<Frame>(std::move(frame)));
    return true;
}

void LaserDevice::startFrameMode() {
    if (frameModeActive) {
        return;
    }
    frameModeActive = true;
    setRequestPointsCallback([this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
        frameFillCallback(req, out);
    });
}

void LaserDevice::stopFrameMode() {
    if (!frameModeActive) {
        return;
    }
    setRequestPointsCallback({});
    {
        std::lock_guard<std::mutex> lock(pendingFramesMutex);
        pendingFrames.clear();
    }
    frameModeActive = false;
}

bool LaserDevice::frameModeEnabled() const {
    return frameModeActive;
}

bool LaserDevice::isReadyForNewFrame() const {
    std::lock_guard<std::mutex> lock(pendingFramesMutex);
    const std::size_t pendingCount = pendingFrames.size();
    const std::size_t activeCount = frameQueue.size();
    return (activeCount + pendingCount) <= 1;
}

void LaserDevice::frameFillCallback(const PointFillRequest& request,
                                    std::vector<LaserPoint>& outputBuffer) {
    // Pull any frames provided by other threads into the local queue so the
    // remainder of this function can operate without locking.
    drainPendingFrames();

    if (request.maximumPointsRequired == 0) {
        return;
    }

    const std::size_t minPoints = request.minimumPointsRequired;
    const std::size_t maxPoints = request.maximumPointsRequired;


    if (frameQueue.empty()) {
        // Nothing ready yet, so provide the minimum number of blank samples to
        // keep downstream DAC logic satisfied.
        if(minPoints >0) {
            outputBuffer.insert(outputBuffer.end(), minPoints, LaserPoint{});
        }
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
        if((outputBuffer.size()>minPoints) && (currentFrame->nextPoint>=currentFrame->points.size()) )
            return; 
        // if we're at maxPoints, return
        if(outputBuffer.size()==maxPoints) 
            return; 
        // else it means that we're not at minPoints and we're at the end of the frame
        // if we have no more frames, repeat this one
        if(frameQueue.size()==1) { 
            currentFrame->nextPoint=0; 
            currentFrame->playCount++; 
        } else { 
            while(frameQueue.size()>1) { 
                frameQueue.pop_front(); 
            } 
            currentFrame = frameQueue.front();
        } 
    }

}

void LaserDevice::drainPendingFrames() {
    std::lock_guard<std::mutex> lock(pendingFramesMutex);
    while (!pendingFrames.empty()) {
        frameQueue.push_back(pendingFrames.front());
        pendingFrames.pop_front();
    }
}



} // namespace libera::core
