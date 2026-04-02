#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <fstream>

struct ILDAPoint {
    int16_t  x = 0;
    int16_t  y = 0;
    int16_t  z = 0;          // 0 for 2D records
    bool     blank = false;  // blanking flag
    uint8_t  colorIndex = 0; // for indexed formats (0/1)
    uint32_t color = 0;      // packed 0xRRGGBB (true-colour or palette-mapped)
    bool     is3D = false;
    bool     isIndexed = false;
};

class ILDAParser {
public:
    // One vector<ILDAPoint> per ILDA frame (formats 0,1,4,5).
    // Vertices are **native ILDA int16**; no mapping/flip/normalise.
    static std::vector<std::vector<ILDAPoint>> load(const std::string& path);

private:
    // Parsing
    static bool readHeader(std::ifstream& file, uint8_t& formatCode, uint16_t& numRecords);
    static std::vector<ILDAPoint> readPointsIndexed(std::ifstream& file,
                                                    uint8_t formatCode,
                                                    uint16_t numRecords,
                                                    const std::array<uint32_t,256>& palette);
    static std::vector<ILDAPoint> readPointsTrueColour(std::ifstream& file,
                                                       uint8_t formatCode,
                                                       uint16_t numRecords);
    static void readPalette(std::ifstream& file,
                            uint16_t numRecords,
                            std::array<uint32_t,256>& palette);
    static bool readBEi16(std::ifstream& f, int16_t& out);

    // Utilities
    static std::array<uint32_t,256> defaultPalette();
    static const char* formatName(uint8_t fmt);
};
