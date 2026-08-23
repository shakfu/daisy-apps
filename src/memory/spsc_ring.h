#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace daisyapps {

// Lock-free single-producer / single-consumer byte ring over an EXTERNALLY-provided buffer (so it can
// live in the SDRAM arena, not in static RAM). One thread writes (produces), one reads (consumes);
// here that is the audio ISR on one side and the main-loop SD pump on the other.
//
// Free-running unsigned 32-bit indices (kfifo style): the whole capacity is usable (no reserved slot),
// and `head - tail` is wrap-safe because the in-flight count is always <= capacity, so the unsigned
// subtraction is correct even when the raw counters wrap. Capacity must be a power of two; init()
// enforces that by rounding down rather than trusting the caller (see below).
//
// Memory ordering is the textbook SPSC handshake: the producer publishes data with a release store to
// `_head`; the consumer observes it with an acquire load, and vice-versa for `_tail`. std::atomic
// <uint32_t> is lock-free on the Cortex-M7 (single core) and on the host.
class SpscRing {
public:
    // Not copyable: two SpscRings over one buffer, each with its own head/tail, would silently
    // corrupt every transfer. There is no legitimate reason to copy one.
    SpscRing()                           = default;
    SpscRing(const SpscRing&)            = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    // Largest power of two <= n (0 for n == 0). Used to enforce init()'s precondition rather than
    // trust it; constexpr so a caller can check its own constant at compile time.
    static constexpr uint32_t floor_pow2(uint32_t n) {
        if (n == 0) return 0;
        uint32_t p = 1;
        while (p <= (n >> 1)) p <<= 1;
        return p;
    }
    static constexpr bool is_pow2(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

    // `buf` must hold at least `capacity` bytes.
    //
    // The whole wrap-free index scheme (`head - tail` on free-running unsigned counters, masked
    // indexing) depends on the capacity being a power of two: with any other value the mask stops
    // matching the capacity and EVERY read and write lands at the wrong offset, silently. That is a
    // precondition no caller can be trusted to keep forever, so it is enforced here rather than
    // documented: a non-power-of-two is ROUNDED DOWN to the next one below it. The ring then holds
    // less than the buffer it was given - which capacity() reports honestly, and which costs latency
    // rather than correctness - instead of corrupting every transfer through it.
    void init(uint8_t* buf, uint32_t capacity) {
        _cap  = floor_pow2(capacity);
        _buf  = _cap ? buf : nullptr;
        _mask = _cap - 1;
        reset();
    }

    void reset() {
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

    uint32_t capacity() const { return _cap; }

    // --- producer side ---------------------------------------------------------------------------
    uint32_t writable() const {
        const uint32_t head = _head.load(std::memory_order_relaxed);
        const uint32_t tail = _tail.load(std::memory_order_acquire);
        return _cap - (head - tail);
    }

    // Copy up to `n` bytes in; returns the number actually written (< n iff the ring is near full).
    uint32_t write(const uint8_t* src, uint32_t n) {
        const uint32_t head  = _head.load(std::memory_order_relaxed);
        const uint32_t tail  = _tail.load(std::memory_order_acquire);
        const uint32_t space = _cap - (head - tail);
        if (n > space) n = space;
        if (n == 0) return 0;          // also guards the _cap == 0 (uninitialised) case

        uint32_t first = _cap - (head & _mask);   // bytes until the buffer wraps
        if (first > n) first = n;
        std::memcpy(_buf + (head & _mask), src, first);
        if (n > first) std::memcpy(_buf, src + first, n - first);

        _head.store(head + n, std::memory_order_release);
        return n;
    }

    // --- consumer side ---------------------------------------------------------------------------
    uint32_t readable() const {
        const uint32_t head = _head.load(std::memory_order_acquire);
        const uint32_t tail = _tail.load(std::memory_order_relaxed);
        return head - tail;
    }

    // Copy up to `n` bytes out; returns the number actually read (< n iff the ring is near empty).
    uint32_t read(uint8_t* dst, uint32_t n) {
        const uint32_t head  = _head.load(std::memory_order_acquire);
        const uint32_t tail  = _tail.load(std::memory_order_relaxed);
        const uint32_t avail = head - tail;
        if (n > avail) n = avail;
        if (n == 0) return 0;          // also guards the _cap == 0 (uninitialised) case

        uint32_t first = _cap - (tail & _mask);
        if (first > n) first = n;
        std::memcpy(dst, _buf + (tail & _mask), first);
        if (n > first) std::memcpy(dst + first, _buf, n - first);

        _tail.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    uint8_t*              _buf  = nullptr;
    uint32_t              _cap  = 0;
    uint32_t              _mask = 0;
    std::atomic<uint32_t> _head{0};  // producer publishes here
    std::atomic<uint32_t> _tail{0};  // consumer publishes here
};

} // namespace daisyapps
