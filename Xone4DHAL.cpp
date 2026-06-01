/* Xone4DHAL.cpp – AudioServerPlugIn driver for Allen & Heath Xone:4D.
 * MIT License.
 *
 * Architecture
 * ────────────
 * Audio HAL ──pull──> DoIOOperation ──> XoneUSBTransport rings ──> USB endpoints
 *
 *   Object hierarchy:
 *     PlugIn   (id=1, kAudioObjectPlugInObject)
 *     Device   (id=2)
 *       Stream input  (id=3)
 *       Stream output (id=4)
 *
 *   Streams expose 8-channel S24_3LE packed PCM at 44.1 / 48 / 88.2 / 96 kHz.
 *   Both physical and virtual formats are identical → no conversion in plugin.
 *   The XoneUSBTransport instance handles the Ploytec wire format on its own
 *   worker thread.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <atomic>
#include <cstring>

#include "XoneUSBTransport.h"

#define HLOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[XoneHAL] " fmt, ##__VA_ARGS__)

// ───────────────────────────────────────────────────────────────────────────
// Object IDs
// ───────────────────────────────────────────────────────────────────────────
static constexpr AudioObjectID kObjectID_PlugIn        = kAudioObjectPlugInObject; // = 1
static constexpr AudioObjectID kObjectID_Device        = 2;
static constexpr AudioObjectID kObjectID_Stream_Input  = 3;
static constexpr AudioObjectID kObjectID_Stream_Output = 4;

// ───────────────────────────────────────────────────────────────────────────
// Audio format
// ───────────────────────────────────────────────────────────────────────────
static constexpr UInt32  kChannels       = 8;
static constexpr UInt32  kBitsPerSample  = 24;
static constexpr UInt32  kBytesPerSample = 3;
static constexpr UInt32  kBytesPerFrame  = kChannels * kBytesPerSample;
static constexpr UInt32  kZeroTSPeriod   = 16384;  // ≥ 10923 frames
static constexpr UInt32  kSafetyOffset   = 80;     // one USB packet
static constexpr UInt32  kDeviceLatency  = 256;
static constexpr UInt32  kStreamLatency  = 0;

static const Float64 kSupportedRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
static constexpr UInt32 kNumRates = sizeof(kSupportedRates) / sizeof(kSupportedRates[0]);

// ───────────────────────────────────────────────────────────────────────────
// Global plugin state
// ───────────────────────────────────────────────────────────────────────────
static AudioServerPlugInHostRef gHost = nullptr;
static pthread_mutex_t          gStateLock = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<UInt32>      gRefCount{1};
static std::atomic<Float64>     gNominalRate{48000.0};
static std::atomic<bool>        gStreamInputActive{true};
static std::atomic<bool>        gStreamOutputActive{true};
static std::atomic<bool>        gDeviceRunning{false};
// True while the USB hardware is actually present on the bus. Toggled by the
// availability callback from XoneUSBTransport. When false:
//   - kAudioPlugInPropertyDeviceList reports an empty list (the device row
//     disappears from Audio MIDI Setup, Traktor, etc.).
//   - kAudioDevicePropertyDeviceIsAlive reports 0 for clients that already
//     hold a reference. This is exactly the behaviour of a normal USB sound
//     card: plug it in and it shows up, yank it and it goes away.
static std::atomic<bool>        gDeviceAlive{false};

// Tell CoreAudio that something about the plug-in/device tree changed. Called
// from the USB worker thread when the hardware presence flips — gHost-side
// dispatch handles the cross-thread hand-off internally.
static void NotifyDeviceAvailability(bool alive) {
    bool prev = gDeviceAlive.exchange(alive);
    if (prev == alive) return;
    HLOG("availability: %s", alive ? "device alive" : "device gone");
    if (!gHost) return;
    AudioObjectPropertyAddress addr{
        0,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    // Plug-in object's device list changed (the count toggles between 0 and 1).
    addr.mSelector = kAudioPlugInPropertyDeviceList;
    gHost->PropertiesChanged(gHost, kObjectID_PlugIn, 1, &addr);
    // For any client that already kept a reference to the device object, flip
    // its IsAlive so it stops trying to use the (now dead) endpoint.
    addr.mSelector = kAudioDevicePropertyDeviceIsAlive;
    gHost->PropertiesChanged(gHost, kObjectID_Device, 1, &addr);
}

// ───────────────────────────────────────────────────────────────────────────
// Helpers
// ───────────────────────────────────────────────────────────────────────────
// Both virtual and physical formats are the wire format (8-ch packed S24_3LE).
// CoreAudio clients (DAWs, Traktor) will use AudioConverter to bridge their
// Float32 mix to this. Matching the DriverKit version exactly so the wire data
// passes through untouched.
static AudioStreamBasicDescription MakeASBD(Float64 sampleRate) {
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate       = sampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = (AudioFormatFlags)kAudioFormatFlagIsSignedInteger
                           | (AudioFormatFlags)kAudioFormatFlagsNativeEndian
                           | (AudioFormatFlags)kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket   = kBytesPerFrame;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = kBytesPerFrame;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel   = kBitsPerSample;
    return asbd;
}

static OSStatus WriteCFString(CFStringRef s, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    *(CFStringRef*)outData = s;  // caller releases
    CFRetain(s);
    *outDataSize = sizeof(CFStringRef);
    (void)inDataSize;
    return noErr;
}

// ───────────────────────────────────────────────────────────────────────────
// Driver interface forward declaration
// ───────────────────────────────────────────────────────────────────────────
extern "C" __attribute__((visibility("default")))
void* Xone4DHAL_Create(CFAllocatorRef, CFUUIDRef typeUUID);

// ───────────────────────────────────────────────────────────────────────────
// IUnknown
// ───────────────────────────────────────────────────────────────────────────
static HRESULT QueryInterface(void* driver, REFIID iid, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, iid);
    if (!requested) return E_INVALIDARG;
    CFUUIDRef iUnknown = IUnknownUUID;
    CFUUIDRef iPlugIn  = kAudioServerPlugInDriverInterfaceUUID;
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, iUnknown) || CFEqual(requested, iPlugIn)) {
        gRefCount.fetch_add(1);
        *outInterface = driver;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG AddRef(void*)  { return gRefCount.fetch_add(1) + 1; }
static ULONG Release(void*) { return gRefCount.fetch_sub(1) - 1; }

// ───────────────────────────────────────────────────────────────────────────
// Initialize / device lifecycle
// ───────────────────────────────────────────────────────────────────────────
static OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
    gHost = inHost;
    HLOG("Initialize");
    // Hook up availability before opening so we cover the initial-open case
    // too (transport's openAndHandshake fires the handler once it succeeds).
    XoneUSBTransport::shared().setAvailabilityHandler(&NotifyDeviceAvailability);
    // Probe the device at init time. If the USB isn't on the bus yet this is
    // expected to fail — the worker thread we start next will watch for the
    // hot-plug event and open the device whenever it appears.
    XoneUSBTransport::shared().openAndHandshake();
    // Start the monitor worker regardless of whether the open succeeded. It
    // holds the IOKit hot-plug + sleep/wake notifications, which is exactly
    // what's needed for the device to come back online after idle/sleep,
    // coreaudiod re-spawn, or first connect — without it the plug-in stays
    // blind to the bus and the user has to `killall coreaudiod`.
    XoneUSBTransport::shared().ensureMonitor();
    return noErr;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*)   { return noErr; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return noErr; }

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64 inChangeAction, void*) {
    // We use inChangeAction as the new nominal sample rate (rounded).
    uint32_t newRate = (uint32_t)inChangeAction;
    if (newRate != 44100 && newRate != 48000 && newRate != 88200 && newRate != 96000)
        return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateLock);
    gNominalRate.store((Float64)newRate);
    XoneUSBTransport::shared().setSampleRate(newRate);
    pthread_mutex_unlock(&gStateLock);
    return noErr;
}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return noErr; }

// ───────────────────────────────────────────────────────────────────────────
// HasProperty / IsPropertySettable
// ───────────────────────────────────────────────────────────────────────────
static Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t,
                           const AudioObjectPropertyAddress* inAddress) {
    if (!inAddress) return false;

    switch (inObjectID) {
    case kObjectID_PlugIn:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        }
        return false;

    case kObjectID_Device:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyPreferredChannelsForStereo:
            return true;
        }
        return false;

    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t,
                                   const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    *outIsSettable = false;
    if (!inAddress) return kAudioHardwareUnknownPropertyError;
    if (inObjectID == kObjectID_Device) {
        switch (inAddress->mSelector) {
        case kAudioDevicePropertyNominalSampleRate:
            *outIsSettable = true;
            return noErr;
        }
    }
    if (inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) {
        switch (inAddress->mSelector) {
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = true;
            return noErr;
        }
    }
    return noErr;
}

// ───────────────────────────────────────────────────────────────────────────
// GetPropertyDataSize
// ───────────────────────────────────────────────────────────────────────────
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t,
                                    const AudioObjectPropertyAddress* inAddress,
                                    UInt32, const void*, UInt32* outDataSize) {
    if (!inAddress || !outDataSize) return kAudioHardwareUnknownPropertyError;

    switch (inObjectID) {
    case kObjectID_PlugIn:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:                   *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:          *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            // Empty list when the hardware is gone — same effect as if the
            // device had never been published.
            *outDataSize = gDeviceAlive.load() ? sizeof(AudioObjectID) : 0;
            return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice:    *outDataSize = sizeof(AudioObjectID); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;

    case kObjectID_Device:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:                            *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:                         *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyZeroTimeStampPeriod:              *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyNominalSampleRate:                *outDataSize = sizeof(Float64); return noErr;
        case kAudioObjectPropertyOwnedObjects: {
            // device, two streams
            *outDataSize = 2 * sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioDevicePropertyRelatedDevices:                   *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyControlList:                       *outDataSize = 0; return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:      *outDataSize = kNumRates * sizeof(AudioValueRange); return noErr;
        case kAudioDevicePropertyStreams: {
            if (inAddress->mScope == kAudioObjectPropertyScopeInput)        *outDataSize = sizeof(AudioObjectID);
            else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)  *outDataSize = sizeof(AudioObjectID);
            else                                                            *outDataSize = 2 * sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:       *outDataSize = 2 * sizeof(UInt32); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;

    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:               *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyName:                *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:             *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:      *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = kNumRates * sizeof(AudioStreamRangedDescription);
            return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareBadObjectError;
}

// ───────────────────────────────────────────────────────────────────────────
// GetPropertyData
// ───────────────────────────────────────────────────────────────────────────
static OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t,
                                const AudioObjectPropertyAddress* inAddress,
                                UInt32 inQualifierDataSize, const void* inQualifierData,
                                UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    if (!inAddress || !outDataSize || !outData) return kAudioHardwareUnknownPropertyError;
    (void)inQualifierDataSize; (void)inDataSize;

    switch (inObjectID) {
    case kObjectID_PlugIn:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:   *(AudioClassID*)outData = kAudioObjectClassID;  *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass:       *(AudioClassID*)outData = kAudioPlugInClassID;  *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:       *(AudioObjectID*)outData = kAudioObjectUnknown; *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyManufacturer:return WriteCFString(CFSTR("Ozzy / DigitalKiss"), inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (gDeviceAlive.load()) {
                ((AudioObjectID*)outData)[0] = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else {
                *outDataSize = 0;
            }
            return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef requestedUID = (inQualifierData ? *(CFStringRef*)inQualifierData : nullptr);
            if (requestedUID && CFEqual(requestedUID, CFSTR("com.digitalkiss.xone.4d"))) {
                *(AudioObjectID*)outData = kObjectID_Device;
            } else {
                *(AudioObjectID*)outData = kAudioObjectUnknown;
            }
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioPlugInPropertyResourceBundle:
            return WriteCFString(CFSTR(""), inDataSize, outDataSize, outData);
        }
        return kAudioHardwareUnknownPropertyError;

    case kObjectID_Device:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:   *(AudioClassID*)outData = kAudioObjectClassID;  *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass:       *(AudioClassID*)outData = kAudioDeviceClassID;  *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:       *(AudioObjectID*)outData = kObjectID_PlugIn;    *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName:        return WriteCFString(CFSTR("Xone:4D (Ozzy HAL)"), inDataSize, outDataSize, outData);
        case kAudioObjectPropertyManufacturer:return WriteCFString(CFSTR("Allen & Heath"),     inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceUID:   return WriteCFString(CFSTR("com.digitalkiss.xone.4d"), inDataSize, outDataSize, outData);
        case kAudioDevicePropertyModelUID:    return WriteCFString(CFSTR("Xone4D_Model"),           inDataSize, outDataSize, outData);
        case kAudioDevicePropertyTransportType:*(UInt32*)outData = kAudioDeviceTransportTypeUSB;   *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyClockDomain:  *(UInt32*)outData = 0;                                *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsAlive:*(UInt32*)outData = gDeviceAlive.load() ? 1 : 0;       *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsRunning:*(UInt32*)outData = gDeviceRunning.load() ? 1 : 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyIsHidden:     *(UInt32*)outData = 0;                                *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyLatency:      *(UInt32*)outData = kDeviceLatency;                   *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertySafetyOffset: *(UInt32*)outData = kSafetyOffset;                    *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyZeroTimeStampPeriod:*(UInt32*)outData = kZeroTSPeriod;              *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyNominalSampleRate:  *(Float64*)outData = gNominalRate.load();       *outDataSize = sizeof(Float64); return noErr;
        case kAudioObjectPropertyOwnedObjects: {
            ((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
            ((AudioObjectID*)outData)[1] = kObjectID_Stream_Output;
            *outDataSize = 2 * sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioDevicePropertyRelatedDevices:
            ((AudioObjectID*)outData)[0] = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange* ranges = (AudioValueRange*)outData;
            for (UInt32 i = 0; i < kNumRates; i++) {
                ranges[i].mMinimum = kSupportedRates[i];
                ranges[i].mMaximum = kSupportedRates[i];
            }
            *outDataSize = kNumRates * sizeof(AudioValueRange);
            return noErr;
        }
        case kAudioDevicePropertyStreams: {
            if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                ((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
                *outDataSize = sizeof(AudioObjectID);
            } else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
                ((AudioObjectID*)outData)[0] = kObjectID_Stream_Output;
                *outDataSize = sizeof(AudioObjectID);
            } else {
                ((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
                ((AudioObjectID*)outData)[1] = kObjectID_Stream_Output;
                *outDataSize = 2 * sizeof(AudioObjectID);
            }
            return noErr;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            ((UInt32*)outData)[0] = 1;
            ((UInt32*)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return noErr;
        }
        return kAudioHardwareUnknownPropertyError;

    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output: {
        const bool isInput = (inObjectID == kObjectID_Stream_Input);
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:  *(AudioClassID*)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass:      *(AudioClassID*)outData = kAudioStreamClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:      *(AudioObjectID*)outData = kObjectID_Device;   *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName:       return WriteCFString(isInput ? CFSTR("Xone:4D Input") : CFSTR("Xone:4D Output"),
                                                                 inDataSize, outDataSize, outData);
        case kAudioStreamPropertyIsActive:
            *(UInt32*)outData = (isInput ? gStreamInputActive.load() : gStreamOutputActive.load()) ? 1 : 0;
            *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyDirection:  *(UInt32*)outData = isInput ? 1 : 0;             *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyTerminalType:*(UInt32*)outData = isInput ? kAudioStreamTerminalTypeLine
                                                                          : kAudioStreamTerminalTypeLine;
                                              *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyStartingChannel:*(UInt32*)outData = 1;                       *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyLatency:    *(UInt32*)outData = kStreamLatency;              *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *(AudioStreamBasicDescription*)outData = MakeASBD(gNominalRate.load());
            *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription* fmts = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < kNumRates; i++) {
                fmts[i].mFormat = MakeASBD(kSupportedRates[i]);
                fmts[i].mSampleRateRange.mMinimum = kSupportedRates[i];
                fmts[i].mSampleRateRange.mMaximum = kSupportedRates[i];
            }
            *outDataSize = kNumRates * sizeof(AudioStreamRangedDescription);
            return noErr;
        }
        }
        return kAudioHardwareUnknownPropertyError;
    }
    }
    return kAudioHardwareBadObjectError;
}

// ───────────────────────────────────────────────────────────────────────────
// SetPropertyData
// ───────────────────────────────────────────────────────────────────────────
static OSStatus SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID inObjectID, pid_t,
                                const AudioObjectPropertyAddress* inAddress,
                                UInt32, const void*, UInt32 inDataSize, const void* inData) {
    if (!inAddress) return kAudioHardwareUnknownPropertyError;

    if (inObjectID == kObjectID_Device &&
        inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 newRate = *(const Float64*)inData;
        uint32_t rounded = (uint32_t)(newRate + 0.5);
        bool supported = false;
        for (UInt32 i = 0; i < kNumRates; i++)
            if (rounded == (uint32_t)kSupportedRates[i]) { supported = true; break; }
        if (!supported) return kAudioHardwareIllegalOperationError;

        // Ask host to perform configuration change.
        if (gHost) {
            gHost->RequestDeviceConfigurationChange(
                gHost, kObjectID_Device, (UInt64)rounded, nullptr);
        }
        return noErr;
    }

    if ((inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) &&
        inAddress->mSelector == kAudioStreamPropertyIsActive) {
        if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32 isActive = *(const UInt32*)inData;
        if (inObjectID == kObjectID_Stream_Input)  gStreamInputActive.store(isActive != 0);
        else                                       gStreamOutputActive.store(isActive != 0);
        if (gHost) {
            AudioObjectPropertyAddress addrs[] = {{ kAudioStreamPropertyIsActive,
                                                    kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain }};
            gHost->PropertiesChanged(gHost, inObjectID, 1, addrs);
        }
        return noErr;
    }

    // Stream format set requests: validate, then trigger a config change.
    if ((inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) &&
        (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
         inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription* asbd = (const AudioStreamBasicDescription*)inData;
        if (asbd->mFormatID != kAudioFormatLinearPCM ||
            asbd->mChannelsPerFrame != kChannels ||
            asbd->mBitsPerChannel   != kBitsPerSample)
            return kAudioHardwareIllegalOperationError;
        uint32_t rounded = (uint32_t)(asbd->mSampleRate + 0.5);
        bool supported = false;
        for (UInt32 i = 0; i < kNumRates; i++)
            if (rounded == (uint32_t)kSupportedRates[i]) { supported = true; break; }
        if (!supported) return kAudioHardwareIllegalOperationError;
        if (gHost) {
            gHost->RequestDeviceConfigurationChange(
                gHost, kObjectID_Device, (UInt64)rounded, nullptr);
        }
        return noErr;
    }

    (void)driver;
    return kAudioHardwareUnsupportedOperationError;
}

// ───────────────────────────────────────────────────────────────────────────
// IO operations
// ───────────────────────────────────────────────────────────────────────────
static OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    if (deviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    HLOG("StartIO");
    pthread_mutex_lock(&gStateLock);
    bool ok = true;
    if (!XoneUSBTransport::shared().isOpen()) {
        ok = XoneUSBTransport::shared().openAndHandshake();
    }
    if (ok) {
        ok = XoneUSBTransport::shared().startIO();
    }
    if (ok) gDeviceRunning.store(true);
    pthread_mutex_unlock(&gStateLock);
    return ok ? noErr : (OSStatus)kAudioHardwareUnspecifiedError;
}

static OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    if (deviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    HLOG("StopIO");
    pthread_mutex_lock(&gStateLock);
    XoneUSBTransport::shared().stopIO();
    gDeviceRunning.store(false);
    pthread_mutex_unlock(&gStateLock);
    return noErr;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32,
                                 Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (deviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    XoneUSBTransport::shared().getZeroTimeStamp(outSampleTime, outHostTime, outSeed);
    if (*outHostTime == 0) {
        // No USB packets yet – synthesise a stamp from the current host time so
        // CoreAudio doesn't refuse to start.
        *outHostTime   = mach_absolute_time();
        *outSampleTime = 0;
        *outSeed       = 1;
    }
    return noErr;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32,
                                  UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (deviceID != kObjectID_Device) return kAudioHardwareBadObjectError;
    *outWillDoInPlace = true;
    switch (inOperationID) {
    case kAudioServerPlugInIOOperationReadInput:
    case kAudioServerPlugInIOOperationWriteMix:
        *outWillDo = true; break;
    default:
        *outWillDo = false; break;
    }
    return noErr;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32,
                                 UInt32, const AudioServerPlugInIOCycleInfo*) { return noErr; }
static OSStatus EndIOOperation  (AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32,
                                 UInt32, const AudioServerPlugInIOCycleInfo*) { return noErr; }

static OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID deviceID,
                              AudioObjectID streamID, UInt32, UInt32 inOperationID,
                              UInt32 inIOBufferFrameSize,
                              const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                              void* ioMainBuffer, void* /*ioSecondaryBuffer*/) {
    if (deviceID != kObjectID_Device) return kAudioHardwareBadObjectError;

    if (inOperationID == kAudioServerPlugInIOOperationReadInput &&
        streamID == kObjectID_Stream_Input) {
        const uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
        XoneUSBTransport::shared().readInput(sampleTime, inIOBufferFrameSize, (uint8_t*)ioMainBuffer);
        return noErr;
    }
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix &&
        streamID == kObjectID_Stream_Output) {
        const uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
        XoneUSBTransport::shared().writeOutput(sampleTime, inIOBufferFrameSize, (const uint8_t*)ioMainBuffer);
        return noErr;
    }
    return noErr;
}

// ───────────────────────────────────────────────────────────────────────────
// Driver interface table (v3 ABI)
// ───────────────────────────────────────────────────────────────────────────
static AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation
};
static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;

// ───────────────────────────────────────────────────────────────────────────
// CFPlugIn factory entry point
// ───────────────────────────────────────────────────────────────────────────
extern "C" __attribute__((visibility("default")))
void* Xone4DHAL_Create(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    gRefCount.store(1);
    return &gDriverInterfacePtr;
}
