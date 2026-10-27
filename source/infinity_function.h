// USB HID "function" of the Disney Infinity base: descriptors and control-request
// handling. The command protocol itself lives in the C core (infinity/infinity_core.*).
//
// Pure C++17, no libnx dependency, so it is unit-tested on a PC. Like HidFunction this
// class is NOT thread safe; the caller serialises (usb_device.cpp holds Shared::mutex).
//
// Identity and descriptors follow RPCS3's emulated base and the FlipBase Flipper app
// (VID 0E6F, PID 0129, one HID interface, 32-byte interrupt IN 0x81 and OUT 0x01).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hid_function.h"  // SetupPacket, ControlAction, Speed
#include "infinity/infinity_core.h"

namespace sky {
namespace inf {

constexpr uint16_t kVendorId  = 0x0E6F;
constexpr uint16_t kProductId = 0x0129;
constexpr uint16_t kBcdDevice = 0x0200;
// Only used when inf_strings=1 in usb.ini; the Flipper build has no strings.
constexpr const char* kManufacturerString = "Disney Interactive";
constexpr const char* kProductString      = "Infinity Base";

constexpr size_t kPacketSize = INF_PACKET_SIZE;  // 32

constexpr size_t kReportDescriptorSize = 29;
extern const uint8_t kReportDescriptor[kReportDescriptorSize];

// libnx numbers endpoints from the interface number: IN = 0x81 + n, OUT = 0x01 + n.
constexpr uint8_t endpointInAddress(uint8_t interfaceNumber)  { return uint8_t(0x81 + interfaceNumber); }
constexpr uint8_t endpointOutAddress(uint8_t interfaceNumber) { return uint8_t(0x01 + interfaceNumber); }

// Interface + HID + endpoint (+ SuperSpeed companion) descriptors for one bus speed, as
// separate chunks in the order they must be appended. `interval` (1..255) applies to
// both endpoints when non-zero (default: 1 ms, which is 4 at high/super speed).
// `outAddressOverride` (non-zero) replaces the OUT endpoint address.
std::vector<std::vector<uint8_t>> interfaceDescriptors(uint8_t interfaceNumber, Speed speed,
                                                       uint8_t interval = 0,
                                                       uint8_t outAddressOverride = 0);

class InfinityFunction {
public:
    explicit InfinityFunction(InfBase& base) : base_(base) {}

    void setInterfaceNumber(uint8_t n) { interface_ = n; }

    // Handle a SETUP packet addressed to our interface.
    ControlAction onSetup(const SetupPacket& sp);

    // Data stage of a Receive action finished (`len` bytes received).
    void onControlOut(const uint8_t* data, size_t len);

    // Interrupt OUT endpoint delivered a command (short packets are zero padded).
    void onInterruptOut(const uint8_t* data, size_t len);

    // Next packet for the interrupt IN endpoint, without removing it. The USB layer posts
    // it and calls dropInput() only when the post was accepted.
    bool peekInput(Report& out) const;
    void dropInput() { inf_base_drop_in(&base_); }

    // Bus reset / (re)configuration / disconnect: drop queued packets and the handshake,
    // keep the figures.
    void onBusReset() { inf_base_reset_link(&base_); }

private:
    InfBase& base_;
    uint8_t interface_ = 0;
    bool pendingSetReport_ = false;
};

}  // namespace inf
}  // namespace sky
