#include "portal.h"

#include <algorithm>
#include <cstring>

namespace sky {

static uint32_t serialOf(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
           (uint32_t(data[3]) << 24);
}

void Portal::pushReply(const Report& r) {
    // The game polls faster than it sends commands; a runaway queue means the
    // host stopped reading. Keep it bounded.
    if (replies_.size() >= 64) replies_.pop_front();
    replies_.push_back(r);
}

void Portal::activate() {
    if (activated_) return;
    for (auto& s : slots_) {
        if (s.present()) {
            s.queued.push_back(3);
            s.queued.push_back(1);
        }
    }
    activated_ = true;
}

void Portal::deactivate() {
    for (auto& s : slots_) {
        if (!s.queued.empty()) {
            s.status = s.queued.back();
            s.queued.clear();
        }
        s.status &= 1;
    }
    activated_ = false;
}

void Portal::hostReset() {
    speakerOn_ = false;
    deactivate();
    replies_.clear();
}

Report Portal::statusReport() {
    Report r{};
    r[0] = 'S';
    uint32_t status = 0;
    for (int i = kSlots - 1; i >= 0; --i) {
        Slot& s = slots_[i];
        if (!s.queued.empty()) {
            s.status = s.queued.front();
            s.queued.pop_front();
        }
        status <<= 2;
        status |= (s.status & 3);
    }
    r[1] = uint8_t(status);
    r[2] = uint8_t(status >> 8);
    r[3] = uint8_t(status >> 16);
    r[4] = uint8_t(status >> 24);
    r[5] = counter_++;
    r[6] = 0x01;
    return r;
}

Report Portal::nextInReport() {
    if (!replies_.empty()) {
        Report r = replies_.front();
        replies_.pop_front();
        stats_.replyReports++;
        return r;
    }
    stats_.statusReports++;
    return statusReport();
}

static bool isCommandLetter(uint8_t c) {
    switch (c) {
        case 'A': case 'C': case 'J': case 'L': case 'M': case 'Q':
        case 'R': case 'S': case 'V': case 'W': case 'Z':
            return true;
        default:
            return false;
    }
}

void Portal::handleOut(const uint8_t* buf, size_t len) {
    if (speakerOn_) {
        stats_.audioPackets++;
        return;
    }
    handleCommand(buf, len);
}

void Portal::handleCommand(const uint8_t* in, size_t len) {
    // Some hosts prefix the payload with a HID report ID of 0 (33 bytes instead of 32,
    // or 2 instead of 1): skip it. The Flipper reference does the same; without this
    // every command from such a host looks like an unknown command 0x00.
    if (len > 1 && in[0] == 0x00 && isCommandLetter(in[1])) {
        ++in;
        --len;
    }
    if (len == 0) return;
    uint8_t cmd[kReportSize] = {};
    std::memcpy(cmd, in, std::min(len, kReportSize));

    stats_.commands++;
    stats_.lastCommand = cmd[0];
    stats_.lastArg1 = cmd[1];
    stats_.lastArg2 = cmd[2];

    Report r{};
    switch (cmd[0]) {
        case 'A':  // activate (any argument, as RPCS3 and the Flipper reference do)
            r[0] = 'A';
            r[1] = cmd[1];
            r[2] = 0xFF;
            r[3] = 0x77;
            pushReply(r);
            activate();
            break;

        case 'C':  // ring LED colour, no reply
            led_[0] = cmd[1];
            led_[1] = cmd[2];
            led_[2] = cmd[3];
            break;

        case 'J':  // Trap Team light/speaker-related query
            r[0] = 'J';
            pushReply(r);
            break;

        case 'L':  // side lights, no reply
            break;

        case 'M':  // speaker on/off; the audio itself arrives on the OUT endpoint
            speakerOn_ = cmd[1] != 0;
            r[0] = 'M';
            r[1] = cmd[1];
            r[2] = 0x00;
            r[3] = 0x19;
            pushReply(r);
            break;

        case 'Q': {  // read one 16-byte block of a figure
            const int sl = cmd[1] & 0x0F;
            const uint8_t block = cmd[2];
            if (sl >= kSlots || block >= kBlocks) break;
            r[0] = 'Q';
            r[2] = block;
            const Slot& s = slots_[sl];
            if (s.present()) {
                r[1] = uint8_t(0x10 | sl);
                std::memcpy(&r[3], &s.data[size_t(block) * kBlockSize], kBlockSize);
            } else {
                r[1] = uint8_t(sl);
            }
            pushReply(r);
            break;
        }

        case 'R':  // ready/reset
            r[0] = 'R';
            r[1] = 0x02;
            r[2] = 0x1B;  // firmware version, as in the Flipper reference (works with Trap Team)
            pushReply(r);
            speakerOn_ = false;
            deactivate();
            break;

        case 'S':  // status request; status is delivered by polling, no reply
        case 'V':
            break;

        case 'W': {  // write one 16-byte block
            const int sl = cmd[1] & 0x0F;
            const uint8_t block = cmd[2];
            if (sl >= kSlots || block >= kBlocks) break;
            r[0] = 'W';
            r[2] = block;
            Slot& s = slots_[sl];
            if (s.present()) {
                r[1] = uint8_t(0x10 | sl);
                std::memcpy(&s.data[size_t(block) * kBlockSize], &cmd[3], kBlockSize);
                s.dirty = true;
            } else {
                r[1] = uint8_t(sl);
            }
            pushReply(r);
            break;
        }

        default:
            stats_.unknown++;
            stats_.lastUnknown = cmd[0];
            break;
    }
}

int Portal::loadFigure(const uint8_t* data, size_t size, const std::string& tag) {
    if (size != kFigureSize) return -1;
    const uint32_t serial = serialOf(data);

    // Prefer the slot this exact figure used last (the game then sees the same
    // figure in the same place), otherwise take the lowest free slot.
    int found = -1;
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = slots_[i];
        if (!s.present() && s.hasLastSerial && s.lastSerial == serial) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        for (int i = 0; i < kSlots; ++i) {
            if (!slots_[i].present()) {
                found = i;
                break;
            }
        }
    }
    if (found < 0) return -1;

    Slot& s = slots_[found];
    std::memcpy(s.data.data(), data, kFigureSize);
    s.tag = tag;
    s.dirty = false;
    s.status = 3;
    s.queued.push_back(3);
    s.queued.push_back(1);
    s.lastSerial = serial;
    s.hasLastSerial = true;
    return found;
}

bool Portal::removeFigure(int slot) {
    if (slot < 0 || slot >= kSlots) return false;
    Slot& s = slots_[slot];
    if (!s.present()) return false;
    // Drop any arrival transitions the game has not seen yet, so the slot goes
    // empty immediately instead of flickering present for a few polls.
    s.queued.clear();
    s.status = 2;
    s.queued.push_back(2);
    s.queued.push_back(0);
    s.tag.clear();
    s.dirty = false;
    return true;
}

void Portal::removeAll() {
    for (int i = 0; i < kSlots; ++i) removeFigure(i);
}

bool Portal::takeDirty(int slot, std::array<uint8_t, kFigureSize>& out, std::string& tag) {
    if (slot < 0 || slot >= kSlots) return false;
    Slot& s = slots_[slot];
    if (!s.present() || !s.dirty) return false;
    out = s.data;
    tag = s.tag;
    s.dirty = false;
    return true;
}

void Portal::markDirty(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    if (slots_[slot].present()) slots_[slot].dirty = true;
}

}  // namespace sky
