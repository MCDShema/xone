/* XoneMidi.cpp — see header. */
#include "XoneMidi.h"

#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

#define MLOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[XoneHAL/MIDI] " fmt, ##__VA_ARGS__)

static const char* kSocketPath = "/tmp/com.digitalkiss.xone.midi";

bool XoneMidi::start() {
    mStopped.store(false);
    /* Don't connect eagerly — the agent may not be running yet (user not
     * logged in, agent crashed and being restarted by launchd). connect()
     * is attempted lazily on the first feedIn() and re-attempted on
     * disconnect. */
    return true;
}

void XoneMidi::stop() {
    mStopped.store(true);
    closeSocket();
}

void XoneMidi::closeSocket() {
    int fd = mSock.exchange(-1);
    if (fd >= 0) close(fd);
}

void XoneMidi::ensureConnected() {
    if (mSock.load() >= 0 || mStopped.load()) return;

    pthread_mutex_lock(&mConnectMutex);
    if (mSock.load() < 0 && !mStopped.load()) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            MLOG("socket() failed: %s", strerror(errno));
        } else {
            /* Non-blocking connect: if the agent isn't there we want to
             * fail immediately, not stall the USB worker thread. */
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);
            int r = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
            if (r == 0 || (r < 0 && errno == EINPROGRESS)) {
                /* Restore blocking mode so writes don't drop bytes when the
                 * agent's recv buffer is briefly full. Writes are tiny
                 * (≤ kilobyte) and the agent drains immediately. */
                fcntl(fd, F_SETFL, flags);
                mSock.store(fd);
                MLOG("connected to agent at %s", kSocketPath);
            } else {
                close(fd);
            }
        }
    }
    pthread_mutex_unlock(&mConnectMutex);
}

void XoneMidi::feedIn(const uint8_t* data, size_t len) {
    if (mStopped.load() || len == 0) return;

    /* Strip Ploytec 0xFD idle padding — those bytes are not real MIDI. */
    uint8_t filtered[1024];
    size_t  fLen = 0;
    for (size_t i = 0; i < len && fLen < sizeof(filtered); i++) {
        if (data[i] != 0xFD) filtered[fLen++] = data[i];
    }
    if (fLen == 0) return;

    ensureConnected();
    int fd = mSock.load();
    if (fd < 0) return;

    /* Block SIGPIPE locally so a dead agent doesn't kill the whole
     * coreaudiod helper. SO_NOSIGPIPE on the socket would be cleaner but
     * isn't always honored — we set both. */
    int set = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));

    ssize_t n = send(fd, filtered, fLen, 0);
    if (n < 0) {
        MLOG("send() failed: %s — dropping connection", strerror(errno));
        closeSocket();
    }
}
