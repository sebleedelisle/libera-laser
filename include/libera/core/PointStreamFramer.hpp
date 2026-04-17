#pragma once

#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/core/LaserPoint.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace libera::core {

struct Frame;

/**
 * @brief Splits a continuous point stream into natural frames for frame-ingester DACs.
 *
 * Frame-based DACs (Helios USB, PluginController) replay the last submitted
 * frame until a replacement arrives. If the frame boundary falls mid-shape the
 * DAC snaps back to a partial picture, causing visible flicker.
 *
 * PointStreamFramer accumulates points from a callback, detects natural loop
 * closures (blanked points near the accumulation anchor), and emits visually
 * complete frames. If no natural boundary is found it falls back to a fixed-
 * size emit identical to the previous behaviour.
 */
class PointStreamFramer {
public:
    /// Set the target frame size in points (e.g. pointRate * 10ms / 1000).
    void setNominalFrameSize(std::size_t size);

    /// Set the hard upper limit on emitted frame size (e.g. HELIOS_MAX_POINTS).
    void setMaxFrameSize(std::size_t size);

    /**
     * @brief Extract one natural frame from the point stream.
     *
     * Pulls points from @p callback into an internal accumulator, searches for
     * a natural boundary, and writes the result into @p outputFrame. Returns
     * false if the callback is null or produces no points.
     *
     * @param callback         The user-installed point-generation callback.
     * @param templateRequest  Base PointFillRequest with timing/index fields.
     * @param outputFrame      Receives the emitted frame.
     */
    bool extractFrame(const RequestPointsCallback& callback,
                      const PointFillRequest& templateRequest,
                      Frame& outputFrame);

    /// Discard all accumulated state (e.g. on content source change).
    void reset();

private:
    void pullPoints(const RequestPointsCallback& callback,
                    const PointFillRequest& templateRequest,
                    std::size_t count);

    /// Search the accumulator for the best natural frame boundary.
    /// Returns the split index (emit points [0, index)), or 0 if none found.
    std::size_t findNaturalBoundary() const;

    static bool isBlanked(const LaserPoint& p);

    std::vector<LaserPoint> accumulator;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    bool anchorSet = false;
    std::size_t nominalFrameSize = 300;
    std::size_t maxFrameSize = 4095;
    std::size_t consecutiveForceEmits = 0;
    std::uint64_t totalPointsConsumed = 0;

    // Tunable parameters
    static constexpr float distanceThreshold = 0.15f;
    static constexpr float blankThreshold = 0.01f;
    static constexpr float searchWindowMinFactor = 0.5f;
    static constexpr float searchWindowMaxFactor = 2.0f;
    static constexpr std::size_t forceEmitFallbackCount = 3;
};

} // namespace libera::core
