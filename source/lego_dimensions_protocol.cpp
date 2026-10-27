#include "lego_dimensions_protocol.h"
#include <algorithm>

namespace sky::lego {
uint8_t checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = static_cast<uint8_t>(sum + data[i]);
    return sum;
}

Report initReport() {
    Report r{};
    const uint8_t init[] = {0x55, 0x0f, 0xb0, 0x01, 0x28, 0x63, 0x29, 0x20,
                            0x4c, 0x45, 0x47, 0x4f, 0x20, 0x32, 0x30, 0x31,
                            0x34, 0xf7};
    std::copy(std::begin(init), std::end(init), r.begin());
    return r;
}

Report colourReport(Pad pad, uint8_t r, uint8_t g, uint8_t b) {
    Report out{};
    const uint8_t cmd[] = {0x55, 0x06, 0xc0, 0x02, static_cast<uint8_t>(pad), r, g, b};
    std::copy(std::begin(cmd), std::end(cmd), out.begin());
    out[sizeof(cmd)] = checksum(cmd, sizeof(cmd));
    return out;
}

Report tokenReport(Pad pad, uint8_t tokenIndex, bool placed, const std::array<uint8_t, 7>& uid) {
    Report out{};
    out[0] = 0x56;       // ToyPad tag event frame
    out[1] = 0x0b;       // payload length
    out[2] = static_cast<uint8_t>(pad);
    out[3] = 0x00;
    out[4] = tokenIndex;
    out[5] = placed ? 0x00 : 0x01;
    std::copy(uid.begin(), uid.end(), out.begin() + 6);
    out[13] = checksum(out.data(), 13);
    return out;
}
} // namespace sky::lego
