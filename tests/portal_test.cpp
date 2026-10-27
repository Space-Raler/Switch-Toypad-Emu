// Host-side tests for the portal protocol core.
//   g++ -std=c++17 -Wall -Wextra -Isource tests/portal_test.cpp source/portal.cpp -o portal_test && ./portal_test
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "portal.h"

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

static std::vector<uint8_t> makeFigure(uint32_t serial) {
    std::vector<uint8_t> d(kFigureSize);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 7 + serial);
    d[0] = uint8_t(serial);
    d[1] = uint8_t(serial >> 8);
    d[2] = uint8_t(serial >> 16);
    d[3] = uint8_t(serial >> 24);
    return d;
}

static void send(Portal& p, std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    p.handleCommand(v.data(), v.size());
}

static uint32_t statusBits(const Report& r) {
    return uint32_t(r[1]) | (uint32_t(r[2]) << 8) | (uint32_t(r[3]) << 16) | (uint32_t(r[4]) << 24);
}

static int slotBits(const Report& r, int slot) { return (statusBits(r) >> (slot * 2)) & 3; }

int main() {
    // --- handshake replies -------------------------------------------------
    {
        Portal p;
        send(p, {'R'});
        Report r = p.nextInReport();
        CHECK(r[0] == 'R' && r[1] == 0x02 && r[2] == 0x1B);

        send(p, {'A', 0x01});
        r = p.nextInReport();
        CHECK(r[0] == 'A' && r[1] == 0x01 && r[2] == 0xFF && r[3] == 0x77);
        CHECK(p.activated());

        send(p, {'M', 0x00});
        r = p.nextInReport();
        CHECK(r[0] == 'M' && r[3] == 0x19);

        send(p, {'C', 10, 20, 30});
        CHECK(p.ledR() == 10 && p.ledG() == 20 && p.ledB() == 30);
        CHECK(!p.hasQueuedReply());  // 'C' has no reply

        // With nothing queued, polling yields status reports with a counter.
        Report s1 = p.nextInReport(), s2 = p.nextInReport();
        CHECK(s1[0] == 'S' && s2[0] == 'S');
        CHECK(uint8_t(s1[5] + 1) == s2[5]);
        CHECK(s1[6] == 0x01);
    }

    // --- replies are FIFO and take priority over status ---------------------
    {
        Portal p;
        send(p, {'M', 0x05});
        send(p, {'J'});
        CHECK(p.nextInReport()[0] == 'M');
        CHECK(p.nextInReport()[0] == 'J');
        CHECK(p.nextInReport()[0] == 'S');
    }

    // --- figure arrival: 3 then 1 ------------------------------------------
    {
        Portal p;
        auto fig = makeFigure(0x11223344);
        int slot = p.loadFigure(fig.data(), fig.size(), "a.bin");
        CHECK(slot == 0);
        Report r = p.nextInReport();
        CHECK(slotBits(r, 0) == 3);
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 1);
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 1);  // stays present
        CHECK(slotBits(r, 1) == 0);
    }

    // --- read blocks --------------------------------------------------------
    {
        Portal p;
        auto fig = makeFigure(0xAABBCCDD);
        int slot = p.loadFigure(fig.data(), fig.size(), "x");
        CHECK(slot == 0);
        send(p, {'Q', 0x10, 0x05});
        Report r = p.nextInReport();
        CHECK(r[0] == 'Q' && r[1] == (0x10 | 0) && r[2] == 0x05);
        CHECK(std::memcmp(&r[3], &fig[5 * 16], 16) == 0);

        // Absent slot -> no 0x10 bit, no data
        send(p, {'Q', 0x13, 0x00});
        r = p.nextInReport();
        CHECK(r[0] == 'Q' && r[1] == 3 && r[2] == 0);
        for (int i = 3; i < 19; ++i) CHECK(r[i] == 0);

        // Out of range block / slot are ignored
        send(p, {'Q', 0x10, 0x40});
        CHECK(!p.hasQueuedReply());
    }

    // --- write blocks + dirty tracking --------------------------------------
    {
        Portal p;
        auto fig = makeFigure(1);
        p.loadFigure(fig.data(), fig.size(), "w.bin");
        std::vector<uint8_t> w = {'W', 0x10, 0x08};
        for (int i = 0; i < 16; ++i) w.push_back(uint8_t(0xA0 + i));
        p.handleCommand(w.data(), w.size());
        Report r = p.nextInReport();
        CHECK(r[0] == 'W' && r[1] == 0x10 && r[2] == 0x08);
        CHECK(p.slot(0).dirty);

        send(p, {'Q', 0x10, 0x08});
        r = p.nextInReport();
        for (int i = 0; i < 16; ++i) CHECK(r[3 + i] == uint8_t(0xA0 + i));

        std::array<uint8_t, kFigureSize> out;
        std::string tag;
        CHECK(p.takeDirty(0, out, tag));
        CHECK(tag == "w.bin");
        CHECK(out[8 * 16] == 0xA0 && out[8 * 16 + 15] == 0xAF);
        CHECK(!p.takeDirty(0, out, tag));  // cleared

        // Short write command must not read past the buffer (zero filled)
        std::vector<uint8_t> shortw = {'W', 0x10, 0x09, 0xEE};
        p.handleCommand(shortw.data(), shortw.size());
        send(p, {'Q', 0x10, 0x09});
        r = p.nextInReport();  // W reply
        r = p.nextInReport();  // Q reply
        CHECK(r[3] == 0xEE && r[4] == 0);
    }

    // --- removal: 2 then 0, then reuse of the same slot ----------------------
    {
        Portal p;
        auto a = makeFigure(100), b = makeFigure(200);
        CHECK(p.loadFigure(a.data(), a.size(), "a") == 0);
        CHECK(p.loadFigure(b.data(), b.size(), "b") == 1);
        p.nextInReport();
        p.nextInReport();  // both settled at 1

        CHECK(p.removeFigure(0));
        CHECK(!p.removeFigure(0));
        Report r = p.nextInReport();
        CHECK(slotBits(r, 0) == 2 && slotBits(r, 1) == 1);
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 0 && slotBits(r, 1) == 1);

        // Slot 0 is empty again; figure "a" returns to its remembered slot.
        CHECK(p.loadFigure(a.data(), a.size(), "a2") == 0);
    }

    // --- removing before the game saw the arrival leaves no ghost ------------
    {
        Portal p;
        auto a = makeFigure(77);
        CHECK(p.loadFigure(a.data(), a.size(), "a") == 0);
        CHECK(p.removeFigure(0));
        CHECK(!p.slot(0).present());
        send(p, {'Q', 0x10, 0x00});
        Report r = p.nextInReport();
        CHECK(r[0] == 'Q' && r[1] == 0);   // absent: no 0x10 bit
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 2);
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 0);
    }

    // --- slot affinity -------------------------------------------------------
    {
        Portal p;
        auto a = makeFigure(1), b = makeFigure(2), c = makeFigure(3);
        CHECK(p.loadFigure(a.data(), a.size(), "a") == 0);
        CHECK(p.loadFigure(b.data(), b.size(), "b") == 1);
        CHECK(p.removeFigure(0));
        CHECK(p.removeFigure(1));
        p.nextInReport();
        p.nextInReport();
        // c is new -> lowest free slot (0); b should still get its old slot 1.
        CHECK(p.loadFigure(c.data(), c.size(), "c") == 0);
        CHECK(p.loadFigure(b.data(), b.size(), "b") == 1);
        // remove c (slot 0) and a returns... a's old slot was 0 which c overwrote
        CHECK(p.removeFigure(0));
        CHECK(p.loadFigure(a.data(), a.size(), "a") == 0);
    }

    // --- activation while figures are present --------------------------------
    {
        Portal p;
        auto a = makeFigure(9);
        p.loadFigure(a.data(), a.size(), "a");
        // Game starts: R (deactivate) then A (activate) -> arrival replayed.
        send(p, {'R'});
        p.nextInReport();                 // R reply
        send(p, {'A', 1});
        p.nextInReport();                 // A reply
        // Queue was cleared by deactivate() and refilled by activate().
        Report r = p.nextInReport();
        CHECK(slotBits(r, 0) == 3);
        r = p.nextInReport();
        CHECK(slotBits(r, 0) == 1);
    }

    // --- capacity ------------------------------------------------------------
    {
        Portal p;
        for (int i = 0; i < kSlots; ++i) {
            auto f = makeFigure(1000 + i);
            CHECK(p.loadFigure(f.data(), f.size(), "f") == i);
        }
        auto extra = makeFigure(5000);
        CHECK(p.loadFigure(extra.data(), extra.size(), "x") == -1);
        // wrong size
        uint8_t small[10] = {};
        CHECK(p.loadFigure(small, sizeof small, "s") == -1);
        // 16 slots fit in the 4 status bytes
        Report r = p.nextInReport();
        for (int i = 0; i < kSlots; ++i) CHECK(slotBits(r, i) == 3);
    }

    // --- unknown commands are counted, never crash ---------------------------
    {
        Portal p;
        send(p, {'Z', 1, 2, 3});
        CHECK(p.stats().unknown == 1 && p.stats().lastUnknown == 'Z');
        p.handleCommand(nullptr, 0);
        CHECK(p.stats().commands == 1);
    }

    // --- markDirty lets a failed save be retried -------------------------------
    {
        Portal p;
        auto a = makeFigure(31);
        p.loadFigure(a.data(), a.size(), "a.bin");
        p.markDirty(5);   // empty slot: ignored
        p.markDirty(-1);  // out of range: ignored
        std::array<uint8_t, kFigureSize> out;
        std::string tag;
        CHECK(!p.takeDirty(0, out, tag));  // untouched figure is not dirty
        p.markDirty(0);
        CHECK(p.takeDirty(0, out, tag) && tag == "a.bin");
        CHECK(!p.takeDirty(0, out, tag));
    }

    // --- report-ID prefix, speaker audio ------------------------------------------
    {
        Portal p;
        send(p, {0x00, 'M', 0x01});               // host prefixed a report ID of 0
        CHECK(p.stats().unknown == 0);
        CHECK(p.speakerOn());
        Report r = p.nextInReport();
        CHECK(r[0] == 'M' && r[1] == 0x01 && r[3] == 0x19);

        // Speaker on: OUT packets are audio. Bytes that look like a write must not touch figures.
        std::vector<uint8_t> fig(kFigureSize, 0x11);
        CHECK(p.loadFigure(fig.data(), fig.size(), "a.bin") == 0);
        uint8_t audio[kReportSize] = {'W', 0x00, 0x05};
        for (int i = 3; i < 19; ++i) audio[i] = 0xEE;
        p.handleOut(audio, sizeof audio);
        CHECK(p.stats().audioPackets == 1);
        CHECK(!p.slot(0).dirty && p.slot(0).data[5 * kBlockSize] == 0x11);

        send(p, {'M', 0x00});
        CHECK(!p.speakerOn());
        p.handleOut(audio, sizeof audio);          // speaker off: a command again
        CHECK(p.slot(0).dirty && p.slot(0).data[5 * kBlockSize] == 0xEE);

        send(p, {'M', 0x01});
        CHECK(p.speakerOn());
        send(p, {'R'});
        CHECK(!p.speakerOn());
        send(p, {'M', 0x01});
        p.hostReset();
        CHECK(!p.speakerOn());
    }

    // --- 'A' with argument 0 still activates (RPCS3 / Flipper reference) ------------
    {
        Portal p;
        send(p, {'A', 0x00});
        CHECK(p.activated());
    }

    // --- host reset ------------------------------------------------------------
    {
        Portal p;
        send(p, {'A', 1});
        CHECK(p.activated());
        p.hostReset();
        CHECK(!p.activated());
        CHECK(!p.hasQueuedReply());
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
