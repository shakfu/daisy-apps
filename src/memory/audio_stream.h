#pragma once

#include "memory/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace daisyapps {

// Streaming audio to/from a slow block device (the SD card), decoupled from FatFs so the core logic is
// host-testable. The audio ISR touches only the lock-free SpscRing (cheap, bounded, never blocks); the
// main loop does the actual slow I/O through these abstract chunk source/sink seams. On the device the
// source/sink wrap `Card`/FatFs; in `host/` they wrap memory, so the producer/consumer logic, underrun/
// overrun handling, EOF and finalize can all be proven off-target.
//
// Convention: bytes throughout (the stream is format-agnostic). Callers keep frame alignment by always
// moving whole frames; `kFrameBytes`-aligned scratch/chunk sizes make every transfer frame-aligned.

// Slow source of bytes (the file body on SD), pumped from the main loop.
struct IChunkSource {
    virtual ~IChunkSource() = default;
    // Read up to `n` bytes into `dst`; return bytes read. A short read with eof()==true means the
    // file is exhausted; a short read with eof()==false just means "nothing more available right now".
    virtual uint32_t read(uint8_t* dst, uint32_t n) = 0;
    virtual bool     eof() const = 0;
    // Seek back to the start of the body so a looping stream can keep reading. Default no-op.
    virtual void     rewind() {}
};

// Slow sink of bytes (the file being recorded on SD), pumped from the main loop.
struct IChunkSink {
    virtual ~IChunkSink() = default;
    virtual uint32_t write(const uint8_t* src, uint32_t n) = 0; // returns bytes written
    virtual void     finalize() = 0;                            // close out (e.g. patch WAV header)
};

// ---- Playback: SD -> ring -> ISR -------------------------------------------------------------------
//
// pump() (main loop) reads ahead from the source into the ring while space allows; consume() (ISR)
// drains the ring, zero-filling (silence) and counting an underrun if the pump fell behind.
class PlayStream {
public:
    void init(SpscRing* ring, uint8_t* scratch, uint32_t scratch_bytes) {
        _ring = ring; _scratch = scratch; _scratch_n = scratch_bytes;
    }

    void start(IChunkSource* src) {
        _src = src;
        _eof.store(false, std::memory_order_relaxed);
        _underruns.store(0, std::memory_order_relaxed);
        _ring->reset();
    }

    // Enable/disable looping. When on, pump() rewinds the source at EOF and keeps filling instead of
    // finishing, so the stream repeats seamlessly. Sticky across start() (it is a mode, not stream state).
    void set_loop(bool loop) { _loop = loop; }

    // Main loop: top up the ring from the source while there is room, in scratch-sized reads. A short
    // read does NOT mean "stop" (a chunked source like FatFs can return fewer bytes than asked without
    // being at EOF); only a zero-byte read ends the round - then eof() distinguishes file-end from
    // momentarily-nothing-available. Bounded per call by the ring's free space (~the read-ahead window).
    void pump() {
        if (!_src || _eof.load(std::memory_order_relaxed)) return;
        int rewinds = 0;                            // guard: don't spin forever on an empty source
        for (;;) {
            const uint32_t space = _ring->writable();
            if (space == 0) return;                 // ring full: read-ahead is satisfied
            uint32_t want = space < _scratch_n ? space : _scratch_n;
            const uint32_t got = _src->read(_scratch, want);
            if (got) { _ring->write(_scratch, got); rewinds = 0; continue; }
            if (_src->eof()) {
                if (_loop && rewinds < 2) { _src->rewind(); rewinds++; continue; }  // loop to the top
                _eof.store(true, std::memory_order_relaxed);  // not looping (or empty source): file ended
            }
            return;
        }
    }

    // ISR: deliver `n` bytes. On underrun, zero-fill the shortfall (silence) and count it - but a short
    // read at true end-of-stream (EOF reached and ring drained) is the natural finish, not an underrun.
    uint32_t consume(uint8_t* dst, uint32_t n) {
        const uint32_t got = _ring->read(dst, n);
        if (got < n) {
            std::memset(dst + got, 0, n - got);
            if (!finished()) _underruns.fetch_add(1, std::memory_order_relaxed);
        }
        return got;
    }

    bool     finished()  const { return _eof.load(std::memory_order_relaxed) && _ring->readable() == 0; }
    uint32_t underruns() const { return _underruns.load(std::memory_order_relaxed); }

private:
    SpscRing*     _ring = nullptr;
    IChunkSource* _src  = nullptr;
    uint8_t*      _scratch = nullptr;
    uint32_t      _scratch_n = 0;
    // _eof is written by the main-loop pump and read by the ISR (via finished()); _underruns is the
    // mirror image. Both are single words that would be benign in practice on this core, but plain
    // objects touched from two contexts are a data race the compiler is entitled to fold or hoist -
    // relaxed atomics cost nothing here and make them defined. _loop is main-loop-only (set_loop and
    // pump), so it stays plain.
    std::atomic<bool>     _eof{false};
    bool                  _loop = false;
    std::atomic<uint32_t> _underruns{0};
};

// ---- Recording: ISR -> ring -> SD ------------------------------------------------------------------
//
// produce() (ISR) pushes input into the ring, dropping and counting overflow if the pump fell behind;
// pump() (main loop) drains the ring to the sink. stop() requests end-of-record: the next pumps flush
// whatever remains and then finalize() the sink exactly once.
class RecordStream {
public:
    void init(SpscRing* ring, uint8_t* scratch, uint32_t scratch_bytes) {
        _ring = ring; _scratch = scratch; _scratch_n = scratch_bytes;
    }

    void start(IChunkSink* sink) {
        _sink = sink; _stopping = false; _done = false;
        _overruns.store(0, std::memory_order_relaxed);
        _ring->reset();
    }

    // ISR: push `n` bytes into the ring; bytes that don't fit are dropped and counted (never blocks).
    uint32_t produce(const uint8_t* src, uint32_t n) {
        const uint32_t put = _ring->write(src, n);
        if (put < n) _overruns.fetch_add(n - put, std::memory_order_relaxed);
        return put;
    }

    // Main loop: drain the ring to the sink in scratch-sized writes; finalize once flushed after stop().
    void pump() {
        if (!_sink || _done) return;
        for (;;) {
            const uint32_t avail = _ring->readable();
            if (avail == 0) break;
            const uint32_t want = avail < _scratch_n ? avail : _scratch_n;
            const uint32_t got  = _ring->read(_scratch, want);
            _sink->write(_scratch, got);
        }
        if (_stopping && _ring->readable() == 0) {
            _sink->finalize();
            _done = true;
        }
    }

    void     stop()             { _stopping = true; } // request end; pump() flushes remaining, finalizes
    bool     finished() const   { return _done; }     // all produced bytes written + finalized
    uint32_t overruns() const   { return _overruns.load(std::memory_order_relaxed); } // bytes dropped on a full ring

private:
    SpscRing*   _ring = nullptr;
    IChunkSink* _sink = nullptr;
    uint8_t*    _scratch = nullptr;
    uint32_t    _scratch_n = 0;
    bool                  _stopping = false;   // main-loop only (stop/pump)
    bool                  _done = false;       // main-loop only (pump), read by finished()
    // Incremented in the ISR (produce), read from the main loop - relaxed atomic for the same reason
    // as PlayStream::_underruns above.
    std::atomic<uint32_t> _overruns{0};
};

} // namespace daisyapps
