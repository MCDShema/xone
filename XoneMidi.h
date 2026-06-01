/* XoneMidi.h — HAL-side bridge for USB MIDI IN (endpoint 0x83).
 *
 * Connects (best-effort) to a Unix-domain socket owned by `xone-midi-agent`,
 * a separate user-session daemon that holds the CoreMIDI virtual source
 * "Xone:4D". We send raw MIDI bytes (after Ploytec 0xFD padding is stripped)
 * over the socket; the agent forwards them to CoreMIDI clients.
 *
 * Why the indirection: calling MIDIClientCreate from inside the coreaudiod
 * helper that hosts our HAL plug-in deadlocks MIDIServer globally on macOS
 * Tahoe (the XPC connection wedges the global server). The agent runs as a
 * normal user process where CoreMIDI works fine.
 *
 * The connection is non-blocking and lazy: if the socket doesn't exist
 * (agent isn't running yet) or the agent disconnects, feedIn() silently
 * drops MIDI bytes. Audio is never affected.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

class XoneMidi {
public:
    bool start();
    void stop();

    /* Push USB bytes from MIDI IN endpoint. Safe to call from any thread.
     * Filters 0xFD (Ploytec idle padding) and forwards the rest. */
    void feedIn(const uint8_t* data, size_t len);

private:
    void ensureConnected();   // non-blocking connect to socket if not already
    void closeSocket();

    std::atomic<int>  mSock{-1};
    pthread_mutex_t   mConnectMutex = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<bool> mStopped{true};
};
