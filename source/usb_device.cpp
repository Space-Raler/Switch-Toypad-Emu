#include "usb_device.h"

#include "breadcrumb.h"

#include <malloc.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sky {

namespace {

constexpr size_t kPage = 0x1000;
constexpr u64 kCtrlTimeoutNs = 1000000000ULL;  // 1 s for a control data stage
constexpr u64 kPollNs = 50000000ULL;           // wake the worker at least every 50 ms
constexpr u64 kInfPollNs = 4000000ULL;         // Infinity: also notice UI-queued packets within 4 ms
constexpr u32 kResultCancelled = 0x828C;         // a transfer cancelled because the host went away
constexpr u64 kBackoffNs = 100000000ULL;       // pause after a failed transfer post (100 ms)
const char* const kConfigPath = "sdmc:/switch/skyportal/usb.ini";
const char* const kBuildTag = "v17-auto-replug";
constexpr u64 kStatusTimeoutNs = 300000000ULL;  // 0.3 s for a control status stage
constexpr u64 kReplugGapNs = 1500000000ULL;    // how long a manual re-plug stays detached

// Standard SET_ADDRESS/GET_DESCRIPTOR(device/config) are answered by usb:ds from
// the descriptors we register. Requests addressed to our HID interface arrive
// through the interface's SetupEvent.
//
// Control transfers without a data stage (SET_IDLE, SET_PROTOCOL) are completed
// by queueing a zero-length IN transfer on the control endpoint, the same model
// Linux gadget drivers use. This is the least certain part of the USB layer; if
// a host stalls on SET_IDLE, flip this off and watch the debug log.
constexpr bool kZeroLengthStatusForNoData = true;

uint8_t* allocPage() {
    void* p = memalign(kPage, kPage);
    if (p) std::memset(p, 0, kPage);
    return static_cast<uint8_t*>(p);
}

Speed toSpeed(UsbDeviceSpeed s) {
    switch (s) {
        case UsbDeviceSpeed_Full: return Speed::Full;
        case UsbDeviceSpeed_High: return Speed::High;
        default: return Speed::Super;
    }
}

// Tiny key=value reader for usb.ini so USB experiments need no rebuild.
int readIntConfig(const char* path, const char* key, int def) {
    FILE* f = std::fopen(path, "r");
    if (!f) return def;
    char line[128];
    int value = def;
    const size_t klen = std::strlen(key);
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (std::strncmp(line, key, klen) == 0 && line[klen] == '=') value = std::atoi(line + klen + 1);
    }
    std::fclose(f);
    return value;
}

void writeDefaultConfig(const char* path) {
    FILE* probe = std::fopen(path, "r");
    if (probe) {
        std::fclose(probe);
        return;
    }
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fputs("# Skylanders portal USB settings (read at app start; edit, then relaunch).\n"
               "# Polling interval of the IN endpoint (how often the console reads packets), 1..255.\n"
               "# full_interval is in ms at full speed. high_interval is an exponent at high/super speed\n"
               "# (2^(n-1) x 125 us, 1..16): 8 = 16 ms. The working Flipper build polls IN every 22 ms;\n"
               "# polling at 1 ms makes games miss command replies.\n"
               "full_interval=22\n"
               "high_interval=8\n"
               "# Polling interval of the OUT endpoint (same units). The real portal uses 1 ms.\n"
               "out_full_interval=1\n"
               "out_high_interval=4\n"
               "# OUT endpoint number (1..15). The real portal uses 2; try 1 if the app reports USB failed.\n"
               "out_endpoint=2\n"
               "# Highest bus speed to offer: 0 = full (real portal), 1 = up to high, 2 = up to super.\n"
               "max_speed=2\n"
               "# 1 = look like a plain USB 1.1 device (bcdUSB 1.10, no BOS), like the real portal.\n"
               "usb11=0\n"
               "# 1 = report the manufacturer/product strings (real portal), 0 = none (the Flipper build).\n"
               "strings=1\n"
               "# 1 = finish every control transfer with an explicit status stage (zero-length OUT after a\n"
               "# data-in reply such as the report descriptor, zero-length IN after SET_REPORT). 0 = v9 behaviour.\n"
               "ctrl_status=1\n"
               "# --- Disney Infinity mode (used only while the app is in Infinity mode) ---\n"
               "# Polling interval of both endpoints (same units as above). The Flipper build uses 1 ms.\n"
               "inf_full_interval=1\n"
               "inf_high_interval=4\n"
               "# OUT endpoint number, 1..15 (the Flipper build uses 1).\n"
               "inf_out_endpoint=1\n"
               "# 1 = bcdUSB 1.10 and no BOS. 0 = USB 2.0/3.0 like the Flipper build.\n"
               "inf_usb11=0\n"
               "# 1 = report made-up manufacturer/product strings; 0 = none (the Flipper build).\n"
               "inf_strings=0\n"
               "# --- LEGO Dimensions ToyPad mode ---\n"
               "lego_full_interval=1\nlego_high_interval=4\nlego_out_endpoint=1\nlego_usb11=0\nlego_strings=1\n"
               "# --- all modes: re-plug automatically while the console has not started using the device ---\n"
               "# 1 = if the console is linked but the game has not talked to us after auto_replug_after\n"
               "# seconds, detach and re-attach so a game that only notices new devices sees one.\n"
               "auto_replug=1\nauto_replug_after=8\n",
               f);
    std::fclose(f);
}

const char* stateName(UsbState s) {
    switch (s) {
        case UsbState_Detached: return "Detached";
        case UsbState_Attached: return "Attached";
        case UsbState_Powered: return "Powered";
        case UsbState_Default: return "Reset";
        case UsbState_Address: return "Addressed";
        case UsbState_Configured: return "Configured";
        case UsbState_Suspended: return "Suspended";
    }
    return "?";
}

bool isNotFound(Result rc) { return rc == MAKERESULT(Module_Libnx, LibnxError_NotFound); }

}  // namespace

void UsbDevice::note(const char* fmt, ...) {
    char line[72];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    ScopedLock lock(diagMutex_);
    for (int i = 0; i + 1 < UsbDiag::kLogLines; ++i)
        std::memcpy(diag_.log[i], diag_.log[i + 1], sizeof diag_.log[i]);
    std::memcpy(diag_.log[UsbDiag::kLogLines - 1], line, sizeof line);
}

void UsbDevice::countError(Result rc) {
    ScopedLock lock(diagMutex_);
    diag_.errors++;
    diag_.lastError = rc;
}

void UsbDevice::noteError(Result rc, const char* what) {
    countError(rc);
    note("ERR %s: 0x%X", what, rc);
}

UsbDiag UsbDevice::diag() {
    ScopedLock lock(diagMutex_);
    return diag_;
}

Result UsbDevice::setupDevice(bool safeProfile) {
    Result rc = 0;

    writeDefaultConfig(kConfigPath);
    const char* cfg = kConfigPath;
    // The safe profile ignores usb.ini entirely: it is the fallback when the requested
    // profile could not even be registered with usb:ds.
    auto readInt = [&](const char* key, int def) { return safeProfile ? def : readIntConfig(cfg, key, def); };
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

    const bool infMode = mode_ == Mode::Infinity;
    const bool legoMode = mode_ == Mode::LegoDimensions;
    const bool specialMode = infMode || legoMode;
    // Skylanders: IN polled slowly (22 ms), OUT fast (1 ms). Infinity: both 1 ms, OUT endpoint 1,
    // like the Flipper build (the keys are separate so one mode's experiments leave the other alone).
    const int inFull = specialMode ? clamp(readInt(legoMode ? "lego_full_interval" : "inf_full_interval", 1), 1, 255) : clamp(readInt("full_interval", 22), 1, 255);
    const int inHigh = specialMode ? clamp(readInt(legoMode ? "lego_high_interval" : "inf_high_interval", 4), 1, 16) : clamp(readInt("high_interval", 8), 1, 16);
    const int outFull = specialMode ? inFull : clamp(readInt("out_full_interval", 1), 1, 255);
    const int outHigh = specialMode ? inHigh : clamp(readInt("out_high_interval", 4), 1, 16);
    const int outEp = specialMode ? clamp(readInt(legoMode ? "lego_out_endpoint" : "inf_out_endpoint", 1), 1, 15) : clamp(readInt("out_endpoint", 2), 1, 15);
    // usb:ds cannot be held at full speed: the Switch negotiates high speed on its own, and a
    // device that registered only full-speed descriptors then has no endpoints at all (every
    // transfer post fails, the host never sends a request). So every speed is always registered
    // and a lower max_speed in usb.ini is ignored (and said so in the log).
    const int maxSpeedAsked = clamp(readInt("max_speed", 2), 0, 2);
    const int maxSpeed = 2;
    const bool usb11 = specialMode ? readInt(legoMode ? "lego_usb11" : "inf_usb11", 0) != 0 : readInt("usb11", 0) != 0;
    const bool strings = specialMode ? (legoMode ? readInt("lego_strings", 1) : readInt("inf_strings", 0)) != 0 : readInt("strings", 1) != 0;
    ctrlStatus_ = readInt("ctrl_status", 1) != 0;
    autoReplug_ = readInt("auto_replug", 1) != 0;
    autoReplugMs_ = uint32_t(clamp(readInt("auto_replug_after", 8), 3, 120)) * 1000u;
    note("skyportal %s %s%s", kBuildTag, modeName(mode_), safeProfile ? " (safe fallback profile)" : "");
    if (maxSpeedAsked < 2) note("max_speed=%d ignored (usb:ds needs every speed)", maxSpeedAsked);
    note("usb.ini: in %d/%d out %d/%d ep 0x%02X maxspeed %d usb11 %d str %d cs %d", inFull, inHigh, outFull,
         outHigh, outEp, maxSpeed, usb11 ? 1 : 0, strings ? 1 : 0, ctrlStatus_ ? 1 : 0);
    {
        ScopedLock lock(diagMutex_);
        std::snprintf(diag_.config, sizeof diag_.config,
                      "%s %s %s: in %d/%d out %d/%d ep %d maxspeed %d usb11 %d str %d cs %d", kBuildTag,
                      legoMode ? "LEGO" : infMode ? "Infinity" : "Skylanders", safeProfile ? "SAFE DEFAULTS" : "usb.ini", inFull, inHigh, outFull, outHigh, outEp, maxSpeed,
                      usb11 ? 1 : 0, strings ? 1 : 0, ctrlStatus_ ? 1 : 0);
    }

    static const u16 langs[1] = {0x0409};
    u8 iManufacturer = 0, iProduct = 0;
    if (strings) {
        rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
        if (R_SUCCEEDED(rc))
            rc = usbDsAddUsbStringDescriptor(&iManufacturer, legoMode ? lego::kManufacturerString : infMode ? inf::kManufacturerString : kManufacturerString);
        if (R_SUCCEEDED(rc))
            rc = usbDsAddUsbStringDescriptor(&iProduct, legoMode ? lego::kProductString : infMode ? inf::kProductString : kProductString);
        if (R_FAILED(rc)) return rc;
    }

    usb_device_descriptor dd;
    std::memset(&dd, 0, sizeof dd);
    dd.bLength = USB_DT_DEVICE_SIZE;
    dd.bDescriptorType = USB_DT_DEVICE;
    dd.bcdUSB = 0x0110;
    dd.bMaxPacketSize0 = 0x40;
    dd.idVendor = legoMode ? lego::kVendorId : infMode ? inf::kVendorId : kVendorId;
    dd.idProduct = legoMode ? lego::kProductId : infMode ? inf::kProductId : kProductId;
    dd.bcdDevice = legoMode ? lego::kBcdDevice : infMode ? inf::kBcdDevice : kBcdDevice;
    dd.iManufacturer = iManufacturer;
    dd.iProduct = iProduct;
    dd.iSerialNumber = 0;  // neither the real portal nor the Infinity base has a serial number
    dd.bNumConfigurations = 1;

    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dd);
    if (R_SUCCEEDED(rc) && maxSpeed >= 1) {
        dd.bcdUSB = usb11 ? 0x0110 : 0x0200;
        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dd);
    }
    if (R_SUCCEEDED(rc) && maxSpeed >= 2) {
        dd.bcdUSB = usb11 ? 0x0110 : 0x0300;
        dd.bMaxPacketSize0 = 0x09;  // 2^9 = 512
        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dd);
    }
    if (R_FAILED(rc)) return rc;

    // Binary Object Store, same as libnx's own usb_comms.
    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    if (!usb11) {  // a real portal is USB 1.1 and has no BOS
        rc = usbDsSetBinaryObjectStore(bos, sizeof bos);
        if (R_FAILED(rc)) return rc;
    }

    rc = usbDsRegisterInterface(&iface_);
    if (R_FAILED(rc)) return rc;
    const u8 ifn = iface_->interface_index;
    hid_.setInterfaceNumber(ifn);
    inf_.setInterfaceNumber(ifn);
    lego_.setInterfaceNumber(ifn);

    const UsbDeviceSpeed speeds[] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    for (UsbDeviceSpeed sp : speeds) {
        if ((sp == UsbDeviceSpeed_High && maxSpeed < 1) || (sp == UsbDeviceSpeed_Super && maxSpeed < 2)) continue;
        const bool full = sp == UsbDeviceSpeed_Full;
        const uint8_t interval = uint8_t(full ? inFull : inHigh);
        const uint8_t outInterval = uint8_t(full ? outFull : outHigh);
        const auto chunks = legoMode ? lego::interfaceDescriptors(ifn, toSpeed(sp), interval, uint8_t(outEp))
                           : infMode ? inf::interfaceDescriptors(ifn, toSpeed(sp), interval, uint8_t(outEp))
                                     : interfaceDescriptors(ifn, toSpeed(sp), interval, uint8_t(outEp), outInterval);
        for (const auto& chunk : chunks) {
            rc = usbDsInterface_AppendConfigurationData(iface_, sp, chunk.data(), chunk.size());
            if (R_FAILED(rc)) return rc;
        }
    }

    rc = usbDsInterface_RegisterEndpoint(iface_, &epIn_, legoMode ? lego::endpointInAddress(ifn) : infMode ? inf::endpointInAddress(ifn) : endpointInAddress(ifn));
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_RegisterEndpoint(iface_, &epOut_, uint8_t(outEp));
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(iface_);
    if (R_FAILED(rc)) return rc;

    return usbDsEnable();
}

Result UsbDevice::bringUp(bool safeProfile) {
    crumb("usb: usbDsInitialize (%s profile)", safeProfile ? "safe" : "usb.ini");
    Result rc = usbDsInitialize();
    crumb("usb: usbDsInitialize rc=0x%X", rc);
    if (R_FAILED(rc)) return rc;
    usbInitialized_ = true;

    crumb("usb: setupDevice");
    rc = setupDevice(safeProfile);
    crumb("usb: setupDevice rc=0x%X", rc);
    if (R_FAILED(rc)) {
        usbDsExit();
        usbInitialized_ = false;
        iface_ = nullptr;
        epIn_ = epOut_ = nullptr;
    }
    return rc;
}

Result UsbDevice::start(Mode mode) {
    if (!hosversionAtLeast(5, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    // A fresh start (the first one, or the other mode after stop()): forget the last run.
    mode_ = mode;
    configured_ = false;
    inPending_ = outPending_ = false;
    lastState_ = UsbState_Detached;
    postFails_ = 0;
    reconnectRequested_ = false;
    waiting_ = false;
    waitStartMs_ = replugTries_ = graceUntilMs_ = 0;
    {
        ScopedLock lock(diagMutex_);
        diag_ = UsbDiag();
        diag_.mode = mode;
    }
    {
        ScopedLock lock(shared_.mutex);
        hid_.onBusReset();
        inf_.onBusReset();
        lego_.onBusReset();
    }

    crumb("usb: start mode %d, allocating buffers", int(mode));
    inBuf_ = allocPage();
    outBuf_ = allocPage();
    ctrlBuf_ = allocPage();
    setupBuf_ = allocPage();
    if (!inBuf_ || !outBuf_ || !ctrlBuf_ || !setupBuf_) {
        stop();
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    startTick_ = armGetSystemTick();

    Result rc = bringUp(false);
    if (R_FAILED(rc)) {
        // usb.ini asked for something usb:ds refused (for example a full-speed-only
        // setup). Try again with the plain defaults so the app still comes up, and say so.
        const Result firstFailure = rc;
        rc = bringUp(true);
        if (R_SUCCEEDED(rc)) note("usb.ini profile failed (0x%X), using defaults", firstFailure);
    }
    if (R_FAILED(rc)) {
        stop();
        return rc;
    }

    stopRequested_ = false;
    crumb("usb: starting worker thread");
    rc = threadCreate(&thread_, &UsbDevice::threadEntry, this, nullptr, 0x8000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&thread_);
    crumb("usb: worker thread rc=0x%X", rc);
    if (R_FAILED(rc)) {
        stop();
        return rc;
    }
    threadStarted_ = true;
    note("USB device registered, waiting for host");
    return 0;
}

void UsbDevice::stop() {
    if (threadStarted_) {
        stopRequested_ = true;
        threadWaitForExit(&thread_);
        threadClose(&thread_);
        threadStarted_ = false;
    }
    if (usbInitialized_) {
        usbDsExit();
        usbInitialized_ = false;
    }
    iface_ = nullptr;
    epIn_ = epOut_ = nullptr;
    free(inBuf_);
    free(outBuf_);
    free(ctrlBuf_);
    free(setupBuf_);
    inBuf_ = outBuf_ = ctrlBuf_ = setupBuf_ = nullptr;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void UsbDevice::run() {
    crumb("usb: worker running");
    while (!stopRequested_) {
        UsbState state = UsbState_Detached;
        if (R_FAILED(usbDsGetState(&state))) state = UsbState_Detached;

        if (reconnectRequested_.exchange(false)) {
            doReconnect();
            continue;
        }

        if (state != lastState_) {
            lastState_ = state;
            {
                ScopedLock lock(diagMutex_);
                diag_.stateChanges++;
            }
            note("%ums state: %s", sinceMs(), stateName(state));
        }

        const bool nowConfigured = (state == UsbState_Configured);
        if (nowConfigured && !configured_) onConfigured();
        if (!nowConfigured && configured_) onDeconfigured();

        if (configured_) {
            // Failed posts back off briefly so a persistent error cannot spin the CPU.
            if (!outPending_ && !postOut()) svcSleepThread(kBackoffNs);
            if (!inPending_ && !postIn()) svcSleepThread(kBackoffNs);
        }

        {
            ScopedLock lock(diagMutex_);
            diag_.state = state;
            UsbDeviceSpeed speed = UsbDeviceSpeed_None;
            if (nowConfigured && R_SUCCEEDED(usbDsGetSpeed(&speed))) diag_.speed = speed;
            if (!nowConfigured) diag_.speed = UsbDeviceSpeed_None;
        }

        watchdog(state, nowConfigured);

        s32 idx = 0;
        waitMulti(&idx, (mode_ == Mode::Infinity || mode_ == Mode::LegoDimensions) ? kInfPollNs : kPollNs, waiterForEvent(usbDsGetStateChangeEvent()), waiterForEvent(&iface_->SetupEvent),
                  waiterForEvent(&epOut_->CompletionEvent), waiterForEvent(&epIn_->CompletionEvent));

        // Events are manual-reset: poll each one, clear it, and handle it.
        if (R_SUCCEEDED(eventWait(usbDsGetStateChangeEvent(), 0))) eventClear(usbDsGetStateChangeEvent());
        if (R_SUCCEEDED(eventWait(&iface_->SetupEvent, 0))) {
            eventClear(&iface_->SetupEvent);
            {
                ScopedLock lock(diagMutex_);
                diag_.setupEvents++;
            }
            handleSetup();
        }
        if (R_SUCCEEDED(eventWait(&epOut_->CompletionEvent, 0))) {
            eventClear(&epOut_->CompletionEvent);
            {
                ScopedLock lock(diagMutex_);
                diag_.outEvents++;
            }
            completeOut();
        }
        if (R_SUCCEEDED(eventWait(&epIn_->CompletionEvent, 0))) {
            eventClear(&epIn_->CompletionEvent);
            {
                ScopedLock lock(diagMutex_);
                diag_.inEvents++;
            }
            completeIn();
        }
    }

    if (configured_) onDeconfigured();
}

uint32_t UsbDevice::sinceMs() const {
    return uint32_t(armTicksToNs(armGetSystemTick() - startTick_) / 1000000ULL);
}

// Detach and re-attach so the console sees the device arrive again. Games that look for
// the portal only when a USB device appears can miss the first arrival (for example
// while the console is still setting up the port); a fresh plug-in is the cheap fix.
void UsbDevice::doReconnect() {
    note("%ums re-plug: detaching", sinceMs());
    if (configured_) onDeconfigured();
    Result rc = usbDsDisable();
    if (R_FAILED(rc)) {
        noteError(rc, "usbDsDisable");
        return;
    }
    svcSleepThread(kReplugGapNs);
    rc = usbDsEnable();
    if (R_FAILED(rc)) {
        noteError(rc, "usbDsEnable");
        return;
    }
    {
        ScopedLock lock(diagMutex_);
        diag_.reconnects++;
    }
    note("%ums re-plug: attached again", sinceMs());
    // The detach we just did is not a real unplug (see watchdog()).
    waiting_ = false;
    graceUntilMs_ = sinceMs() + 3000u;
}

// Has the game on the console started using us on this connection?
bool UsbDevice::gameActive() {
    ScopedLock lock(shared_.mutex);
    if (mode_ == Mode::Infinity) return shared_.inf.handshake_seen;
    if (mode_ == Mode::LegoDimensions) return lego_.hostActive();
    return shared_.portal.activated();
}

// Re-plug watchdog. Some games only notice a device when it arrives, so a game that is started (or
// reaches its "connect your portal" screen) after we were linked would never see us until the cable is
// moved. While the console is linked but the game has not talked to us, or enumeration is stuck, this
// detaches and re-attaches every autoReplugMs_ (every 20 s after the first three tries). It stops as
// soon as the game talks to us, and starts over after a real unplug.
void UsbDevice::watchdog(UsbState state, bool nowConfigured) {
    if (!autoReplug_ || reconnectRequested_) return;
    const uint32_t now = sinceMs();
    if (state == UsbState_Detached) {
        // Our own re-plug passes through Detached; only a longer absence is a real unplug.
        if ((int32_t)(now - graceUntilMs_) >= 0) {
            waiting_ = false;
            replugTries_ = 0;
        }
        return;
    }
    if (nowConfigured && gameActive()) {
        waiting_ = false;
        replugTries_ = 0;
        return;
    }
    if (!waiting_) {
        waiting_ = true;
        waitStartMs_ = now;
        return;
    }
    // Stuck mid-enumeration gets a shorter fuse than "linked but unused".
    const uint32_t limit = !nowConfigured ? 6000u : (replugTries_ < 3 ? autoReplugMs_ : 20000u);
    if (now - waitStartMs_ < limit) return;
    waiting_ = false;
    ++replugTries_;
    note("%ums auto re-plug #%u", now, replugTries_);
    doReconnect();
}

void UsbDevice::onConfigured() {
    configured_ = true;
    inPending_ = outPending_ = false;
    {
        ScopedLock lock(shared_.mutex);
        if (mode_ == Mode::Infinity) inf_.onBusReset();
        else if (mode_ == Mode::LegoDimensions) lego_.onBusReset();
        else hid_.onBusReset();
    }
    {
        ScopedLock lock(diagMutex_);
        diag_.configurations++;
    }
    UsbDeviceSpeed speed = UsbDeviceSpeed_None;
    usbDsGetSpeed(&speed);
    const char* name = speed == UsbDeviceSpeed_Full ? "full" : speed == UsbDeviceSpeed_High ? "high"
                       : speed == UsbDeviceSpeed_Super ? "super" : "?";
    note("host configured the device (%s speed)", name);
}

void UsbDevice::onDeconfigured() {
    configured_ = false;
    if (inPending_) usbDsEndpoint_Cancel(epIn_);
    if (outPending_) usbDsEndpoint_Cancel(epOut_);
    inPending_ = outPending_ = false;
    {
        ScopedLock lock(shared_.mutex);
        if (mode_ == Mode::Infinity) inf_.onBusReset();
        else if (mode_ == Mode::LegoDimensions) lego_.onBusReset();
        else hid_.onBusReset();
    }
    note("host disconnected");
}

bool UsbDevice::postIn() {
    const bool infinity = mode_ == Mode::Infinity;
    const bool legoMode = mode_ == Mode::LegoDimensions;
    {
        ScopedLock lock(shared_.mutex);
        if (infinity || legoMode) {
            // The Infinity base and LEGO ToyPad only send when they have something: a figure event or a reply.
            // (The portal, by contrast, always has a status report to send.)
            Report r;
            if (infinity) { if (!inf_.peekInput(r)) return true; }
            else if (!lego_.peekInput(r)) return true;
            std::memcpy(inBuf_, r.data(), r.size());
        } else {
            const Report r = hid_.nextInput();
            std::memcpy(inBuf_, r.data(), r.size());
        }
    }
    u32 urb = 0;
    const Result rc = usbDsEndpoint_PostBufferAsync(epIn_, inBuf_, kReportSize, &urb);
    if (R_FAILED(rc)) {
        countError(rc);
        if (++postFails_ <= 3 || postFails_ % 500 == 0)
            note("post IN failed: 0x%X (%u so far)", rc, postFails_);
        return false;
    }
    inUrb_ = urb;
    inPending_ = true;
    if (infinity || legoMode) {
        // The packet is on its way: take it off the queue. If the link drops before the host
        // reads it, the queue is cleared by the bus reset anyway.
        ScopedLock lock(shared_.mutex);
        if (infinity) inf_.dropInput(); else lego_.dropInput();
    }
    return true;
}

bool UsbDevice::postOut() {
    std::memset(outBuf_, 0, kReportSize);
    u32 urb = 0;
    const Result rc = usbDsEndpoint_PostBufferAsync(epOut_, outBuf_, kReportSize, &urb);
    if (R_FAILED(rc)) {
        countError(rc);
        if (++postFails_ <= 3 || postFails_ % 500 == 0)
            note("post OUT failed: 0x%X (%u so far)", rc, postFails_);
        return false;
    }
    outUrb_ = urb;
    outPending_ = true;
    return true;
}

void UsbDevice::completeIn() {
    UsbDsReportData rd;
    Result rc = usbDsEndpoint_GetReportData(epIn_, &rd);
    if (R_FAILED(rc)) {
        noteError(rc, "IN report");
        inPending_ = false;
        return;
    }
    if (!inPending_) return;  // late completion of a cancelled transfer
    u32 sent = 0;
    rc = usbDsParseReportData(&rd, inUrb_, nullptr, &sent);
    if (isNotFound(rc)) return;  // our transfer is still in flight
    inPending_ = false;
    if (R_FAILED(rc)) {
        // The console dropping the connection cancels the transfer: expected, not an error.
        if (u32(rc) != kResultCancelled) noteError(rc, "IN transfer");
        return;
    }
    uint32_t n;
    {
        ScopedLock lock(diagMutex_);
        n = ++diag_.inCompleted;
    }
    if (n <= 5) note("IN #%u done: %u bytes, first %02X", n, sent, inBuf_[0]);
}

void UsbDevice::completeOut() {
    UsbDsReportData rd;
    Result rc = usbDsEndpoint_GetReportData(epOut_, &rd);
    if (R_FAILED(rc)) {
        noteError(rc, "OUT report");
        outPending_ = false;
        return;
    }
    if (!outPending_) return;
    u32 got = 0;
    rc = usbDsParseReportData(&rd, outUrb_, nullptr, &got);
    if (isNotFound(rc)) return;
    outPending_ = false;
    if (R_FAILED(rc)) {
        // The console dropping the connection cancels the transfer: expected, not an error.
        if (u32(rc) != kResultCancelled) noteError(rc, "OUT transfer");
        return;
    }
    if (got > kReportSize) got = kReportSize;
    if (got > 0) {
        ScopedLock lock(shared_.mutex);
        if (mode_ == Mode::Infinity) inf_.onInterruptOut(outBuf_, got);
        else if (mode_ == Mode::LegoDimensions) lego_.onInterruptOut(outBuf_, got);
        else hid_.onInterruptOut(outBuf_, got);
    }
    uint32_t n;
    {
        ScopedLock lock(diagMutex_);
        n = ++diag_.outCompleted;
    }
    if (n <= 12) note("OUT #%u: %u bytes: %02X %02X %02X %02X", n, got, outBuf_[0], outBuf_[1], outBuf_[2], outBuf_[3]);
}

// ---------------------------------------------------------------------------
// Control endpoint
// ---------------------------------------------------------------------------

bool UsbDevice::controlIn(const uint8_t* data, size_t len) {
    if (len > kPage) len = kPage;
    if (len) std::memcpy(ctrlBuf_, data, len);

    u32 urb = 0;
    Result rc = usbDsInterface_CtrlInPostBufferAsync(iface_, ctrlBuf_, len, &urb);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl IN post");
        return false;
    }
    rc = eventWait(&iface_->CtrlInCompletionEvent, kCtrlTimeoutNs);
    eventClear(&iface_->CtrlInCompletionEvent);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl IN timeout");
        usbDsInterface_StallCtrl(iface_);
        return false;
    }
    UsbDsReportData rd;
    rc = usbDsInterface_GetCtrlInReportData(iface_, &rd);
    if (R_SUCCEEDED(rc)) rc = usbDsParseReportData(&rd, urb, nullptr, nullptr);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl IN result");
        return false;
    }
    return true;
}

bool UsbDevice::controlOut(size_t len, size_t& received) {
    received = 0;
    if (len > kPage) len = kPage;
    std::memset(ctrlBuf_, 0, len);

    u32 urb = 0;
    Result rc = usbDsInterface_CtrlOutPostBufferAsync(iface_, ctrlBuf_, len, &urb);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl OUT post");
        return false;
    }
    rc = eventWait(&iface_->CtrlOutCompletionEvent, kCtrlTimeoutNs);
    eventClear(&iface_->CtrlOutCompletionEvent);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl OUT timeout");
        usbDsInterface_StallCtrl(iface_);
        return false;
    }
    UsbDsReportData rd;
    u32 got = 0;
    rc = usbDsInterface_GetCtrlOutReportData(iface_, &rd);
    if (R_SUCCEEDED(rc)) rc = usbDsParseReportData(&rd, urb, nullptr, &got);
    if (R_FAILED(rc)) {
        noteError(rc, "ctrl OUT result");
        return false;
    }
    received = got > len ? len : got;
    return true;
}

// Status stage of a control transfer. usb:ds exposes the control pipe as two plain endpoint-0
// transfers (CtrlIn = 0x80, CtrlOut = 0x00) and nothing in the API says the service completes the
// status stage by itself. Symptom of a missing one: the host sees the data stage arrive, then its
// zero-length status packet is never answered and the request times out (Linux: usbhid
// "can't add hid device: -110", the PS5 re-reading the report descriptor three times).
// After a device->host data stage the host sends a zero-length OUT; after a host->device data
// stage it expects a zero-length IN. Returns false (and logs) if it did not complete.
bool UsbDevice::statusStage(bool zeroLengthIn) {
    u32 urb = 0;
    Event* ev = zeroLengthIn ? &iface_->CtrlInCompletionEvent : &iface_->CtrlOutCompletionEvent;
    Result rc = zeroLengthIn ? usbDsInterface_CtrlInPostBufferAsync(iface_, ctrlBuf_, 0, &urb)
                             : usbDsInterface_CtrlOutPostBufferAsync(iface_, ctrlBuf_, 0, &urb);
    if (R_FAILED(rc)) {
        noteError(rc, zeroLengthIn ? "status IN post" : "status OUT post");
        return false;
    }
    rc = eventWait(ev, kStatusTimeoutNs);
    eventClear(ev);
    if (R_FAILED(rc)) {
        note("%s status: no completion", zeroLengthIn ? "IN" : "OUT");
        countError(rc);
        return false;
    }
    UsbDsReportData rd;
    rc = zeroLengthIn ? usbDsInterface_GetCtrlInReportData(iface_, &rd)
                      : usbDsInterface_GetCtrlOutReportData(iface_, &rd);
    if (R_SUCCEEDED(rc)) rc = usbDsParseReportData(&rd, urb, nullptr, nullptr);
    if (R_FAILED(rc)) {
        noteError(rc, zeroLengthIn ? "status IN result" : "status OUT result");
        return false;
    }
    return true;
}

void UsbDevice::handleSetup() {
    std::memset(setupBuf_, 0, 8);
    const Result rc = usbDsInterface_GetSetupPacket(iface_, setupBuf_, 8);
    if (R_FAILED(rc)) {
        noteError(rc, "get setup");
        return;
    }
    const SetupPacket sp = SetupPacket::parse(setupBuf_);
    {
        ScopedLock lock(diagMutex_);
        diag_.setupRequests++;
    }

    ControlAction action;
    {
        ScopedLock lock(shared_.mutex);
        action = mode_ == Mode::Infinity ? inf_.onSetup(sp) : mode_ == Mode::LegoDimensions ? lego_.onSetup(sp) : hid_.onSetup(sp);
    }

    const char* outcome = "?";
    switch (action.kind) {
        case ControlAction::Kind::Stall:
            usbDsInterface_StallCtrl(iface_);
            outcome = "stall";
            break;

        case ControlAction::Kind::Send:
            if (!controlIn(action.data.data(), action.data.size())) {
                outcome = "send failed";
            } else if (ctrlStatus_ && !action.data.empty()) {
                outcome = statusStage(false) ? "sent+status" : "sent, status FAILED";
            } else {
                outcome = "sent";
            }
            break;

        case ControlAction::Kind::Receive: {
            size_t got = 0;
            if (controlOut(action.rxLen, got)) {
                {
                    ScopedLock lock(shared_.mutex);
                    if (mode_ == Mode::Infinity) inf_.onControlOut(ctrlBuf_, got);
                    else if (mode_ == Mode::LegoDimensions) lego_.onControlOut(ctrlBuf_, got);
                    else hid_.onControlOut(ctrlBuf_, got);
                }
                if (ctrlStatus_) outcome = statusStage(true) ? "received+status" : "received, status FAILED";
                else outcome = "received";
            } else {
                outcome = "recv failed";
            }
            break;
        }

        case ControlAction::Kind::Ack:
            if (kZeroLengthStatusForNoData) {
                outcome = controlIn(nullptr, 0) ? "acked" : "ack failed";
            } else {
                outcome = "acked (no ZLP)";
            }
            break;
    }

    note("SETUP %02X %02X v%04X i%04X l%u -> %s", sp.bmRequestType, sp.bRequest, sp.wValue, sp.wIndex,
         sp.wLength, outcome);
}

}  // namespace sky
