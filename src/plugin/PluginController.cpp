#include "libera/plugin/PluginController.hpp"

#include "libera/core/BufferEstimator.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace libera::plugin {

namespace {

// Used when the plugin doesn't report buffer state: target ~10ms at the
// current rate, split evenly between min and max so fillFromFrameQueue
// has flexibility when content frames are shorter than the batch.
constexpr int FALLBACK_MIN_BATCH_POINTS = 20;
constexpr int FALLBACK_MAX_BATCH_POINTS = 4096;
constexpr double FALLBACK_BATCH_DURATION_MS = 10.0;

int fallbackBatchPointCount(std::uint32_t pointRate) {
    if (pointRate == 0) return FALLBACK_MIN_BATCH_POINTS;
    const double raw = (static_cast<double>(pointRate) * FALLBACK_BATCH_DURATION_MS) / 1000.0;
    const auto rounded = static_cast<int>(std::lround(raw));
    return std::clamp(rounded, FALLBACK_MIN_BATCH_POINTS, FALLBACK_MAX_BATCH_POINTS);
}

void convertPoints(const std::vector<core::LaserPoint>& src,
                   std::vector<libera_point_t>& dst) {
    dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const auto& p = src[i];
        auto& d = dst[i];
        d.x = static_cast<int16_t>(std::clamp(p.x, -1.0f, 1.0f) * 32767.0f);
        d.y = static_cast<int16_t>(std::clamp(p.y, -1.0f, 1.0f) * 32767.0f);
        d.r = static_cast<uint16_t>(std::clamp(p.r, 0.0f, 1.0f) * 65535.0f);
        d.g = static_cast<uint16_t>(std::clamp(p.g, 0.0f, 1.0f) * 65535.0f);
        d.b = static_cast<uint16_t>(std::clamp(p.b, 0.0f, 1.0f) * 65535.0f);
        d.i = static_cast<uint16_t>(std::clamp(p.i, 0.0f, 1.0f) * 65535.0f);
        d.u1 = static_cast<uint16_t>(std::clamp(p.u1, 0.0f, 1.0f) * 65535.0f);
        d.u2 = static_cast<uint16_t>(std::clamp(p.u2, 0.0f, 1.0f) * 65535.0f);
    }
}

const char* describeStatus(libera_status_t status) {
    switch (status) {
        case LIBERA_OK:                   return "ok";
        case LIBERA_ERR_DISCONNECTED:     return "disconnected";
        case LIBERA_ERR_TIMEOUT:          return "timeout";
        case LIBERA_ERR_BUSY:             return "busy";
        case LIBERA_ERR_PROTOCOL:         return "protocol";
        case LIBERA_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case LIBERA_ERR_INTERNAL:         return "internal";
    }
    return "unknown";
}

} // anonymous namespace

PluginController::PluginController(const PluginFunctions& funcs,
                                   const std::string& controllerId)
: funcs(funcs)
, controllerId(controllerId) {}

PluginController::~PluginController() {
    stop();
    if (pluginHandle) {
        funcs.disconnect(pluginHandle);
        pluginHandle = nullptr;
    }
}

bool PluginController::open() {
    pluginHandle = funcs.connect(controllerId.c_str(),
                                 static_cast<libera_host_ctx_t>(this));
    if (!pluginHandle) {
        libera::log::logError("Plugin: failed to connect to ", controllerId);
        return false;
    }
    connected.store(true);
    setConnectionState(true);
    // Propagate the current point rate immediately.
    if (funcs.set_point_rate) {
        funcs.set_point_rate(pluginHandle, getPointRate());
    }
    return true;
}

void PluginController::setPointRate(std::uint32_t pointRateValue) {
    core::LaserController::setPointRate(pointRateValue);
    if (pluginHandle && funcs.set_point_rate) {
        funcs.set_point_rate(pluginHandle, pointRateValue);
    }
}

std::optional<core::BufferState> PluginController::getBufferState() const {
    const auto pts = cachedPointsInBuffer.load(std::memory_order_relaxed);
    const auto cap = cachedTotalBufferPoints.load(std::memory_order_relaxed);
    if (pts < 0 || cap <= 0) return std::nullopt;
    return buildBufferState(cap, pts);
}

void PluginController::recordLatencyFromPlugin(std::uint64_t nanoseconds) {
    recordLatencySample(std::chrono::nanoseconds(nanoseconds));
}

std::vector<PluginProperty> PluginController::listProperties() const {
    std::vector<PluginProperty> props;
    if (!pluginHandle || !funcs.list_properties) return props;

    auto emit = [](void* raw, const char* key, const char* label) {
        auto* out = static_cast<std::vector<PluginProperty>*>(raw);
        out->push_back({
            key ? std::string(key) : std::string{},
            label ? std::string(label) : std::string{}
        });
    };
    funcs.list_properties(pluginHandle, emit, &props);
    return props;
}

std::optional<std::string> PluginController::getProperty(const std::string& key) const {
    if (!pluginHandle || !funcs.get_property) return std::nullopt;

    // First call: small stack buffer.  If the value doesn't fit, retry
    // once with a heap buffer sized to the returned length.
    char stackBuf[256];
    int needed = funcs.get_property(pluginHandle, key.c_str(),
                                    stackBuf, sizeof(stackBuf));
    if (needed < 0) return std::nullopt;

    if (static_cast<std::size_t>(needed) < sizeof(stackBuf)) {
        return std::string(stackBuf, static_cast<std::size_t>(needed));
    }

    std::string big;
    big.resize(static_cast<std::size_t>(needed) + 1);
    const int rc = funcs.get_property(pluginHandle, key.c_str(),
                                      big.data(),
                                      static_cast<std::uint32_t>(big.size()));
    if (rc < 0) return std::nullopt;
    big.resize(static_cast<std::size_t>(std::min(rc, needed)));
    return big;
}

void PluginController::reportErrorFromPlugin(const char* code, const char* /*label*/) {
    if (!code || !*code) {
        recordIntermittentError("plugin.error");
    } else {
        recordIntermittentError(code);
    }
}

void PluginController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    std::vector<libera_point_t> pluginPoints;
    pluginPoints.reserve(FALLBACK_MAX_BATCH_POINTS);

    while (running.load()) {
        if (!connected.load(std::memory_order_relaxed)) {
            setConnectionState(false);
            std::this_thread::sleep_for(100ms);
            continue;
        }
        setConnectionState(true);

        // Keep the plugin's armed state in sync.
        funcs.set_armed(pluginHandle, isArmed());

        const auto rate = getPointRate();

        // Query the plugin's buffer state.  When available, this drives
        // latency-aware sizing identical to the built-in controllers: we
        // aim to keep the plugin's buffer filled to `targetBufferPoints`
        // (rate * targetLatency), and ask for the deficit.
        libera_buffer_state_t bs{-1, -1};
        bool haveBuffer = false;
        if (funcs.get_buffer_state &&
            funcs.get_buffer_state(pluginHandle, &bs) == 0 &&
            bs.total_buffer_points > 0 && bs.points_in_buffer >= 0) {
            cachedPointsInBuffer.store(bs.points_in_buffer, std::memory_order_relaxed);
            cachedTotalBufferPoints.store(bs.total_buffer_points, std::memory_order_relaxed);
            haveBuffer = true;
        }

        std::size_t minPoints;
        std::size_t maxPoints;

        if (haveBuffer) {
            const int capacity = bs.total_buffer_points;
            const int fullness = bs.points_in_buffer;
            const int freeSpace = std::max(0, capacity - fullness);
            const int target = core::BufferEstimator::targetBufferPoints(
                rate, capacity, targetLatency(),
                /* minimumBufferFloor   */ 0,
                /* safetyHeadroomPoints */ 0);
            const int deficit = std::max(0, target - fullness);

            // Nothing to send: buffer already at or above target, or no
            // room for new points.  Sleep a short time so we don't spin.
            if (deficit <= 0 || freeSpace <= 0) {
                std::this_thread::sleep_for(2ms);
                continue;
            }

            minPoints = static_cast<std::size_t>(deficit);
            maxPoints = static_cast<std::size_t>(freeSpace);
        } else {
            // No buffer state from the plugin — use a fixed ~10ms batch.
            const int fixed = fallbackBatchPointCount(rate);
            minPoints = static_cast<std::size_t>(std::max(fixed / 2, 1));
            maxPoints = static_cast<std::size_t>(fixed);
        }

        // When the plugin reports buffer state, we know how long the queued
        // points will take to play — so the first point of THIS new batch
        // will render after that.  This matters for frame scheduling:
        // fillFromFrameQueue uses this time to decide which queued frame is
        // due.  Getting it wrong means frames play late (glitches on rapidly
        // changing content).
        // When the plugin doesn't report buffer state, we fall back to the
        // host's configured target latency — that's what a well-behaved
        // plugin's pipeline should be converging toward anyway, and it
        // matches the stamp sendFrame() puts on unscheduled frames.
        auto renderTimeLead = std::chrono::duration_cast<std::chrono::milliseconds>(targetLatency());
        if (haveBuffer && rate > 0 && bs.points_in_buffer > 0) {
            const double ms = (static_cast<double>(bs.points_in_buffer) * 1000.0)
                              / static_cast<double>(rate);
            renderTimeLead = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double, std::milli>(ms));
        }

        // Pull points through requestPoints(), which invokes the frame-mode
        // callback (fillFromFrameQueue) and then applies scanner sync,
        // startup/shutdown blanking, and other processing.
        core::PointFillRequest request;
        request.minimumPointsRequired = minPoints;
        request.maximumPointsRequired = maxPoints;
        request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now() + renderTimeLead;

        if (!requestPoints(request) || pointsToSend.empty()) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        convertPoints(pointsToSend, pluginPoints);

        const auto status = funcs.send_points(pluginHandle,
                                              pluginPoints.data(),
                                              static_cast<uint32_t>(pluginPoints.size()));
        pointsToSend.clear();

        if (status != LIBERA_OK) {
            libera::log::logError("Plugin: send_points failed (",
                                  describeStatus(status), ")");
            std::string code = "plugin.send.";
            code += describeStatus(status);
            if (status == LIBERA_ERR_DISCONNECTED) {
                recordConnectionError(code);
                connected.store(false, std::memory_order_relaxed);
            } else {
                recordIntermittentError(code);
            }
        }

        // 1 Hz diagnostic summary — prints whenever PluginController is
        // busy in the run loop.
        const auto nowSample = std::chrono::steady_clock::now();
        if (nowSample >= nextLogAt) {
            const double blankPct = diagSentPoints > 0
                ? (100.0 * diagBlankPoints) / static_cast<double>(diagSentPoints)
                : 0.0;
            libera::log::logInfo("[PluginController] ",
                "rate=", rate,
                " sent=", diagSentPoints,
                " blank=", diagBlankPoints, " (", blankPct, "%)",
                " underruns=", diagUnderruns,
                " fullSleeps=", diagBufferFullSleeps,
                " ringFill=[", (diagMinFullness == INT_MAX ? 0 : diagMinFullness),
                             "..", diagMaxFullness, "]",
                " qFrames=[", (diagMinQueuedFrames == SIZE_MAX ? 0 : diagMinQueuedFrames),
                             "..", diagMaxQueuedFrames, "]",
                " leadMs=[", (diagMinRenderLeadMs == INT_MAX ? 0 : diagMinRenderLeadMs),
                            "..", diagMaxRenderLeadMs, "]");
            diagSentPoints = 0;
            diagBlankPoints = 0;
            diagUnderruns = 0;
            diagBufferFullSleeps = 0;
            diagMinFullness = INT_MAX;
            diagMaxFullness = 0;
            diagMinRenderLeadMs = INT_MAX;
            diagMaxRenderLeadMs = 0;
            diagMinQueuedFrames = SIZE_MAX;
            diagMaxQueuedFrames = 0;
            nextLogAt = nowSample + 1s;
        }
    }

    // Disarm on shutdown.
    if (pluginHandle) {
        funcs.set_armed(pluginHandle, false);
    }
}

} // namespace libera::plugin
