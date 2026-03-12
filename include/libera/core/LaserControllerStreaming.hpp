#pragma once

#include <vector>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <chrono>
#include <optional>
#include <mutex>
#include <utility>
#include "LaserPoint.hpp"
#include "libera/log/Log.hpp"

namespace libera::core {

/**
 * @brief Information provided when the controller asks for new points.
 */
struct PointFillRequest {
    /// Minimum number of points that must be produced by the callback.
    std::size_t minimumPointsRequired = 0;

    /// Maximum number of points that should be produced by the callback.
    std::size_t maximumPointsRequired = 0;

    /// Host-side estimate of when the first point in this batch will reach the mirrors.
    /// (This is advisory; implementations can ignore or use it for scheduling.)
    std::chrono::steady_clock::time_point estimatedFirstPointRenderTime{};

    /// Absolute running counter for emitted points.
    std::uint64_t currentPointIndex = 0;

    [[nodiscard]] bool needsPoints(std::size_t minPoints) const {
        return (minimumPointsRequired > minPoints) || (maximumPointsRequired > minPoints);
    }
};

struct DacBufferState {
    int pointsInBuffer = 0;
    int totalBufferPoints = 0;
};

struct DacLatencyStats {
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    std::size_t sampleCount = 0;
};

/**
 * @brief Callback contract for point generation.
 *
 * The callback must:
 * - Append new points to @p outputBuffer using `push_back` / `emplace_back`.
 * - Produce at least `request.minimumPointsRequired` points.
 * - Produce no more than `request.maximumPointsRequired`.
 * - Not call `reserve()` or `resize()` on @p outputBuffer (avoid allocations
 *   inside the realtime path; the framework reserves large buffers up front).
 * - It may produce more than the minimum, up to outputBuffer.capacity().
 *
 * The caller reads outputBuffer.size() after the callback to know how many
 * points were written.
 */
using RequestPointsCallback =
    std::function<void(const PointFillRequest &request,
                       std::vector<LaserPoint> &outputBuffer)>;

/**
 * @brief Base controller class that manages callback-driven point generation.
 *
 * Subclasses (e.g. EtherDreamController, HeliosController) are responsible
 * for actually sending points to hardware. This base class only handles:
 * - Storing a user-provided callback.
 * - Requesting batches of new points via requestPoints().
 * - Accumulating generated points into an internal buffer for later use.
 *
 * Threading model:
 * - Base manages a worker thread that calls virtual `run()` until `stop()`.
 * - Derived classes implement `run()` (e.g., poll status, send points).
 * - `running` is an atomic flag checked by the loop.
 */
class LaserControllerStreaming {
public:
    /**
     * @brief Construct the controller and reserve internal buffers.
     *
     * Currently reserves ~30k points for the transmission buffer, which is
     * more than most hardware FIFOs. This avoids most reallocations in practice.
     */
    LaserControllerStreaming();
    virtual ~LaserControllerStreaming();
    /**
     * @brief Install or replace the callback that generates points.
     * @param callback Function object or lambda conforming to RequestPointsCallback.
     */
    void setRequestPointsCallback(const RequestPointsCallback &callback);

    /**
     * @brief Ask the callback for more points and append them to the main buffer.
     *
     * Typical usage is from a hardware-specific run loop: call requestPoints() to
     * invoke the user-supplied callback, then send pointsToSend to the DAC.
     *
     * @param request Fill request (min points required, estimated render time).
     * @return false if no callback is installed, true if points were appended.
     */
    bool requestPoints(const PointFillRequest &request);

    /// Start the worker thread.
    void start();

    /// Request the thread to stop and wait for it to finish.
    void stop();

    /**
     * @brief Configure the desired output point rate (points per second).
     *
     * The base implementation simply stores the value; subclasses can override
     * to validate, clamp, or immediately propagate the change to hardware.
     */
    virtual void setPointRate(std::uint32_t pointRateValue);

    /**
     * @brief Retrieve the last configured point rate (points per second).
     */
    virtual std::uint32_t getPointRate() const noexcept;

    /// Host-side view of DAC buffer fullness, if the backend can provide it.
    virtual std::optional<DacBufferState> getBufferState() const;

    /// Rolling latency percentiles (transport/admission), if supported.
    virtual std::optional<DacLatencyStats> getLatencyStats() const;

    // arm status - needs to be set or no output! 
    bool getArmed() const noexcept; 
    void setArmed(bool state = true); 

    // Offset expressed in 1/10,000th of a second (0.1 ms) units.
    void setScannerSync(double offsetTenThousandths); 
    double getScannerSync() const noexcept; 
    void setVerbose(bool enabled);
    bool isVerbose() const noexcept;

    /// Reset the startup blanking window to 1 ms worth of points.
    void resetStartupBlank();

protected:
    /// Worker loop implemented by subclasses.
    virtual void run() = 0;

    template <typename... Args>
    void logInfoVerbose(Args&&... args) const {
        if (verbose.load(std::memory_order_relaxed)) {
            libera::log::logInfo(std::forward<Args>(args)...);
        }
    }

    double pointsToMillis(std::size_t pointCount) const;
    double pointsToMillis(std::size_t pointCount, std::uint32_t rate) const;
    int millisToPoints(double millis) const;
    int millisToPoints(float millis) const { return millisToPoints(static_cast<double>(millis)); }
    int millisToPoints(std::int64_t millis) const { return millisToPoints(static_cast<double>(millis)); }

    template <typename Rep, typename Period>
    int millisToPoints(const std::chrono::duration<Rep, Period>& duration) const {
        const auto millis = std::chrono::duration<double, std::milli>(duration).count();
        return millisToPoints(millis);
    }

    /// Add one latency sample to the rolling window used by getLatencyStats().
    void recordLatencySample(std::chrono::steady_clock::duration sample);

    /// Set host-estimated controller buffer capacity for default getBufferState().
    void setEstimatedBufferCapacity(int totalBufferPoints);

    /// Clear host-estimated buffer state; default getBufferState() returns nullopt.
    void clearEstimatedBufferState();

    /// Update the projected buffer anchor used by default getBufferState().
    /// pointRateValue=0 uses getPointRate() at the time of this call.
    void updateEstimatedBufferAnchor(
        int anchorBufferFullness,
        std::chrono::steady_clock::time_point anchorTime,
        std::uint32_t pointRateValue = 0);

    /// Convenience overload that stamps the anchor at steady_clock::now().
    void updateEstimatedBufferAnchorNow(
        int anchorBufferFullness,
        std::uint32_t pointRateValue = 0);

    /// Estimate current buffer fullness by projecting consumption from an anchor.
    /// If projection is not possible, fallbackBufferFullness is returned.
    int calculateBufferFullnessFromAnchor(
        int anchorBufferFullness,
        std::chrono::steady_clock::time_point anchorTime,
        std::uint32_t pointRate,
        int fallbackBufferFullness,
        bool* projected = nullptr) const;

    static std::uint16_t encodeUnsigned16FromSignedUnit(float value);
    static std::uint16_t encodeUnsigned16FromUnit(float value);
    static std::uint16_t encodeUnsigned12FromSignedUnit(float value);
    static std::uint16_t encodeUnsigned12FromUnit(float value);
    static std::uint8_t encodeUnsigned8FromUnit(float value);

    static int clampBufferFullnessToCapacity(int pointsInBuffer,
                                             int totalBufferPoints);

    /// Shared helper for controllers that expose point-buffer fullness.
    static std::optional<DacBufferState> buildBufferState(int totalBufferPoints,
                                                          int pointsInBuffer);

    std::atomic<bool> armed{false};

    std::thread worker;
    std::atomic<bool> running{false};
    
    std::atomic<std::uint32_t> pointRate{30000};
    std::atomic<bool> verbose{false};

    /// The installed callback that generates points (may be empty if not set).
    RequestPointsCallback requestPointsCallback{};

    /// Main buffer of points pending transmission to the DAC.
    std::vector<LaserPoint> pointsToSend;
    // Stores 1/10,000th of a second units so we match legacy colour-shift semantics.
    std::atomic<double> scannerSyncTime{2.0}; // in 1/10,000 of a second
    std::deque<LaserPoint> scannerSyncColourDelayLine;
    std::atomic<int> startupBlankPointsRemaining{0};

private:
    using SteadyRep = std::chrono::steady_clock::duration::rep;

    static constexpr std::size_t latencySampleWindow = 512;
    mutable std::mutex latencySamplesMutex;
    std::deque<double> latencySamplesMs;

    std::atomic<int> estimatedBufferCapacity{0};
    std::atomic<int> estimatedBufferAnchorFullness{0};
    std::atomic<std::uint32_t> estimatedBufferAnchorPointRate{0};
    std::atomic<SteadyRep> estimatedBufferAnchorTick{0};
    std::atomic<bool> estimatedBufferAnchorValid{false};
};

} // namespace libera::core
