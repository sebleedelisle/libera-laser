#include "libera/plugin/PluginController.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace libera::plugin {

namespace {

constexpr int FRAME_POINT_COUNT = 512;
constexpr int MAX_PLUGIN_POINTS = 4096;

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
    }
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
    pluginHandle = funcs.connect(controllerId.c_str());
    if (!pluginHandle) {
        libera::log::logError("Plugin: failed to connect to ", controllerId);
        return false;
    }
    connected.store(true);
    setConnectionState(true);
    return true;
}

void PluginController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    std::vector<libera_point_t> pluginPoints;
    pluginPoints.reserve(MAX_PLUGIN_POINTS);

    while (running.load()) {
        if (!connected.load(std::memory_order_relaxed)) {
            setConnectionState(false);
            std::this_thread::sleep_for(100ms);
            continue;
        }
        setConnectionState(true);

        // Update armed state in the plugin.
        funcs.set_armed(pluginHandle, isArmed());

        // Pull points from the frame queue.
        core::PointFillRequest request;
        request.minimumPointsRequired = FRAME_POINT_COUNT;
        request.maximumPointsRequired = FRAME_POINT_COUNT;
        request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now() + 10ms;

        fillFromFrameQueue(request, pointsToSend);

        if (pointsToSend.empty()) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        convertPoints(pointsToSend, pluginPoints);

        const auto rate = getPointRate();
        const int rc = funcs.send_points(pluginHandle,
                                         pluginPoints.data(),
                                         static_cast<uint32_t>(pluginPoints.size()),
                                         rate);
        pointsToSend.clear();

        if (rc != 0) {
            libera::log::logError("Plugin: send_points failed (", rc, ")");
            recordIntermittentError("plugin.send_failed");
        }
    }

    // Disarm on shutdown.
    if (pluginHandle) {
        funcs.set_armed(pluginHandle, false);
    }
}

} // namespace libera::plugin
