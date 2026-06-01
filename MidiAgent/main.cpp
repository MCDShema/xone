/* main.cpp — xone-midi-agent
 *
 * A tiny user-session daemon that owns the CoreMIDI virtual source "Xone:4D"
 * and forwards bytes received over a Unix-domain socket to it.
 *
 * Why this exists:
 *   The HAL plug-in (which physically claims the USB Xone:4D and can read
 *   MIDI bytes from endpoint 0x83) lives inside the coreaudiod helper
 *   process. Calling MIDIClientCreate from that process deadlocks MIDIServer
 *   on macOS Tahoe (XPC connection wedges the global server). To get MIDI
 *   exposed cleanly, we keep CoreMIDI access in a separate normal user
 *   process — this agent — and let the HAL ferry bytes over a socket.
 *
 *   Architecture:
 *     USB MIDI IN (0x83)
 *       └─ HAL plug-in: read → strip Ploytec 0xFD padding → write to socket
 *           └─ /tmp/com.digitalkiss.xone.midi
 *               └─ this agent: read → MIDIReceived → CoreMIDI source "Xone:4D"
 *                   └─ Traktor / DAW
 *
 *   Two CFRunLoop-driven concerns share the agent:
 *     1. Socket acceptor (on a dedicated pthread). HAL connects, the agent
 *        reads bytes, the parser turns them into MIDI events. Always running
 *        regardless of USB state — the socket is harmless when there's no
 *        device.
 *     2. IOKit USB watcher (on the main run loop). When the Xone:4D appears
 *        on the bus we create the CoreMIDI virtual source so it shows up in
 *        Audio MIDI Setup / Traktor; when it goes away we dispose the source
 *        so the system tree matches the physical state — same behaviour as
 *        any normal USB MIDI device. UniqueID is stable across re-creation,
 *        so Traktor mappings stay intact.
 */

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <IOKit/IOMessage.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* kSocketPath = "/tmp/com.digitalkiss.xone.midi";

// Xone:4D USB identifiers.
static const SInt32 kVendorID  = 0x0A4A;
static const SInt32 kProductID = 0xFF4D;

/* Log to both os_log AND stdout. os_log on Tahoe occasionally drops messages
 * from launchd-spawned agents (privacy/rate-limit); fprintf to stdout is
 * captured by the plist's StandardOutPath so it always lands in
 * /tmp/xone-midi-agent.out.log — making diagnostics reliable in the field. */
#define ALOG(...) do { \
    char _alog_buf[512]; \
    snprintf(_alog_buf, sizeof(_alog_buf), __VA_ARGS__); \
    os_log(OS_LOG_DEFAULT, "[XoneMidiAgent] %{public}s", _alog_buf); \
    fprintf(stdout, "[XoneMidiAgent] %s\n", _alog_buf); \
    fflush(stdout); \
} while (0)

// Production: no per-byte tracing. Turn on temporarily for diagnostics.
static const bool kDebugTrace = false;

static void hex_dump(char* out, size_t outLen, const uint8_t* data, size_t n) {
    size_t p = 0;
    for (size_t i = 0; i < n && p + 4 < outLen; i++) {
        p += (size_t)snprintf(out + p, outLen - p, "%02X ", data[i]);
    }
    if (p > 0 && out[p - 1] == ' ') out[p - 1] = '\0';
    else if (p < outLen)            out[p]     = '\0';
}

// ───────────────────────────────────────────────────────────────────────────
// CoreMIDI source — created on USB plug-in, disposed on unplug
// ───────────────────────────────────────────────────────────────────────────
//
// g_source is read by the socket thread on every MIDI event and rewritten by
// the IOKit thread on plug/unplug — atomic suffices because MIDIEndpointRef
// is just a 32-bit token.
static MIDIClientRef                g_client = 0;
static std::atomic<MIDIEndpointRef> g_source{0};
static volatile sig_atomic_t        g_stop = 0;

// IOKit / hot-plug bookkeeping (main thread).
static IONotificationPortRef        g_notifyPort = nullptr;
static io_iterator_t                g_firstMatchIter = IO_OBJECT_NULL;
static io_object_t                  g_termNotifier = IO_OBJECT_NULL;

static bool create_midi_source() {
    if (g_source.load() != 0) return true;
    if (g_client == 0) {
        OSStatus s = MIDIClientCreate(CFSTR("Xone:4D"), nullptr, nullptr, &g_client);
        if (s != noErr) { ALOG("MIDIClientCreate failed: %d", (int)s); return false; }
    }
    MIDIEndpointRef src = 0;
    OSStatus s = MIDISourceCreate(g_client, CFSTR("Xone:4D"), &src);
    if (s != noErr) {
        ALOG("MIDISourceCreate failed: %d", (int)s);
        return false;
    }
    // Stable UniqueID so Traktor mappings keep matching across replug cycles.
    MIDIObjectSetStringProperty(src, kMIDIPropertyManufacturer, CFSTR("Allen & Heath"));
    MIDIObjectSetStringProperty(src, kMIDIPropertyModel,        CFSTR("Xone:4D"));
    MIDIObjectSetIntegerProperty(src, kMIDIPropertyUniqueID, 0x584F4E34); // 'XON4'
    g_source.store(src);
    ALOG("CoreMIDI source created");
    return true;
}

static void dispose_midi_source() {
    MIDIEndpointRef src = g_source.exchange(0);
    if (src) {
        MIDIEndpointDispose(src);
        ALOG("CoreMIDI source disposed");
    }
}

// ───────────────────────────────────────────────────────────────────────────
// MIDI parser — running-status capable
// ───────────────────────────────────────────────────────────────────────────
//
// The Xone:4D ships MIDI bytes a few at a time inside fixed-size USB frames
// (5 bytes per read, mostly 0xFD padding plus a 0xFF terminator). Within one
// USB read we may get a partial event; the next event may continue under
// running status with no fresh status byte. If we just forward the raw byte
// stream to MIDIReceived, the receiver mis-interprets running-status data as
// fresh events whenever a packet boundary doesn't line up with a MIDI
// message — and the result looks like "fader snaps to 0 or 127" (data1 and
// data2 swap roles).
//
// The fix is to parse on this side: keep a running status byte, accumulate
// data bytes until a complete event is formed, then emit each event with
// its full status byte at the head so the receiver can't get confused.
static uint8_t s_status = 0;
static uint8_t s_data[2] = {0, 0};
static uint8_t s_dataCount = 0;
static uint8_t s_dataNeeded = 0;

static uint8_t midi_data_bytes_for(uint8_t status) {
    if (status >= 0xF8) return 0;          // System Real-Time: 1-byte
    if (status >= 0xF0) {
        switch (status) {
            case 0xF1: case 0xF3: return 1;
            case 0xF2:            return 2;
            default:              return 0; // F0 SysEx start handled separately;
                                            // F4,F5,F6,F7 are 0-data
        }
    }
    // Channel voice 0x80..0xEF
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;    // Program Change, Channel Pressure
        default:              return 2;    // Note On/Off, Aftertouch, CC, PB
    }
}

static void emit_event(const uint8_t* bytes, size_t len) {
    MIDIEndpointRef src = g_source.load();
    if (!src || len == 0) return;          // No source yet (USB not plugged in)
    if (kDebugTrace) {
        char hex[64];
        hex_dump(hex, sizeof(hex), bytes, len);
        ALOG("emit[%zu]: %s", len, hex);
    }
    uint8_t pktBuf[64];
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(pktBuf);
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, sizeof(pktBuf), p,
                          mach_absolute_time(), len, bytes);
    if (p) MIDIReceived(src, list);
}

static void process_byte(uint8_t b) {
    // Ploytec framing artifacts — NOT MIDI:
    //   0xFD = empty slot within a 5-byte USB frame
    //   0xFF = end-of-frame terminator (always at the 5th byte of every frame)
    // Both happen to be valid MIDI byte values (FD = reserved real-time, FF =
    // System Reset). The Xone:4D sends them as wire padding, not as MIDI
    // events. If we don't strip 0xFF here, every USB frame ends with a fake
    // "System Reset" which makes CC streams unparseable.
    if (b == 0xFD || b == 0xFF) return;

    if (b >= 0xF8) {                       // System Real-Time
        emit_event(&b, 1);
        return;                            // Doesn't reset running status
    }
    if (b & 0x80) {                        // Status byte
        s_status     = b;
        s_dataCount  = 0;
        s_dataNeeded = midi_data_bytes_for(b);
        if (s_dataNeeded == 0) {           // E.g. Tune Request (0xF6)
            emit_event(&b, 1);
            if (b >= 0xF0) s_status = 0;   // System Common clears running status
        }
        return;
    }
    // Data byte
    if (s_status == 0) return;             // No active status — orphan, ignore
    s_data[s_dataCount++] = b;
    if (s_dataCount >= s_dataNeeded) {
        uint8_t out[3] = {s_status, s_data[0], s_data[1]};
        emit_event(out, (size_t)s_dataNeeded + 1);
        s_dataCount = 0;                   // Channel voice keeps running status
    }
}

static void send_to_midi(const uint8_t* data, size_t len) {
    if (kDebugTrace) {
        char hex[256];
        hex_dump(hex, sizeof(hex), data, len);
        ALOG("RX[%zu]: %s", len, hex);
    }
    for (size_t i = 0; i < len; i++) process_byte(data[i]);
}

static void on_signal(int) { g_stop = 1; }

// ───────────────────────────────────────────────────────────────────────────
// Unix socket — server side. Runs in a dedicated pthread; CoreMIDI source
// existence is independent of socket state, so the loop never has to care
// whether the device is plugged in.
// ───────────────────────────────────────────────────────────────────────────

static int create_listening_socket() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { ALOG("socket() failed: %s", strerror(errno)); return -1; }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    unlink(kSocketPath);                       // remove stale leftover
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ALOG("bind(%s) failed: %s", kSocketPath, strerror(errno));
        close(fd); return -1;
    }
    chmod(kSocketPath, 0666);                  // any user/HAL can connect

    if (listen(fd, 1) < 0) {
        ALOG("listen() failed: %s", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

static void* socket_thread(void* arg) {
    int srv = *(int*)arg;
    while (!g_stop) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (g_stop) break;
            if (errno == EINTR) continue;
            ALOG("accept() failed: %s", strerror(errno));
            usleep(100 * 1000);
            continue;
        }
        ALOG("HAL connected");

        // Reset parser state — a previous session may have left a half-formed
        // event in the buffer (e.g. agent killed mid-CC). Without this, the
        // next data byte under the same running status would complete a
        // phantom event drawn from two unrelated controls.
        s_status = 0;
        s_dataCount = 0;
        s_dataNeeded = 0;

        uint8_t chunk[1024];
        while (!g_stop) {
            ssize_t n = read(cli, chunk, sizeof(chunk));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            send_to_midi(chunk, (size_t)n);
        }
        close(cli);
        ALOG("HAL disconnected");
    }
    return nullptr;
}

// ───────────────────────────────────────────────────────────────────────────
// IOKit USB watcher — creates/disposes the CoreMIDI virtual source so the
// device tree matches whether Xone:4D is physically connected.
// ───────────────────────────────────────────────────────────────────────────

static void on_device_terminated(void* /*refCon*/, io_service_t /*service*/,
                                 natural_t msgType, void* /*msgArg*/) {
    if (msgType != kIOMessageServiceIsTerminated) return;
    ALOG("USB Xone:4D unplugged");
    if (g_termNotifier != IO_OBJECT_NULL) {
        IOObjectRelease(g_termNotifier);
        g_termNotifier = IO_OBJECT_NULL;
    }
    dispose_midi_source();
}

static void on_first_match(void* /*refCon*/, io_iterator_t iter) {
    bool saw = false;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        // Hook a per-device termination notification so unplug fires our
        // callback immediately. Only one device is ever expected; if a
        // second one appears the new termination notifier replaces the old
        // (no leak because we release the previous one in the dispose path).
        if (g_termNotifier != IO_OBJECT_NULL) {
            IOObjectRelease(g_termNotifier);
            g_termNotifier = IO_OBJECT_NULL;
        }
        kern_return_t kr = IOServiceAddInterestNotification(
            g_notifyPort, svc, kIOGeneralInterest,
            &on_device_terminated, nullptr, &g_termNotifier);
        if (kr != KERN_SUCCESS) {
            ALOG("IOServiceAddInterestNotification failed: 0x%x", kr);
            g_termNotifier = IO_OBJECT_NULL;
        }
        IOObjectRelease(svc);
        saw = true;
    }
    if (saw) {
        ALOG("USB Xone:4D present");
        create_midi_source();
    }
}

static bool setup_iokit_watcher() {
    g_notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!g_notifyPort) {
        ALOG("IONotificationPortCreate failed");
        return false;
    }
    CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(g_notifyPort);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) { ALOG("IOServiceMatching failed"); return false; }
    CFNumberRef vid = CFNumberCreate(NULL, kCFNumberSInt32Type, &kVendorID);
    CFNumberRef pid = CFNumberCreate(NULL, kCFNumberSInt32Type, &kProductID);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID),  vid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pid);
    CFRelease(vid);
    CFRelease(pid);

    // IOServiceAddMatchingNotification consumes the dictionary reference.
    // The initial iterator drain inside on_first_match() also arms the
    // notification — without it we'd never get re-fired on subsequent plug.
    kern_return_t kr = IOServiceAddMatchingNotification(
        g_notifyPort, kIOFirstMatchNotification, match,
        &on_first_match, nullptr, &g_firstMatchIter);
    if (kr != KERN_SUCCESS) {
        ALOG("IOServiceAddMatchingNotification failed: 0x%x", kr);
        return false;
    }
    // Process whatever is already on the bus so we start in the right state.
    on_first_match(nullptr, g_firstMatchIter);
    ALOG("watching for VID 0x%04X PID 0x%04X", kVendorID, kProductID);
    return true;
}

static void teardown_iokit_watcher() {
    if (g_termNotifier != IO_OBJECT_NULL) {
        IOObjectRelease(g_termNotifier);
        g_termNotifier = IO_OBJECT_NULL;
    }
    if (g_firstMatchIter != IO_OBJECT_NULL) {
        IOObjectRelease(g_firstMatchIter);
        g_firstMatchIter = IO_OBJECT_NULL;
    }
    if (g_notifyPort) {
        IONotificationPortDestroy(g_notifyPort);
        g_notifyPort = nullptr;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// main — run the IOKit run-loop in the main thread; socket on a worker
// ───────────────────────────────────────────────────────────────────────────

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int srv = create_listening_socket();
    if (srv < 0) return 1;
    ALOG("listening on %s", kSocketPath);

    pthread_t sockTid = 0;
    if (pthread_create(&sockTid, nullptr, &socket_thread, &srv) != 0) {
        ALOG("pthread_create(socket) failed");
        close(srv);
        unlink(kSocketPath);
        return 1;
    }

    if (!setup_iokit_watcher()) {
        // We can still serve socket connections and forward MIDI bytes, but
        // the CoreMIDI source won't appear unless we successfully watch the
        // bus. Fall back to creating the source unconditionally so we don't
        // leave the user with a dead agent.
        ALOG("IOKit watcher setup failed — falling back to always-on source");
        create_midi_source();
    }

    // Pump the run-loop until SIGINT/SIGTERM.
    while (!g_stop) {
        SInt32 r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        if (r == kCFRunLoopRunStopped || r == kCFRunLoopRunFinished) break;
    }

    teardown_iokit_watcher();
    dispose_midi_source();
    if (g_client) { MIDIClientDispose(g_client); g_client = 0; }

    // Unblock accept(): closing the listener makes accept() return.
    close(srv);
    pthread_join(sockTid, nullptr);
    unlink(kSocketPath);
    ALOG("stopped");
    return 0;
}
