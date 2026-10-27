#include "lego_dimensions_protocol.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace sky::lego;
    const auto init = initReport();
    assert(init[0] == 0x55 && init[1] == 0x0f && init[2] == 0xb0);
    assert(init[17] == 0xf7 && init[18] == 0);
    const auto red = colourReport(Pad::All, 255, 0, 0);
    assert(red[0] == 0x55 && red[1] == 0x06 && red[2] == 0xc0);
    assert(red[4] == 0 && red[5] == 255 && red[8] == checksum(red.data(), 8));
    const std::array<uint8_t,7> uid{{1,2,3,4,5,6,7}};
    const auto add = tokenReport(Pad::Center, 2, true, uid);
    assert(add[0] == 0x56 && add[1] == 0x0b && add[2] == 1 && add[4] == 2 && add[5] == 0);
    assert(add[6] == 1 && add[12] == 7 && add[13] == checksum(add.data(), 13));
    const auto rem = tokenReport(Pad::Left, 2, false, uid);
    assert(rem[2] == 2 && rem[5] == 1);
    std::puts("LEGO Dimensions protocol primitive tests: 4 passed");
}
