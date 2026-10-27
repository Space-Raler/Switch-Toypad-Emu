// USB HID "function" of the Skylanders Portal of Power: descriptors and
// control-request handling.
//
// Pure C++17, no libnx dependency, so it is unit-tested on a PC. The libnx
// glue (usb_device.cpp) only moves bytes between the USB service and this
// class. Like Portal, this class is NOT thread safe; the caller serialises.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "portal.h"

namespace sky {

// Identity of the real portal.
constexpr uint16_t kVendorId  = 0x1430;
constexpr uint16_t kProductId = 0x0150;
constexpr uint16_t kBcdDevice = 0x0100;
constexpr const char* kManufacturerString = "Activision";
constexpr const char* kProductString      = "Spyro Porta";

constexpr size_t kReportDescriptorSize = 29;
extern const uint8_t kReportDescriptor[kReportDescriptorSize];

enum class Speed { Full, High, Super };

// Interface + HID + endpoint (+ SuperSpeed companion) descriptors for one bus
// speed, as separate chunks in the order they must be appended.
// `intervalOverride` (1..255) replaces the default polling interval when non-zero.
// `outAddressOverride` (non-zero) replaces the OUT endpoint address (default 0x01 + interface).
// `outIntervalOverride` (non-zero) sets the OUT endpoint's interval separately; the
// Flipper reference polls IN slowly (22 ms) but OUT fast (1 ms).
std::vector<std::vector<uint8_t>> interfaceDescriptors(uint8_t interfaceNumber, Speed speed,
                                                       uint8_t intervalOverride = 0,
                                                       uint8_t outAddressOverride = 0,
                                                       uint8_t outIntervalOverride = 0);

// libnx assigns endpoint numbers from the interface number: IN = 0x81 + n,
// OUT = 0x01 + n. (The real portal uses OUT 0x02; hosts read the address from
// the descriptor, so this only matters for hard-coded drivers.)
constexpr uint8_t endpointInAddress(uint8_t interfaceNumber)  { return uint8_t(0x81 + interfaceNumber); }
constexpr uint8_t endpointOutAddress(uint8_t interfaceNumber) { return uint8_t(0x01 + interfaceNumber); }

// The 8-byte USB SETUP packet.
struct SetupPacket {
    uint8_t  bmRequestType = 0;
    uint8_t  bRequest = 0;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;

    static SetupPacket parse(const uint8_t raw[8]);
    bool deviceToHost() const { return (bmRequestType & 0x80) != 0; }
};

// What the glue must do to finish a control transfer.
struct ControlAction {
    enum class Kind {
        Stall,    // unsupported request
        Send,     // device->host data stage: send `data` (already trimmed to wLength)
        Receive,  // host->device data stage: read up to `rxLen` bytes, then call onControlOut()
        Ack,      // no data stage: just complete the status stage
    };
    Kind kind = Kind::Stall;
    std::vector<uint8_t> data;
    size_t rxLen = 0;
};

class HidFunction {
public:
    explicit HidFunction(Portal& portal) : portal_(portal) {}

    void setInterfaceNumber(uint8_t n) { interface_ = n; }

    // Handle a SETUP packet addressed to our interface.
    ControlAction onSetup(const SetupPacket& sp);

    // Data stage of a Receive action finished (`len` bytes received).
    void onControlOut(const uint8_t* data, size_t len);

    // Interrupt OUT endpoint delivered a report.
    void onInterruptOut(const uint8_t* data, size_t len) { portal_.handleOut(data, len); }

    // Next report for the interrupt IN endpoint.
    Report nextInput() { return portal_.nextInReport(); }

    // Bus reset / (re)configuration / disconnect.
    void onBusReset() { portal_.hostReset(); }

private:
    Portal& portal_;
    uint8_t interface_ = 0;
    bool pendingSetReport_ = false;
};

}  // namespace sky
