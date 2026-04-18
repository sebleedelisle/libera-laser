#include "libera/core/PointStreamFramer.hpp"
#include "libera/core/LaserController.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace libera::core {

void PointStreamFramer::setNominalFrameSize(std::size_t size) {
    nominalFrameSize = std::max<std::size_t>(size, 1);
}

void PointStreamFramer::setMaxFrameSize(std::size_t size) {
    maxFrameSize = std::max<std::size_t>(size, 1);
}

void PointStreamFramer::setVirtualBufferTarget(std::size_t size) {
    virtualBufferTarget = size;
}

void PointStreamFramer::setTransportBufferedPoints(std::size_t size) {
    transportBufferedPoints = size;
}

std::size_t PointStreamFramer::bufferedPointCount() const {
    return accumulator.size();
}

bool PointStreamFramer::extractFrame(const RequestPointsCallback& callback,
                                     const PointFillRequest& templateRequest,
                                     Frame& outputFrame) {
    if (!callback) {
        return false;
    }

    // Determine how many points we want in the accumulator before searching.
    // Normally 2x nominal so we can see a full loop, but if we've been
    // force-emitting repeatedly (no natural boundaries found), don't over-buffer.
    const float maxFactor = (consecutiveForceEmits >= forceEmitFallbackCount)
                                ? 1.1f
                                : searchWindowMaxFactor;
    const std::size_t lookaheadTarget =
        std::min(static_cast<std::size_t>(nominalFrameSize * maxFactor), maxFrameSize);
    const std::size_t desiredBufferedPoints =
        std::max(virtualBufferTarget, lookaheadTarget);
    const std::size_t currentBufferedPoints = transportBufferedPoints + accumulator.size();

    // Pull only the deficit needed to reach the shared virtual backlog target.
    // This keeps callback generation bounded when the frame transport is
    // already sitting on a deep queue of previously accepted frames.
    if (currentBufferedPoints < desiredBufferedPoints) {
        const std::size_t deficit = desiredBufferedPoints - currentBufferedPoints;
        pullPoints(callback, templateRequest, deficit);
    }

    if (accumulator.empty()) {
        return false;
    }

    // Establish anchor from the first point if needed.
    if (!anchorSet) {
        anchorX = accumulator[0].x;
        anchorY = accumulator[0].y;
        anchorSet = true;
    }

    // Search for a natural boundary.
    const std::size_t splitIndex = findNaturalBoundary();

    if (splitIndex > 0) {
        // Natural boundary found — emit points [0, splitIndex).
        outputFrame = Frame{};
        outputFrame.points.assign(accumulator.begin(),
                                  accumulator.begin() + static_cast<std::ptrdiff_t>(splitIndex));
        accumulator.erase(accumulator.begin(),
                          accumulator.begin() + static_cast<std::ptrdiff_t>(splitIndex));

        // Reset anchor to the start of the remaining accumulator.
        if (!accumulator.empty()) {
            anchorX = accumulator[0].x;
            anchorY = accumulator[0].y;
        } else {
            anchorSet = false;
        }
        consecutiveForceEmits = 0;
        return true;
    }

    // No natural boundary — force-emit at nominal size.
    const std::size_t emitCount = std::min(nominalFrameSize, accumulator.size());
    outputFrame = Frame{};
    outputFrame.points.assign(accumulator.begin(),
                              accumulator.begin() + static_cast<std::ptrdiff_t>(emitCount));
    accumulator.erase(accumulator.begin(),
                      accumulator.begin() + static_cast<std::ptrdiff_t>(emitCount));

    // Re-establish anchor from whatever is next.
    if (!accumulator.empty()) {
        anchorX = accumulator[0].x;
        anchorY = accumulator[0].y;
    } else {
        anchorSet = false;
    }
    ++consecutiveForceEmits;
    return true;
}

void PointStreamFramer::reset() {
    accumulator.clear();
    anchorX = 0.0f;
    anchorY = 0.0f;
    anchorSet = false;
    virtualBufferTarget = 0;
    transportBufferedPoints = 0;
    consecutiveForceEmits = 0;
    totalPointsConsumed = 0;
}

void PointStreamFramer::pullPoints(const RequestPointsCallback& callback,
                                   const PointFillRequest& templateRequest,
                                   std::size_t count) {
    if (count == 0) return;

    PointFillRequest req = templateRequest;
    req.minimumPointsRequired = count;
    req.maximumPointsRequired = count;
    // Keep the callback's absolute point index advancing from the transport's
    // current playout cursor rather than restarting at zero inside the framer.
    req.currentPointIndex = templateRequest.currentPointIndex + totalPointsConsumed;

    std::vector<LaserPoint> batch;
    batch.reserve(count);
    callback(req, batch);

    accumulator.insert(accumulator.end(), batch.begin(), batch.end());
    totalPointsConsumed += batch.size();
}

std::size_t PointStreamFramer::findNaturalBoundary() const {
    if (accumulator.size() < 2) {
        return 0;
    }

    const float minFactor = (consecutiveForceEmits >= forceEmitFallbackCount)
                                ? 0.9f
                                : searchWindowMinFactor;
    const float maxFactor = (consecutiveForceEmits >= forceEmitFallbackCount)
                                ? 1.1f
                                : searchWindowMaxFactor;

    const std::size_t windowStart =
        static_cast<std::size_t>(nominalFrameSize * minFactor);
    const std::size_t windowEnd =
        std::min({static_cast<std::size_t>(nominalFrameSize * maxFactor),
                  maxFrameSize,
                  accumulator.size()});

    if (windowStart >= windowEnd) {
        return 0;
    }

    std::size_t bestCandidate = 0;
    float bestScore = -1.0f;

    for (std::size_t i = windowStart; i < windowEnd; ++i) {
        const LaserPoint& p = accumulator[i];

        if (!isBlanked(p)) {
            continue;
        }

        // Distance from anchor.
        const float dx = p.x - anchorX;
        const float dy = p.y - anchorY;
        const float dist = std::sqrt(dx * dx + dy * dy);

        // Position score: closer to anchor is better.
        const float positionScore = 1.0f - std::clamp(dist / distanceThreshold, 0.0f, 1.0f);
        if (positionScore <= 0.0f) {
            continue; // Too far from anchor.
        }

        // Size score: prefer frame sizes near 1.0x nominal.
        const float sizeRatio = static_cast<float>(i) / static_cast<float>(nominalFrameSize);
        const float sizeScore = 1.0f - std::abs(sizeRatio - 1.0f);

        // Bonus: prefer the last blanked point before lit content resumes
        // (the blank-to-lit transition is the ideal split point).
        float gapEndBonus = 0.0f;
        if (i + 1 < accumulator.size() && !isBlanked(accumulator[i + 1])) {
            gapEndBonus = 1.0f;
        }

        const float score = positionScore * 0.6f + sizeScore * 0.3f + gapEndBonus * 0.1f;

        if (score > bestScore) {
            bestScore = score;
            bestCandidate = i + 1; // Split AFTER this blanked point.
        }
    }

    // Require a minimum quality threshold.
    if (bestScore < 0.3f) {
        return 0;
    }

    return bestCandidate;
}

bool PointStreamFramer::isBlanked(const LaserPoint& p) {
    return (p.r + p.g + p.b) < blankThreshold;
}

} // namespace libera::core
