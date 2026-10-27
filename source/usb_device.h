// libnx USB device layer: makes the Switch enumerate as a Skylanders portal.
//
// A worker thread owns all usb:ds calls. It keeps one interrupt-IN transfer
// and one interrupt-OUT transfer posted while the host has us configured, and
// answers HID control requests (report descriptor, SET_REPORT, ...). All
// protocol logic lives in HidFunction/Portal; this file only moves bytes.
#pragma once

#include <switch.h>

#include <atomic>
#include <cstdint>

#include "hid_function.h"
#include "infinity/infinity_core.h"
#include "infinity_function.h"
#include "lego_dimensions_function.h"
#include "portal.h"

namespace sky {

// What the Switch pretends to be on the USB port. It is chosen when the USB device is
// registered, so changing it means stop() and start() again (the console sees an unplug
// and a plug-in).
enum class Mode { Skylanders = 0, Infinity = 1, LegoDimensions = 2 };

inline const char* modeName(Mode m) { return m == Mode::Infinity ? "Disney Infinity" : m == Mode::LegoDimensions ? "LEGO Dimensions" : "Skylanders"; }

// State shared between the UI thread and the USB thread. Both emulated devices live here
// (so figures stay put when the mode is switched); only the active one is reachable
// from the console.
struct Shared {
    Shared() { inf_base_init(&inf); }
    Portal portal;
    InfBase inf;
    lego::ToyPadFunction lego;
    Mutex mutex{};
};

class ScopedLock {
public:
    explicit ScopedLock(Mutex& m) : m_(&m) { mutexLock(m_); }
    ~ScopedLock() { mutexUnlock(m_); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    Mutex* m_;
};

// Snapshot of what the USB layer is doing, for the on-screen debug view.
struct UsbDiag {
    static constexpr int kLogLines = 24;
    UsbState state = UsbState_Detached;
    UsbDeviceSpeed speed = UsbDeviceSpeed_None;
    Mode mode = Mode::Skylanders;
    uint32_t configurations = 0;  // times the host configured us
    uint32_t setupRequests = 0;
    uint32_t inCompleted = 0;
    uint32_t outCompleted = 0;
    uint32_t errors = 0;
    Result lastError = 0;
    uint32_t stateChanges = 0;   // usb:ds state transitions seen (enumeration activity)
    uint32_t setupEvents = 0;    // times the interface SetupEvent fired
    uint32_t inEvents = 0;       // IN endpoint completion events (even if not ours)
    uint32_t outEvents = 0;      // OUT endpoint completion events
    uint32_t reconnects = 0;     // manual re-plugs done from the debug page
    char config[96] = {};        // the settings actually in effect (pinned on the debug page)
    char log[kLogLines][72] = {};  // oldest first
};

class UsbDevice {
public:
    explicit UsbDevice(Shared& shared) : shared_(shared), hid_(shared.portal), inf_(shared.inf), lego_(shared.lego) {}
    ~UsbDevice() { stop(); }

    // Registers the device with usb:ds and starts the worker thread. Can be called again
    // after stop() to come back as the other device.
    Result start(Mode mode = Mode::Skylanders);
    void stop();
    Mode mode() const { return mode_; }

    UsbDiag diag();

    // Ask the worker to detach and re-attach the USB device, so the console sees a
    // fresh plug-in without touching the cable. Safe to call from the UI thread.
    void requestReconnect() { reconnectRequested_ = true; }

private:
    static void threadEntry(void* self) { static_cast<UsbDevice*>(self)->run(); }
    void run();

    Result setupDevice(bool safeProfile);
    Result bringUp(bool safeProfile);
    void doReconnect();
    bool gameActive();
    void watchdog(UsbState state, bool nowConfigured);
    void onConfigured();
    void onDeconfigured();
    bool postIn();
    bool postOut();
    void completeIn();
    void completeOut();
    void handleSetup();
    bool controlIn(const uint8_t* data, size_t len);
    bool controlOut(size_t len, size_t& received);
    bool statusStage(bool zeroLengthIn);

    void note(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void noteError(Result rc, const char* what);
    void countError(Result rc);  // counts without logging (for repeating failures)

    Shared& shared_;
    HidFunction hid_;
    inf::InfinityFunction inf_;
    lego::ToyPadFunction& lego_;
    Mode mode_ = Mode::Skylanders;

    bool usbInitialized_ = false;
    UsbDsInterface* iface_ = nullptr;
    UsbDsEndpoint* epIn_ = nullptr;
    UsbDsEndpoint* epOut_ = nullptr;

    // usb:ds transfers need 0x1000-aligned buffers.
    uint8_t* inBuf_ = nullptr;
    uint8_t* outBuf_ = nullptr;
    uint8_t* ctrlBuf_ = nullptr;
    uint8_t* setupBuf_ = nullptr;

    Thread thread_{};
    bool threadStarted_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> reconnectRequested_{false};
    UsbState lastState_ = UsbState_Detached;
    u64 startTick_ = 0;
    uint32_t sinceMs() const;

    uint32_t postFails_ = 0;
    bool ctrlStatus_ = true;  // usb.ini ctrl_status

    // Automatic re-plug (usb.ini auto_replug): while the console is linked, or stuck mid-enumeration,
    // and the game has not started using the device, detach/attach again every so often.
    bool autoReplug_ = true;
    uint32_t autoReplugMs_ = 8000;
    bool waiting_ = false;
    uint32_t waitStartMs_ = 0;
    uint32_t replugTries_ = 0;
    uint32_t graceUntilMs_ = 0;

    bool configured_ = false;
    bool inPending_ = false, outPending_ = false;
    u32 inUrb_ = 0, outUrb_ = 0;

    Mutex diagMutex_{};
    UsbDiag diag_;
};

}  // namespace sky
