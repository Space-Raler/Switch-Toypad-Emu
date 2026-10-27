// LEGO Dimensions Toy Pad USB HID function.
// Implements the public ToyPad HID frame protocol, including tag events, page reads/writes,
// model identification, and the TEA-protected seed/challenge exchange.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include "hid_function.h"
#include "lego_dimensions_protocol.h"

namespace sky::lego {
constexpr uint16_t kVendorId = 0x0E6F;
constexpr uint16_t kProductId = 0x0241;
constexpr uint16_t kBcdDevice = 0x0210;
constexpr const char* kManufacturerString = "LEGO System A/S";
constexpr const char* kProductString = "LEGO READER V2.10";
constexpr size_t kReportDescriptorSize = 29;
extern const uint8_t kReportDescriptor[kReportDescriptorSize];
std::vector<std::vector<uint8_t>> interfaceDescriptors(uint8_t interfaceNumber, Speed speed,
                                                       uint8_t interval = 0, uint8_t outAddressOverride = 0);
constexpr uint8_t endpointInAddress(uint8_t n) { return uint8_t(0x81 + n); }
constexpr uint8_t endpointOutAddress(uint8_t n) { return uint8_t(0x01 + n); }

struct TagProfile {
    std::array<uint8_t,7> uid{};
    bool vehicle = false;
    uint32_t figureId = 0; // character ID; for vehicles this is also stored in token memory
    std::array<uint8_t,180> memory{}; // 45 four-byte pages, as exposed by the ToyPad protocol
};

class ToyPadFunction {
public:
    ToyPadFunction();
    void setInterfaceNumber(uint8_t n) { interface_ = n; }
    ControlAction onSetup(const SetupPacket& sp);
    void onControlOut(const uint8_t* data, size_t len);
    void onInterruptOut(const uint8_t* data, size_t len);
    bool peekInput(Report& out) const;
    void dropInput();
    void onBusReset();
    bool place(size_t position, const std::array<uint8_t,7>& uid);
    bool placeProfile(size_t position, const TagProfile& profile);
    bool takeDirty(size_t position, TagProfile& profile);
    void markDirty(size_t position);
    bool remove(size_t position);
    bool move(size_t from, size_t to);
    bool occupied(size_t position) const;
    bool dirty(size_t position) const;
    const std::array<uint8_t,7>& uid(size_t position) const;
    // True once the game has sent its wake command on this connection (cleared by a bus reset).
    bool hostActive() const { return hostInitialized_; }
private:
    void queue(const Report& r);
    void queuePresentTags();
    void handleCommand(const uint8_t* data, size_t len);
    void handleFrame(const uint8_t* data, size_t len);
    void queueResponse(uint8_t cid, const uint8_t* payload, size_t len);
    std::array<std::array<uint8_t,7>, kPositionCount> uids_{};
    std::array<TagProfile, kPositionCount> profiles_{};
    std::array<bool, kPositionCount> dirty_{};
    std::array<bool, kPositionCount> occupied_{};
    // ToyPad token indices are independent of the seven visual pad positions.
    std::array<uint8_t, kPositionCount> tokenIndices_{};
    std::deque<Report> out_;
    uint8_t interface_ = 0;
    bool pendingSetReport_ = false;
    bool hostInitialized_ = false;
};
} // namespace sky::lego
