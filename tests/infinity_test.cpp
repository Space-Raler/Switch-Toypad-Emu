// Host-side tests for the Disney Infinity mode: USB descriptors, control requests, the
// OUT -> reply -> IN path through inf::InfinityFunction, and the generic figure-file helpers.
//   make test   (builds the C core with gcc and this file with g++)
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "figures.h"
#include "infinity/infinity_figures.h"
#include "infinity_function.h"

using namespace sky;

static int g_checks = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        ++g_checks;                                               \
        if (!(c)) {                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);      \
            std::exit(1);                                         \
        }                                                         \
    } while (0)

static SetupPacket setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len) {
    SetupPacket sp;
    sp.bmRequestType = type;
    sp.bRequest = req;
    sp.wValue = value;
    sp.wIndex = index;
    sp.wLength = len;
    return sp;
}

static std::vector<uint8_t> flatten(const std::vector<std::vector<uint8_t>>& chunks) {
    std::vector<uint8_t> out;
    for (const auto& c : chunks) out.insert(out.end(), c.begin(), c.end());
    return out;
}

// A command packet as the game sends it: FF <len> <command> <sequence> <args...>
static void command(inf::InfinityFunction& fn, uint8_t cmd, uint8_t seq, std::vector<uint8_t> args = {}) {
    uint8_t p[inf::kPacketSize] = {0};
    p[0] = 0xFF;
    p[1] = uint8_t(args.size() + 2);
    p[2] = cmd;
    p[3] = seq;
    for (size_t i = 0; i < args.size(); ++i) p[4 + i] = args[i];
    fn.onInterruptOut(p, sizeof p);
}

static Report takeIn(inf::InfinityFunction& fn) {
    Report r{};
    CHECK(fn.peekInput(r));
    Report again{};
    CHECK(fn.peekInput(again));
    CHECK(r == again);  // peeking does not consume
    fn.dropInput();
    return r;
}

static void testDescriptors() {
    // Report descriptor: vendor page, 32 x 8-bit input and output, 29 bytes.
    CHECK(sizeof(inf::kReportDescriptor) == 29);
    int inputs = 0, outputs = 0;
    uint32_t size = 0, count = 0;
    for (size_t i = 0; i < sizeof(inf::kReportDescriptor);) {
        const uint8_t item = inf::kReportDescriptor[i];
        const size_t len = item & 3;
        const uint8_t tag = item & 0xFC;
        const uint32_t v = len ? inf::kReportDescriptor[i + 1] : 0;
        if (tag == 0x74) size = v;
        if (tag == 0x94) count = v;
        if (tag == 0x80) {
            CHECK(size * count == 256);
            ++inputs;
        }
        if (tag == 0x90) {
            CHECK(size * count == 256);
            ++outputs;
        }
        i += 1 + len;
    }
    CHECK(inputs == 1 && outputs == 1);

    const Speed speeds[] = {Speed::Full, Speed::High, Speed::Super};
    for (Speed sp : speeds) {
        const auto chunks = inf::interfaceDescriptors(0, sp);
        CHECK(chunks.size() == (sp == Speed::Super ? 6u : 4u));
        const auto& intf = chunks[0];
        CHECK(intf.size() == 9 && intf[1] == 0x04 && intf[4] == 2 && intf[5] == 3);
        const auto& hid = chunks[1];
        CHECK(hid.size() == 9 && hid[1] == 0x21 && hid[7] == 29 && hid[8] == 0);
        const auto& epIn = chunks[2];
        CHECK(epIn.size() == 7 && epIn[1] == 0x05 && epIn[2] == 0x81 && epIn[3] == 0x03 && epIn[4] == 32 && epIn[5] == 0);
        const auto& epOut = chunks[sp == Speed::Super ? 4 : 3];
        CHECK(epOut.size() == 7 && epOut[2] == 0x01 && epOut[4] == 32);
        const uint8_t expected = sp == Speed::Full ? 1 : 4;  // 1 ms
        CHECK(epIn[6] == expected && epOut[6] == expected);
        if (sp == Speed::Super) CHECK(chunks[3].size() == 6 && chunks[3][1] == 0x30 && chunks[5].size() == 6);
    }
    // Overrides: interval for both endpoints, OUT address.
    const auto custom = inf::interfaceDescriptors(0, Speed::Full, 8, 2);
    CHECK(custom[2][6] == 8 && custom[3][6] == 8 && custom[3][2] == 2);
    // Interface number shifts both addresses.
    const auto second = inf::interfaceDescriptors(1, Speed::Full);
    CHECK(second[0][2] == 1 && second[2][2] == 0x82 && second[3][2] == 0x02);
    CHECK(flatten(inf::interfaceDescriptors(0, Speed::Full)).size() == 9 + 9 + 7 + 7);
    CHECK(inf::kVendorId == 0x0E6F && inf::kProductId == 0x0129);
}

static void testControl() {
    InfBase base;
    inf_base_init(&base);
    inf::InfinityFunction fn(base);

    // Report descriptor and HID descriptor, trimmed to wLength.
    ControlAction a = fn.onSetup(setup(0x81, 0x06, 0x2200, 0, 255));
    CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 29 && a.data[0] == 0x06 && a.data[28] == 0xC0);
    a = fn.onSetup(setup(0x81, 0x06, 0x2200, 0, 9));
    CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 9);
    a = fn.onSetup(setup(0x81, 0x06, 0x2100, 0, 255));
    CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 9 && a.data[1] == 0x21);
    a = fn.onSetup(setup(0x81, 0x06, 0x2300, 0, 255));  // unknown descriptor type
    CHECK(a.kind == ControlAction::Kind::Stall);

    // HID class requests.
    CHECK(fn.onSetup(setup(0x21, 0x0A, 0, 0, 0)).kind == ControlAction::Kind::Ack);   // SET_IDLE
    CHECK(fn.onSetup(setup(0x21, 0x0B, 0, 0, 0)).kind == ControlAction::Kind::Ack);   // SET_PROTOCOL
    CHECK(fn.onSetup(setup(0xA1, 0x02, 0, 0, 1)).kind == ControlAction::Kind::Send);  // GET_IDLE
    CHECK(fn.onSetup(setup(0xA1, 0x03, 0, 0, 1)).data[0] == 1);                       // GET_PROTOCOL
    CHECK(fn.onSetup(setup(0xA1, 0x01, 0x0100, 0, 32)).kind == ControlAction::Kind::Stall);  // GET_REPORT
    // Standard requests to the interface.
    CHECK(fn.onSetup(setup(0x81, 0x00, 0, 0, 2)).data.size() == 2);                    // GET_STATUS
    CHECK(fn.onSetup(setup(0x81, 0x0A, 0, 0, 1)).kind == ControlAction::Kind::Send);   // GET_INTERFACE
    CHECK(fn.onSetup(setup(0x01, 0x0B, 0, 0, 0)).kind == ControlAction::Kind::Ack);    // SET_INTERFACE 0
    CHECK(fn.onSetup(setup(0x01, 0x0B, 1, 0, 0)).kind == ControlAction::Kind::Stall);  // SET_INTERFACE 1
    // Device / endpoint recipients belong to usb:ds.
    CHECK(fn.onSetup(setup(0x80, 0x06, 0x0100, 0, 18)).kind == ControlAction::Kind::Stall);
    CHECK(fn.onSetup(setup(0x82, 0x00, 0, 0, 2)).kind == ControlAction::Kind::Stall);

    // SET_REPORT: data stage, then the command is handled like an interrupt-OUT packet.
    a = fn.onSetup(setup(0x21, 0x09, 0x0200, 0, 32));
    CHECK(a.kind == ControlAction::Kind::Receive && a.rxLen == 32);
    uint8_t cmd[32] = {0xFF, 0x02, 0x80, 0x07};
    fn.onControlOut(cmd, sizeof cmd);
    CHECK(base.commands_seen == 1);
    Report r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x15 && r[2] == 0x00);

    // With a leading report ID 0 (33 bytes) the ID is skipped.
    CHECK(fn.onSetup(setup(0x21, 0x09, 0x0200, 0, 33)).kind == ControlAction::Kind::Receive);
    uint8_t withId[33] = {0x00, 0xFF, 0x02, 0x80, 0x08};
    fn.onControlOut(withId, sizeof withId);
    CHECK(base.commands_seen == 2);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA);

    // Not a command (no 0xFF start), or no SET_REPORT pending: ignored.
    CHECK(fn.onSetup(setup(0x21, 0x09, 0x0200, 0, 32)).kind == ControlAction::Kind::Receive);
    uint8_t junk[32] = {0x12, 0x34, 0x80};
    fn.onControlOut(junk, sizeof junk);
    fn.onControlOut(cmd, sizeof cmd);  // pending flag already consumed
    CHECK(base.commands_seen == 2);
    CHECK(fn.onSetup(setup(0x21, 0x09, 0, 0, 0)).kind == ControlAction::Kind::Ack);  // empty SET_REPORT
}

static void testProtocolFlow() {
    InfBase base;
    inf_base_init(&base);
    inf::InfinityFunction fn(base);
    Report r{};
    CHECK(!fn.peekInput(r));  // idle: nothing to send

    // Base info request: the 24-byte answer.
    command(fn, 0x80, 0x01);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x15 && r[2] == 0x00);
    CHECK(!fn.peekInput(r));

    // A short OUT packet is zero padded and still handled.
    const uint8_t shortPkt[4] = {0xFF, 0x02, 0x80, 0x09};
    fn.onInterruptOut(shortPkt, sizeof shortPkt);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && base.commands_seen == 2);

    // Handshake flag follows the challenge command.
    CHECK(!base.handshake_seen);
    command(fn, 0x81, 0x02, {1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(base.handshake_seen);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x01 && r[2] == 0x02);

    // No figures: "which figures are present" answers with an empty list.
    command(fn, 0xA1, 0x03);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x01 && r[2] == 0x03);

    // A figure arrives: an event packet is queued without the game asking.
    uint8_t uid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t fig[INF_FIGURE_SIZE];
    CHECK(inf_figure_create_blank(fig, 0x0F4241, 1, uid));  // Mr. Incredible
    CHECK(inf_base_load_figure(&base, 3, fig));
    r = takeIn(fn);
    CHECK(r[0] == 0xAB && r[1] == 0x04 && r[2] == 2 /* position */ && r[5] == 0x00 /* arrived */);
    CHECK(!fn.peekInput(r));

    // The game lists the figures, asks for its identity, and reads a block.
    command(fn, 0xA1, 0x04);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x03 && (r[3] & 0xF0) == 0x20 && r[4] == 0x09);
    const uint8_t order = r[3] & 0x0F;
    command(fn, 0xB4, 0x05, {order});
    r = takeIn(fn);
    CHECK(r[1] == 0x09 && std::memcmp(&r[4], uid, 7) == 0);
    command(fn, 0xA2, 0x06, {order, 1});  // block 1 -> file block 4
    r = takeIn(fn);
    CHECK(r[1] == 0x12 && std::memcmp(&r[4], fig + 16 * 4, 16) == 0);

    // The game writes a block: the figure is flagged for saving, exactly once.
    std::vector<uint8_t> args = {order, 1, 0};
    for (int i = 0; i < 16; ++i) args.push_back(uint8_t(0xA0 + i));
    command(fn, 0xA3, 0x07, args);
    r = takeIn(fn);
    CHECK(r[0] == 0xAA && r[1] == 0x02);
    uint8_t saved[INF_FIGURE_SIZE];
    CHECK(inf_base_take_dirty(&base, 3, saved));
    CHECK(saved[16 * 4] == 0xA0 && saved[16 * 4 + 15] == 0xAF);
    CHECK(!inf_base_take_dirty(&base, 3, saved));

    // Remove: "removed" event; a bus reset clears queued packets and the handshake but keeps figures.
    CHECK(inf_base_remove_figure(&base, 3));
    r = takeIn(fn);
    CHECK(r[0] == 0xAB && r[5] == 0x01);
    command(fn, 0x80, 0x08);
    fn.onBusReset();
    CHECK(!fn.peekInput(r));
    CHECK(!base.handshake_seen);
    CHECK(inf_base_load_figure(&base, 3, fig));
    fn.onBusReset();
    CHECK(base.figures[3].present);
}

static void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);
}

static std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

static void testFigureFiles() {
    char tmpl[] = "/tmp/skyportal_inf_test_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const std::string dir = tmpl;

    writeFile(dir + "/b.bin", std::vector<uint8_t>(320, 0x22));
    writeFile(dir + "/A.bin", std::vector<uint8_t>(320, 0x11));
    writeFile(dir + "/portal.bin", std::vector<uint8_t>(1024, 0x33));  // wrong size for this mode
    writeFile(dir + "/short.bin", std::vector<uint8_t>(319, 0x44));
    writeFile(dir + "/c.bin.bak", std::vector<uint8_t>(320, 0x55));

    auto list = scanFiguresOfSize(dir, 320);
    CHECK(list.size() == 2 && list[0].name == "A.bin" && list[1].name == "b.bin");
    CHECK(scanFiguresOfSize(dir, 1024).size() == 1);
    CHECK(scanFigures(dir).size() == 1);  // the Skylanders function still wants 1024 bytes

    uint8_t buf[320];
    CHECK(readFigureBytes(dir + "/A.bin", buf, sizeof buf) && buf[0] == 0x11 && buf[319] == 0x11);
    CHECK(!readFigureBytes(dir + "/short.bin", buf, sizeof buf));
    CHECK(!readFigureBytes(dir + "/portal.bin", buf, sizeof buf));  // longer than asked: not a dump
    CHECK(!readFigureBytes(dir + "/missing.bin", buf, sizeof buf));

    // Saving keeps the first original as .bak and leaves no .tmp behind.
    std::vector<uint8_t> v1(320, 0x77), v2(320, 0x88);
    std::string err;
    CHECK(saveFigureBytes(dir + "/A.bin", v1.data(), v1.size(), err));
    CHECK(readAll(dir + "/A.bin") == v1);
    CHECK(readAll(dir + "/A.bin.bak") == std::vector<uint8_t>(320, 0x11));
    CHECK(saveFigureBytes(dir + "/A.bin", v2.data(), v2.size(), err));
    CHECK(readAll(dir + "/A.bin") == v2);
    CHECK(readAll(dir + "/A.bin.bak") == std::vector<uint8_t>(320, 0x11));  // still the first original
    struct stat st;
    CHECK(::stat((dir + "/A.bin.tmp").c_str(), &st) != 0);
    CHECK(!saveFigureBytes(dir + "/nodir/x.bin", v1.data(), v1.size(), err) && !err.empty());

    std::string cmd = "rm -rf " + dir;
    CHECK(std::system(cmd.c_str()) == 0);
}

static void testFigureTable() {
    // Table lookups and the categories the UI uses to choose a slot.
    const InfFigureInfo* mr = inf_figure_lookup(0x0F4241);
    CHECK(mr && std::strcmp(mr->name, "Mr. Incredible") == 0 && mr->series == 1);
    const InfFigureInfo* ps = inf_figure_lookup(0x1E8481);
    CHECK(ps && 0x1E8481u / 1000000u == 2);
    CHECK(0x0F4241u / 1000000u == 1);
    CHECK(0x2DC6C3u / 1000000u == 3);
    CHECK(0x3D0912u / 1000000u == 4);
    CHECK(inf_figure_lookup(0xFFFFFF) == nullptr);

    // A generated figure decodes back to its number.
    const uint8_t uid[7] = {0x04, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t fig[INF_FIGURE_SIZE];
    CHECK(inf_figure_create_blank(fig, 0x1E8481, 1, uid));
    CHECK(inf_figure_decode_number(fig) == 0x1E8481);
}

int main() {
    testDescriptors();
    testControl();
    testProtocolFlow();
    testFigureFiles();
    testFigureTable();
    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
