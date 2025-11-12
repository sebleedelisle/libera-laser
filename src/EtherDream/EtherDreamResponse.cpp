#include "libera/etherdream/EtherDreamResponse.hpp"

#include <iomanip>
#include <sstream>

namespace libera::etherdream {
namespace {
std::uint16_t read_le_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0])
         | static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t read_le_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}
} // namespace

bool EtherDreamResponse::decode(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 22) {
        return false;
    }

    response = data[0];
    command = data[1];

    const auto* statusBytes = data + 2;
    status.protocol = statusBytes[0];
    status.lightEngineState = static_cast<LightEngineState>(statusBytes[1]);
    status.playbackState = static_cast<PlaybackState>(statusBytes[2]);
    status.source = statusBytes[3];
    status.lightEngineFlags = read_le_u16(statusBytes + 4);
    status.playbackFlags = read_le_u16(statusBytes + 6);
    status.sourceFlags = read_le_u16(statusBytes + 8);
    status.bufferFullness = read_le_u16(statusBytes + 10);
    status.pointRate = read_le_u32(statusBytes + 12);
    status.pointCount = read_le_u32(statusBytes + 16);

    return true;
}

const char* EtherDreamStatus::toString(LightEngineState state) {
    switch (state) {
        case LightEngineState::Ready:   return "ready";
        case LightEngineState::Warmup:  return "warmup";
        case LightEngineState::Cooldown:return "cooldown";
        case LightEngineState::Estop:   return "estop";
    }
    return "unknown";
}

const char* EtherDreamStatus::toString(PlaybackState state) {
    switch (state) {
        case PlaybackState::Idle:     return "idle";
        case PlaybackState::Prepared: return "prepared";
        case PlaybackState::Playing:  return "playing";
        case PlaybackState::Paused:   return "paused";
    }
    return "unknown";
}

std::string EtherDreamStatus::describe() const {
    std::ostringstream os;
    os << "lt=" << toString(lightEngineState)
       << " pb=" << toString(playbackState)
       << " buffer=" << bufferFullness; 
     //  << " rate=" << pointRate
     //  << " count=" << pointCount
     //  << " flags{L=0x" << std::hex << std::uppercase << lightEngineFlags
     //  << " P=0x" << playbackFlags
    //   << " S=0x" << sourceFlags << std::dec << std::nouppercase << "}";
    return os.str();
}

std::string EtherDreamStatus::toHexLine(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return {};
    }

    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        if (i) os << ' ';
        os << std::setw(2) << static_cast<int>(data[i]);
    }
    return os.str();
}

} // namespace libera::etherdream
