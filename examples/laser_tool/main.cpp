// Laser Tool — laser controller example
//
// Cross-platform (macOS / Windows / Linux) using:
//   GLFW + OpenGL3 for windowing
//   Dear ImGui for the UI
//   libera-core for controller discovery and output

#include "libera.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ILDAParser.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

using namespace libera;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static constexpr float PI = 3.14159265358979323846f;
static constexpr float TAU = 2.0f * PI;

static constexpr int NUM_PATTERNS = 4;
static const char* patternNames[NUM_PATTERNS] = {
    "White Square", "White Circle", "RGBW Square", "Hot Corners"
};

enum class OutputMode { Shape, Point, Pattern };

static constexpr int NUM_POINT_PATTERNS = 5;
static const char* pointPatternNames[NUM_POINT_PATTERNS] = {
    "Single", "2 Vert", "2 Horiz", "4 Grid", "8 Row"
};

struct ILDAPattern {
    std::string name;
    std::string path;
    std::vector<ILDAPoint> points;
};

struct ControllerEntry {
    std::string id;
    std::string label;
    std::string type;
    std::uint32_t maxPointRate = 0;
    bool enabled = false;
    bool connecting = false;
    std::shared_ptr<core::LaserController> controller;
    std::future<std::shared_ptr<core::LaserController>> connectFuture;
};

struct DiscoveredInfo {
    std::string id;
    std::string label;
    std::string type;
    std::uint32_t maxPointRate = 0;
};

struct AppState {
    bool armed = false;
    float brightness = 10.0f;
    float red = 100.0f, green = 100.0f, blue = 100.0f;
    bool redEnabled = true, greenEnabled = true, blueEnabled = true;
    int outputMode = 0; // 0=Shape, 1=Point, 2=Pattern
    int patternIndex = 0;
    int pointPatternIndex = 0;
    float dutyCycle = 25.0f;
    int pointRateIndex = 2;
    int customPointRate = 30000;
    float outputSize = 50.0f;
    float outputX = 0.0f;
    float outputY = 0.0f;

    std::vector<ILDAPattern> ildaPatterns;
    int selectedIldaPattern = 0;

    bool flipX = false;
    bool flipY = false;
    float scannerSync = 2.0f;

    System liberaSystem;
    std::vector<ControllerEntry> controllers;
    std::thread discoveryThread;
    std::atomic<bool> discoveryRunning{false};
    std::mutex discoveredMutex;
    std::vector<DiscoveredInfo> latestDiscovered;
    std::atomic<bool> discoveryResultReady{false};
    std::atomic<bool> discoveryRequested{false};

    struct PointRatePreset {
        const char* label;
        const char* shortLabel;
        int value;
    };
    static constexpr PointRatePreset pointRatePresets[] = {
        {"10,000 pps", "10k",  10000},
        {"20,000 pps", "20k",  20000},
        {"30,000 pps", "30k",  30000},
        {"40,000 pps", "40k",  40000},
        {"50,000 pps", "50k",  50000},
    };
    static constexpr int numPresets = sizeof(pointRatePresets) / sizeof(pointRatePresets[0]);
    static constexpr int customIndex = numPresets;

    int effectivePointRate() const {
        if (pointRateIndex >= 0 && pointRateIndex < numPresets)
            return pointRatePresets[pointRateIndex].value;
        return customPointRate;
    }

    void effectiveRGB(float& r, float& g, float& b) const {
        float br = brightness / 100.0f;
        r = redEnabled   ? br * (red   / 100.0f) : 0.0f;
        g = greenEnabled ? br * (green / 100.0f) : 0.0f;
        b = blueEnabled  ? br * (blue  / 100.0f) : 0.0f;
    }

    void resetOutput() {
        outputSize = 50.0f;
        outputX = 0.0f;
        outputY = 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Apply flip transforms to a point
// ---------------------------------------------------------------------------

static void applyTransform(float& x, float& y, const AppState& state) {
    if (state.flipX) x = -x;
    if (state.flipY) y = -y;
}

// ---------------------------------------------------------------------------
// Frame generation
// ---------------------------------------------------------------------------

static core::Frame makeSquareFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f;
    constexpr int pointsPerSide = 100;
    frame.points.reserve(pointsPerSide * 4 + 1);
    const float corners[][2] = {
        {cx - s, cy - s}, {cx + s, cy - s},
        {cx + s, cy + s}, {cx - s, cy + s},
    };
    for (int side = 0; side < 4; ++side) {
        int next = (side + 1) % 4;
        for (int i = 0; i < pointsPerSide; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(pointsPerSide);
            float x = corners[side][0] + t * (corners[next][0] - corners[side][0]);
            float y = corners[side][1] + t * (corners[next][1] - corners[side][1]);
            frame.points.push_back({x, y, 1, 1, 1});
        }
    }
    frame.points.push_back(frame.points.front());
    return frame;
}

static core::Frame makeCircleFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float radius = size * 0.8f;
    constexpr int pointCount = 400;
    frame.points.reserve(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(pointCount);
        float angle = t * TAU;
        frame.points.push_back({
            cx + radius * std::cos(angle),
            cy + radius * std::sin(angle),
            1, 1, 1
        });
    }
    return frame;
}

static core::Frame makeRGBWSquareFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f;
    constexpr int pointsPerSide = 100;
    frame.points.reserve(pointsPerSide * 4);

    const float corners[][2] = {
        {cx - s, cy - s}, {cx + s, cy - s},
        {cx + s, cy + s}, {cx - s, cy + s},
    };
    struct SideCol { float r, g, b; };
    const SideCol sideCols[] = {
        {0, 1, 0},
        {1, 0, 0},
        {1, 1, 1},
        {0, 0, 1},
    };

    for (int side = 0; side < 4; ++side) {
        int next = (side + 1) % 4;
        for (int i = 0; i < pointsPerSide; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(pointsPerSide);
            float x = corners[side][0] + t * (corners[next][0] - corners[side][0]);
            float y = corners[side][1] + t * (corners[next][1] - corners[side][1]);
            frame.points.push_back({x, y, sideCols[side].r, sideCols[side].g, sideCols[side].b});
        }
    }
    frame.points.push_back(frame.points.front());
    return frame;
}

static core::Frame makeHotCornersFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f;
    constexpr int cornerDwell = 30;
    constexpr int edgePoints = 20;
    constexpr int totalPerSide = cornerDwell + edgePoints;
    frame.points.reserve(totalPerSide * 4);

    const float corners[][2] = {
        {cx - s, cy - s}, {cx + s, cy - s},
        {cx + s, cy + s}, {cx - s, cy + s},
    };

    for (int side = 0; side < 4; ++side) {
        int next = (side + 1) % 4;
        for (int i = 0; i < cornerDwell; ++i) {
            frame.points.push_back({corners[side][0], corners[side][1], 1, 1, 1});
        }
        for (int i = 0; i < edgePoints; ++i) {
            float t = static_cast<float>(i + 1) / static_cast<float>(edgePoints);
            float x = corners[side][0] + t * (corners[next][0] - corners[side][0]);
            float y = corners[side][1] + t * (corners[next][1] - corners[side][1]);
            frame.points.push_back({x, y, 1, 1, 1});
        }
    }
    return frame;
}

static std::vector<std::pair<float,float>> getPointPositions(int patternIndex, float cx, float cy, float spacing) {
    std::vector<std::pair<float,float>> pts;
    float s = spacing * 0.3f;
    switch (patternIndex) {
        case 0:
            pts.push_back({cx, cy});
            break;
        case 1:
            pts.push_back({cx, cy - s});
            pts.push_back({cx, cy + s});
            break;
        case 2:
            pts.push_back({cx - s, cy});
            pts.push_back({cx + s, cy});
            break;
        case 3:
            pts.push_back({cx - s, cy - s});
            pts.push_back({cx + s, cy - s});
            pts.push_back({cx + s, cy + s});
            pts.push_back({cx - s, cy + s});
            break;
        case 4:
        {
            float halfW = spacing * 0.8f;
            for (int i = 0; i < 8; ++i) {
                float t = (static_cast<float>(i) / 7.0f - 0.5f) * 2.0f * halfW;
                pts.push_back({cx + t, cy});
            }
            for (int i = 6; i >= 1; --i) {
                float t = (static_cast<float>(i) / 7.0f - 0.5f) * 2.0f * halfW;
                pts.push_back({cx + t, cy});
            }
            break;
        }
        default:
            pts.push_back({cx, cy});
            break;
    }
    return pts;
}

static core::Frame makePointFrame(int patternIndex,
                                   float cx, float cy, float size, float dutyCycle) {
    core::Frame frame;
    auto positions = getPointPositions(patternIndex, cx, cy, size);
    int numDots = static_cast<int>(positions.size());
    int dwellPerDot = std::max(4, 100 / numDots);
    int onPoints = std::max(1, static_cast<int>(std::round(dutyCycle / 100.0f * dwellPerDot)));
    int transitPoints = (numDots > 1) ? 20 : 0;

    frame.points.reserve((dwellPerDot + transitPoints) * numDots);
    for (int d = 0; d < numDots; ++d) {
        auto [px, py] = positions[d];

        for (int i = 0; i < dwellPerDot; ++i) {
            if (i < onPoints) {
                frame.points.push_back({px, py, 1, 1, 1});
            } else {
                frame.points.push_back({px, py, 0, 0, 0});
            }
        }

        if (numDots > 1) {
            auto [nx, ny] = positions[(d + 1) % numDots];
            for (int i = 0; i < transitPoints; ++i) {
                float t = static_cast<float>(i + 1) / static_cast<float>(transitPoints + 1);
                float tx = px + t * (nx - px);
                float ty = py + t * (ny - py);
                frame.points.push_back({tx, ty, 0, 0, 0});
            }
        }
    }
    return frame;
}

static core::Frame makeShapeFrame(int patternIndex, float size, float cx, float cy) {
    switch (patternIndex) {
        case 0: return makeSquareFrame(size, cx, cy);
        case 1: return makeCircleFrame(size, cx, cy);
        case 2: return makeRGBWSquareFrame(size, cx, cy);
        case 3: return makeHotCornersFrame(size, cx, cy);
    }
    return makeSquareFrame(size, cx, cy);
}

// ---------------------------------------------------------------------------
// ILDA pattern loading and frame conversion
// ---------------------------------------------------------------------------

static std::string getPatternsDir(const char* argv0) {
    std::string exePath(argv0);
    auto lastSlash = exePath.find_last_of("/\\");
    std::string dir = (lastSlash != std::string::npos) ? exePath.substr(0, lastSlash) : ".";
    return dir + "/patterns";
}

static void loadILDAPatterns(AppState& state, const std::string& dir) {
    state.ildaPatterns.clear();

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((dir + "\\*.ild").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        std::string filename = fd.cFileName;
#else
    auto* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.size() < 4) continue;
        std::string ext = filename.substr(filename.size() - 4);
        for (auto& c : ext) c = static_cast<char>(::tolower(c));
        if (ext != ".ild") continue;
#endif
        std::string fullPath = dir + "/" + filename;
        auto frames = ILDAParser::load(fullPath);
        if (!frames.empty() && !frames[0].empty()) {
            std::string name = filename;
            auto dot = name.find_last_of('.');
            if (dot != std::string::npos) name = name.substr(0, dot);

            state.ildaPatterns.push_back({name, fullPath, frames[0]});
        }
#ifdef _WIN32
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#else
    }
    closedir(d);
#endif

    std::sort(state.ildaPatterns.begin(), state.ildaPatterns.end(),
              [](const ILDAPattern& a, const ILDAPattern& b) { return a.name < b.name; });
}

static core::Frame makeILDAFrame(const std::vector<ILDAPoint>& ildaPoints,
                                  float size, float cx, float cy) {
    core::Frame frame;
    frame.points.reserve(ildaPoints.size());

    for (auto& ip : ildaPoints) {
        float nx = static_cast<float>(ip.x) / 32768.0f * size + cx;
        float ny = static_cast<float>(ip.y) / 32768.0f * size + cy;

        if (ip.blank) {
            frame.points.push_back({nx, ny, 0, 0, 0});
        } else {
            float ir = static_cast<float>((ip.color >> 16) & 0xFF) / 255.0f;
            float ig = static_cast<float>((ip.color >> 8) & 0xFF) / 255.0f;
            float ib = static_cast<float>(ip.color & 0xFF) / 255.0f;
            frame.points.push_back({nx, ny, ir, ig, ib});
        }
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Draw a pattern into a drawlist at a given rect
// ---------------------------------------------------------------------------

static void drawPatternInRect(ImDrawList* drawList, int patternIndex,
                               float size, float cx, float cy,
                               ImVec2 rectPos, ImVec2 rectSize,
                               float brightnessScale = 1.0f,
                               bool flipYForPreview = false) {
    core::Frame frame = makeShapeFrame(patternIndex, size, cx, cy);

    auto mapX = [&](float x) { return rectPos.x + (x + 1.0f) * 0.5f * rectSize.x; };
    auto mapY = [&](float y) {
        if (flipYForPreview) y = -y;
        return rectPos.y + (y + 1.0f) * 0.5f * rectSize.y;
    };

    for (std::size_t i = 1; i < frame.points.size(); ++i) {
        auto& pt = frame.points[i];
        uint8_t cr = static_cast<uint8_t>(std::min(255.0f, pt.r * 255.0f * brightnessScale));
        uint8_t cg = static_cast<uint8_t>(std::min(255.0f, pt.g * 255.0f * brightnessScale));
        uint8_t cb = static_cast<uint8_t>(std::min(255.0f, pt.b * 255.0f * brightnessScale));
        ImU32 col = IM_COL32(cr, cg, cb, 255);

        drawList->AddLine(
            ImVec2(mapX(frame.points[i - 1].x), mapY(frame.points[i - 1].y)),
            ImVec2(mapX(frame.points[i].x),     mapY(frame.points[i].y)),
            col, 1.5f);
    }
}

static void drawILDAInRect(ImDrawList* drawList, const std::vector<ILDAPoint>& points,
                            ImVec2 rectPos, ImVec2 rectSize, bool flipY = false,
                            float brightnessScale = 1.0f,
                            float scale = 1.0f, float cx = 0.0f, float cy = 0.0f) {
    if (points.empty()) return;

    auto mapX = [&](float x) { return rectPos.x + (x + 1.0f) * 0.5f * rectSize.x; };
    auto mapY = [&](float y) {
        if (flipY) y = -y;
        return rectPos.y + (y + 1.0f) * 0.5f * rectSize.y;
    };

    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].blank || points[i - 1].blank) continue;

        float x0 = static_cast<float>(points[i - 1].x) / 32768.0f * scale + cx;
        float y0 = static_cast<float>(points[i - 1].y) / 32768.0f * scale + cy;
        float x1 = static_cast<float>(points[i].x) / 32768.0f * scale + cx;
        float y1 = static_cast<float>(points[i].y) / 32768.0f * scale + cy;

        uint32_t c = points[i].color;
        uint8_t r = static_cast<uint8_t>(std::min(255.0f, ((c >> 16) & 0xFF) * brightnessScale));
        uint8_t g = static_cast<uint8_t>(std::min(255.0f, ((c >> 8) & 0xFF) * brightnessScale));
        uint8_t b = static_cast<uint8_t>(std::min(255.0f, (c & 0xFF) * brightnessScale));

        drawList->AddLine(
            ImVec2(mapX(x0), mapY(y0)),
            ImVec2(mapX(x1), mapY(y1)),
            IM_COL32(r, g, b, 255), 1.5f);
    }
}

// ---------------------------------------------------------------------------
// Preview drawing with interactive output rect
// ---------------------------------------------------------------------------

static void drawPreview(AppState& state, ImVec2 pos, ImVec2 size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255));
    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(80, 80, 80, 255));

    float previewSize = state.outputSize / 100.0f;
    float previewCx = state.outputX / 100.0f;
    float previewCy = state.outputY / 100.0f;

    float previewBrightness = state.armed ? 1.0f : 0.47f;

    if (state.outputMode == 0) {
        drawPatternInRect(drawList, state.patternIndex, previewSize, previewCx, previewCy,
                          pos, size, previewBrightness, true);
    } else if (state.outputMode == 1) {
        auto bMapX = [&](float x) { return pos.x + (x + 1.0f) * 0.5f * size.x; };
        auto bMapY = [&](float y) { return pos.y + ((-y) + 1.0f) * 0.5f * size.y; };
        uint8_t bv = static_cast<uint8_t>(previewBrightness * 255);
        auto positions = getPointPositions(state.pointPatternIndex, previewCx, previewCy, previewSize);
        for (auto& [px, py] : positions) {
            float dotX = bMapX(px);
            float dotY = bMapY(py);
            drawList->AddCircleFilled(ImVec2(dotX, dotY), 4.0f, IM_COL32(bv, bv, bv, 255));
            drawList->AddLine(ImVec2(dotX - 8, dotY), ImVec2(dotX + 8, dotY), IM_COL32(80, 80, 80, 140));
            drawList->AddLine(ImVec2(dotX, dotY - 8), ImVec2(dotX, dotY + 8), IM_COL32(80, 80, 80, 140));
        }
    } else if (state.outputMode == 2) {
        int idx = state.selectedIldaPattern;
        if (idx >= 0 && idx < static_cast<int>(state.ildaPatterns.size())) {
            float brightness = state.armed ? 1.0f : 0.47f;
            drawILDAInRect(drawList, state.ildaPatterns[idx].points, pos, size, true,
                           brightness, previewSize * 0.8f, previewCx, previewCy);
        }
    }

    if (!state.armed) {
        const char* text = "DISARMED";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(
            ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + size.y - textSize.y - 8.0f),
            IM_COL32(100, 100, 100, 255), text);
    }

    // Output bounding box
    float s = previewSize * 0.8f;
    auto mapX = [&](float x) { return pos.x + (x + 1.0f) * 0.5f * size.x; };
    auto mapY = [&](float y) { return pos.y + ((-y) + 1.0f) * 0.5f * size.y; };

    float rectLeft   = mapX(previewCx - s);
    float rectTop    = mapY(previewCy + s);
    float rectRight  = mapX(previewCx + s);
    float rectBottom = mapY(previewCy - s);
    if (rectTop > rectBottom) std::swap(rectTop, rectBottom);
    if (rectLeft > rectRight) std::swap(rectLeft, rectRight);

    drawList->AddRect(ImVec2(rectLeft, rectTop), ImVec2(rectRight, rectBottom),
                      IM_COL32(80, 80, 80, 180), 0.0f, 0, 1.0f);
}

// ---------------------------------------------------------------------------
// Background discovery thread
// ---------------------------------------------------------------------------

static void discoveryThreadFunc(AppState& state) {
    while (state.discoveryRunning.load()) {
        auto discovered = state.liberaSystem.discoverControllers();
        {
            std::lock_guard<std::mutex> lock(state.discoveredMutex);
            state.latestDiscovered.clear();
            for (auto& d : discovered)
                state.latestDiscovered.push_back({d->idValue(), d->labelValue(), d->type(), d->maxPointRate()});
            state.discoveryResultReady.store(true);
        }
        for (int i = 0; i < 20 && state.discoveryRunning.load(); ++i) {
            if (state.discoveryRequested.load()) { state.discoveryRequested.store(false); break; }
            std::this_thread::sleep_for(100ms);
        }
    }
}

static void startAsyncConnect(AppState& state, ControllerEntry& entry);

static void applyDiscoveryResults(AppState& state) {
    if (!state.discoveryResultReady.load()) return;
    std::vector<DiscoveredInfo> discovered;
    {
        std::lock_guard<std::mutex> lock(state.discoveredMutex);
        discovered = std::move(state.latestDiscovered);
        state.discoveryResultReady.store(false);
    }
    for (auto& entry : state.controllers) {
        bool stillPresent = false;
        for (auto& d : discovered) { if (d.id == entry.id) { stillPresent = true; break; } }
        if (!stillPresent && entry.controller) { entry.controller.reset(); entry.enabled = false; }
    }
    for (auto& d : discovered) {
        bool exists = false;
        for (auto& entry : state.controllers) { if (entry.id == d.id) { exists = true; break; } }
        if (!exists) {
            state.controllers.push_back({d.id, d.label, d.type, d.maxPointRate, false, false, nullptr, {}});
        }
    }
}

static void pollAsyncConnections(AppState& state) {
    for (auto& entry : state.controllers) {
        if (entry.connecting && entry.connectFuture.valid()) {
            if (entry.connectFuture.wait_for(0ms) == std::future_status::ready) {
                entry.controller = entry.connectFuture.get();
                entry.connecting = false;
                if (entry.controller)
                    entry.controller->setPointRate(static_cast<std::uint32_t>(state.effectivePointRate()));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Frame building
// ---------------------------------------------------------------------------

static core::Frame buildCurrentFrame(const AppState& state) {
    float sz = state.outputSize / 100.0f;
    float cx = state.outputX / 100.0f;
    float cy = state.outputY / 100.0f;

    core::Frame frame;
    if (state.outputMode == 1) {
        frame = makePointFrame(state.pointPatternIndex, cx, cy, sz, state.dutyCycle);
    } else if (state.outputMode == 2) {
        int idx = state.selectedIldaPattern;
        if (idx >= 0 && idx < static_cast<int>(state.ildaPatterns.size()))
            frame = makeILDAFrame(state.ildaPatterns[idx].points, sz * 0.8f, cx, cy);
        else
            frame = makeShapeFrame(0, sz, cx, cy);
    } else {
        frame = makeShapeFrame(state.patternIndex, sz, cx, cy);
    }

    // Apply brightness and RGB as post-processing
    float r, g, b;
    state.effectiveRGB(r, g, b);
    for (auto& pt : frame.points) {
        pt.r *= r;
        pt.g *= g;
        pt.b *= b;
    }

    return frame;
}

// ---------------------------------------------------------------------------
// Frame sending
// ---------------------------------------------------------------------------

static void sendFramesToControllers(AppState& state) {
    core::Frame frame = buildCurrentFrame(state);

    for (auto& pt : frame.points)
        applyTransform(pt.x, pt.y, state);

    for (auto& entry : state.controllers) {
        if (!entry.enabled || !entry.controller) continue;
        entry.controller->setArmed(state.armed);
        entry.controller->setPointRate(static_cast<std::uint32_t>(state.effectivePointRate()));
        entry.controller->setScannerSync(static_cast<double>(state.scannerSync));
        if (entry.controller->isReadyForNewFrame()) {
            core::Frame copy = frame;
            entry.controller->sendFrame(std::move(copy));
        }
    }
}

static void startAsyncConnect(AppState& state, ControllerEntry& entry) {
    entry.connecting = true;
    std::string entryId = entry.id;
    System* sys = &state.liberaSystem;
    entry.connectFuture = std::async(std::launch::async, [sys, entryId]() -> std::shared_ptr<core::LaserController> {
        auto discovered = sys->discoverControllers();
        for (auto& d : discovered) {
            if (d->idValue() == entryId) return sys->connectController(*d);
        }
        return nullptr;
    });
}

static void disconnectController(ControllerEntry& entry) {
    if (entry.controller) { entry.controller->setArmed(false); entry.controller.reset(); }
    entry.connecting = false;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int /*argc*/, char* argv[]) {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    const char* glslVersion = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glslVersion = "#version 130";
#endif

    AppState state;

    GLFWwindow* window = glfwCreateWindow(900, 700, "Laser Tool", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Scale default font for HiDPI
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float dpiScale = xscale;
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 13.0f * dpiScale;
    io.Fonts->AddFontDefault(&fontConfig);
    io.FontGlobalScale = 1.0f / dpiScale;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    loadILDAPatterns(state, getPatternsDir(argv[0]));

    state.discoveryRunning.store(true);
    state.discoveryThread = std::thread(discoveryThreadFunc, std::ref(state));

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        applyDiscoveryResults(state);
        pollAsyncConnections(state);
        sendFramesToControllers(state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Laser Tool", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        float windowWidth = ImGui::GetContentRegionAvail().x;
        float windowHeight = ImGui::GetContentRegionAvail().y;
        float controlPanelWidth = 340.0f;
        float bottomPanelHeight = 180.0f;

        float previewSize = std::min(windowWidth - controlPanelWidth - 20.0f,
                                      windowHeight - bottomPanelHeight - 20.0f);
        if (previewSize < 100.0f) previewSize = 100.0f;

        ImVec2 previewPos = ImGui::GetCursorScreenPos();
        drawPreview(state, previewPos, ImVec2(previewSize, previewSize));

        // ---- Right: Controls ----
        float rightX = previewPos.x + previewSize + 16.0f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, previewPos.y));
        ImGui::BeginGroup();

        float sliderWidth = controlPanelWidth - 16.0f;
        ImGui::PushItemWidth(sliderWidth);

        // ARM button
        {
            const char* armLabel = state.armed ? "!! ARMED !!" : "ARM";
            if (ImGui::Button(armLabel, ImVec2(controlPanelWidth - 16.0f, 44.0f)))
                state.armed = !state.armed;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Brightness slider
        ImGui::SliderFloat("Brightness", &state.brightness, 0.0f, 100.0f, "%.0f%%");

        // RGB sliders with enable checkboxes
        {
            float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
            float sliderW = controlPanelWidth - 16.0f - checkboxWidth;

            ImGui::Checkbox("##redEn", &state.redEnabled);
            ImGui::SameLine();
            ImGui::PushItemWidth(sliderW);
            ImGui::SliderFloat("Red", &state.red, 0.0f, 100.0f, "%.0f%%");
            ImGui::PopItemWidth();

            ImGui::Checkbox("##greenEn", &state.greenEnabled);
            ImGui::SameLine();
            ImGui::PushItemWidth(sliderW);
            ImGui::SliderFloat("Green", &state.green, 0.0f, 100.0f, "%.0f%%");
            ImGui::PopItemWidth();

            ImGui::Checkbox("##blueEn", &state.blueEnabled);
            ImGui::SameLine();
            ImGui::PushItemWidth(sliderW);
            ImGui::SliderFloat("Blue", &state.blue, 0.0f, 100.0f, "%.0f%%");
            ImGui::PopItemWidth();
        }

        ImGui::PushItemWidth(sliderWidth);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Mode selector
        ImGui::RadioButton("Shape", &state.outputMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Point", &state.outputMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Pattern", &state.outputMode, 2);

        ImGui::Spacing();

        // Mode-specific content
        if (state.outputMode == 0) {
            ImGui::Text("Test Pattern");
            {
                float thumbSize = 80.0f;
                float spacing = 8.0f;
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                float rowStartX = ImGui::GetCursorScreenPos().x;

                for (int i = 0; i < NUM_PATTERNS; ++i) {
                    ImVec2 thumbPos = ImGui::GetCursorScreenPos();
                    bool selected = (state.patternIndex == i);
                    ImU32 borderCol = selected ? IM_COL32(66, 150, 250, 255) : IM_COL32(60, 60, 60, 255);
                    float borderThickness = selected ? 2.0f : 1.0f;

                    drawList->AddRectFilled(thumbPos, ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                                            IM_COL32(10, 10, 10, 255));
                    drawList->AddRect(thumbPos, ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                                      borderCol, 2.0f, 0, borderThickness);

                    float thumbBright = selected ? 1.0f : 0.6f;
                    drawPatternInRect(drawList, i, 1.0f, 0.0f, 0.0f,
                                      thumbPos, ImVec2(thumbSize, thumbSize), thumbBright);

                    ImVec2 labelSize = ImGui::CalcTextSize(patternNames[i]);
                    drawList->AddText(
                        ImVec2(thumbPos.x + (thumbSize - labelSize.x) * 0.5f, thumbPos.y + thumbSize + 2.0f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 255),
                        patternNames[i]);

                    ImGui::SetCursorScreenPos(thumbPos);
                    char btnId[32];
                    std::snprintf(btnId, sizeof(btnId), "##thumb%d", i);
                    if (ImGui::InvisibleButton(btnId, ImVec2(thumbSize, thumbSize)))
                        state.patternIndex = i;

                    bool endOfRow = ((i + 1) % 4 == 0);
                    bool lastItem = (i == NUM_PATTERNS - 1);
                    if (!endOfRow && !lastItem) {
                        ImGui::SameLine(0, spacing);
                    } else {
                        ImGui::SetCursorScreenPos(ImVec2(
                            rowStartX,
                            thumbPos.y + thumbSize + labelSize.y + 8.0f));
                    }
                }
            }
        } else if (state.outputMode == 1) {
            {
                float thumbSize = 60.0f;
                float spacing = 6.0f;
                ImDrawList* ptDrawList = ImGui::GetWindowDrawList();
                float ptRowStartX = ImGui::GetCursorScreenPos().x;

                for (int i = 0; i < NUM_POINT_PATTERNS; ++i) {
                    ImVec2 thumbPos = ImGui::GetCursorScreenPos();
                    bool selected = (state.pointPatternIndex == i);
                    ImU32 borderCol = selected ? IM_COL32(66, 150, 250, 255) : IM_COL32(60, 60, 60, 255);
                    float borderThickness = selected ? 2.0f : 1.0f;

                    ptDrawList->AddRectFilled(thumbPos,
                        ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                        IM_COL32(10, 10, 10, 255));
                    ptDrawList->AddRect(thumbPos,
                        ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                        borderCol, 2.0f, 0, borderThickness);

                    uint8_t tv = selected ? 255 : 150;
                    auto positions = getPointPositions(i, 0.0f, 0.0f, 1.0f);
                    for (auto& [px, py] : positions) {
                        float dx = thumbPos.x + (px + 1.0f) * 0.5f * thumbSize;
                        float dy = thumbPos.y + ((-py) + 1.0f) * 0.5f * thumbSize;
                        ptDrawList->AddCircleFilled(ImVec2(dx, dy), 3.0f, IM_COL32(tv, tv, tv, 255));
                    }

                    ImVec2 labelSize = ImGui::CalcTextSize(pointPatternNames[i]);
                    ptDrawList->AddText(
                        ImVec2(thumbPos.x + (thumbSize - labelSize.x) * 0.5f,
                               thumbPos.y + thumbSize + 2.0f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 255),
                        pointPatternNames[i]);

                    ImGui::SetCursorScreenPos(thumbPos);
                    char btnId[32];
                    std::snprintf(btnId, sizeof(btnId), "##ptpat%d", i);
                    if (ImGui::InvisibleButton(btnId, ImVec2(thumbSize, thumbSize)))
                        state.pointPatternIndex = i;

                    bool endOfRow = ((i + 1) % 4 == 0);
                    bool lastItem = (i == NUM_POINT_PATTERNS - 1);
                    if (!endOfRow && !lastItem) {
                        ImGui::SameLine(0, spacing);
                    } else {
                        ImGui::SetCursorScreenPos(ImVec2(
                            ptRowStartX,
                            thumbPos.y + thumbSize + labelSize.y + 8.0f));
                    }
                }
            }

            ImGui::Text("Duty Cycle");
            ImGui::PushItemWidth(controlPanelWidth - 16.0f);
            ImGui::SliderFloat("##duty", &state.dutyCycle, 1.0f, 100.0f, "%.0f%%");
            ImGui::PopItemWidth();
        } else {
            if (state.ildaPatterns.empty()) {
                ImGui::TextDisabled("No .ild files found in patterns/ folder");
            } else {
                float thumbSize = 80.0f;
                float spacing = 8.0f;
                ImDrawList* ildaDrawList = ImGui::GetWindowDrawList();
                int cols = 4;
                float ildaRowStartX = ImGui::GetCursorScreenPos().x;

                for (int i = 0; i < static_cast<int>(state.ildaPatterns.size()); ++i) {
                    ImVec2 thumbPos = ImGui::GetCursorScreenPos();
                    bool selected = (state.selectedIldaPattern == i);
                    ImU32 borderCol = selected ? IM_COL32(66, 150, 250, 255) : IM_COL32(60, 60, 60, 255);
                    float borderThickness = selected ? 2.0f : 1.0f;

                    ildaDrawList->AddRectFilled(thumbPos,
                        ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                        IM_COL32(10, 10, 10, 255));
                    ildaDrawList->AddRect(thumbPos,
                        ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                        borderCol, 2.0f, 0, borderThickness);

                    drawILDAInRect(ildaDrawList, state.ildaPatterns[i].points,
                                   thumbPos, ImVec2(thumbSize, thumbSize), false,
                                   selected ? 1.0f : 0.6f);

                    const char* name = state.ildaPatterns[i].name.c_str();
                    ImVec2 labelSize = ImGui::CalcTextSize(name);
                    ildaDrawList->AddText(
                        ImVec2(thumbPos.x + std::max(0.0f, (thumbSize - labelSize.x) * 0.5f),
                               thumbPos.y + thumbSize + 2.0f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 255),
                        name);

                    ImGui::SetCursorScreenPos(thumbPos);
                    char btnId[32];
                    std::snprintf(btnId, sizeof(btnId), "##ilda%d", i);
                    if (ImGui::InvisibleButton(btnId, ImVec2(thumbSize, thumbSize)))
                        state.selectedIldaPattern = i;

                    int col = i % cols;
                    if (col < cols - 1 && i < static_cast<int>(state.ildaPatterns.size()) - 1) {
                        ImGui::SameLine(0, spacing);
                    } else {
                        ImGui::SetCursorScreenPos(ImVec2(
                            ildaRowStartX,
                            thumbPos.y + thumbSize + labelSize.y + 8.0f));
                    }
                }
            }
        }

        ImGui::Spacing();

        // Point rate
        ImGui::Text("Point Rate");
        {
            for (int i = 0; i < AppState::numPresets; ++i) {
                ImGui::RadioButton(AppState::pointRatePresets[i].shortLabel, &state.pointRateIndex, i);
                ImGui::SameLine();
            }
            ImGui::RadioButton("Custom", &state.pointRateIndex, AppState::customIndex);
            if (state.pointRateIndex == AppState::customIndex) {
                ImGui::PushItemWidth(controlPanelWidth - 16.0f);
                ImGui::InputInt("##custompps", &state.customPointRate, 1000, 5000);
                state.customPointRate = std::clamp(state.customPointRate, 1000, 100000);
                ImGui::PopItemWidth();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Output section
        {
            ImGui::Text("Output  %.0f%%", state.outputSize);
            ImGui::SameLine(controlPanelWidth - 70.0f);
            if (ImGui::SmallButton("Reset")) state.resetOutput();

            float totalWidth = controlPanelWidth - 16.0f;
            float dragW = (totalWidth - 60.0f) / 3.0f;
            ImGui::PushItemWidth(dragW);

            ImGui::Text("Size"); ImGui::SameLine();
            ImGui::DragFloat("##size", &state.outputSize, 0.5f, 0.0f, 100.0f, "%.0f%%");
            state.outputSize = std::clamp(state.outputSize, 0.0f, 100.0f);

            ImGui::SameLine(0, 8); ImGui::Text("X"); ImGui::SameLine();
            ImGui::DragFloat("##xoffset", &state.outputX, 0.5f, -100.0f, 100.0f, "%.0f%%");
            state.outputX = std::clamp(state.outputX, -100.0f, 100.0f);

            ImGui::SameLine(0, 8); ImGui::Text("Y"); ImGui::SameLine();
            ImGui::DragFloat("##yoffset", &state.outputY, 0.5f, -100.0f, 100.0f, "%.0f%%");
            state.outputY = std::clamp(state.outputY, -100.0f, 100.0f);

            ImGui::PopItemWidth();
        }

        ImGui::Spacing();

        // Flip
        ImGui::Checkbox("Flip X", &state.flipX);
        ImGui::SameLine();
        ImGui::Checkbox("Flip Y", &state.flipY);

        ImGui::Spacing();

        // Scanner sync
        ImGui::Text("Scanner Sync");
        ImGui::PushItemWidth(controlPanelWidth - 16.0f);
        ImGui::SliderFloat("##scansync", &state.scannerSync, 0.0f, 10.0f, "%.1f");
        ImGui::PopItemWidth();

        ImGui::PopItemWidth();
        ImGui::EndGroup();

        // ---- Bottom: Controller list ----
        ImGui::SetCursorScreenPos(ImVec2(previewPos.x, previewPos.y + previewSize + 12.0f));
        ImGui::BeginChild("Controllers", ImVec2(previewSize, 0), ImGuiChildFlags_Border);

        ImGui::Text("Discovered Controllers");
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) state.discoveryRequested.store(true);
        ImGui::Separator();

        if (state.controllers.empty()) {
            ImGui::TextDisabled("Searching for controllers...");
        } else {
            for (auto& entry : state.controllers) {
                ImGui::PushID(entry.id.c_str());

                // Status indicator
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float sz = ImGui::GetFrameHeight();
                    ImDrawList* ctrlDrawList = ImGui::GetWindowDrawList();
                    ImU32 statusCol;
                    core::ControllerStatus status = core::ControllerStatus::Good;
                    if (!entry.controller || !entry.enabled) {
                        statusCol = IM_COL32(77, 77, 77, 255);
                    } else {
                        status = entry.controller->getStatus();
                        switch (status) {
                            case core::ControllerStatus::Good:   statusCol = IM_COL32(0, 255, 0, 255); break;
                            case core::ControllerStatus::Issues: statusCol = IM_COL32(255, 128, 0, 255); break;
                            case core::ControllerStatus::Error:  statusCol = IM_COL32(255, 0, 0, 255); break;
                        }
                    }
                    ctrlDrawList->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), statusCol, 2.0f);

                    ImGui::InvisibleButton("##status", ImVec2(sz, sz));
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        if (!entry.controller || !entry.enabled) {
                            ImGui::Text("Not connected");
                        } else {
                            const char* statusText[] = {"Good", "Issues", "Error"};
                            ImGui::Text("Status: %s", statusText[static_cast<int>(status)]);
                            auto errors = entry.controller->getErrors();
                            if (!errors.empty()) {
                                ImGui::Separator();
                                for (auto& err : errors)
                                    ImGui::Text("%s: %llu", err.label.c_str(),
                                                static_cast<unsigned long long>(err.count));
                            }
                        }
                        ImGui::EndTooltip();
                    }
                    if (ImGui::IsItemClicked() && entry.controller)
                        entry.controller->clearErrors();

                    ImGui::SameLine();
                }

                bool wasEnabled = entry.enabled;
                ImGui::Checkbox("##enable", &entry.enabled);
                ImGui::SameLine();
                ImGui::Text("%s", entry.label.c_str());

                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Type: %s", entry.type.c_str());
                    if (entry.maxPointRate > 0) ImGui::Text("Max: %u pps", entry.maxPointRate);
                    ImGui::Text("ID: %s", entry.id.c_str());
                    ImGui::EndTooltip();
                }

                if (entry.enabled && !wasEnabled && !entry.connecting)
                    startAsyncConnect(state, entry);
                else if (!entry.enabled && wasEnabled)
                    disconnectController(entry);

                ImGui::PopID();
            }
        }

        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    state.discoveryRunning.store(false);
    if (state.discoveryThread.joinable()) state.discoveryThread.join();
    for (auto& entry : state.controllers) disconnectController(entry);
    state.liberaSystem.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
