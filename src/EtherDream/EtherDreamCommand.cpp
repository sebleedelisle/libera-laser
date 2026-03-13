#include "libera/etherdream/EtherDreamCommand.hpp"

#include <algorithm>
#include <cmath>

namespace libera::etherdream {
namespace {
constexpr float COORD_SCALE = 32767.0f;
constexpr float CHANNEL_SCALE = 65535.0f;
constexpr std::uint16_t RATE_CHANGE_BIT = 0x8000u;
}

void EtherDreamCommand::setDataCommand(std::uint16_t pointCount) {
    reset();
    buffer.appendChar('d');
    buffer.appendUInt16(pointCount);
    opcode = 'd';
}

void EtherDreamCommand::addPoint(const core::LaserPoint& point, bool setRateChangeFlag) {
    std::uint16_t control = setRateChangeFlag ? RATE_CHANGE_BIT : 0u;
    buffer.appendUInt16(control);
    buffer.appendInt16(encodeCoordinate(point.x));
    buffer.appendInt16(encodeCoordinate(point.y));
    buffer.appendUInt16(encodeChannel(point.r));
    buffer.appendUInt16(encodeChannel(point.g));
    buffer.appendUInt16(encodeChannel(point.b));
    // EtherDream still carries a dedicated intensity word for older devices.
    buffer.appendUInt16(encodeChannel(point.i));
    buffer.appendUInt16(encodeChannel(point.u1));
    buffer.appendUInt16(encodeChannel(point.u2));
}

void EtherDreamCommand::setBeginCommand(std::uint32_t pointRate) {
    reset();
    buffer.appendChar('b');
    buffer.appendUInt16(0); // reserved flags
    buffer.appendUInt32(pointRate);
    opcode = 'b';
}

void EtherDreamCommand::setPointRateCommand(std::uint32_t pointRate) {
    reset();
    buffer.appendChar('q');
    buffer.appendUInt32(pointRate);
    opcode = 'q';
}

void EtherDreamCommand::setSingleByteCommand(char opcodeValue) {
    reset();
    buffer.appendChar(opcodeValue);
    opcode = opcodeValue;
}

void EtherDreamCommand::reset() {
    buffer.clear();
    opcode = 0;
}

std::int16_t EtherDreamCommand::encodeCoordinate(float value) noexcept {
    float clamped = std::clamp(value, -1.0f, 1.0f);
    float scaled = clamped * COORD_SCALE;
    auto rounded = static_cast<std::int32_t>(scaled >= 0.f ? scaled + 0.5f : scaled - 0.5f);
    auto signedWord = static_cast<std::int16_t>(std::clamp(rounded, -32768, 32767));
    return signedWord;
}

std::uint16_t EtherDreamCommand::encodeChannel(float value) noexcept {
    float clamped = std::clamp(value, 0.0f, 1.0f);
    float scaled = clamped * CHANNEL_SCALE;
    auto rounded = static_cast<std::int32_t>(scaled + 0.5f);
    auto limited = static_cast<std::uint16_t>(std::clamp(rounded, 0, 65535));
    return limited;
}

} // namespace libera::etherdream
