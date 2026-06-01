/* XoneRing.h – single-producer / single-consumer lock-free ring buffer
 * MIT License.
 *
 * Used as the audio bridge between the USB transport thread (producer of input
 * samples / consumer of output samples) and the CoreAudio DoIOOperation
 * callback (consumer of input samples / producer of output samples).
 *
 * Capacity must be a power of two for the masking arithmetic to work.
 * Element type is uint8_t; the caller decides how to interpret frames.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

class XoneRing {
public:
    XoneRing() = default;

    /* capacity in bytes; need not be a power of two. We use modulo so frames
       of arbitrary size (e.g. 24 B for 8-ch packed S24_3LE) don't straddle a
       wrap-around boundary. */
    bool init(uint32_t capacity_bytes) {
        if (capacity_bytes == 0) return false;
        mCapacity = capacity_bytes;
        mBuf      = new (std::nothrow) uint8_t[capacity_bytes];
        if (!mBuf) return false;
        std::memset(mBuf, 0, capacity_bytes);
        mWrite.store(0, std::memory_order_relaxed);
        mRead.store(0, std::memory_order_relaxed);
        return true;
    }

    ~XoneRing() {
        delete[] mBuf;
    }

    uint32_t capacity() const { return mCapacity; }

    /* Producer side. Writes wrap and never block. If the consumer is too slow
       the writer overruns it – we accept that as samples lost (preferable to
       blocking the USB or audio thread). */
    void write(const uint8_t* src, uint32_t bytes) {
        const uint32_t w = mWrite.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < bytes; i++) {
            mBuf[(w + i) % mCapacity] = src[i];
        }
        mWrite.store(w + bytes, std::memory_order_release);
    }

    /* Consumer side. Reads at an absolute offset (sample-time based). */
    void read(uint8_t* dst, uint32_t bytes) {
        const uint32_t r = mRead.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < bytes; i++) {
            dst[i] = mBuf[(r + i) % mCapacity];
        }
        mRead.store(r + bytes, std::memory_order_release);
    }

    /* Peek without advancing read pointer (used when CoreAudio gives us the
       offset itself via the IO cycle info). */
    void peek_at(uint32_t abs_offset, uint8_t* dst, uint32_t bytes) const {
        for (uint32_t i = 0; i < bytes; i++) {
            dst[i] = mBuf[(abs_offset + i) % mCapacity];
        }
    }

    void poke_at(uint32_t abs_offset, const uint8_t* src, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; i++) {
            mBuf[(abs_offset + i) % mCapacity] = src[i];
        }
    }

    void zero() {
        if (mBuf) std::memset(mBuf, 0, mCapacity);
    }

    uint32_t writePos() const { return mWrite.load(std::memory_order_acquire); }
    uint32_t readPos()  const { return mRead.load(std::memory_order_acquire); }

private:
    uint8_t*               mBuf      = nullptr;
    uint32_t               mCapacity = 0;
    std::atomic<uint32_t>  mWrite{0};
    std::atomic<uint32_t>  mRead{0};
};
