#include "libera/core/Dummy/DummyController.hpp"
#include "libera/log/Log.hpp"
#include <vector>
#include <chrono>

using namespace libera;
using namespace libera::core;

int main() {
    dummy::DummyController ctl;

    // Install a trivial callback that appends exactly the required number of points.
    ctl.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.minimumPointsRequired; ++i) {
                out.push_back(LaserPoint{/*x*/0, /*y*/0, /*r*/1, /*g*/1, /*b*/1, /*i*/1, /*flags*/0});
            }
        }
    );

    PointFillRequest req;
    req.minimumPointsRequired = 10;
    req.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();

    const bool ok = ctl.requestPoints(req);
    logInfo("Smoke test: requestPoints returned", ok ? "true" : "false");
    return ok ? 0 : 1;
}
