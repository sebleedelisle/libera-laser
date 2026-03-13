#pragma once

namespace libera::core {

// A single point to be sent to a laser controller.
// - x, y : normalised coordinates (-1..1)
// - r, g, b : colour channels (0..1 suggested)
// - u1, u2 : user fields for extension (waveforms, safety masks, etc.)

struct LaserPoint {
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float u1 = 0.0f;
    float u2 = 0.0f;
};

} // namespace libera::core
