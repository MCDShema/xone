/* XoneUSBTransport.h – user-space USB transport for Xone:4D, using IOUSBLib.
 * MIT License.
 *
 * Owns:
 *   - IOUSBDeviceInterface / IOUSBInterfaceInterface handles
 *   - A dedicated USB worker thread running a CFRunLoop for async completions
 *   - Two SPSC rings (input + output) that exchange packed S24_3LE PCM with the
 *     AudioServerPlugIn DoIOOperation callback.
 *
 *   The ring layout: 8 channels × 3 bytes/sample = 24 bytes/frame.
 *   Total ring size: kRingFrames * 24 bytes (power-of-two for cheap masking).
 *
 *   Frame-time is tracked monotonically; producer and consumer index by absolute
 *   sample-time, mod ring capacity, so no need for back-pressure.
 */
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <atomic>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include "XoneMidi.h"
#include "XoneRing.h"

class XoneUSBTransport {
public:
    static constexpr uint32_t kChannels         = 8;
    static constexpr uint32_t kBytesPerFrame    = 24;          // 8 × 24-bit packed
    static constexpr uint32_t kRingFrames       = 16384;       // ring depth in frames
    static constexpr uint32_t kRingBytes        = kRingFrames * kBytesPerFrame;
    static constexpr uint32_t kFramesPerXfer    = 80;          // PLOYTEC_FRAMES_PER_PKT
    static constexpr uint32_t kDoubleBuffer     = 4;           // outstanding USB transfers per direction
    // Must mirror kSafetyOffset reported via kAudioDevicePropertySafetyOffset
    // in Xone4DHAL.cpp. CoreAudio writes ahead of the device cursor by this
    // many frames, so the USB worker must lift its read cursor by the same
    // amount to land on the slots HAL just filled.
    static constexpr uint32_t kSafetyOffsetFrames = 80;

    // Capture (IN) read lag. readInput serves frames trailing the newest
    // device-written frame by this many frames, so the HAL read pointer
    // follows the device-clocked write pointer on the SAME clock and can
    // never lap it (no host-vs-device drift, survives brief USB stalls).
    // ~21 ms at 48 kHz — comfortably more than any CoreAudio input buffer,
    // trading a little monitoring latency for freeze-proof capture.
    static constexpr uint32_t kInputReadLagFrames = 1024;

    static XoneUSBTransport& shared();

    XoneUSBTransport();
    ~XoneUSBTransport();

    /* Notified whenever the underlying USB device becomes available (true) or
       goes away (false). Used by the HAL plug-in to publish or hide the
       AudioDevice object so the system tree matches the physical state. The
       callback fires on the worker thread; keep it short. */
    using AvailabilityHandler = void(*)(bool alive);
    void setAvailabilityHandler(AvailabilityHandler h) { mAvailHandler = h; }

    /* Probe for the device on the bus. Returns true if VID/PID 0x0A4A:0xFF4D
       is present. Does not open it. */
    bool deviceConnected();

    /* Open the device, run Ploytec handshake, leave it idle (no streaming). */
    bool openAndHandshake();

    /* Start the worker thread that owns IOKit hot-plug + power notifications.
       Idempotent and safe to call before the USB device is opened — in that
       case the worker still arms the hot-plug listener, so a later USB plug-in
       triggers reopenDeviceAfterWake automatically. Must be called from the
       HAL plug-in's Initialize before audio clients begin StartIO/StopIO. */
    bool ensureMonitor();

    /* Start interrupt OUT/IN streaming + run loop. Idempotent. */
    bool startIO();

    /* Stop streaming and tear down USB transfers; keep the device open. */
    void stopIO();

    /* Close the USB device entirely. */
    void closeDevice();

    /* Change hardware sample rate (44.1k / 48k / 88.2k / 96k). Safe to call
       while idle; while streaming it will stop, set, restart. */
    bool setSampleRate(uint32_t rate);

    /* Consumer-side (HAL DoIOOperation). The HAL passes its absolute sample
       time and frame count; the transport copies the corresponding chunk of
       PCM data from / into the rings. */
    void readInput(uint64_t sampleTime, uint32_t frames, uint8_t* dst);
    void writeOutput(uint64_t sampleTime, uint32_t frames, const uint8_t* src);

    /* Current device-clock anchor. Sample time and host time at which the
       last IN packet completed. Used by HAL GetZeroTimeStamp. */
    void getZeroTimeStamp(double* outSampleTime, uint64_t* outHostTime, uint64_t* outSeed);

    /* Whether device is plugged in & opened. */
    bool isOpen() const { return mDeviceOpen.load(); }
    bool isRunning() const { return mRunning.load(); }

    uint32_t currentSampleRate() const { return mSampleRate; }

private:
    // ── USB plumbing ──
    bool       findDevice(io_service_t* outService);
    bool       openInterfaces();
    bool       claimPipes();
    bool       findPipeRef(IOUSBInterfaceInterface942** intf, uint8_t epAddr, uint8_t* outRef);
    IOReturn   controlRequest(uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              void* data, uint16_t length);
    bool       doHandshake();
    bool       sendSampleRateCmd(uint32_t rate);
    void       teardownInterfaces();

    // ── Async transfers ──
    bool submitOut(uint32_t slot);
    bool submitIn(uint32_t slot);
    bool submitMidiIn(uint32_t slot);
    static void inCompletionTrampoline(void* refcon, IOReturn result, void* arg0);
    static void outCompletionTrampoline(void* refcon, IOReturn result, void* arg0);
    static void midiInCompletionTrampoline(void* refcon, IOReturn result, void* arg0);
    void onInComplete(uint32_t slot, IOReturn result, uint32_t bytes);
    void onOutComplete(uint32_t slot, IOReturn result, uint32_t bytes);
    void onMidiInComplete(uint32_t slot, IOReturn result, uint32_t bytes);
    static void outTimerHandler(void* ctx);

    // ── Worker thread ──
    static void* workerEntry(void* arg);
    void workerLoop();
    void shutdownWorker();

    // ── System power events (sleep/wake) ──
    static void powerCallbackTrampoline(void* refCon, io_service_t service,
                                        uint32_t messageType, void* messageArgument);
    void onPowerEvent(uint32_t messageType, void* messageArgument);
    // immediate=true skips the post-wake re-enumeration delay — used by the
    // capture watchdog, where the device never left the bus and openAndHandshake
    // succeeds on the first try (keeps the recovery blip short).
    void reopenDeviceAfterWake(bool immediate = false);
    void attachInterfaceEventSources();

    // ── USB hot-plug (unplug / re-plug while running) ──
    void setupHotplugNotification();
    void teardownHotplugNotification();
    void registerDeviceInterestNotification();
    static void hotplugCallbackTrampoline(void* refCon, io_iterator_t iter);
    static void deviceInterestTrampoline(void* refCon, io_service_t service,
                                         uint32_t messageType, void* messageArgument);
    void onHotplug(io_iterator_t iter);
    void handleDeviceLost();

    // ── State ──
    IOUSBDeviceInterface942**     mDevice = nullptr;
    IOUSBInterfaceInterface942**  mIntf0  = nullptr;
    IOUSBInterfaceInterface942**  mIntf1  = nullptr;
    CFRunLoopSourceRef            mIntf0Src = nullptr;
    CFRunLoopSourceRef            mIntf1Src = nullptr;
    dispatch_queue_t              mTimerQueue = nullptr;
    dispatch_source_t             mOutDispatchTimer = nullptr;
    uint32_t                      mOutPingPong = 0;

    // Power management (sleep/wake)
    io_connect_t                  mPowerConnect = 0;
    IONotificationPortRef         mPowerPort = nullptr;
    io_object_t                   mPowerNotifier = 0;
    std::atomic<bool>             mSuspended{false};

    // Hot-plug — device unplugged from USB and may be re-plugged later
    IONotificationPortRef         mHotplugPort = nullptr;
    io_iterator_t                 mHotplugIter = IO_OBJECT_NULL;
    io_object_t                   mDeviceInterestNotifier = IO_OBJECT_NULL;
    std::atomic<bool>             mDeviceLost{false};

    uint8_t  mOutPipeRef = 0;    // 1-based index inside owning interface
    uint8_t  mInPipeRef  = 0;
    uint8_t  mMidiInPipeRef = 0;
    IOUSBInterfaceInterface942** mOutPipeIntf    = nullptr; // which intf owns PCM OUT
    IOUSBInterfaceInterface942** mInPipeIntf     = nullptr; // which intf owns PCM IN
    IOUSBInterfaceInterface942** mMidiInPipeIntf = nullptr; // which intf owns MIDI IN
    static constexpr uint32_t kMidiInBufSize = 512;
    static constexpr uint32_t kMidiInSlots   = 4;
    XoneMidi mMidi;

    uint32_t mSampleRate = 48000;

    pthread_t       mWorker = 0;
    CFRunLoopRef    mWorkerLoop = nullptr;
    std::atomic<bool> mWorkerReady{false};
    std::atomic<bool> mShutdown{false};
    std::atomic<bool> mRunning{false};     // USB worker thread alive
    std::atomic<bool> mFeeding{false};     // HAL StartIO/StopIO state
    std::atomic<bool> mDeviceOpen{false};

    // USB buffers
    uint8_t* mOutBuf[kDoubleBuffer] = {};
    uint8_t* mInBuf [kDoubleBuffer] = {};

    // Per-slot async completion context (refcon for IOUSBLib callbacks).
    struct CompletionCtx {
        XoneUSBTransport* self;
        uint32_t          slot;
    };
    CompletionCtx mOutCtx[kDoubleBuffer];
    CompletionCtx mInCtx [kDoubleBuffer];
    uint8_t* mMidiInBuf[kMidiInSlots] = {};
    CompletionCtx mMidiInCtx[kMidiInSlots];

    // Diagnostics for the capture (IN) path — bounded logging only.
    uint32_t              mInDiagCount = 0;    // first-N completion byte logs
    uint64_t              mInDiagLastLogTick = 0; // throttle drift logging
    std::atomic<uint64_t> mLastReadSampleTime{0}; // last served read-end (diag)
    std::atomic<uint64_t> mReadCursor{0};         // monotonic capture read pos;
                                                  // 0 ⇒ readInput resyncs to the
                                                  // current write lag (used to
                                                  // re-arm cleanly on StartIO)

    // Sample-time bookkeeping
    std::atomic<uint64_t> mInSampleTime{0};   // device input frames received
    std::atomic<uint64_t> mOutSampleTime{0};  // host output frames consumed
    std::atomic<uint64_t> mAnchorHostTime{0}; // mach_absolute_time at IO start
    double                mHostTicksPerFrame = 0.0;
    std::atomic<uint64_t> mClockSeed{1};
    static constexpr uint32_t kZeroTSPeriod = 16384;

    XoneRing mInRing;
    XoneRing mOutRing;

    AvailabilityHandler mAvailHandler = nullptr;
};
