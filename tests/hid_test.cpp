// Host-side tests for the USB/HID layer logic (descriptors + control requests).
//   g++ -std=c++17 -Wall -Wextra -Isource tests/hid_test.cpp source/hid_function.cpp source/portal.cpp -o hid_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hid_function.h"

using namespace sky;

static int g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

using Bytes = std::vector<uint8_t>;

static SetupPacket setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length) {
    const uint8_t raw[8] = {type,
                            req,
                            uint8_t(value),
                            uint8_t(value >> 8),
                            uint8_t(index),
                            uint8_t(index >> 8),
                            uint8_t(length),
                            uint8_t(length >> 8)};
    return SetupPacket::parse(raw);
}

// Walks a concatenated descriptor blob, checking every bLength.
static std::vector<Bytes> split(const Bytes& blob) {
    std::vector<Bytes> out;
    size_t i = 0;
    while (i < blob.size()) {
        const size_t len = blob[i];
        CHECK(len >= 2 && i + len <= blob.size());
        out.emplace_back(blob.begin() + i, blob.begin() + i + len);
        i += len;
    }
    return out;
}

int main() {
    // --- report descriptor ---------------------------------------------------
    {
        CHECK(sizeof kReportDescriptor == 29);
        CHECK(kReportDescriptor[0] == 0x06 && kReportDescriptor[28] == 0xC0);
        // Walk the HID items to make sure the descriptor parses cleanly, and that
        // the input/output reports are 8 bits x 32.
        size_t i = 0;
        int size = 0, count = 0, inputs = 0, outputs = 0, collections = 0, ends = 0;
        while (i < sizeof kReportDescriptor) {
            const uint8_t b = kReportDescriptor[i];
            const size_t n = (b & 3) == 3 ? 4 : (b & 3);
            CHECK(i + 1 + n <= sizeof kReportDescriptor);
            const uint8_t tag = b & 0xFC;
            if (tag == 0x74) size = kReportDescriptor[i + 1];    // Report Size
            if (tag == 0x94) count = kReportDescriptor[i + 1];   // Report Count
            if (tag == 0x80) { ++inputs; CHECK(size * count == 8 * int(kReportSize)); }
            if (tag == 0x90) { ++outputs; CHECK(size * count == 8 * int(kReportSize)); }
            if (tag == 0xA0) ++collections;
            if (tag == 0xC0) ++ends;
            i += 1 + n;
        }
        CHECK(i == sizeof kReportDescriptor);
        CHECK(inputs == 1 && outputs == 1 && collections == 1 && ends == 1);
    }

    // --- configuration descriptors for each bus speed ---------------------------
    for (Speed speed : {Speed::Full, Speed::High, Speed::Super}) {
        const uint8_t ifn = 0;
        Bytes blob;
        for (const auto& chunk : interfaceDescriptors(ifn, speed)) blob.insert(blob.end(), chunk.begin(), chunk.end());
        const auto d = split(blob);
        const bool ss = (speed == Speed::Super);
        CHECK(d.size() == (ss ? 6u : 4u));

        // Interface: HID class, two endpoints.
        CHECK(d[0].size() == 9 && d[0][1] == 0x04 && d[0][2] == ifn && d[0][4] == 2 && d[0][5] == 0x03);

        // HID descriptor length field must match the report descriptor we serve.
        CHECK(d[1].size() == 9 && d[1][1] == 0x21);
        CHECK(d[1][6] == 0x22 && (d[1][7] | (d[1][8] << 8)) == int(kReportDescriptorSize));

        // Endpoints: interrupt, 32-byte packets, IN then OUT, matching the helpers.
        const Bytes& in = d[2];
        const Bytes& out = ss ? d[4] : d[3];
        CHECK(in.size() == 7 && in[1] == 0x05 && in[2] == endpointInAddress(ifn) && (in[2] & 0x80));
        CHECK(in[3] == 0x03 && (in[4] | (in[5] << 8)) == int(kReportSize));
        CHECK(out.size() == 7 && out[2] == endpointOutAddress(ifn) && !(out[2] & 0x80) && out[3] == 0x03);
        CHECK(in[6] == (speed == Speed::Full ? 1 : 4) && out[6] == in[6]);
        if (ss) CHECK(d[3][1] == 0x30 && d[5][1] == 0x30);
    }
    CHECK(endpointInAddress(0) == 0x81 && endpointOutAddress(0) == 0x01);
    CHECK(endpointInAddress(1) == 0x82 && endpointOutAddress(1) == 0x02);

    // --- setup packet parsing -------------------------------------------------
    {
        const SetupPacket sp = setup(0x81, 0x06, 0x2200, 0x0003, 0x0100);
        CHECK(sp.bmRequestType == 0x81 && sp.bRequest == 0x06 && sp.wValue == 0x2200);
        CHECK(sp.wIndex == 3 && sp.wLength == 0x100 && sp.deviceToHost());
    }

    Portal portal;
    HidFunction hid(portal);

    // --- GET_DESCRIPTOR(report) is trimmed to wLength ---------------------------
    {
        ControlAction a = hid.onSetup(setup(0x81, 0x06, 0x2200, 0, 0x0200));
        CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == kReportDescriptorSize);
        CHECK(std::memcmp(a.data.data(), kReportDescriptor, kReportDescriptorSize) == 0);

        a = hid.onSetup(setup(0x81, 0x06, 0x2200, 0, 8));
        CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 8 && a.data[0] == 0x06);

        a = hid.onSetup(setup(0x81, 0x06, 0x2100, 0, 255));
        CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 9 && a.data[1] == 0x21);

        // Unknown descriptor type: stall.
        a = hid.onSetup(setup(0x81, 0x06, 0x2300, 0, 64));
        CHECK(a.kind == ControlAction::Kind::Stall);
    }

    // --- SET_REPORT drives the portal; the command arrives via onControlOut -----
    {
        ControlAction a = hid.onSetup(setup(0x21, 0x09, 0x0200, 0, 32));
        CHECK(a.kind == ControlAction::Kind::Receive && a.rxLen == 32);
        Bytes cmd(32, 0);
        cmd[0] = 'A';
        cmd[1] = 1;
        hid.onControlOut(cmd.data(), cmd.size());
        CHECK(portal.activated());
        Report r = hid.nextInput();
        CHECK(r[0] == 'A' && r[1] == 1 && r[2] == 0xFF && r[3] == 0x77);

        // A stray data stage without a preceding SET_REPORT is ignored.
        hid.onControlOut(cmd.data(), cmd.size());
        CHECK(!portal.hasQueuedReply());

        // Oversized wLength is bounded.
        a = hid.onSetup(setup(0x21, 0x09, 0x0200, 0, 0xFFFF));
        CHECK(a.kind == ControlAction::Kind::Receive && a.rxLen == 0x1000);

        // A new setup cancels a stale pending SET_REPORT.
        hid.onSetup(setup(0x21, 0x0A, 0, 0, 0));
        hid.onControlOut(cmd.data(), cmd.size());
        CHECK(!portal.hasQueuedReply());

        // SET_REPORT with no data is just acknowledged.
        a = hid.onSetup(setup(0x21, 0x09, 0x0200, 0, 0));
        CHECK(a.kind == ControlAction::Kind::Ack);
    }

    // --- interrupt OUT path ---------------------------------------------------------
    {
        const uint8_t cmd[] = {'M', 0x07};
        hid.onInterruptOut(cmd, sizeof cmd);
        Report r = hid.nextInput();
        CHECK(r[0] == 'M' && r[1] == 0x07 && r[3] == 0x19);
    }

    // --- GET_REPORT returns a status report -------------------------------------------
    {
        ControlAction a = hid.onSetup(setup(0xA1, 0x01, 0x0100, 0, 32));
        CHECK(a.kind == ControlAction::Kind::Send && a.data.size() == 32 && a.data[0] == 'S');
    }

    // --- idle / protocol -------------------------------------------------------------------
    {
        CHECK(hid.onSetup(setup(0x21, 0x0A, 0, 0, 0)).kind == ControlAction::Kind::Ack);
        CHECK(hid.onSetup(setup(0x21, 0x0B, 1, 0, 0)).kind == ControlAction::Kind::Ack);
        ControlAction a = hid.onSetup(setup(0xA1, 0x02, 0, 0, 1));
        CHECK(a.kind == ControlAction::Kind::Send && a.data == Bytes{0x00});
        a = hid.onSetup(setup(0xA1, 0x03, 0, 0, 1));
        CHECK(a.kind == ControlAction::Kind::Send && a.data == Bytes{0x01});
        CHECK(hid.onSetup(setup(0xA1, 0x7F, 0, 0, 4)).kind == ControlAction::Kind::Stall);
    }

    // --- standard SET_INTERFACE / GET_INTERFACE (alt setting 0) are accepted ------------------
    {
        CHECK(hid.onSetup(setup(0x01, 0x0B, 0, 0, 0)).kind == ControlAction::Kind::Ack);
        CHECK(hid.onSetup(setup(0x01, 0x0B, 1, 0, 0)).kind == ControlAction::Kind::Stall);  // no alt 1
        ControlAction a = hid.onSetup(setup(0x81, 0x0A, 0, 0, 1));
        CHECK(a.kind == ControlAction::Kind::Send && a.data == Bytes{0x00});
    }

    // --- report-ID-prefixed SET_REPORT (33 bytes) works --------------------------------------
    {
        Portal p2;
        HidFunction h2(p2);
        ControlAction a = h2.onSetup(setup(0x21, 0x09, 0x0200, 0, 33));
        CHECK(a.kind == ControlAction::Kind::Receive);
        uint8_t cmd[33] = {};
        cmd[1] = 'R';
        h2.onControlOut(cmd, sizeof cmd);
        CHECK(p2.stats().unknown == 0 && p2.hasQueuedReply());
        CHECK(p2.nextInReport()[0] == 'R');
    }

    // --- requests for other recipients are not ours -----------------------------------------
    {
        CHECK(hid.onSetup(setup(0x80, 0x06, 0x0100, 0, 18)).kind == ControlAction::Kind::Stall);  // device
        CHECK(hid.onSetup(setup(0x02, 0x01, 0, 0x81, 0)).kind == ControlAction::Kind::Stall);     // endpoint
    }

    // --- bus reset puts the portal back to inactive but keeps figures ----------------------------
    {
        std::vector<uint8_t> fig(kFigureSize, 0x5A);
        CHECK(portal.loadFigure(fig.data(), fig.size(), "f") == 0);
        const uint8_t act[] = {'A', 1};
        hid.onInterruptOut(act, sizeof act);
        CHECK(portal.activated());
        hid.onBusReset();
        CHECK(!portal.activated());
        CHECK(portal.slot(0).present());
        CHECK(!portal.hasQueuedReply());
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
