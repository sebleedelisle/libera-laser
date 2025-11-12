#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/log/Log.hpp"

#include <array>
#include <cstdint>

using namespace libera;
using namespace libera::etherdream;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

static std::array<std::uint8_t, 22> makeAck(char cmd, std::uint16_t bufferFullness, std::uint32_t pointRate) {
    std::array<std::uint8_t, 22> raw{};
    raw[0] = static_cast<std::uint8_t>('a');
    raw[1] = static_cast<std::uint8_t>(cmd);
    raw[2] = 0;                       // protocol
    raw[3] = static_cast<std::uint8_t>(LightEngineState::Ready);
    raw[4] = static_cast<std::uint8_t>(PlaybackState::Prepared);
    raw[5] = 0;                       // source
    // flags all zero
    raw[12] = static_cast<std::uint8_t>(bufferFullness & 0xFFu);
    raw[13] = static_cast<std::uint8_t>((bufferFullness >> 8) & 0xFFu);

    raw[14] = static_cast<std::uint8_t>(pointRate & 0xFFu);
    raw[15] = static_cast<std::uint8_t>((pointRate >> 8) & 0xFFu);
    raw[16] = static_cast<std::uint8_t>((pointRate >> 16) & 0xFFu);
    raw[17] = static_cast<std::uint8_t>((pointRate >> 24) & 0xFFu);
    // remaining bytes leave as zero
    return raw;
}

static void testDecode() {
    auto raw = makeAck('p', 512, 30000);
    EtherDreamResponse response;
    ASSERT_TRUE(response.decode(raw.data(), raw.size()), "decode ack succeeds");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "response tag");
    ASSERT_EQ(response.command, static_cast<std::uint8_t>('p'), "command echo");
    ASSERT_EQ(response.status.bufferFullness, static_cast<std::uint16_t>(512), "buffer fullness");
    ASSERT_EQ(response.status.pointRate, static_cast<std::uint32_t>(30000), "point rate");
    ASSERT_TRUE(response.status.playbackState == PlaybackState::Prepared, "playback state prepared");
}

static void testRejectShort() {
    EtherDreamResponse response;
    ASSERT_TRUE(!response.decode(nullptr, 0), "decode rejects short buffer");
}

int main() {
    testDecode();
    testRejectShort();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }
    logInfo("EtherDreamResponse tests passed");
    return 0;
}
