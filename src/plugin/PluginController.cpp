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
// current rate, split evenly between min and max so the shared content-source
// adapter has some flexibility when queued frames are shorter than the batch.
constexpr int FALLBACK_MIN_BATCH_POINTS = 20;
constexpr int FALLBACK_MAX_BATCH_POINTS = 4096;
constexpr double FALLBACK_BATCH_DURATION_MS = 10.0;

int fallbackBatchPointCount(std::uint32_t pointRate) {
    if (pointRate == 0) {
        return FALLBACK_MIN_BATCH_POINTS;
    }

    const double raw = (static_cast<double>(pointRate) * FALLBACK_BATCH_DURATION_MS) / 1000.0;
    const auto rounded = static_cast<int>(std::lround(raw));
    return std::clamp(rounded, FALLBACK_MIN_BATCH_POINTS, FALLBACK_MAX_BATCH_POINTS);
}

void convertPoints(const std::vector<core::LaserPoint>& src,
                   std::vector<libera_point_t>& dst) {
    dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const auto& point = src[i];
        auto& out = dst[i];
        out.x = static_cast<int16_t>(std::clamp(point.x, -1.0f, 1.0f) * 32767.0f);
        out.y = static_cast<int16_t>(std::clamp(point.y, -1.0f, 1.0f) * 32767.0f);
        out.r = static_cast<uint16_t>(std::clamp(point.r, 0.0f, 1.0f) * 65535.0f);
        out.g = static_cast<uint16_t>(std::clamp(point.g, 0.0f, 1.0f) * 65535.0f);
        out.b = static_cast<uint16_t>(std::clamp(point.b, 0.0f, 1.0f) * 65535.0f);
        out.i = static_cast<uint16_t>(std::clamp(point.i, 0.0f, 1.0f) * 65535.0f);
        out.u1 = static_cast<uint16_t>(std::clamp(point.u1, 0.0f, 1.0f) * 65535.0f);
        out.u2 = static_cast<uint16_t>(std::clamp(point.u2, 0.0f, 1.0f) * 65535.0f);
    }
}

const char* describeStatus(libera_status_t status) {
    switch (status) {
        case LIBERA_OK:
            return "ok";
        case LIBERA_ERR_DISCONNECTED:
            return "disconnected";
        case LIBERA_ERR_TIMEOUT:
            return "timeout";
        case LIBERA_ERR_BUSY:
            return "busy";
        case LIBERA_ERR_PROTOCOL:
            return "protocol";
        case LIBERA_ERR_INVALID_ARGUMENT:
            return "invalid_argument";
        case LIBERA_ERR_INTERNAL:
            return "internal";
    }
    return "unknown";
}

std::optional<std::uint32_t> propertyIndexForKey(const libera_plugin_api_t* api,
                                                 const std::string& key) {
    if (!api || !api->properties || api->property_count == 0) {
        return std::nullopt;
    }

    for (std::uint32_t i = 0; i < api->property_count; ++i) {
        const char* candidate = api->properties[i].key;
        if (candidate && key == candidate) {
            return i;
        }
    }

    return std::nullopt;
}

} // namespace

PluginController::PluginController(const libera_plugin_api_t* api,
                                   void* backendHandle,
                                   const libera_controller_info_t& controllerInfo)
: api(api)
, backendHandle(backendHandle)
, controllerInfo(controllerInfo) {}

PluginController::~PluginController() {
    stopThread();
    if (pluginHandle && api && api->destroy_controller) {
        api->destroy_controller(pluginHandle);
        pluginHandle = nullptr;
    }
}

bool PluginController::open() {
    if (!api || !api->connect_controller) {
        return false;
    }

    pluginHandle = api->connect_controller(
        backendHandle,
        &controllerInfo,
        static_cast<libera_host_ctx_t>(this));
    if (!pluginHandle) {
        libera::log::logError("Plugin: failed to connect to ", controllerInfo.id);
        return false;
    }

    connected.store(true, std::memory_order_relaxed);
    setConnectionState(true);

    if (api->set_point_rate) {
        api->set_point_rate(pluginHandle, getPointRate());
    }

    lastSentArmed = !isArmed();
    return true;
}

void PluginController::setPointRate(std::uint32_t pointRateValue) {
    core::LaserController::setPointRate(pointRateValue);
    if (pluginHandle && api && api->set_point_rate) {
        api->set_point_rate(pluginHandle, pointRateValue);
    }
}

void PluginController::recordLatencyFromPlugin(std::uint64_t nanoseconds) {
    recordLatencySample(std::chrono::nanoseconds(nanoseconds));
}

std::vector<PluginProperty> PluginController::listProperties() const {
    std::vector<PluginProperty> props;
    if (!api || !api->properties || api->property_count == 0) {
        return props;
    }

    props.reserve(api->property_count);
    for (std::uint32_t i = 0; i < api->property_count; ++i) {
        const char* key = api->properties[i].key;
        const char* label = api->properties[i].label;
        props.push_back({
            key ? std::string(key) : std::string{},
            label ? std::string(label) : std::string{}
        });
    }

    return props;
}

std::optional<std::string> PluginController::getProperty(const std::string& key) const {
    if (!pluginHandle || !api || !api->read_property) {
        return std::nullopt;
    }

    const auto propertyIndex = propertyIndexForKey(api, key);
    if (!propertyIndex) {
        return std::nullopt;
    }

    char stackBuf[256];
    const int needed = api->read_property(pluginHandle,
                                          *propertyIndex,
                                          stackBuf,
                                          sizeof(stackBuf));
    if (needed < 0) {
        return std::nullopt;
    }

    if (static_cast<std::size_t>(needed) < sizeof(stackBuf)) {
        return std::string(stackBuf, static_cast<std::size_t>(needed));
    }

    std::string big;
    big.resize(static_cast<std::size_t>(needed) + 1);
    const int rc = api->read_property(pluginHandle,
                                      *propertyIndex,
                                      big.data(),
                                      static_cast<std::uint32_t>(big.size()));
    if (rc < 0) {
        return std::nullopt;
    }

    big.resize(static_cast<std::size_t>(std::min(rc, needed)));
    return big;
}

void PluginController::reportErrorFromPlugin(const char* code, const char* /*label*/) {
    if (!code || !*code) {
        recordIntermittentError("plugin.error");
        return;
    }

    recordIntermittentError(code);
}

void PluginController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    std::vector<libera_point_t> pluginPoints;
    pluginPoints.reserve(FALLBACK_MAX_BATCH_POINTS);

    constexpr auto reconnectRetryDelay = 1s;

    while (running.load()) {
        if (!connected.load(std::memory_order_relaxed)) {
            setConnectionState(false);

            if (pluginHandle && api->destroy_controller) {
                api->destroy_controller(pluginHandle);
                pluginHandle = nullptr;
            }

            pluginHandle = api->connect_controller(
                backendHandle,
                &controllerInfo,
                static_cast<libera_host_ctx_t>(this));
            if (!pluginHandle) {
                std::this_thread::sleep_for(reconnectRetryDelay);
                continue;
            }

            if (api->set_point_rate) {
                api->set_point_rate(pluginHandle, getPointRate());
            }

            lastSentArmed = !isArmed();
            resetStartupBlank();
            connected.store(true, std::memory_order_relaxed);
        }

        setConnectionState(true);

        const bool armedNow = isArmed();
        if (armedNow != lastSentArmed && api->set_armed) {
            api->set_armed(pluginHandle, armedNow);
            lastSentArmed = armedNow;
        } else if (!api->set_armed) {
            lastSentArmed = armedNow;
        }

        const auto rate = getPointRate();

        libera_buffer_state_t bufferState{-1, -1};
        bool haveBuffer = false;
        if (api->get_buffer_state &&
            api->get_buffer_state(pluginHandle, &bufferState) == 0 &&
            bufferState.total_buffer_points > 0 &&
            bufferState.points_in_buffer >= 0) {
            setEstimatedBufferCapacity(bufferState.total_buffer_points);
            updateEstimatedBufferSnapshotNow(bufferState.points_in_buffer, rate);
            haveBuffer = true;
        }

        std::size_t minPoints = 0;
        std::size_t maxPoints = 0;

        if (haveBuffer) {
            const int capacity = bufferState.total_buffer_points;
            const int fullness = bufferState.points_in_buffer;
            const int freeSpace = std::max(0, capacity - fullness);
            const int target = core::BufferEstimator::targetBufferPoints(
                rate, capacity, targetLatency(),
                /* minimumBufferFloor   */ 0,
                /* safetyHeadroomPoints */ 0);
            const int deficit = std::max(0, target - fullness);

            if (deficit <= 0 || freeSpace <= 0) {
                std::this_thread::sleep_for(2ms);
                continue;
            }

            minPoints = static_cast<std::size_t>(deficit);
            maxPoints = static_cast<std::size_t>(freeSpace);
        } else {
            const int fixed = fallbackBatchPointCount(rate);
            minPoints = static_cast<std::size_t>(std::max(fixed / 2, 1));
            maxPoints = static_cast<std::size_t>(fixed);
        }

        auto renderTimeLead = std::chrono::duration_cast<std::chrono::milliseconds>(targetLatency());
        if (haveBuffer && rate > 0 && bufferState.points_in_buffer > 0) {
            const double ms =
                (static_cast<double>(bufferState.points_in_buffer) * 1000.0) /
                static_cast<double>(rate);
            renderTimeLead = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double, std::milli>(ms));
        }

        core::PointFillRequest request;
        request.minimumPointsRequired = minPoints;
        request.maximumPointsRequired = maxPoints;
        request.estimatedFirstPointRenderTime =
            std::chrono::steady_clock::now() + renderTimeLead;

        if (!requestPoints(request) || pointsToSend.empty()) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        convertPoints(pointsToSend, pluginPoints);

        const auto status = api->send_points(pluginHandle,
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
    }

    if (pluginHandle && api->set_armed) {
        api->set_armed(pluginHandle, false);
    }
}

} // namespace libera::plugin
