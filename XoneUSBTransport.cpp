/* XoneUSBTransport.cpp – user-space IOUSBLib transport for Xone:4D.
 * MIT License.
 */
#include "XoneUSBTransport.h"
#include "PloytecCodec.h"
#include "ploytec_defs.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/USBSpec.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <cmath>
#include <cstring>
#include <cstdio>

#define XLOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[XoneHAL/USB] " fmt, ##__VA_ARGS__)

XoneUSBTransport& XoneUSBTransport::shared() {
    static XoneUSBTransport gInstance;
    return gInstance;
}

XoneUSBTransport::XoneUSBTransport() {
    mInRing.init(kRingBytes);
    mOutRing.init(kRingBytes);
    for (uint32_t i = 0; i < kDoubleBuffer; i++) {
        mOutBuf[i] = new uint8_t[PLOYTEC_INT_OUT_PKT_SIZE];
        mInBuf [i] = new uint8_t[PLOYTEC_INT_IN_PKT_SIZE];
        std::memset(mOutBuf[i], 0, PLOYTEC_INT_OUT_PKT_SIZE);
        std::memset(mInBuf [i], 0, PLOYTEC_INT_IN_PKT_SIZE);
        mOutCtx[i].self = this; mOutCtx[i].slot = i;
        mInCtx [i].self = this; mInCtx [i].slot = i;
    }
    for (uint32_t i = 0; i < kMidiInSlots; i++) {
        mMidiInBuf[i] = new uint8_t[kMidiInBufSize];
        std::memset(mMidiInBuf[i], 0, kMidiInBufSize);
        mMidiInCtx[i].self = this; mMidiInCtx[i].slot = i;
    }
}

XoneUSBTransport::~XoneUSBTransport() {
    closeDevice();
    for (uint32_t i = 0; i < kDoubleBuffer; i++) {
        delete[] mOutBuf[i];
        delete[] mInBuf [i];
    }
    for (uint32_t i = 0; i < kMidiInSlots; i++) {
        delete[] mMidiInBuf[i];
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Device discovery / open
// ───────────────────────────────────────────────────────────────────────────

bool XoneUSBTransport::findDevice(io_service_t* outService) {
    *outService = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return false;

    SInt32 vid = PLOYTEC_VENDOR_ID;
    SInt32 pid = PLOYTEC_PID_XONE_4D;
    CFDictionarySetValue(match, CFSTR(kUSBVendorID),
        CFNumberCreate(NULL, kCFNumberSInt32Type, &vid));
    CFDictionarySetValue(match, CFSTR(kUSBProductID),
        CFNumberCreate(NULL, kCFNumberSInt32Type, &pid));

    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
        return false;

    io_service_t svc = IOIteratorNext(iter);
    IOObjectRelease(iter);
    if (svc == IO_OBJECT_NULL) return false;
    *outService = svc;
    return true;
}

bool XoneUSBTransport::deviceConnected() {
    io_service_t svc = IO_OBJECT_NULL;
    bool ok = findDevice(&svc);
    if (svc != IO_OBJECT_NULL) IOObjectRelease(svc);
    return ok;
}

bool XoneUSBTransport::openAndHandshake() {
    if (mDeviceOpen.load()) return true;

    io_service_t svc = IO_OBJECT_NULL;
    if (!findDevice(&svc)) {
        XLOG("Xone:4D not found on bus");
        return false;
    }

    IOCFPlugInInterface** plugin = nullptr;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || !plugin) {
        XLOG("IOCreatePlugInInterfaceForService failed: 0x%x", kr);
        return false;
    }

    HRESULT hr = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
        (LPVOID*)&mDevice);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || !mDevice) {
        XLOG("QueryInterface(IOUSBDeviceInterfaceID942) failed: 0x%lx", (long)hr);
        return false;
    }

    IOReturn r = (*mDevice)->USBDeviceOpen(mDevice);
    if (r != kIOReturnSuccess) {
        XLOG("USBDeviceOpen failed: 0x%x (already claimed by kext?)", r);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
        return false;
    }

    // Configuration 1 should already be active – set it anyway, idempotent.
    UInt8 cfg = 0;
    (*mDevice)->GetConfiguration(mDevice, &cfg);
    if (cfg != 1) {
        r = (*mDevice)->SetConfiguration(mDevice, 1);
        if (r != kIOReturnSuccess) {
            XLOG("SetConfiguration(1) warn: 0x%x", r);
        }
    }

    if (!openInterfaces()) {
        XLOG("openInterfaces failed");
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
        return false;
    }

    if (!doHandshake()) {
        XLOG("Ploytec handshake failed");
        teardownInterfaces();
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
        return false;
    }

    if (!claimPipes()) {
        XLOG("Could not find PCM IN/OUT pipes after alt-setting 1");
        teardownInterfaces();
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
        return false;
    }

    mDeviceOpen.store(true);
    XLOG("Device opened, handshake done, pipes ready");
    if (mAvailHandler) mAvailHandler(true);
    return true;
}

bool XoneUSBTransport::openInterfaces() {
    IOUSBFindInterfaceRequest req = {
        kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare
    };
    io_iterator_t iter = IO_OBJECT_NULL;
    IOReturn r = (*mDevice)->CreateInterfaceIterator(mDevice, &req, &iter);
    if (r != kIOReturnSuccess) return false;

    io_service_t s = IO_OBJECT_NULL;
    int idx = 0;
    while ((s = IOIteratorNext(iter)) != IO_OBJECT_NULL && idx < 2) {
        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(s);
        if (kr != KERN_SUCCESS || !plugin) continue;

        IOUSBInterfaceInterface942** intf = nullptr;
        HRESULT hr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942),
            (LPVOID*)&intf);
        IODestroyPlugInInterface(plugin);
        if (hr != S_OK || !intf) continue;

        r = (*intf)->USBInterfaceOpen(intf);
        if (r != kIOReturnSuccess) {
            (*intf)->Release(intf);
            continue;
        }
        if (idx == 0) mIntf0 = intf;
        else          mIntf1 = intf;
        idx++;
    }
    IOObjectRelease(iter);

    if (!mIntf0 || !mIntf1) return false;

    // Alt setting 1 = audio active, on both interfaces
    r = (*mIntf0)->SetAlternateInterface(mIntf0, PLOYTEC_ALT_SETTING_AUDIO);
    if (r != kIOReturnSuccess) { XLOG("intf0 SetAlt(1) failed: 0x%x", r); return false; }
    r = (*mIntf1)->SetAlternateInterface(mIntf1, PLOYTEC_ALT_SETTING_AUDIO);
    if (r != kIOReturnSuccess) { XLOG("intf1 SetAlt(1) failed: 0x%x", r); return false; }
    return true;
}

bool XoneUSBTransport::findPipeRef(IOUSBInterfaceInterface942** intf,
                                   uint8_t epAddr, uint8_t* outRef) {
    UInt8 numEP = 0;
    if ((*intf)->GetNumEndpoints(intf, &numEP) != kIOReturnSuccess) return false;
    // pipeRef 0 = control; 1..numEP = the interface's data endpoints
    for (UInt8 i = 1; i <= numEP; i++) {
        UInt8 dir=0, num=0, txType=0, interval=0;
        UInt16 mps=0;
        if ((*intf)->GetPipeProperties(intf, i, &dir, &num, &txType, &mps, &interval) != kIOReturnSuccess)
            continue;
        // direction: 0=out, 1=in. Endpoint addr high bit = direction.
        uint8_t addr = (uint8_t)(num | ((dir == kUSBIn) ? 0x80 : 0x00));
        if (addr == epAddr) {
            *outRef = i;
            return true;
        }
    }
    return false;
}

bool XoneUSBTransport::claimPipes() {
    uint8_t ref = 0;
    if (findPipeRef(mIntf0, PLOYTEC_EP_PCM_OUT, &ref)) {
        mOutPipeIntf = mIntf0; mOutPipeRef = ref;
    } else if (findPipeRef(mIntf1, PLOYTEC_EP_PCM_OUT, &ref)) {
        mOutPipeIntf = mIntf1; mOutPipeRef = ref;
    } else {
        return false;
    }
    if (findPipeRef(mIntf0, PLOYTEC_EP_PCM_IN, &ref)) {
        mInPipeIntf = mIntf0; mInPipeRef = ref;
    } else if (findPipeRef(mIntf1, PLOYTEC_EP_PCM_IN, &ref)) {
        mInPipeIntf = mIntf1; mInPipeRef = ref;
    } else {
        return false;
    }
    // MIDI IN is best-effort — if we can't find it we still get audio.
    if (findPipeRef(mIntf0, PLOYTEC_EP_MIDI_IN, &ref)) {
        mMidiInPipeIntf = mIntf0; mMidiInPipeRef = ref;
    } else if (findPipeRef(mIntf1, PLOYTEC_EP_MIDI_IN, &ref)) {
        mMidiInPipeIntf = mIntf1; mMidiInPipeRef = ref;
    }
    XLOG("OUT pipe ref=%u on intf%s, IN pipe ref=%u on intf%s, MIDI-IN ref=%u%s",
         mOutPipeRef, (mOutPipeIntf == mIntf0 ? "0" : "1"),
         mInPipeRef,  (mInPipeIntf  == mIntf0 ? "0" : "1"),
         mMidiInPipeRef,
         mMidiInPipeIntf ? (mMidiInPipeIntf == mIntf0 ? " on intf0" : " on intf1")
                         : " (not found — MIDI input disabled)");

    // Diagnostic: report the IN endpoint's wMaxPacketSize and polling
    // interval. The legacy A&H kext drives capture in whole 8-frame (512 B)
    // blocks sized off this value and doubles its block count for
    // high-bandwidth (≥0x201) endpoints; logging it lets us confirm our fixed
    // 5120-byte read assumption matches the hardware.
    if (mInPipeIntf && mInPipeRef) {
        UInt8 dir=0, num=0, txType=0, interval=0; UInt16 mps=0;
        if ((*mInPipeIntf)->GetPipeProperties(mInPipeIntf, mInPipeRef,
                                              &dir, &num, &txType, &mps, &interval)
            == kIOReturnSuccess) {
            XLOG("IN pipe: wMaxPacketSize=%u txType=%u interval=%u (our read=%u)",
                 mps, txType, interval, (unsigned)PLOYTEC_INT_IN_PKT_SIZE);
        }
    }
    return true;
}

void XoneUSBTransport::teardownInterfaces() {
    if (mIntf0) {
        if (mIntf0Src) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), mIntf0Src, kCFRunLoopDefaultMode);
            mIntf0Src = nullptr;
        }
        (*mIntf0)->USBInterfaceClose(mIntf0);
        (*mIntf0)->Release(mIntf0);
        mIntf0 = nullptr;
    }
    if (mIntf1) {
        if (mIntf1Src) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), mIntf1Src, kCFRunLoopDefaultMode);
            mIntf1Src = nullptr;
        }
        (*mIntf1)->USBInterfaceClose(mIntf1);
        (*mIntf1)->Release(mIntf1);
        mIntf1 = nullptr;
    }
    mOutPipeIntf = mInPipeIntf = nullptr;
    mOutPipeRef = mInPipeRef = 0;
}

void XoneUSBTransport::closeDevice() {
    shutdownWorker();
    teardownInterfaces();
    if (mDevice) {
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
    }
    mDeviceOpen.store(false);
}

// ───────────────────────────────────────────────────────────────────────────
// Vendor control transfers (synchronous; only used during open/handshake/rate)
// ───────────────────────────────────────────────────────────────────────────

IOReturn XoneUSBTransport::controlRequest(uint8_t bmRequestType, uint8_t bRequest,
                                          uint16_t wValue, uint16_t wIndex,
                                          void* data, uint16_t length) {
    if (!mDevice) return kIOReturnNoDevice;
    IOUSBDevRequest req = {};
    req.bmRequestType = bmRequestType;
    req.bRequest      = bRequest;
    req.wValue        = wValue;
    req.wIndex        = wIndex;
    req.wLength       = length;
    req.pData         = data;
    req.wLenDone      = 0;
    return (*mDevice)->DeviceRequest(mDevice, &req);
}

bool XoneUSBTransport::sendSampleRateCmd(uint32_t rate) {
    uint8_t buf[3];
    ploytec_encode_rate(rate, buf);
    IOReturn r = controlRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ,
                                0x0100, PLOYTEC_EP_RATE_IN, buf, 3);
    if (r != kIOReturnSuccess) return false;
    r = controlRequest(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ,
                       0x0100, PLOYTEC_EP_RATE_OUT, buf, 3);
    return r == kIOReturnSuccess;
}

bool XoneUSBTransport::doHandshake() {
    uint8_t buf[16] = {};
    IOReturn r;

    r = controlRequest(0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, buf, 15);
    if (r != kIOReturnSuccess) { XLOG("Firmware read failed: 0x%x", r); return false; }
    buf[15] = '\0';
    XLOG("Firmware: %s", (char*)buf);

    if (!sendSampleRateCmd(mSampleRate)) { XLOG("SetSampleRate failed"); return false; }

    r = controlRequest(0xC0, PLOYTEC_CMD_STATUS, 0, PLOYTEC_REG_AJ_INPUT_SEL, buf, 1);
    if (r != kIOReturnSuccess) { XLOG("Status read failed: 0x%x", r); return false; }
    uint8_t status = buf[0];

    uint16_t wVal = ploytec_confirm_wvalue(status);
    r = controlRequest(0x40, PLOYTEC_CMD_STATUS, wVal, PLOYTEC_REG_AJ_INPUT_SEL, nullptr, 0);
    if (r != kIOReturnSuccess) { XLOG("Status confirm failed: 0x%x", r); return false; }

    XLOG("Handshake OK, status=0x%02X", status);
    return true;
}

bool XoneUSBTransport::setSampleRate(uint32_t rate) {
    if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000)
        return false;
    // Rate change is the one transition that genuinely needs a worker
    // restart: the CFRunLoopTimer interval is fixed at creation time
    // (kFramesPerXfer / sampleRate) so a new period requires a new timer,
    // and the cleanest way to recreate it is to bounce the worker.
    const bool was_running = mRunning.load();
    const bool was_feeding = mFeeding.load();
    if (was_running) shutdownWorker();
    mSampleRate = rate;
    bool ok = true;
    if (mDeviceOpen.load()) {
        ok = sendSampleRateCmd(rate);
    }
    if (was_running) {
        startIO();
        // startIO() unconditionally raises mFeeding; restore prior state if
        // we were in a paused (StopIO) state when the rate change came in.
        if (!was_feeding) {
            mFeeding.store(false);
            mOutRing.zero();
            mInRing.zero();
        }
    }
    return ok;
}

// ───────────────────────────────────────────────────────────────────────────
// Worker thread + async transfers
// ───────────────────────────────────────────────────────────────────────────

void* XoneUSBTransport::workerEntry(void* arg) {
    static_cast<XoneUSBTransport*>(arg)->workerLoop();
    return nullptr;
}

void XoneUSBTransport::attachInterfaceEventSources() {
    if (!mWorkerLoop) return;
    if (mIntf0 && !mIntf0Src) {
        if ((*mIntf0)->CreateInterfaceAsyncEventSource(mIntf0, &mIntf0Src) == kIOReturnSuccess) {
            CFRunLoopAddSource(mWorkerLoop, mIntf0Src, kCFRunLoopDefaultMode);
        }
    }
    if (mIntf1 && !mIntf1Src) {
        if ((*mIntf1)->CreateInterfaceAsyncEventSource(mIntf1, &mIntf1Src) == kIOReturnSuccess) {
            CFRunLoopAddSource(mWorkerLoop, mIntf1Src, kCFRunLoopDefaultMode);
        }
    }
}

void XoneUSBTransport::workerLoop() {
    mWorkerLoop = CFRunLoopGetCurrent();
    attachInterfaceEventSources();

    // Register for system sleep/wake notifications so we can abort in-flight
    // USB transfers before the bus suspends (avoids a "hiss" transient at
    // sleep) and re-prime the pipeline after wake.
    mPowerConnect = IORegisterForSystemPower(this, &mPowerPort,
                                             &XoneUSBTransport::powerCallbackTrampoline,
                                             &mPowerNotifier);
    if (mPowerConnect != MACH_PORT_NULL) {
        CFRunLoopSourceRef pwrSrc = IONotificationPortGetRunLoopSource(mPowerPort);
        if (pwrSrc) CFRunLoopAddSource(mWorkerLoop, pwrSrc, kCFRunLoopDefaultMode);
    } else {
        XLOG("IORegisterForSystemPower failed; sleep/wake will not be handled");
    }

    // Watch for the device being unplugged and re-plugged at runtime, so that
    // we automatically reopen it instead of requiring `sudo killall coreaudiod`.
    setupHotplugNotification();

    // MIDI bridge to the user-session agent. Start it unconditionally — the
    // socket is lazy-connected on the first feedIn(), so it costs nothing
    // when the device isn't open yet.
    mMidi.start();

    // Prime IN and MIDI-IN only if the USB device is actually open. When the
    // worker is started early (from Initialize, before the user has plugged
    // in the device or while coreaudiod has just re-spawned and the device
    // wasn't opened cleanly) the pipes are null — priming would no-op anyway.
    // The hot-plug callback (reopenDeviceAfterWake) primes them after the
    // device successfully opens.
    if (mDeviceOpen.load()) {
        for (uint32_t i = 0; i < kDoubleBuffer; i++) submitIn(i);
        if (mMidiInPipeIntf && mMidiInPipeRef) {
            for (uint32_t i = 0; i < kMidiInSlots; i++) submitMidiIn(i);
        }
    }

    // Dispatch-source-based timer with DISPATCH_TIMER_STRICT. Replaces an
    // earlier CFRunLoopTimer attempt: CFRunLoopTimer at 0.833 ms period
    // (96 kHz / 80 frames) is on the edge of CFRunLoop's scheduling
    // resolution — every couple of minutes the runloop gets stuck on some
    // unrelated task long enough to miss a timer fire, which manifests as
    // an audible click (a stutter roughly every 5 minutes). Dispatch timers with
    // STRICT scheduling run on a dedicated GCD queue with kernel-level
    // precision, so jitter drops by ~10×.
    mOutPingPong = 0;
    mTimerQueue = dispatch_queue_create("com.digitalkiss.xone.outtimer",
                                        dispatch_queue_attr_make_with_qos_class(
                                            DISPATCH_QUEUE_SERIAL,
                                            QOS_CLASS_USER_INTERACTIVE, 0));
    if (mTimerQueue) {
        mOutDispatchTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
                                                    0, DISPATCH_TIMER_STRICT,
                                                    mTimerQueue);
    }
    if (mOutDispatchTimer) {
        const uint64_t periodNs =
            (uint64_t)((double)kFramesPerXfer * 1.0e9 / (double)mSampleRate);
        dispatch_set_context(mOutDispatchTimer, this);
        dispatch_source_set_event_handler_f(mOutDispatchTimer,
                                            &XoneUSBTransport::outTimerHandler);
        dispatch_source_set_timer(mOutDispatchTimer,
                                  dispatch_time(DISPATCH_TIME_NOW, periodNs),
                                  periodNs,
                                  0);   // leeway 0 for strict
        dispatch_resume(mOutDispatchTimer);
    } else {
        XLOG("dispatch timer creation failed; OUT pacing will be off");
    }

    mWorkerReady.store(true);

    // IN capture watchdog state. The capture endpoint is resubmitted from its
    // own completion (onInComplete), so if the device/host ever stops
    // delivering IN packets the chain dies silently and never restarts — the
    // field logs showed IN diag heartbeats stopping dead after ~12 min while
    // OUT (timer-driven) kept running, and capture froze into a short loop.
    // OUT has its dispatch timer as a natural heartbeat; IN gets this one.
    uint64_t wdLastInSample = mInSampleTime.load(std::memory_order_acquire);
    uint64_t wdLastChangeTick = mach_absolute_time();
    int      wdAbortCount = 0;   // consecutive light re-prime attempts
    const double wdHostTicksPerSec =
        (mHostTicksPerFrame > 0.0) ? (mHostTicksPerFrame * (double)mSampleRate) : 0.0;

    while (!mShutdown.load()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);

        // Two-stage IN-capture watchdog. The capture endpoint resubmits from
        // its own completion (onInComplete), so if the device/host ever stops
        // delivering IN packets the chain dies and never restarts — the field
        // logs showed IN heartbeats stopping dead (≈12 min in, and right when
        // playback was stopped) while OUT, timer-driven, kept running. Capture
        // then froze into a short loop and only `killall coreaudiod` fixed it.
        //
        //   Stage 1 (≤3×, ~200 ms apart): AbortPipe(IN). This forces the stuck
        //     transfers to complete with kIOReturnAborted; onInComplete then
        //     resubmits each slot (no buffer race — each slot is re-armed by
        //     its own completion on this same thread).
        //   Stage 2 (if Stage 1 didn't revive it): full reopenDeviceAfterWake,
        //     the same teardown+reopen+re-prime path used for sleep/hot-plug,
        //     which is guaranteed to bring capture back. mSuspended is raised
        //     first so the OUT dispatch timer bails out of submitOut while the
        //     interfaces are torn down (no use-after-free on the timer queue).
        if (wdHostTicksPerSec > 0.0 && mDeviceOpen.load() && !mSuspended.load()
            && !mShutdown.load()) {
            const uint64_t nowTick = mach_absolute_time();
            const uint64_t curIn   = mInSampleTime.load(std::memory_order_acquire);
            if (curIn != wdLastInSample) {
                wdLastInSample   = curIn;
                wdLastChangeTick = nowTick;
                wdAbortCount     = 0;            // healthy — reset escalation
            } else {
                const double stalledMs =
                    1000.0 * (double)(nowTick - wdLastChangeTick) / wdHostTicksPerSec;
                if (stalledMs > 200.0) {
                    if (wdAbortCount < 3 && mInPipeIntf && mInPipeRef) {
                        XLOG("IN watchdog: capture stalled %.0f ms — re-priming IN pipe (try %d)",
                             stalledMs, wdAbortCount + 1);
                        (*mInPipeIntf)->AbortPipe(mInPipeIntf, mInPipeRef);
                        wdAbortCount++;
                        wdLastChangeTick = nowTick;     // give it ~200 ms to recover
                    } else {
                        XLOG("IN watchdog: re-prime failed — full device reopen");
                        mSuspended.store(true);          // gate OUT timer during teardown
                        reopenDeviceAfterWake(true);      // immediate: device still on bus
                        wdAbortCount     = 0;
                        wdLastInSample   = mInSampleTime.load(std::memory_order_acquire);
                        wdLastChangeTick = mach_absolute_time();
                    }
                }
            }
        }
    }

    // Abort pending transfers and unwind.
    if (mOutDispatchTimer) {
        dispatch_source_cancel(mOutDispatchTimer);
        dispatch_release(mOutDispatchTimer);
        mOutDispatchTimer = nullptr;
    }
    if (mTimerQueue) {
        dispatch_release(mTimerQueue);
        mTimerQueue = nullptr;
    }
    if (mPowerConnect != MACH_PORT_NULL) {
        CFRunLoopSourceRef pwrSrc = IONotificationPortGetRunLoopSource(mPowerPort);
        if (pwrSrc) CFRunLoopRemoveSource(mWorkerLoop, pwrSrc, kCFRunLoopDefaultMode);
        IODeregisterForSystemPower(&mPowerNotifier);
        IOServiceClose(mPowerConnect);
        IONotificationPortDestroy(mPowerPort);
        mPowerConnect = 0;
        mPowerPort = nullptr;
        mPowerNotifier = 0;
    }
    teardownHotplugNotification();
    if (mInPipeIntf && mInPipeRef)  (*mInPipeIntf)->AbortPipe(mInPipeIntf, mInPipeRef);
    if (mOutPipeIntf && mOutPipeRef) (*mOutPipeIntf)->AbortPipe(mOutPipeIntf, mOutPipeRef);
    if (mMidiInPipeIntf && mMidiInPipeRef)
        (*mMidiInPipeIntf)->AbortPipe(mMidiInPipeIntf, mMidiInPipeRef);
    mMidi.stop();

    // Drain a few more cycles so completion callbacks run with kIOReturnAborted.
    for (int i = 0; i < 4; i++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }

    if (mIntf0Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf0Src, kCFRunLoopDefaultMode);
        mIntf0Src = nullptr;
    }
    if (mIntf1Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf1Src, kCFRunLoopDefaultMode);
        mIntf1Src = nullptr;
    }
}

void XoneUSBTransport::outTimerHandler(void* ctx) {
    auto* self = static_cast<XoneUSBTransport*>(ctx);
    if (!self || self->mShutdown.load()) return;
    const uint32_t slot = self->mOutPingPong;
    self->mOutPingPong = (slot + 1) % kDoubleBuffer;
    self->submitOut(slot);
}

void XoneUSBTransport::powerCallbackTrampoline(void* refCon, io_service_t,
                                               uint32_t messageType, void* arg) {
    auto* self = static_cast<XoneUSBTransport*>(refCon);
    if (self) self->onPowerEvent(messageType, arg);
}

// ───────────────────────────────────────────────────────────────────────────
// USB hot-plug
//
// Without this, unplugging the Xone:4D and plugging it back in leaves
// coreaudiod's HAL holding dead USB handles — audio silently stops and the
// only way to recover used to be `sudo killall coreaudiod`. We:
//   - Register a `kIOFirstMatchNotification` for VID/PID 0x0A4A:0xFF4D on the
//     worker thread's run-loop. macOS calls our trampoline whenever a device
//     with that VID/PID appears (including replug into the same port).
//   - Detect device disappearance via `kIOReturnNoDevice` /
//     `kIOReturnNotResponding` in the USB completion callbacks (they all run
//     on the same worker thread), and call `handleDeviceLost()` to tear down
//     stale handles cleanly. The transport then waits for the hot-plug
//     notification to re-open the device.
// ───────────────────────────────────────────────────────────────────────────

void XoneUSBTransport::setupHotplugNotification() {
    if (mHotplugPort || !mWorkerLoop) return;

    mHotplugPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!mHotplugPort) {
        XLOG("hotplug: IONotificationPortCreate failed");
        return;
    }
    CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(mHotplugPort);
    if (src) CFRunLoopAddSource(mWorkerLoop, src, kCFRunLoopDefaultMode);

    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) {
        IONotificationPortDestroy(mHotplugPort);
        mHotplugPort = nullptr;
        return;
    }
    SInt32 vid = PLOYTEC_VENDOR_ID;
    SInt32 pid = PLOYTEC_PID_XONE_4D;
    CFNumberRef vidNum = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef pidNum = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vidNum);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pidNum);
    CFRelease(vidNum);
    CFRelease(pidNum);

    // IOServiceAddMatchingNotification consumes the dictionary's reference.
    kern_return_t kr = IOServiceAddMatchingNotification(
        mHotplugPort,
        kIOFirstMatchNotification,
        match,
        &XoneUSBTransport::hotplugCallbackTrampoline,
        this,
        &mHotplugIter);
    if (kr != KERN_SUCCESS) {
        XLOG("hotplug: IOServiceAddMatchingNotification failed: 0x%x", kr);
        IONotificationPortDestroy(mHotplugPort);
        mHotplugPort = nullptr;
        return;
    }

    // Drain initial matches. IOServiceAddMatchingNotification returns the set
    // of services that already match at registration time — we MUST iterate
    // them or the notification will never fire again. If we find an existing
    // service but our own device-open attempt failed earlier (which happens
    // when coreaudiod re-spawns after idle and HAL's Initialize-time
    // openAndHandshake hit a transient USB state), kick off a reopen now —
    // otherwise we'd sit idle forever waiting for a hot-plug event that
    // already happened before we were listening.
    io_service_t svc;
    bool sawExisting = false;
    while ((svc = IOIteratorNext(mHotplugIter)) != IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        sawExisting = true;
    }
    XLOG("hotplug: watching for VID 0x%04X PID 0x%04X (existing=%d, open=%d)",
         PLOYTEC_VENDOR_ID, PLOYTEC_PID_XONE_4D, sawExisting, mDeviceOpen.load());
    if (sawExisting && !mDeviceOpen.load()) {
        XLOG("hotplug: device on bus but not opened — opening now");
        reopenDeviceAfterWake();
        mDeviceLost.store(false);
    }

    // Also wire up the per-device "Is Terminated" notification, so an unplug
    // event from IOKit reaches us instantly — without waiting for an USB
    // transfer to fail. This is the canonical detection path; the
    // kIOReturnNoDevice checks below are belt-and-braces.
    registerDeviceInterestNotification();
}

void XoneUSBTransport::registerDeviceInterestNotification() {
    if (!mHotplugPort) return;
    if (mDeviceInterestNotifier != IO_OBJECT_NULL) {
        IOObjectRelease(mDeviceInterestNotifier);
        mDeviceInterestNotifier = IO_OBJECT_NULL;
    }

    io_service_t svc = IO_OBJECT_NULL;
    if (!findDevice(&svc)) {
        // Not on the bus right now — that's fine, we'll re-register on next
        // successful open. The FirstMatch notification will tell us when the
        // device shows up.
        return;
    }
    kern_return_t kr = IOServiceAddInterestNotification(
        mHotplugPort, svc,
        kIOGeneralInterest,
        &XoneUSBTransport::deviceInterestTrampoline,
        this,
        &mDeviceInterestNotifier);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        XLOG("hotplug: IOServiceAddInterestNotification failed: 0x%x", kr);
        mDeviceInterestNotifier = IO_OBJECT_NULL;
        return;
    }
    XLOG("hotplug: registered termination interest on current device");
}

void XoneUSBTransport::deviceInterestTrampoline(void* refCon, io_service_t,
                                                uint32_t messageType, void*) {
    auto* self = static_cast<XoneUSBTransport*>(refCon);
    if (!self) return;
    // We only care about the terminated event — every other general-interest
    // message (matched/published/etc.) is noise for our purposes.
    if (messageType == kIOMessageServiceIsTerminated) {
        XLOG("interest: device terminated by IOKit (unplugged)");
        self->handleDeviceLost();
    }
}

void XoneUSBTransport::teardownHotplugNotification() {
    if (mDeviceInterestNotifier != IO_OBJECT_NULL) {
        IOObjectRelease(mDeviceInterestNotifier);
        mDeviceInterestNotifier = IO_OBJECT_NULL;
    }
    if (mHotplugIter) {
        IOObjectRelease(mHotplugIter);
        mHotplugIter = IO_OBJECT_NULL;
    }
    if (mHotplugPort) {
        CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(mHotplugPort);
        if (src && mWorkerLoop)
            CFRunLoopRemoveSource(mWorkerLoop, src, kCFRunLoopDefaultMode);
        IONotificationPortDestroy(mHotplugPort);
        mHotplugPort = nullptr;
    }
}

void XoneUSBTransport::hotplugCallbackTrampoline(void* refCon, io_iterator_t iter) {
    auto* self = static_cast<XoneUSBTransport*>(refCon);
    if (self) self->onHotplug(iter);
}

void XoneUSBTransport::onHotplug(io_iterator_t iter) {
    // Must drain the iterator — IOKit re-arms the notification only after all
    // current matches have been consumed. Initial matches at registration
    // time were already drained in setupHotplugNotification, so any call to
    // this trampoline reflects a genuine new appearance of the Xone:4D on
    // the bus (a replug, or a USB-bus reset that re-enumerated devices).
    bool sawAny = false;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        sawAny = true;
    }
    if (!sawAny) return;

    XLOG("hotplug: Xone:4D appeared on bus — reopening");
    // reopenDeviceAfterWake closes any stale handles first, so calling it
    // when we still wrongly believe mDeviceOpen is true is safe.
    reopenDeviceAfterWake();
    mDeviceLost.store(false);
    // The new io_service_t needs its own termination notification — the old
    // one was tied to the device that just vanished.
    registerDeviceInterestNotification();
}

void XoneUSBTransport::handleDeviceLost() {
    // Idempotent — multiple completion callbacks may report NoDevice in quick
    // succession (one per outstanding IN/MIDI-IN slot). Only the first one
    // does the teardown; the rest return immediately.
    if (mDeviceLost.exchange(true)) return;
    XLOG("device lost — tearing down USB state, waiting for hot-plug");

    // Gate all further submissions (IN, OUT timer, MIDI-IN re-submit) — they
    // all check mSuspended before issuing the next transfer.
    mSuspended.store(true);

    // Abort whatever pipes are still bound. AbortPipe on an already-dead pipe
    // is harmless; subsequent submissions are gated above anyway.
    if (mInPipeIntf && mInPipeRef)
        (*mInPipeIntf)->AbortPipe(mInPipeIntf, mInPipeRef);
    if (mOutPipeIntf && mOutPipeRef)
        (*mOutPipeIntf)->AbortPipe(mOutPipeIntf, mOutPipeRef);
    if (mMidiInPipeIntf && mMidiInPipeRef)
        (*mMidiInPipeIntf)->AbortPipe(mMidiInPipeIntf, mMidiInPipeRef);

    // Detach interface async event sources and release the interfaces+device
    // so the next openAndHandshake (driven by the hot-plug notification) sees
    // a clean slate. We're already on the worker run-loop thread (completion
    // callbacks fire from the run-loop sources attached to it), so doing this
    // inline is safe.
    if (mIntf0Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf0Src, kCFRunLoopDefaultMode);
        mIntf0Src = nullptr;
    }
    if (mIntf1Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf1Src, kCFRunLoopDefaultMode);
        mIntf1Src = nullptr;
    }
    teardownInterfaces();
    if (mDevice) {
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
    }
    mDeviceOpen.store(false);
    if (mAvailHandler) mAvailHandler(false);

    // Race fallback: if the user replugged before we got to mark the device
    // lost (fast unplug+replug), the new device may already be on the bus —
    // re-open immediately instead of waiting on the hot-plug notification
    // (which won't fire if it already armed against the new io_service_t).
    if (deviceConnected()) {
        XLOG("device already back on bus — reopening immediately");
        reopenDeviceAfterWake();
        mDeviceLost.store(false);
    }
}

void XoneUSBTransport::reopenDeviceAfterWake(bool immediate) {
    // We are running on the worker thread (the power port's runloop source
    // is attached here), so we can rip down USB state and rebuild it inline
    // without coordinating with another thread.

    // 1. Detach the interfaces' async event sources from our runloop before
    //    closing the interfaces themselves.
    if (mIntf0Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf0Src, kCFRunLoopDefaultMode);
        mIntf0Src = nullptr;
    }
    if (mIntf1Src) {
        CFRunLoopRemoveSource(mWorkerLoop, mIntf1Src, kCFRunLoopDefaultMode);
        mIntf1Src = nullptr;
    }

    // 2. Close interfaces + device. teardownInterfaces() also clears pipe refs.
    teardownInterfaces();
    if (mDevice) {
        (*mDevice)->USBDeviceClose(mDevice);
        (*mDevice)->Release(mDevice);
        mDevice = nullptr;
    }
    mDeviceOpen.store(false);

    // 3. Reopen. After a real sleep the device isn't matchable in
    //    IOServiceGetMatchingServices for ~1 s, so we wait then retry. For a
    //    watchdog-triggered reopen (immediate=true) the device never left the
    //    bus, so we skip the upfront delay and open straight away — keeping the
    //    capture-recovery blip down to a fraction of a second.
    if (!immediate) usleep(500 * 1000);
    bool ok = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (openAndHandshake()) { ok = true; break; }
        usleep((immediate ? 50 : 200) * 1000);
    }
    if (!ok) {
        XLOG("wake: failed to reopen device after retries; staying suspended");
        return;
    }

    // 4. Reattach interface async event sources to the worker runloop so
    //    completions fire again.
    attachInterfaceEventSources();

    // 5. Bump the clock seed so CoreAudio re-anchors its timeline; reset
    //    cursors and rings (audio was lost across sleep, no point keeping
    //    stale samples).
    mClockSeed.fetch_add(1);
    mAnchorHostTime.store(mach_absolute_time(), std::memory_order_release);
    mOutSampleTime.store(0);
    mInSampleTime.store(0);
    mOutRing.zero();
    mInRing.zero();

    // 6. Allow submissions, re-prime IN. OUT resumes on the next timer tick.
    mSuspended.store(false);
    for (uint32_t i = 0; i < kDoubleBuffer; i++) submitIn(i);
    if (mMidiInPipeIntf && mMidiInPipeRef) {
        for (uint32_t i = 0; i < kMidiInSlots; i++) submitMidiIn(i);
    }

    // The previous io_service_t is dead — get a fresh termination interest
    // hooked onto the new one so the next unplug is caught instantly.
    registerDeviceInterestNotification();

    XLOG("wake: device reopened, audio resumed");
}

void XoneUSBTransport::onPowerEvent(uint32_t messageType, void* arg) {
    switch (messageType) {
    case kIOMessageCanSystemSleep:
        // Some other subsystem asked if sleep is OK. We don't block sleep.
        IOAllowPowerChange(mPowerConnect, (long)arg);
        break;

    case kIOMessageSystemWillSleep:
        // Sleep is imminent — stop submitting and abort whatever is in flight,
        // otherwise the kernel's USB stack gates our last few packets right at
        // the bus-suspend boundary and the device hears a half-second of
        // garbage (the wheezing/wake-up rasp reported in testing).
        XLOG("power: WillSleep — pausing USB");
        mSuspended.store(true);
        if (mInPipeIntf && mInPipeRef)
            (*mInPipeIntf)->AbortPipe(mInPipeIntf, mInPipeRef);
        if (mOutPipeIntf && mOutPipeRef)
            (*mOutPipeIntf)->AbortPipe(mOutPipeIntf, mOutPipeRef);
        IOAllowPowerChange(mPowerConnect, (long)arg);
        break;

    case kIOMessageSystemHasPoweredOn:
        // Bus is alive again. Re-priming IN alone isn't enough — after a
        // real sleep cycle macOS often re-enumerates USB devices and our
        // existing IOUSBDevice/Interface handles become stale (submissions
        // appear to succeed but produce garbage, heard as hiss until the
        // user toggles sample rate which forces a full restart). Do the
        // full reopen + handshake now.
        XLOG("power: HasPoweredOn — reopening device");
        reopenDeviceAfterWake();
        break;

    default:
        break;
    }
}

bool XoneUSBTransport::ensureMonitor() {
    // Spin up the worker thread once and leave it running for the entire HAL
    // lifetime. The worker owns the IOKit hot-plug + power notifications, so
    // having it alive even when no audio client is active means a USB plug-in
    // after idle (or after coreaudiod gets garbage-collected and re-spawns)
    // is detected immediately — without that, kIOFirstMatchNotification never
    // had a chance to arm and the device stayed invisible until manual
    // `sudo killall coreaudiod`.
    if (mRunning.load()) return true;

    mShutdown.store(false);
    mWorkerReady.store(false);
    mInSampleTime.store(0);
    mOutSampleTime.store(0);
    mClockSeed.fetch_add(1);
    mInRing.zero();
    mOutRing.zero();

    // Host-time clock anchor for GetZeroTimeStamp. Recomputed on every
    // device-open as well (reopenDeviceAfterWake) — both call sites must agree
    // because the OUT cursor drift detector reads it from submitOut.
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double hostTicksPerSecond = (double)tb.denom * 1.0e9 / (double)tb.numer;
    mHostTicksPerFrame = hostTicksPerSecond / (double)mSampleRate;
    mAnchorHostTime.store(mach_absolute_time(), std::memory_order_release);

    mRunning.store(true);
    if (pthread_create(&mWorker, nullptr, &XoneUSBTransport::workerEntry, this) != 0) {
        mRunning.store(false);
        return false;
    }
    // Wait briefly for the worker to attach its run-loop sources, register
    // notifications, and prime transfers (if the device was already open).
    for (int i = 0; i < 100 && !mWorkerReady.load(); i++) {
        usleep(1000);
    }
    XLOG("ensureMonitor: worker ready=%d", mWorkerReady.load());
    return mWorkerReady.load();
}

bool XoneUSBTransport::startIO() {
    // Worker thread lifecycle is decoupled from HAL StartIO/StopIO: it runs
    // continuously from Initialize() until process exit. CoreAudio can cycle
    // Start/Stop several times per second when apps reconfigure the device,
    // and even when no client is around the worker has to stay alive to keep
    // the USB hot-plug/power notifications armed. We just flip mFeeding here;
    // the worker keeps producing packets (silence when not feeding — rings
    // are zeroed in stopIO).
    if (!ensureMonitor()) return false;
    mFeeding.store(true);
    // Re-arm the capture read cursor so a fresh client (e.g. Traktor being
    // quit and relaunched) starts reading at the correct lag behind the
    // current device write position instead of from a stale offset. 0 makes
    // readInput resync on its next call. IN capture itself keeps flowing
    // continuously (it isn't gated by feeding), and the worker's watchdog
    // revives it if the USB stream ever died while no client was attached.
    mReadCursor.store(0, std::memory_order_relaxed);
    XLOG("startIO: feeding on, deviceOpen=%d", mDeviceOpen.load());
    return mDeviceOpen.load();
}

void XoneUSBTransport::stopIO() {
    // Don't actually stop the worker — just flip feeding off and clear the
    // rings. submitOut will continue firing on the timer and produce all-zero
    // USB packets (silence) until the next startIO restores mFeeding=true.
    // See decoupling rationale in startIO().
    if (!mRunning.load()) return;
    mFeeding.store(false);
    mOutRing.zero();
    mInRing.zero();
    XLOG("stopIO: pause feeding (worker stays alive)");
}

void XoneUSBTransport::shutdownWorker() {
    if (!mRunning.load()) return;
    mShutdown.store(true);
    if (mWorkerLoop) CFRunLoopWakeUp(mWorkerLoop);
    pthread_join(mWorker, nullptr);
    mWorker = 0;
    mWorkerLoop = nullptr;
    mRunning.store(false);
    mFeeding.store(false);
    XLOG("worker shut down");
}

bool XoneUSBTransport::submitOut(uint32_t slot) {
    if (!mOutPipeIntf || !mOutPipeRef) return false;
    if (mSuspended.load()) return false;

    // Build a Ploytec OUT packet from current ring contents.
    // Sub-packet layout (482 bytes) — verified against A&H's 3.3.17 kext
    // `dmaEncode10_AH_Int964_C`:
    //   [0..431]   432 B  audio frames 0..8 (9 × 48 B)
    //   [432..433]   2 B  MIDI hole (PLOYTEC_MIDI_IDLE_BYTE per protocol)
    //   [434..481]  48 B  audio frame 9     (1 × 48 B)
    // The MIDI bytes sit BETWEEN frame 8 and frame 9, NOT at the trailing
    // end. Putting MIDI at [480..481] (the old assumption) overwrites the
    // last 2 bytes of frame 9's audio while simultaneously leaving the
    // device to interpret bytes [432..433] (real MIDI slot) as audio — that
    // misalignment produces the 9600 Hz (= sub-packet rate) signal-modulated
    // alias spurs visible in the spectrum at -27 dB.
    uint8_t* pkt = mOutBuf[slot];
    std::memset(pkt, 0, PLOYTEC_INT_OUT_PKT_SIZE);
    for (uint32_t sp = 0; sp < 8; sp++) {
        pkt[sp * 482 + 432] = PLOYTEC_MIDI_IDLE_BYTE;
        pkt[sp * 482 + 433] = PLOYTEC_MIDI_IDLE_BYTE;
    }

    // Discrete cursor advancing by exactly kFramesPerXfer per submit — drift-
    // free in steady state and avoids the per-packet bit-jitter that a pure
    // wall-clock cursor produces. But if a dispatch tick is ever skipped
    // (heavy CPU load, system sleep, etc.) the cursor falls behind permanently
    // and audio reads stale ring data ever after — heard as a spontaneous
    // sustained hiss that only a worker restart clears. Cross-check against
    // wall-clock-derived position on every tick; if we've drifted by more
    // than 10 frames, snap to the host-clock expected value (CoreAudio uses
    // the same anchor in GetZeroTimeStamp, so ring offsets stay aligned).
    uint64_t cursor = mOutSampleTime.load(std::memory_order_acquire);
    {
        const uint64_t anchor = mAnchorHostTime.load(std::memory_order_acquire);
        const double   tpf    = mHostTicksPerFrame;
        if (anchor != 0 && tpf > 0.0) {
            const uint64_t now      = mach_absolute_time();
            const uint64_t elapsed  = (now > anchor) ? (now - anchor) : 0;
            const uint64_t expected = (uint64_t)((double)elapsed / tpf);
            const int64_t  drift    = (int64_t)cursor - (int64_t)expected;
            if (drift < -10 || drift > 10) {
                /* Snap quietly for small drift (the normal case under load).
                 * Log only the big outliers so the system log isn't flooded. */
                if (drift < -100 || drift > 100) {
                    XLOG("OUT cursor drift %lld frames; snap to expected",
                         (long long)drift);
                }
                cursor = expected;
            }
        }
    }
    const uint64_t sStart = cursor + kSafetyOffsetFrames;
    for (uint32_t frame = 0; frame < kFramesPerXfer; frame++) {
        const uint32_t ringOffset = (uint32_t)(((sStart + frame) * kBytesPerFrame) % kRingBytes);
        uint8_t pcm24[kBytesPerFrame];
        mOutRing.peek_at(ringOffset, pcm24, kBytesPerFrame);

        const uint32_t subPkt   = frame / 10;
        const uint32_t inSub    = frame % 10;
        // Frame 9 of each sub-packet lives AFTER the 2-byte MIDI hole at
        // offset 432..433; frames 0..8 sit contiguously before it.
        const uint32_t usbOff   = subPkt * 482 + inSub * PLOYTEC_OUT_FRAME_SIZE
                                  + (inSub == 9 ? 2 : 0);
        ploytec_encode_frame(pkt + usbOff, pcm24);
    }
    mOutSampleTime.store(cursor + kFramesPerXfer, std::memory_order_release);

    IOReturn r = (*mOutPipeIntf)->WritePipeAsync(mOutPipeIntf, mOutPipeRef,
                                                  pkt, PLOYTEC_INT_OUT_PKT_SIZE,
                                                  &XoneUSBTransport::outCompletionTrampoline,
                                                  &mOutCtx[slot]);
    if (r != kIOReturnSuccess) {
        XLOG("WritePipeAsync slot %u failed: 0x%x", slot, r);
        // The sync return is where unplug usually shows up — completion may
        // never fire because the pipe is already torn down.
        if (r == kIOReturnNoDevice || r == kIOReturnNotResponding) {
            handleDeviceLost();
        }
        return false;
    }
    return true;
}

bool XoneUSBTransport::submitIn(uint32_t slot) {
    if (!mInPipeIntf || !mInPipeRef) return false;
    if (mSuspended.load()) return false;
    IOReturn r = (*mInPipeIntf)->ReadPipeAsync(mInPipeIntf, mInPipeRef,
                                                mInBuf[slot], PLOYTEC_INT_IN_PKT_SIZE,
                                                &XoneUSBTransport::inCompletionTrampoline,
                                                &mInCtx[slot]);
    if (r != kIOReturnSuccess) {
        XLOG("ReadPipeAsync slot %u failed: 0x%x", slot, r);
        if (r == kIOReturnNoDevice || r == kIOReturnNotResponding) {
            handleDeviceLost();
        }
        return false;
    }
    return true;
}

// IOUSBLib passes the per-transfer refcon and the byte count in arg0.
void XoneUSBTransport::inCompletionTrampoline(void* refcon, IOReturn result, void* arg0) {
    auto* ctx = (CompletionCtx*)refcon;
    if (!ctx || !ctx->self) return;
    ctx->self->onInComplete(ctx->slot, result, (uint32_t)(uintptr_t)arg0);
}

void XoneUSBTransport::midiInCompletionTrampoline(void* refcon, IOReturn result, void* arg0) {
    auto* ctx = (CompletionCtx*)refcon;
    if (!ctx || !ctx->self) return;
    ctx->self->onMidiInComplete(ctx->slot, result, (uint32_t)(uintptr_t)arg0);
}

bool XoneUSBTransport::submitMidiIn(uint32_t slot) {
    if (!mMidiInPipeIntf || !mMidiInPipeRef) return false;
    if (mSuspended.load()) return false;
    IOReturn r = (*mMidiInPipeIntf)->ReadPipeAsync(mMidiInPipeIntf, mMidiInPipeRef,
                                                   mMidiInBuf[slot], kMidiInBufSize,
                                                   &XoneUSBTransport::midiInCompletionTrampoline,
                                                   &mMidiInCtx[slot]);
    if (r != kIOReturnSuccess && r != kIOReturnAborted) {
        XLOG("ReadPipeAsync MIDI-IN slot %u failed: 0x%x", slot, r);
        if (r == kIOReturnNoDevice || r == kIOReturnNotResponding) {
            handleDeviceLost();
        }
        return false;
    }
    return true;
}

void XoneUSBTransport::onMidiInComplete(uint32_t slot, IOReturn result, uint32_t bytes) {
    if (mShutdown.load()) return;
    if (result == kIOReturnNoDevice || result == kIOReturnNotResponding) {
        handleDeviceLost();
        return;
    }

    if (result == kIOReturnSuccess && bytes > 0) {
        mMidi.feedIn(mMidiInBuf[slot], bytes);
    } else if (result != kIOReturnAborted && result != kIOReturnSuccess) {
        if (result == kIOUSBPipeStalled && mMidiInPipeIntf) {
            (*mMidiInPipeIntf)->ClearPipeStallBothEnds(mMidiInPipeIntf, mMidiInPipeRef);
        }
    }
    if (!mShutdown.load() && !mSuspended.load()) submitMidiIn(slot);
}

void XoneUSBTransport::outCompletionTrampoline(void* refcon, IOReturn result, void* arg0) {
    auto* ctx = (CompletionCtx*)refcon;
    if (!ctx || !ctx->self) return;
    ctx->self->onOutComplete(ctx->slot, result, (uint32_t)(uintptr_t)arg0);
}

void XoneUSBTransport::onInComplete(uint32_t slot, IOReturn result, uint32_t bytes) {
    if (mShutdown.load()) return;
    if (result == kIOReturnNoDevice || result == kIOReturnNotResponding) {
        handleDeviceLost();
        return;
    }
    if (result == kIOReturnSuccess && bytes > 0) {
        const uint32_t frames = bytes / PLOYTEC_IN_FRAME_SIZE;
        const uint32_t safe = (frames > kFramesPerXfer) ? kFramesPerXfer : frames;

        // Plain monotonic write cursor — lay the captured frames down
        // contiguously, advancing by exactly the number delivered. NO
        // wall-clock snapping here: the device is the capture clock, and
        // readInput follows this same cursor at a fixed lag (see
        // kInputReadLagFrames), so reader and writer share one clock and the
        // reader can never lap the writer. Earlier attempts to align this
        // cursor to the host wall-clock either left silence (block written
        // ahead of the reader) or let the host-clock reader slide into the
        // gap during a USB stall and replay the ring — the "reversed loop"
        // then freeze the field logs showed.
        const uint64_t sStart = mInSampleTime.load(std::memory_order_acquire);

        for (uint32_t f = 0; f < safe; f++) {
            uint8_t pcm24[kBytesPerFrame];
            ploytec_decode_frame(pcm24, mInBuf[slot] + f * PLOYTEC_IN_FRAME_SIZE);
            uint32_t ringOffset = (uint32_t)(((sStart + f) * kBytesPerFrame) % kRingBytes);
            mInRing.poke_at(ringOffset, pcm24, kBytesPerFrame);
        }
        const uint64_t newWrite = sStart + safe;
        mInSampleTime.store(newWrite, std::memory_order_release);

        // Bounded diagnostics: confirm the reader stays a fixed lag behind the
        // writer (gap ≈ kInputReadLagFrames, steady). First few completions
        // then a ~2 s heartbeat.
        {
            const double tpf = mHostTicksPerFrame;
            const uint64_t now = mach_absolute_time();
            const uint64_t twoSecTicks = (tpf > 0.0)
                ? (uint64_t)(2.0 * tpf * (double)mSampleRate) : 0;
            const uint64_t readPos = mLastReadSampleTime.load(std::memory_order_relaxed);
            const int64_t  gap = (int64_t)newWrite - (int64_t)readPos;
            if (mInDiagCount < 8) {
                XLOG("IN diag #%u: bytes=%u frames=%u write=%llu read=%llu gap=%lld",
                     mInDiagCount, bytes, frames,
                     (unsigned long long)newWrite, (unsigned long long)readPos,
                     (long long)gap);
                mInDiagCount++;
                mInDiagLastLogTick = now;
            } else if (twoSecTicks && now - mInDiagLastLogTick > twoSecTicks) {
                XLOG("IN diag: write=%llu read=%llu gap=%lld (heartbeat)",
                     (unsigned long long)newWrite, (unsigned long long)readPos,
                     (long long)gap);
                mInDiagLastLogTick = now;
            }
        }
    } else if (result != kIOReturnAborted) {
        // Clear stall and continue.
        if (result == kIOUSBPipeStalled && mInPipeIntf) {
            (*mInPipeIntf)->ClearPipeStallBothEnds(mInPipeIntf, mInPipeRef);
        }
    }
    if (!mShutdown.load() && !mSuspended.load()) submitIn(slot);
}

void XoneUSBTransport::onOutComplete(uint32_t /*slot*/, IOReturn result, uint32_t /*bytes*/) {
    if (mShutdown.load()) return;
    if (result == kIOReturnNoDevice || result == kIOReturnNotResponding) {
        handleDeviceLost();
        return;
    }
    if (result != kIOReturnSuccess && result != kIOReturnAborted) {
        if (result == kIOUSBPipeStalled && mOutPipeIntf) {
            (*mOutPipeIntf)->ClearPipeStallBothEnds(mOutPipeIntf, mOutPipeRef);
        }
    }
    // OUT resubmission is timer-driven (outTimerCallback) to pace packets at
    // the audio-clock rate; do NOT resubmit from completion, that would race
    // with the timer and double-fire the USB pipeline.
}

// ───────────────────────────────────────────────────────────────────────────
// HAL-side bridge: read / write absolute-time-indexed PCM frames
// ───────────────────────────────────────────────────────────────────────────

// Both the HAL stream format and the ring carry packed S24_3LE (24 B/frame).
// No conversion — pass bytes straight through. The Ploytec wire encoding is
// done on the USB worker thread via ploytec_encode_frame / ploytec_decode_frame.
void XoneUSBTransport::readInput(uint64_t sampleTime, uint32_t frames, uint8_t* dst) {
    // Capture is locked to the DEVICE clock, not the host clock. We ignore the
    // absolute CoreAudio sampleTime for ring positioning and instead serve the
    // `frames`-frame window that ends kInputReadLagFrames behind the newest
    // device-written frame. Because the read pointer is derived from the same
    // counter the writer advances, the two share one clock: the reader can
    // never overrun the writer (no host-vs-device drift, no lapping), and a
    // brief USB stall just re-serves the most recent frames instead of reading
    // uninitialised/lapped ring memory (which sounded like a reversed loop and
    // then wedged). The captured stream may slip by sub-millisecond amounts vs
    // CoreAudio's nominal input timeline — inaudible for monitoring/recording
    // and a fair trade for freeze-proof capture.
    const uint64_t w = mInSampleTime.load(std::memory_order_acquire);
    const uint64_t target = (w > kInputReadLagFrames) ? (w - kInputReadLagFrames) : 0;

    // Monotonic read cursor: normally advance by exactly `frames` so the
    // served audio is perfectly contiguous (no per-call ±80-frame jitter from
    // sampling the bursty write counter). Re-sync to the target lag only when
    // we'd otherwise overrun the writer (cursor caught up — underrun risk) or
    // we've fallen more than one extra lag behind (host reading slower than
    // the device). Both conditions are rare clock-drift corrections, so the
    // discontinuity they introduce is infrequent and small.
    uint64_t rc = mReadCursor.load(std::memory_order_relaxed);
    const bool overrun  = (rc + frames > w);                 // would read unwritten
    const bool tooFarBehind = (target > rc + frames + kInputReadLagFrames);
    if (rc == 0 || overrun || tooFarBehind) {
        rc = (target >= frames) ? (target - frames) : 0;     // resync to target
    }
    const uint64_t startFrame = rc;
    mReadCursor.store(rc + frames, std::memory_order_relaxed); // advance monotonically

    // Surface the served position for the worker-thread diagnostics.
    mLastReadSampleTime.store(startFrame + frames, std::memory_order_relaxed);

    const uint32_t bytes = frames * kBytesPerFrame;
    const uint32_t base  = (uint32_t)((startFrame * kBytesPerFrame) % kRingBytes);
    if (base + bytes <= kRingBytes) {
        mInRing.peek_at(base, dst, bytes);
    } else {
        const uint32_t first = kRingBytes - base;
        mInRing.peek_at(base, dst, first);
        mInRing.peek_at(0, dst + first, bytes - first);
    }
}

void XoneUSBTransport::writeOutput(uint64_t sampleTime, uint32_t frames, const uint8_t* src) {
    const uint32_t bytes = frames * kBytesPerFrame;
    const uint32_t base  = (uint32_t)((sampleTime * kBytesPerFrame) % kRingBytes);
    if (base + bytes <= kRingBytes) {
        mOutRing.poke_at(base, src, bytes);
    } else {
        const uint32_t first = kRingBytes - base;
        mOutRing.poke_at(base, src, first);
        mOutRing.poke_at(0, src + first, bytes - first);
    }
}

// Compute zero-timestamp anchors from the single startIO host-time anchor.
// CoreAudio expects anchors at exact multiples of kZeroTSPeriod (16384 frames)
// and a stable seed for the duration of an IO session. Mirrors Apple's
// NullAudio sample driver: the device is treated as host-clock-locked, with
// any actual drift between the USB clock and host clock handled by the HAL's
// rate scaler downstream.
void XoneUSBTransport::getZeroTimeStamp(double* outSampleTime, uint64_t* outHostTime, uint64_t* outSeed) {
    const uint64_t anchor = mAnchorHostTime.load(std::memory_order_acquire);
    *outSeed = mClockSeed.load(std::memory_order_acquire);

    if (anchor == 0 || mHostTicksPerFrame <= 0.0) {
        *outHostTime   = mach_absolute_time();
        *outSampleTime = 0.0;
        return;
    }

    const uint64_t now = mach_absolute_time();
    const uint64_t elapsedTicks = (now > anchor) ? (now - anchor) : 0;
    const double   elapsedFrames = (double)elapsedTicks / mHostTicksPerFrame;
    const double   zeroFrame     = std::floor(elapsedFrames / (double)kZeroTSPeriod)
                                 * (double)kZeroTSPeriod;
    const uint64_t zeroHostTime  = anchor + (uint64_t)(zeroFrame * mHostTicksPerFrame);

    *outSampleTime = zeroFrame;
    *outHostTime   = zeroHostTime;
}
