// ============================================================================
// Laser Tool — libera-core example application
//
// A minimal but functional laser controller test utility demonstrating how to
// use the libera-core library for:
//   - Discovering laser controllers on the network / USB
//   - Connecting to controllers and sending frames
//   - Generating test patterns (shapes, points, ILDA files)
//   - Controlling brightness, colour, point rate, and output geometry
//
// Built with:
//   GLFW + OpenGL3  — cross-platform windowing
//   Dear ImGui      — immediate-mode GUI
//   libera-core     — laser controller discovery, connection, and output
//
// The UI is split into three areas:
//   Left:   A square preview canvas showing the current output
//   Right:  Control panel (arm, brightness, RGB, mode, point rate, etc.)
//   Bottom: List of discovered controllers with enable/disable checkboxes
// ============================================================================

// ---------------------------------------------------------------------------
// Includes
// ---------------------------------------------------------------------------

// libera-core: the main laser controller library. Provides System (discovery),
// LaserController (connection/output), Frame, and LaserPoint types.
#include "libera.h"

// Dear ImGui: immediate-mode UI library. We use the GLFW + OpenGL3 backends.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// ILDA file parser: loads .ild laser show files into point data.
#include "ILDAParser.h"

// Platform-specific OpenGL headers
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// GLFW: cross-platform window creation and input handling
#include <GLFW/glfw3.h>

// Standard library
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

// Platform-specific directory listing (for loading ILDA pattern files)
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

using namespace libera;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr float PI = 3.14159265358979323846f;
static constexpr float TAU = 2.0f * PI;

// Available shape test patterns
static constexpr int NUM_PATTERNS = 4;
static const char* patternNames[NUM_PATTERNS] = {
    "White Square", "White Circle", "RGBW Square", "Hot Corners"
};

// Output modes: Shape draws vector shapes, Point draws static dots,
// Pattern plays back loaded ILDA files.
enum class OutputMode { Shape, Point, Pattern };

// Available point test patterns (static dot arrangements)
static constexpr int NUM_POINT_PATTERNS = 5;
static const char* pointPatternNames[NUM_POINT_PATTERNS] = {
    "Single", "2 Vert", "2 Horiz", "4 Grid", "8 Row"
};

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

// An ILDA pattern loaded from a .ild file on disk.
// We store the first frame's worth of points for playback and thumbnail preview.
struct ILDAPattern {
    std::string name;              // Display name (filename without extension)
    std::string path;              // Full path to the .ild file
    std::vector<ILDAPoint> points; // Point data from the first frame
};

// Represents a single discovered laser controller.
// Tracks connection state and holds the controller instance once connected.
struct ControllerEntry {
    std::string id;                // Unique controller identifier
    std::string label;             // Human-readable name (e.g. "LaserCube 1")
    std::string type;              // Controller type (e.g. "LaserCubeNet", "Helios")
    std::uint32_t maxPointRate = 0;
    bool enabled = false;          // Whether the user has enabled this controller
    bool connecting = false;       // True while an async connection is in progress
    std::shared_ptr<core::LaserController> controller;  // The connected controller (null until connected)
    std::future<std::shared_ptr<core::LaserController>> connectFuture; // Async connection result
};

// Lightweight snapshot of a discovered controller, used to pass discovery
// results from the background thread to the main thread via a mutex.
struct DiscoveredInfo {
    std::string id;
    std::string label;
    std::string type;
    std::uint32_t maxPointRate = 0;
};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
// All UI state and controller state lives here. A single instance is created
// in main() and passed by reference throughout the application.

struct AppState {
    // -- Safety --
    bool armed = false;            // Laser output is only sent when armed

    // -- Colour controls --
    // Brightness is a master dimmer (0-100%). Each colour channel has its own
    // level (0-100%) and an enable toggle. The effective output colour for each
    // channel is: (brightness/100) * (channel/100), or 0 if disabled.
    float brightness = 10.0f;
    float red = 100.0f, green = 100.0f, blue = 100.0f;
    bool redEnabled = true, greenEnabled = true, blueEnabled = true;

    // -- Output mode --
    int outputMode = 0;            // 0=Shape, 1=Point, 2=Pattern (ILDA)
    int patternIndex = 0;          // Which shape pattern is selected
    int pointPatternIndex = 0;     // Which point pattern is selected
    float dutyCycle = 25.0f;       // Point mode: percentage of dwell time the laser is on

    // -- Point rate --
    // Controls how many points per second are sent to the controller.
    // Either a preset (10k-50k) or a custom value.
    int pointRateIndex = 2;        // Index into pointRatePresets, or customIndex
    int customPointRate = 30000;

    // -- Output geometry --
    // Size as a percentage (0-100%), and X/Y offset (-100% to +100%).
    // These control where and how large the pattern appears in the output field.
    float outputSize = 50.0f;
    float outputX = 0.0f;
    float outputY = 0.0f;

    // -- ILDA patterns --
    // Loaded from .ild files in the patterns/ directory next to the executable.
    std::vector<ILDAPattern> ildaPatterns;
    int selectedIldaPattern = 0;

    // -- Transform --
    bool flipX = false;            // Mirror output horizontally
    bool flipY = false;            // Mirror output vertically

    // -- Scanner sync --
    // Delay in 1/10,000s units between consecutive points. Helps with scanner
    // synchronisation on some hardware. Default 2.0 = 0.2ms.
    float scannerSync = 2.0f;

    // -- Controller discovery & connection --
    // Discovery runs on a background thread. Results are passed to the main
    // thread via latestDiscovered + discoveryResultReady (protected by mutex).
    System liberaSystem;                       // The libera System instance
    std::vector<ControllerEntry> controllers;  // All known controllers
    std::thread discoveryThread;
    std::atomic<bool> discoveryRunning{false};
    std::mutex discoveredMutex;
    std::vector<DiscoveredInfo> latestDiscovered;
    std::atomic<bool> discoveryResultReady{false};
    std::atomic<bool> discoveryRequested{false}; // Set true to trigger an immediate rescan

    // -- Point rate presets --
    struct PointRatePreset {
        const char* label;         // Full label (e.g. "30,000 pps")
        const char* shortLabel;    // Short label for UI buttons (e.g. "30k")
        int value;                 // Points per second
    };
    static constexpr PointRatePreset pointRatePresets[] = {
        {"10,000 pps", "10k",  10000},
        {"20,000 pps", "20k",  20000},
        {"30,000 pps", "30k",  30000},
        {"40,000 pps", "40k",  40000},
        {"50,000 pps", "50k",  50000},
    };
    static constexpr int numPresets = sizeof(pointRatePresets) / sizeof(pointRatePresets[0]);
    static constexpr int customIndex = numPresets; // Index value meaning "use customPointRate"

    // Returns the currently selected point rate in points per second.
    int effectivePointRate() const {
        if (pointRateIndex >= 0 && pointRateIndex < numPresets)
            return pointRatePresets[pointRateIndex].value;
        return customPointRate;
    }

    // Computes the effective RGB output values (0.0-1.0) by combining
    // the master brightness with per-channel levels and enable states.
    // This is applied as a post-processing step after frame generation.
    void effectiveRGB(float& r, float& g, float& b) const {
        float br = brightness / 100.0f;
        r = redEnabled   ? br * (red   / 100.0f) : 0.0f;
        g = greenEnabled ? br * (green / 100.0f) : 0.0f;
        b = blueEnabled  ? br * (blue  / 100.0f) : 0.0f;
    }

    // Resets output size and position to defaults.
    void resetOutput() {
        outputSize = 50.0f;
        outputX = 0.0f;
        outputY = 0.0f;
    }
};

// ============================================================================
// FRAME GENERATION
// ============================================================================
//
// All frame generators produce "raw" frames with design colours at full
// brightness (white = {1,1,1}, red = {1,0,0}, etc.). Brightness and user
// RGB adjustments are applied afterwards in buildCurrentFrame() as a
// post-processing multiply. This keeps the generators simple and means
// multi-colour patterns (like RGBW Square) work correctly with per-channel
// enable/disable.
//
// Coordinate system: x and y range from -1.0 to +1.0, where (0,0) is centre.
// Colours are 0.0-1.0 per channel. Blanked points use {0,0,0}.
// ============================================================================

// ---------------------------------------------------------------------------
// Spatial transforms
// ---------------------------------------------------------------------------

// Applies flip transforms to a single point's coordinates.
// Called on every point before sending to controllers.
static void applyTransform(float& x, float& y, const AppState& state) {
    if (state.flipX) x = -x;
    if (state.flipY) y = -y;
}

// ---------------------------------------------------------------------------
// Shape pattern generators
// ---------------------------------------------------------------------------

// Generates a white square outline. Points are evenly distributed along
// each side, creating a smooth vector shape suitable for galvo scanners.
static core::Frame makeSquareFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f; // Leave a small margin within the output area
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
    // Close the loop by repeating the first point
    frame.points.push_back(frame.points.front());
    return frame;
}

// Generates a white circle outline using evenly spaced points around
// the circumference.
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

// Generates a square with each side a different colour:
// green (bottom), red (right), white (top), blue (left).
// Useful for verifying colour channel mapping and output orientation.
static core::Frame makeRGBWSquareFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f;
    constexpr int pointsPerSide = 100;
    frame.points.reserve(pointsPerSide * 4);

    const float corners[][2] = {
        {cx - s, cy - s}, {cx + s, cy - s},
        {cx + s, cy + s}, {cx - s, cy + s},
    };
    // Each side gets a distinct colour at full brightness.
    // User RGB adjustments are applied later in post-processing.
    struct SideCol { float r, g, b; };
    const SideCol sideCols[] = {
        {0, 1, 0},   // Bottom: green
        {1, 0, 0},   // Right:  red
        {1, 1, 1},   // Top:    white
        {0, 0, 1},   // Left:   blue
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

// Generates a square where the scanner dwells (lingers) at each corner
// and moves quickly along the edges. This creates bright corner dots
// connected by dim lines — useful for testing scanner speed and tuning.
static core::Frame makeHotCornersFrame(float size, float cx, float cy) {
    core::Frame frame;
    const float s = size * 0.8f;
    constexpr int cornerDwell = 30;  // Number of points dwelling at each corner
    constexpr int edgePoints = 20;   // Number of points for the fast edge transit
    constexpr int totalPerSide = cornerDwell + edgePoints;
    frame.points.reserve(totalPerSide * 4);

    const float corners[][2] = {
        {cx - s, cy - s}, {cx + s, cy - s},
        {cx + s, cy + s}, {cx - s, cy + s},
    };

    for (int side = 0; side < 4; ++side) {
        int next = (side + 1) % 4;
        // Dwell at this corner (many points at the same position = bright dot)
        for (int i = 0; i < cornerDwell; ++i) {
            frame.points.push_back({corners[side][0], corners[side][1], 1, 1, 1});
        }
        // Move quickly along the edge to the next corner (few points = dim line)
        for (int i = 0; i < edgePoints; ++i) {
            float t = static_cast<float>(i + 1) / static_cast<float>(edgePoints);
            float x = corners[side][0] + t * (corners[next][0] - corners[side][0]);
            float y = corners[side][1] + t * (corners[next][1] - corners[side][1]);
            frame.points.push_back({x, y, 1, 1, 1});
        }
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Point pattern generators
// ---------------------------------------------------------------------------

// Returns the (x, y) positions for a given point pattern arrangement.
// These are used by both the frame generator and the preview renderer.
static std::vector<std::pair<float,float>> getPointPositions(int patternIndex, float cx, float cy, float spacing) {
    std::vector<std::pair<float,float>> pts;
    float s = spacing * 0.3f; // Scale spacing for nice separation
    switch (patternIndex) {
        case 0: // Single centred dot
            pts.push_back({cx, cy});
            break;
        case 1: // Two dots, vertical
            pts.push_back({cx, cy - s});
            pts.push_back({cx, cy + s});
            break;
        case 2: // Two dots, horizontal
            pts.push_back({cx - s, cy});
            pts.push_back({cx + s, cy});
            break;
        case 3: // Four dots in a grid
            pts.push_back({cx - s, cy - s});
            pts.push_back({cx + s, cy - s});
            pts.push_back({cx + s, cy + s});
            pts.push_back({cx - s, cy + s});
            break;
        case 4: // Eight dots in a row (scans left-right then right-left)
        {
            float halfW = spacing * 0.8f;
            for (int i = 0; i < 8; ++i) {
                float t = (static_cast<float>(i) / 7.0f - 0.5f) * 2.0f * halfW;
                pts.push_back({cx + t, cy});
            }
            // Return path (skip endpoints to avoid double-dwell at the ends)
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

// Generates a point-mode frame. Each dot gets a dwell period where the
// scanner stays still. The duty cycle controls what fraction of the dwell
// period the laser is on vs blanked. Between dots, blanked transit points
// smooth the scanner movement.
static core::Frame makePointFrame(int patternIndex,
                                   float cx, float cy, float size, float dutyCycle) {
    core::Frame frame;
    auto positions = getPointPositions(patternIndex, cx, cy, size);
    int numDots = static_cast<int>(positions.size());
    int dwellPerDot = std::max(4, 100 / numDots);
    int onPoints = std::max(1, static_cast<int>(std::round(dutyCycle / 100.0f * dwellPerDot)));
    int transitPoints = (numDots > 1) ? 20 : 0; // Blanked path between dots

    frame.points.reserve((dwellPerDot + transitPoints) * numDots);
    for (int d = 0; d < numDots; ++d) {
        auto [px, py] = positions[d];

        // Dwell on this dot: first 'onPoints' are lit, rest are blanked
        for (int i = 0; i < dwellPerDot; ++i) {
            if (i < onPoints) {
                frame.points.push_back({px, py, 1, 1, 1});
            } else {
                frame.points.push_back({px, py, 0, 0, 0});
            }
        }

        // Smooth blanked transit to the next dot position
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

// Dispatches to the appropriate shape generator based on pattern index.
static core::Frame makeShapeFrame(int patternIndex, float size, float cx, float cy) {
    switch (patternIndex) {
        case 0: return makeSquareFrame(size, cx, cy);
        case 1: return makeCircleFrame(size, cx, cy);
        case 2: return makeRGBWSquareFrame(size, cx, cy);
        case 3: return makeHotCornersFrame(size, cx, cy);
    }
    return makeSquareFrame(size, cx, cy);
}

// ============================================================================
// ILDA PATTERN LOADING
// ============================================================================
//
// ILDA is the standard file format for laser show content. We scan a
// patterns/ directory next to the executable, parse each .ild file using
// ILDAParser, and store the first frame for playback.
// ============================================================================

// Returns the path to the patterns/ directory next to the executable.
static std::string getPatternsDir(const char* argv0) {
    std::string exePath(argv0);
    auto lastSlash = exePath.find_last_of("/\\");
    std::string dir = (lastSlash != std::string::npos) ? exePath.substr(0, lastSlash) : ".";
    return dir + "/patterns";
}

// Scans a directory for .ild files, parses each one, and stores the first
// frame in state.ildaPatterns. Uses platform-specific directory listing.
static void loadILDAPatterns(AppState& state, const std::string& dir) {
    state.ildaPatterns.clear();

#ifdef _WIN32
    // Windows: use FindFirstFile/FindNextFile
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((dir + "\\*.ild").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        std::string filename = fd.cFileName;
#else
    // POSIX: use opendir/readdir
    auto* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.size() < 4) continue;
        // Case-insensitive .ild extension check
        std::string ext = filename.substr(filename.size() - 4);
        for (auto& c : ext) c = static_cast<char>(::tolower(c));
        if (ext != ".ild") continue;
#endif
        std::string fullPath = dir + "/" + filename;
        auto frames = ILDAParser::load(fullPath);
        if (!frames.empty() && !frames[0].empty()) {
            // Use filename (without extension) as the display name
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

    // Sort alphabetically for consistent UI ordering
    std::sort(state.ildaPatterns.begin(), state.ildaPatterns.end(),
              [](const ILDAPattern& a, const ILDAPattern& b) { return a.name < b.name; });
}

// Converts ILDA point data into a libera Frame. ILDA coordinates (-32768..32767)
// are normalised to -1..1, then scaled by size and offset. Blanked points
// get zero colour; lit points keep their native ILDA colours (which will be
// modulated by user RGB in post-processing).
static core::Frame makeILDAFrame(const std::vector<ILDAPoint>& ildaPoints,
                                  float size, float cx, float cy) {
    core::Frame frame;
    frame.points.reserve(ildaPoints.size());

    for (auto& ip : ildaPoints) {
        // Map ILDA int16 coordinates to normalised -1..1 space, then apply size and offset
        float nx = static_cast<float>(ip.x) / 32768.0f * size + cx;
        float ny = static_cast<float>(ip.y) / 32768.0f * size + cy;

        if (ip.blank) {
            frame.points.push_back({nx, ny, 0, 0, 0});
        } else {
            // Extract RGB from the packed 0xRRGGBB colour
            float ir = static_cast<float>((ip.color >> 16) & 0xFF) / 255.0f;
            float ig = static_cast<float>((ip.color >> 8) & 0xFF) / 255.0f;
            float ib = static_cast<float>(ip.color & 0xFF) / 255.0f;
            frame.points.push_back({nx, ny, ir, ig, ib});
        }
    }
    return frame;
}

// ============================================================================
// PREVIEW RENDERING
// ============================================================================
//
// These functions draw pattern previews into ImGui draw lists for the UI.
// They map from the normalised -1..1 coordinate space to screen pixel
// coordinates within a given rectangle.
// ============================================================================

// Draws a shape pattern (by index) into a screen rectangle.
// Used for pattern thumbnail previews in the UI.
static void drawPatternInRect(ImDrawList* drawList, int patternIndex,
                               float size, float cx, float cy,
                               ImVec2 rectPos, ImVec2 rectSize,
                               float brightnessScale = 1.0f,
                               bool flipYForPreview = false) {
    // Generate the frame with raw design colours
    core::Frame frame = makeShapeFrame(patternIndex, size, cx, cy);

    // Map normalised coordinates (-1..1) to screen pixel coordinates
    auto mapX = [&](float x) { return rectPos.x + (x + 1.0f) * 0.5f * rectSize.x; };
    auto mapY = [&](float y) {
        if (flipYForPreview) y = -y; // Flip Y so +Y is up (laser convention) vs down (screen)
        return rectPos.y + (y + 1.0f) * 0.5f * rectSize.y;
    };

    // Draw lines between consecutive points
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

// Draws ILDA point data into a screen rectangle. Similar to drawPatternInRect
// but works directly from ILDAPoint data (before conversion to a Frame).
// Used for ILDA pattern thumbnail previews.
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
        // Skip blanked segments
        if (points[i].blank || points[i - 1].blank) continue;

        float x0 = static_cast<float>(points[i - 1].x) / 32768.0f * scale + cx;
        float y0 = static_cast<float>(points[i - 1].y) / 32768.0f * scale + cy;
        float x1 = static_cast<float>(points[i].x) / 32768.0f * scale + cx;
        float y1 = static_cast<float>(points[i].y) / 32768.0f * scale + cy;

        // Extract colour from packed 0xRRGGBB format
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
// Main preview canvas
// ---------------------------------------------------------------------------

// Draws the main output preview: a black square showing what the laser
// will project. The preview dims when disarmed and shows a "DISARMED" label.
// A grey bounding box shows the output area based on current size/offset.
static void drawPreview(AppState& state, ImVec2 pos, ImVec2 size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Black background with grey border
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255));
    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(80, 80, 80, 255));

    // Convert percentage-based state values to normalised 0..1 / -1..1 range
    float previewSize = state.outputSize / 100.0f;
    float previewCx = state.outputX / 100.0f;
    float previewCy = state.outputY / 100.0f;

    // Dim the preview when disarmed to visually indicate no output
    float previewBrightness = state.armed ? 1.0f : 0.47f;

    // Draw the current pattern/mode into the preview
    if (state.outputMode == 0) {
        // Shape mode: draw the selected vector shape
        drawPatternInRect(drawList, state.patternIndex, previewSize, previewCx, previewCy,
                          pos, size, previewBrightness, true);
    } else if (state.outputMode == 1) {
        // Point mode: draw dots with crosshairs
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
        // Pattern mode: draw the selected ILDA pattern
        int idx = state.selectedIldaPattern;
        if (idx >= 0 && idx < static_cast<int>(state.ildaPatterns.size())) {
            float brightness = state.armed ? 1.0f : 0.47f;
            drawILDAInRect(drawList, state.ildaPatterns[idx].points, pos, size, true,
                           brightness, previewSize * 0.8f, previewCx, previewCy);
        }
    }

    // Show "DISARMED" text when not armed
    if (!state.armed) {
        const char* text = "DISARMED";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(
            ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + size.y - textSize.y - 8.0f),
            IM_COL32(100, 100, 100, 255), text);
    }

    // Draw a bounding box showing the output area
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

// ============================================================================
// CONTROLLER DISCOVERY & CONNECTION
// ============================================================================
//
// Controller discovery runs on a background thread, polling every ~2 seconds.
// Results are passed to the main thread via a mutex-protected vector.
// Connection is asynchronous (std::async) so the UI stays responsive while
// waiting for a controller to respond.
// ============================================================================

// Background thread function: periodically calls discoverControllers() and
// stores the results for the main thread to pick up.
static void discoveryThreadFunc(AppState& state) {
    while (state.discoveryRunning.load()) {
        // Perform a discovery scan (blocks briefly while scanning network/USB)
        auto discovered = state.liberaSystem.discoverControllers();
        {
            // Store results under lock for the main thread to consume
            std::lock_guard<std::mutex> lock(state.discoveredMutex);
            state.latestDiscovered.clear();
            for (auto& d : discovered)
                state.latestDiscovered.push_back({d->idValue(), d->labelValue(), d->type(), d->maxPointRate()});
            state.discoveryResultReady.store(true);
        }
        // Wait ~2 seconds before next scan, but wake early if a rescan is requested
        for (int i = 0; i < 20 && state.discoveryRunning.load(); ++i) {
            if (state.discoveryRequested.load()) { state.discoveryRequested.store(false); break; }
            std::this_thread::sleep_for(100ms);
        }
    }
}

// Forward declaration (startAsyncConnect and applyDiscoveryResults reference each other)
static void startAsyncConnect(AppState& state, ControllerEntry& entry);

// Called from the main thread each frame. Checks if new discovery results are
// available and updates the controller list accordingly.
static void applyDiscoveryResults(AppState& state) {
    if (!state.discoveryResultReady.load()) return;

    // Move results out from under the lock
    std::vector<DiscoveredInfo> discovered;
    {
        std::lock_guard<std::mutex> lock(state.discoveredMutex);
        discovered = std::move(state.latestDiscovered);
        state.discoveryResultReady.store(false);
    }

    // Remove controllers that are no longer discovered
    for (auto& entry : state.controllers) {
        bool stillPresent = false;
        for (auto& d : discovered) { if (d.id == entry.id) { stillPresent = true; break; } }
        if (!stillPresent && entry.controller) { entry.controller.reset(); entry.enabled = false; }
    }

    // Add newly discovered controllers
    for (auto& d : discovered) {
        bool exists = false;
        for (auto& entry : state.controllers) { if (entry.id == d.id) { exists = true; break; } }
        if (!exists) {
            state.controllers.push_back({d.id, d.label, d.type, d.maxPointRate, false, false, nullptr, {}});
        }
    }
}

// Called from the main thread each frame. Checks if any async connections
// have completed and retrieves the connected controller.
static void pollAsyncConnections(AppState& state) {
    for (auto& entry : state.controllers) {
        if (entry.connecting && entry.connectFuture.valid()) {
            if (entry.connectFuture.wait_for(0ms) == std::future_status::ready) {
                entry.controller = entry.connectFuture.get();
                entry.connecting = false;
                // Set the point rate on the newly connected controller
                if (entry.controller)
                    entry.controller->setPointRate(static_cast<std::uint32_t>(state.effectivePointRate()));
            }
        }
    }
}

// ============================================================================
// FRAME BUILDING & SENDING
// ============================================================================
//
// Each frame of the main loop:
//   1. buildCurrentFrame() generates a raw frame based on current mode/pattern
//   2. User brightness/RGB is applied as post-processing
//   3. Spatial transforms (flip) are applied
//   4. The frame is sent to all enabled controllers
// ============================================================================

// Builds a complete output frame based on the current UI state.
// First generates raw pattern data, then applies brightness/RGB.
static core::Frame buildCurrentFrame(const AppState& state) {
    // Convert percentage-based UI values to normalised coordinates
    float sz = state.outputSize / 100.0f;
    float cx = state.outputX / 100.0f;
    float cy = state.outputY / 100.0f;

    // Generate raw frame based on current output mode
    core::Frame frame;
    if (state.outputMode == 1) {
        // Point mode
        frame = makePointFrame(state.pointPatternIndex, cx, cy, sz, state.dutyCycle);
    } else if (state.outputMode == 2) {
        // ILDA pattern mode
        int idx = state.selectedIldaPattern;
        if (idx >= 0 && idx < static_cast<int>(state.ildaPatterns.size()))
            frame = makeILDAFrame(state.ildaPatterns[idx].points, sz * 0.8f, cx, cy);
        else
            frame = makeShapeFrame(0, sz, cx, cy); // Fallback to square
    } else {
        // Shape mode
        frame = makeShapeFrame(state.patternIndex, sz, cx, cy);
    }

    // Post-processing: multiply all point colours by the user's brightness/RGB.
    // This is done after generation so pattern generators don't need to know
    // about the user's colour settings — they just output design colours.
    float r, g, b;
    state.effectiveRGB(r, g, b);
    for (auto& pt : frame.points) {
        pt.r *= r;
        pt.g *= g;
        pt.b *= b;
    }

    return frame;
}

// Sends the current frame to all enabled and connected controllers.
// Called once per main loop iteration.
static void sendFramesToControllers(AppState& state) {
    // Build the frame (pattern + colour post-processing)
    core::Frame frame = buildCurrentFrame(state);

    // Apply spatial transforms (flip X/Y)
    for (auto& pt : frame.points)
        applyTransform(pt.x, pt.y, state);

    // Send to each enabled controller
    for (auto& entry : state.controllers) {
        if (!entry.enabled || !entry.controller) continue;

        // Update controller settings each frame (they may have changed in the UI)
        entry.controller->setArmed(state.armed);
        entry.controller->setPointRate(static_cast<std::uint32_t>(state.effectivePointRate()));
        entry.controller->setScannerSync(static_cast<double>(state.scannerSync));

        // Only send a new frame if the controller is ready (its buffer has space)
        if (entry.controller->isReadyForNewFrame()) {
            core::Frame copy = frame;
            entry.controller->sendFrame(std::move(copy));
        }
    }
}

// Starts an asynchronous connection to a controller. The connection runs on
// a background thread (via std::async) so the UI remains responsive.
static void startAsyncConnect(AppState& state, ControllerEntry& entry) {
    entry.connecting = true;
    std::string entryId = entry.id;
    System* sys = &state.liberaSystem;
    entry.connectFuture = std::async(std::launch::async, [sys, entryId]() -> std::shared_ptr<core::LaserController> {
        // Re-discover to get a fresh ControllerInfo, then connect
        auto discovered = sys->discoverControllers();
        for (auto& d : discovered) {
            if (d->idValue() == entryId) return sys->connectController(*d);
        }
        return nullptr;
    });
}

// Cleanly disconnects a controller: disarms it first, then releases the connection.
static void disconnectController(ControllerEntry& entry) {
    if (entry.controller) { entry.controller->setArmed(false); entry.controller.reset(); }
    entry.connecting = false;
}

// ============================================================================
// MAIN — APPLICATION ENTRY POINT
// ============================================================================
//
// Sets up the window, ImGui, and the main loop. The main loop:
//   1. Polls for new controller discoveries
//   2. Checks async connection results
//   3. Builds and sends frames to controllers
//   4. Renders the ImGui UI
// ============================================================================

int main(int /*argc*/, char* argv[]) {
    // --- Initialise GLFW ---
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }

    // Configure OpenGL version (3.2 Core on macOS, 3.0 elsewhere)
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

    // --- Create the application window ---
    GLFWwindow* window = glfwCreateWindow(900, 700, "Laser Tool", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // --- Initialise Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Don't save/load imgui.ini

    // Scale the default font for HiDPI displays. We render at native resolution
    // and use FontGlobalScale to shrink back to logical size.
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float dpiScale = xscale;
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 13.0f * dpiScale;
    io.Fonts->AddFontDefault(&fontConfig);
    io.FontGlobalScale = 1.0f / dpiScale;

    // Use the default dark theme with slightly more generous spacing
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);

    // Initialise ImGui platform/renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // --- Load ILDA patterns from the patterns/ directory ---
    loadILDAPatterns(state, getPatternsDir(argv[0]));

    // --- Start the background controller discovery thread ---
    state.discoveryRunning.store(true);
    state.discoveryThread = std::thread(discoveryThreadFunc, std::ref(state));

    // =====================================================================
    // MAIN LOOP
    // =====================================================================
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // --- Controller management (runs every frame) ---
        applyDiscoveryResults(state);    // Check for newly discovered controllers
        pollAsyncConnections(state);     // Check if pending connections have completed
        sendFramesToControllers(state);  // Build and send the current frame

        // --- Begin ImGui frame ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a single fullscreen window (no title bar, not resizable)
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Laser Tool", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // --- Layout calculations ---
        float windowWidth = ImGui::GetContentRegionAvail().x;
        float windowHeight = ImGui::GetContentRegionAvail().y;
        float controlPanelWidth = 340.0f;
        float bottomPanelHeight = 180.0f;

        // The preview canvas is a square that fits in the remaining space
        float previewSize = std::min(windowWidth - controlPanelWidth - 20.0f,
                                      windowHeight - bottomPanelHeight - 20.0f);
        if (previewSize < 100.0f) previewSize = 100.0f;

        // --- Draw the preview canvas (left side) ---
        ImVec2 previewPos = ImGui::GetCursorScreenPos();
        drawPreview(state, previewPos, ImVec2(previewSize, previewSize));

        // =================================================================
        // RIGHT PANEL — Controls
        // =================================================================
        float rightX = previewPos.x + previewSize + 16.0f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, previewPos.y));
        ImGui::BeginGroup();

        float sliderWidth = controlPanelWidth - 16.0f;
        ImGui::PushItemWidth(sliderWidth);

        // --- ARM button ---
        // The laser only outputs when armed. This is a safety feature.
        {
            const char* armLabel = state.armed ? "!! ARMED !!" : "ARM";
            if (ImGui::Button(armLabel, ImVec2(controlPanelWidth - 16.0f, 44.0f)))
                state.armed = !state.armed;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Brightness & RGB controls ---
        // Master brightness slider affects all channels equally.
        ImGui::SliderFloat("Brightness", &state.brightness, 0.0f, 100.0f, "%.0f%%");

        // Per-channel sliders with enable checkboxes.
        // The checkbox allows completely disabling a channel (useful for
        // testing individual colours or diagnosing wiring issues).
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

        // --- Output mode selector ---
        // Shape: vector test patterns (square, circle, etc.)
        // Point: static dot patterns for alignment/focus testing
        // Pattern: playback of loaded ILDA files
        ImGui::RadioButton("Shape", &state.outputMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Point", &state.outputMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Pattern", &state.outputMode, 2);

        ImGui::Spacing();

        // --- Mode-specific content ---
        if (state.outputMode == 0) {
            // SHAPE MODE: Show clickable thumbnails for each test pattern.
            // Each thumbnail is drawn using the actual pattern generator
            // into a small ImGui draw list rectangle.
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

                    // Draw thumbnail background and border
                    drawList->AddRectFilled(thumbPos, ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                                            IM_COL32(10, 10, 10, 255));
                    drawList->AddRect(thumbPos, ImVec2(thumbPos.x + thumbSize, thumbPos.y + thumbSize),
                                      borderCol, 2.0f, 0, borderThickness);

                    // Draw the actual pattern into the thumbnail
                    float thumbBright = selected ? 1.0f : 0.6f;
                    drawPatternInRect(drawList, i, 1.0f, 0.0f, 0.0f,
                                      thumbPos, ImVec2(thumbSize, thumbSize), thumbBright);

                    // Centred label below thumbnail
                    ImVec2 labelSize = ImGui::CalcTextSize(patternNames[i]);
                    drawList->AddText(
                        ImVec2(thumbPos.x + (thumbSize - labelSize.x) * 0.5f, thumbPos.y + thumbSize + 2.0f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 255),
                        patternNames[i]);

                    // Invisible button over the thumbnail for click detection
                    ImGui::SetCursorScreenPos(thumbPos);
                    char btnId[32];
                    std::snprintf(btnId, sizeof(btnId), "##thumb%d", i);
                    if (ImGui::InvisibleButton(btnId, ImVec2(thumbSize, thumbSize)))
                        state.patternIndex = i;

                    // Layout: wrap to next row after every 4 thumbnails
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
            // POINT MODE: Show thumbnails for each dot arrangement,
            // plus a duty cycle slider.
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

                    // Draw dots in the thumbnail at their pattern positions
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

            // Duty cycle: what fraction of dwell time the laser is on
            ImGui::Text("Duty Cycle");
            ImGui::PushItemWidth(controlPanelWidth - 16.0f);
            ImGui::SliderFloat("##duty", &state.dutyCycle, 1.0f, 100.0f, "%.0f%%");
            ImGui::PopItemWidth();
        } else {
            // PATTERN MODE: Show thumbnails for loaded ILDA patterns.
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

                    // Draw the ILDA pattern into the thumbnail
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

        // --- Point rate ---
        // Controls the scan speed (points per second). Higher rates give
        // smoother output but require faster galvo scanners.
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

        // --- Output geometry ---
        // Size (0-100%) controls how much of the scanner's range is used.
        // X and Y offsets (-100% to +100%) shift the output within the field.
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

        // --- Flip controls ---
        // Mirror the output horizontally and/or vertically.
        // Useful when the projector is mounted in different orientations.
        ImGui::Checkbox("Flip X", &state.flipX);
        ImGui::SameLine();
        ImGui::Checkbox("Flip Y", &state.flipY);

        ImGui::Spacing();

        // --- Scanner sync ---
        // Adds a delay between consecutive points to help galvo scanners
        // stay synchronised. Units are 1/10,000s (so 2.0 = 0.2ms delay).
        ImGui::Text("Scanner Sync");
        ImGui::PushItemWidth(controlPanelWidth - 16.0f);
        ImGui::SliderFloat("##scansync", &state.scannerSync, 0.0f, 10.0f, "%.1f");
        ImGui::PopItemWidth();

        ImGui::PopItemWidth();
        ImGui::EndGroup();

        // =================================================================
        // BOTTOM PANEL — Discovered Controllers
        // =================================================================
        // Shows all discovered laser controllers with status indicators
        // and enable/disable checkboxes. Enabling a controller starts an
        // async connection; disabling it disconnects cleanly.
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

                // Status indicator: a coloured square showing connection health
                //   Grey   = not connected
                //   Green  = connected and healthy
                //   Orange = connected with issues (e.g. buffer underruns)
                //   Red    = error state
                // Click to clear error counters. Hover for details.
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float sz = ImGui::GetFrameHeight();
                    ImDrawList* ctrlDrawList = ImGui::GetWindowDrawList();
                    ImU32 statusCol;
                    core::ControllerStatus status = core::ControllerStatus::Good;
                    if (!entry.controller || !entry.enabled) {
                        statusCol = IM_COL32(77, 77, 77, 255); // Grey = disconnected
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
                    // Click the status indicator to clear error counters
                    if (ImGui::IsItemClicked() && entry.controller)
                        entry.controller->clearErrors();

                    ImGui::SameLine();
                }

                // Enable/disable checkbox + controller name
                bool wasEnabled = entry.enabled;
                ImGui::Checkbox("##enable", &entry.enabled);
                ImGui::SameLine();
                ImGui::Text("%s", entry.label.c_str());

                // Tooltip with controller details on hover
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Type: %s", entry.type.c_str());
                    if (entry.maxPointRate > 0) ImGui::Text("Max: %u pps", entry.maxPointRate);
                    ImGui::Text("ID: %s", entry.id.c_str());
                    ImGui::EndTooltip();
                }

                // Handle enable/disable state changes
                if (entry.enabled && !wasEnabled && !entry.connecting)
                    startAsyncConnect(state, entry);
                else if (!entry.enabled && wasEnabled)
                    disconnectController(entry);

                ImGui::PopID();
            }
        }

        ImGui::EndChild();
        ImGui::End();

        // --- Render ---
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // =====================================================================
    // SHUTDOWN
    // =====================================================================
    // Clean up in reverse order of initialisation: stop discovery thread,
    // disconnect all controllers, shut down libera, then tear down ImGui/GLFW.

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
