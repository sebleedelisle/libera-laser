#include "ILDAParser.h"
#include <limits>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace {
    constexpr int ILDA_HEADER_SIZE = 32;

    inline uint32_t packRGB(uint8_t r, uint8_t g, uint8_t b) {
        return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }
}

// ---------- utils

bool ILDAParser::readBEi16(std::ifstream& f, int16_t& out) {
    unsigned char b[2];
    if (!f.read(reinterpret_cast<char*>(b), 2)) return false;
    out = static_cast<int16_t>((uint16_t(b[0]) << 8) | uint16_t(b[1]));
    return true;
}

const char* ILDAParser::formatName(uint8_t fmt) {
    switch (fmt) {
        case 0: return "0 (3D indexed)";
        case 1: return "1 (2D indexed)";
        case 2: return "2 (palette)";
        case 4: return "4 (3D true-colour)";
        case 5: return "5 (2D true-colour)";
        default: return "unknown";
    }
}

// ---------- header

bool ILDAParser::readHeader(std::ifstream& file,
                            uint8_t& formatCode,
                            uint16_t& numRecords)
{
    char header[ILDA_HEADER_SIZE];

    int next = file.peek();
    if (next == EOF) return false;

    if (!file.read(header, ILDA_HEADER_SIZE)) return false;

    if (std::strncmp(header, "ILDA", 4) != 0) {
        std::cerr << "[ILDAParser] Missing ILDA magic. Stopping.\n";
        return false;
    }

    formatCode = static_cast<uint8_t>(header[7]);
    numRecords = (static_cast<uint8_t>(header[24]) << 8)
               |  static_cast<uint8_t>(header[25]);
    return true;
}

// ---------- default palette (fallback when file has no format-2 palette)

std::array<uint32_t,256> ILDAParser::defaultPalette() {
    // Fallback list (3-digit hex ramp like the JS demo)
    static const char* kJsDefault[] = {
        "#F00","#F10","#F20","#F30","#F40","#F50","#F60","#F70",
        "#F80","#F90","#FA0","#FB0","#FC0","#FD0","#FE0","#FF0",
        "#FF0","#EF0","#CF0","#AF0","#8F0","#6F0","#4F0","#2F0",
        "#0F0","#0F2","#0F4","#0F6","#0F8","#0FA","#0FC","#0FE",
        "#08F","#07F","#06F","#06F","#05F","#04F","#04F","#02F",
        "#00F","#20F","#40F","#60F","#80F","#A0F","#C0F","#E0F",
        "#F0F","#F2F","#F4F","#F6F","#F8F","#FAF","#FCF","#FEF",
        "#FFF","#FEE","#FCC","#FAA","#F88","#F66","#F44","#022"
    };
    constexpr int JS_N = int(sizeof(kJsDefault)/sizeof(kJsDefault[0]));

    auto hex3 = [](const char* s)->uint32_t {
        auto hx = [](unsigned char c)->int {
            if (c >= '0' && c <= '9') return c - '0';
            c = static_cast<unsigned char>(::toupper(c));
            return (c >= 'A' && c <= 'F') ? (10 + c - 'A') : 0;
        };
        // s like "#RGB"
        int r4 = hx(s[1]), g4 = hx(s[2]), b4 = hx(s[3]);
        uint8_t r = uint8_t((r4 << 4) | r4);
        uint8_t g = uint8_t((g4 << 4) | g4);
        uint8_t b = uint8_t((b4 << 4) | b4);
        return packRGB(r,g,b);
    };

    std::array<uint32_t,256> pal{};
    for (int i = 0; i < 256; ++i) pal[i] = hex3(kJsDefault[i % JS_N]);
    return pal;
}

// ---------- palette section (format 2)

void ILDAParser::readPalette(std::ifstream& file,
                             uint16_t numRecords,
                             std::array<uint32_t,256>& palette)
{
    const uint16_t count = std::min<uint16_t>(numRecords, 256);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t rgb[3] = {0,0,0};
        if (!file.read(reinterpret_cast<char*>(rgb), 3)) {
            std::cerr << "[ILDAParser] Palette truncated at index " << i << "\n";
            break;
        }
        // On disk for palette, order is R,G, B per spec.
        palette[i] = packRGB(rgb[0], rgb[1], rgb[2]);
    }
    if (numRecords > count) {
        file.seekg(static_cast<std::streamoff>(numRecords - count) * 3, std::ios::cur);
    }
}

// ---------- indexed formats (0,1)

std::vector<ILDAPoint> ILDAParser::readPointsIndexed(std::ifstream& file,
                                                     uint8_t formatCode,
                                                     uint16_t numRecords,
                                                     const std::array<uint32_t,256>& palette)
{
    std::vector<ILDAPoint> pts;
    pts.reserve(numRecords);
    const bool is3D = (formatCode == 0);

    for (uint16_t i = 0; i < numRecords; ++i) {
        ILDAPoint p;
        if (!readBEi16(file, p.x)) break;
        if (!readBEi16(file, p.y)) break;
        if (is3D) {
            if (!readBEi16(file, p.z)) break;
            p.is3D = true;
        } else { p.z = 0; p.is3D = false; }

        // Packed 16-bit status+index word (big-endian)
        unsigned char b[2];
        if (!file.read(reinterpret_cast<char*>(b), 2)) break;
        uint16_t word = (uint16_t(b[0]) << 8) | uint16_t(b[1]);

        p.blank      = (word & 0x4000) != 0;              // bit 14 = blanked
        p.colorIndex = static_cast<uint8_t>(word & 0xFF); // (use 0x7F for 7-bit flavour if desired)
        p.isIndexed  = true;

        p.color = palette[p.colorIndex];
        pts.push_back(p);
    }
    return pts;
}

// ---------- true-colour formats (4,5)

std::vector<ILDAPoint> ILDAParser::readPointsTrueColour(std::ifstream& file,
                                                        uint8_t formatCode,
                                                        uint16_t numRecords)
{
    std::vector<ILDAPoint> pts;
    pts.reserve(numRecords);

    const bool is3D = (formatCode == 4);
    for (uint16_t i = 0; i < numRecords; ++i) {
        ILDAPoint p;
        if (!readBEi16(file, p.x)) break;
        if (!readBEi16(file, p.y)) break;
        if (is3D) { if (!readBEi16(file, p.z)) break; p.is3D = true; }
        else      { p.z = 0; p.is3D = false; }

        uint8_t status = 0;
        if (!file.read(reinterpret_cast<char*>(&status), 1)) break;
        p.blank = (status & 0x40) != 0; // bit 6

        uint8_t rgb[3] = {0,0,0};
        if (!file.read(reinterpret_cast<char*>(rgb), 3)) break;

        // Many ILDA true-colour frames on disk are **B, G, R** order.
        // Map to RGB here:
        uint8_t r = rgb[2], g = rgb[1], b = rgb[0];
        p.color = packRGB(r,g,b);

        p.isIndexed = false;
        pts.push_back(p);
    }
    return pts;
}

// ---------- top-level: one vector<ILDAPoint> per frame (native coords)

std::vector<std::vector<ILDAPoint>> ILDAParser::load(const std::string& path)
{
    std::vector<std::vector<ILDAPoint>> frames;

    std::ifstream file(path, std::ios::binary); // path assumed already resolved
    if (!file.is_open()) {
        std::cerr << "[ILDAParser] Failed to open file: " << path << "\n";
        return frames;
    }

    std::array<uint32_t,256> palette = defaultPalette();
    int sectionIndex = 0;

    while (true) {
        uint8_t fmt = 0;
        uint16_t n  = 0;
        if (!readHeader(file, fmt, n)) break;

        std::cout << "[ILDAParser] Section " << sectionIndex
                  << " format " << formatName(fmt)
                  << " records " << n << "\n";

        if (fmt == 2) { // palette
            readPalette(file, n, palette);
            sectionIndex++;
            continue;
        }

        std::vector<ILDAPoint> pts;
        if (fmt == 0 || fmt == 1) {
            pts = readPointsIndexed(file, fmt, n, palette);
        } else if (fmt == 4 || fmt == 5) {
            pts = readPointsTrueColour(file, fmt, n);
        } else {
            std::cerr << "[ILDAParser] Skipping unsupported format: " << int(fmt) << "\n";
            sectionIndex++;
            continue;
        }

        if (!pts.empty()) frames.emplace_back(std::move(pts));
        sectionIndex++;
    }

    file.close();
    return frames;
}
