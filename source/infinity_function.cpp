#include "infinity_function.h"

#include <algorithm>
#include <cstring>

namespace sky {
namespace inf {

// Vendor-defined, one 32-byte input and one 32-byte output report, no report IDs.
// 29 bytes long (0x1D), as the base reports.
const uint8_t kReportDescriptor[kReportDescriptorSize] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (1)
    0xA1, 0x01,              // Collection (Application)
    0x19, 0x01, 0x29, 0x20,  //   Usage Minimum (1), Maximum (32)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8)
    0x95, 0x20,              //   Report Count (32)
    0x81, 0x00,              //   Input (Data,Array,Abs)
    0x19, 0x01, 0x29, 0x20,  //   Usage Minimum (1), Maximum (32)
    0x91, 0x00,              //   Output (Data,Array,Abs)
    0xC0                     // End Collection
};

namespace {

constexpr uint8_t kDtInterface = 0x04;
constexpr uint8_t kDtEndpoint  = 0x05;
constexpr uint8_t kDtHid       = 0x21;
constexpr uint8_t kDtReport    = 0x22;
constexpr uint8_t kDtSsCompanion = 0x30;

constexpr uint8_t kTypeMask       = 0x60;
constexpr uint8_t kTypeStandard   = 0x00;
constexpr uint8_t kTypeClass      = 0x20;
constexpr uint8_t kRecipMask      = 0x1F;
constexpr uint8_t kRecipInterface = 0x01;

constexpr uint8_t kReqGetStatus     = 0x00;
constexpr uint8_t kReqGetDescriptor = 0x06;
constexpr uint8_t kReqGetInterface  = 0x0A;
constexpr uint8_t kReqSetInterface  = 0x0B;

constexpr uint8_t kHidGetIdle     = 0x02;
constexpr uint8_t kHidGetProtocol = 0x03;
constexpr uint8_t kHidSetReport   = 0x09;
constexpr uint8_t kHidSetIdle     = 0x0A;
constexpr uint8_t kHidSetProtocol = 0x0B;

std::vector<uint8_t> hidDescriptor() {
    return {
        0x09, kDtHid,
        0x11, 0x01,  // bcdHID 1.11
        0x00,        // country code
        0x01,        // one class descriptor
        kDtReport,
        uint8_t(kReportDescriptorSize & 0xFF), uint8_t(kReportDescriptorSize >> 8),
    };
}

std::vector<uint8_t> endpointDescriptor(uint8_t address, uint8_t interval) {
    return {
        0x07, kDtEndpoint, address,
        0x03,  // interrupt
        uint8_t(kPacketSize & 0xFF), uint8_t(kPacketSize >> 8),
        interval,
    };
}

ControlAction send(const std::vector<uint8_t>& bytes, uint16_t wLength) {
    ControlAction a;
    a.kind = ControlAction::Kind::Send;
    a.data = bytes;
    if (a.data.size() > wLength) a.data.resize(wLength);
    return a;
}

ControlAction ack() {
    ControlAction a;
    a.kind = ControlAction::Kind::Ack;
    return a;
}

}  // namespace

std::vector<std::vector<uint8_t>> interfaceDescriptors(uint8_t interfaceNumber, Speed speed, uint8_t interval,
                                                       uint8_t outAddressOverride) {
    // Full speed counts 1 ms frames; high/super speed count 125 us microframes as
    // 2^(n-1), so 4 = 8 microframes = 1 ms.
    const uint8_t ival = interval ? interval : ((speed == Speed::Full) ? 1 : 4);

    std::vector<std::vector<uint8_t>> out;
    out.push_back({
        0x09, kDtInterface, interfaceNumber,
        0x00,        // alternate setting
        0x02,        // two endpoints
        0x03,        // class: HID
        0x00, 0x00,  // no boot subclass / protocol
        0x00,        // no interface string
    });
    out.push_back(hidDescriptor());

    const std::vector<uint8_t> companion = {
        0x06, kDtSsCompanion,
        0x00,  // bMaxBurst
        0x00,  // bmAttributes
        uint8_t(kPacketSize & 0xFF), uint8_t(kPacketSize >> 8),  // wBytesPerInterval
    };

    out.push_back(endpointDescriptor(endpointInAddress(interfaceNumber), ival));
    if (speed == Speed::Super) out.push_back(companion);
    out.push_back(endpointDescriptor(outAddressOverride ? outAddressOverride : endpointOutAddress(interfaceNumber), ival));
    if (speed == Speed::Super) out.push_back(companion);
    return out;
}

ControlAction InfinityFunction::onSetup(const SetupPacket& sp) {
    pendingSetReport_ = false;
    const uint8_t type = sp.bmRequestType & kTypeMask;
    const uint8_t recip = sp.bmRequestType & kRecipMask;
    if (recip != kRecipInterface) return {};  // device/endpoint requests belong to the USB service

    if (type == kTypeStandard) {
        if (sp.bRequest == kReqGetDescriptor && sp.deviceToHost()) {
            const uint8_t dt = uint8_t(sp.wValue >> 8);
            if (dt == kDtReport)
                return send(std::vector<uint8_t>(kReportDescriptor, kReportDescriptor + kReportDescriptorSize),
                            sp.wLength);
            if (dt == kDtHid) return send(hidDescriptor(), sp.wLength);
        }
        if (sp.bRequest == kReqGetStatus && sp.deviceToHost()) return send({0x00, 0x00}, sp.wLength);
        if (sp.bRequest == kReqGetInterface && sp.deviceToHost()) return send({0x00}, sp.wLength);
        if (sp.bRequest == kReqSetInterface && !sp.deviceToHost() && sp.wValue == 0) return ack();
        return {};
    }

    if (type == kTypeClass) {
        switch (sp.bRequest) {
            case kHidSetReport: {
                // The reference base takes commands on the interrupt OUT endpoint only, but a
                // host that sends one as SET_REPORT gets it handled the same way.
                if (sp.wLength == 0) return ack();
                ControlAction a;
                a.kind = ControlAction::Kind::Receive;
                a.rxLen = std::min<size_t>(sp.wLength, 0x1000);
                pendingSetReport_ = true;
                return a;
            }
            case kHidGetIdle:     return send({0x00}, sp.wLength);
            case kHidGetProtocol: return send({0x01}, sp.wLength);  // report protocol
            case kHidSetIdle:
            case kHidSetProtocol: return ack();
            default: break;  // GET_REPORT: stalled, like the Flipper build
        }
    }
    return {};
}

void InfinityFunction::onControlOut(const uint8_t* data, size_t len) {
    if (!pendingSetReport_) return;
    pendingSetReport_ = false;
    // A leading report ID 0 is not part of the command.
    if (len == kPacketSize + 1 && data[0] == 0x00) {
        ++data;
        --len;
    }
    // Every command starts with 0xFF; anything else is not for us.
    if (len == 0 || data[0] != 0xFF) return;
    onInterruptOut(data, len);
}

void InfinityFunction::onInterruptOut(const uint8_t* data, size_t len) {
    uint8_t packet[INF_PACKET_SIZE] = {0};
    std::memcpy(packet, data, std::min<size_t>(len, INF_PACKET_SIZE));
    inf_base_handle_out(&base_, packet);
}

bool InfinityFunction::peekInput(Report& out) const {
    uint8_t packet[INF_PACKET_SIZE];
    if (!inf_base_peek_in(&base_, packet)) return false;
    std::memcpy(out.data(), packet, INF_PACKET_SIZE);
    return true;
}

}  // namespace inf
}  // namespace sky
