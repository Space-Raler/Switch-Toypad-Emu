// Skylanders "Portal of Power" protocol core.
//
// Pure C++17, no libnx dependency, so it can be unit-tested on a PC.
// Behaviour follows the RPCS3 emulated portal (which works with all six
// Skylanders games), extended from 8 to 16 figure slots (the real portal
// reports 4 status bytes = 16 slots).
//
// This class is NOT thread safe; the caller serialises access.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace sky {

constexpr int    kSlots      = 16;
constexpr size_t kBlockSize  = 16;
constexpr size_t kBlocks     = 64;
constexpr size_t kFigureSize = kBlockSize * kBlocks;  // 1024-byte figure dump
constexpr size_t kReportSize = 32;                    // every HID report is 32 bytes

using Report = std::array<uint8_t, kReportSize>;

// Per-slot 2-bit status values reported to the game.
//   bit0 = figure present, bit1 = "changed since last report"
//   0 empty | 1 present | 2 just removed | 3 just arrived
struct Slot {
    uint8_t status = 0;
    std::deque<uint8_t> queued;  // transitions the game has not seen yet
    std::array<uint8_t, kFigureSize> data{};
    uint32_t lastSerial = 0;     // UID of the figure that last sat here
    bool     hasLastSerial = false;
    bool     dirty = false;      // data modified by the game, not yet saved
    std::string tag;             // caller's label (file path)

    bool present() const { return (status & 1) != 0; }
};

struct Stats {
    uint32_t commands = 0;       // output reports handled
    uint32_t unknown = 0;        // unrecognised command bytes
    uint32_t statusReports = 0;  // 'S' reports sent
    uint32_t replyReports = 0;   // queued replies sent
    uint8_t  lastCommand = 0;
    uint8_t  lastUnknown = 0;
    uint8_t  lastArg1 = 0;       // for the on-screen "last command" line
    uint8_t  lastArg2 = 0;
    uint32_t audioPackets = 0;   // interrupt-OUT packets treated as speaker audio, not commands
};

class Portal {
public:
    // Host -> portal: one output report (`len` valid bytes, rest treated as 0).
    void handleCommand(const uint8_t* buf, size_t len);

    // Interrupt-OUT packet. While the host has the portal speaker switched on ('M')
    // these carry PCM audio, not commands, so they must never reach handleCommand():
    // sample bytes that happen to look like 'W' would overwrite figure data. (Same
    // rule as the Flipper reference, where the OUT endpoint is audio-only.)
    void handleOut(const uint8_t* buf, size_t len);
    bool speakerOn() const { return speakerOn_; }

    // Portal -> host: next 32-byte input report. Returns the oldest queued
    // command reply if there is one, otherwise a status report.
    Report nextInReport();
    bool hasQueuedReply() const { return !replies_.empty(); }

    // Host (re)connected or reset: portal goes back to the inactive state.
    void hostReset();

    // Figure management. loadFigure returns the slot used, or -1 if the data
    // is the wrong size or every slot is occupied.
    int  loadFigure(const uint8_t* data, size_t size, const std::string& tag);
    bool removeFigure(int slot);
    void removeAll();

    // Copy out a modified figure and clear its dirty flag.
    bool takeDirty(int slot, std::array<uint8_t, kFigureSize>& out, std::string& tag);

    // Flag a present figure as unsaved again (used when saving it failed, so the
    // next save attempt retries instead of silently dropping the game's changes).
    void markDirty(int slot);

    const Slot&  slot(int i) const { return slots_[i]; }
    bool         activated() const { return activated_; }
    uint8_t      ledR() const { return led_[0]; }
    uint8_t      ledG() const { return led_[1]; }
    uint8_t      ledB() const { return led_[2]; }
    const Stats& stats() const { return stats_; }

private:
    void activate();
    void deactivate();
    Report statusReport();
    void pushReply(const Report& r);

    std::array<Slot, kSlots> slots_{};
    std::deque<Report> replies_;
    bool activated_ = false;
    bool speakerOn_ = false;
    uint8_t counter_ = 0;
    std::array<uint8_t, 3> led_{{0, 0, 0}};
    Stats stats_;
};

}  // namespace sky
