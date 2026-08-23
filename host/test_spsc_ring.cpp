// SpscRing + PlayStream/RecordStream: the lock-free half of the SD streaming service.
//
// These run single-threaded here. That is deliberate and it is not a gap: the concurrency argument
// for an SPSC ring is a proof about memory ordering, not something a host test can observe, and the
// bugs that actually bite are the arithmetic ones - wrap at the uint32_t boundary, exact-full and
// exact-empty, the split copy across the buffer end, underrun vs natural end-of-stream. Those are
// all deterministic, and all of them were previously unexercised.

#include "check.h"

#include "memory/audio_stream.h"
#include "memory/spsc_ring.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace daisyapps;

namespace {

// A byte pattern that makes an off-by-one visible: value == index, so a misplaced copy shows up as
// the wrong number rather than as plausible-looking data.
std::vector<uint8_t> ramp(uint32_t n, uint8_t start = 0)
{
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(start + i);
    return v;
}

} // namespace

// --- capacity handling ---------------------------------------------------------------------------

TEST(ring_floor_pow2)
{
    CHECK_EQ(SpscRing::floor_pow2(0), 0u);
    CHECK_EQ(SpscRing::floor_pow2(1), 1u);
    CHECK_EQ(SpscRing::floor_pow2(2), 2u);
    CHECK_EQ(SpscRing::floor_pow2(3), 2u);
    CHECK_EQ(SpscRing::floor_pow2(1023), 512u);
    CHECK_EQ(SpscRing::floor_pow2(1024), 1024u);
    CHECK_EQ(SpscRing::floor_pow2(1025), 1024u);
    CHECK_EQ(SpscRing::floor_pow2(0x80000000u), 0x80000000u);
    CHECK_EQ(SpscRing::floor_pow2(0xFFFFFFFFu), 0x80000000u);
    CHECK(SpscRing::is_pow2(64));
    CHECK(!SpscRing::is_pow2(65));
    CHECK(!SpscRing::is_pow2(0));
}

// init() rounds a non-power-of-two capacity DOWN rather than trusting the caller. Before that, a
// non-power-of-two silently mismatched the mask and every transfer landed at the wrong offset.
TEST(ring_init_rounds_capacity_down)
{
    uint8_t buf[100];
    SpscRing r;
    r.init(buf, 100);
    CHECK_EQ(r.capacity(), 64u);

    // ...and the ring is still coherent at the rounded size: fill it, drain it, get it all back.
    const auto in = ramp(64);
    CHECK_EQ(r.write(in.data(), 64), 64u);
    CHECK_EQ(r.writable(), 0u);
    uint8_t out[64] = {};
    CHECK_EQ(r.read(out, 64), 64u);
    CHECK_EQ(std::memcmp(out, in.data(), 64), 0);
}

// A zero capacity must not be usable, and must not memcpy through a null buffer.
TEST(ring_zero_capacity_is_inert)
{
    SpscRing r;
    r.init(nullptr, 0);
    CHECK_EQ(r.capacity(), 0u);
    CHECK_EQ(r.writable(), 0u);
    CHECK_EQ(r.readable(), 0u);
    uint8_t b[4] = {1, 2, 3, 4};
    CHECK_EQ(r.write(b, 4), 0u);
    CHECK_EQ(r.read(b, 4), 0u);
}

// --- transfer arithmetic -------------------------------------------------------------------------

TEST(ring_whole_capacity_is_usable)
{
    uint8_t buf[16];
    SpscRing r;
    r.init(buf, 16);
    CHECK_EQ(r.writable(), 16u);   // no reserved slot: kfifo-style free-running indices

    const auto in = ramp(16);
    CHECK_EQ(r.write(in.data(), 16), 16u);
    CHECK_EQ(r.readable(), 16u);
    CHECK_EQ(r.writable(), 0u);
    CHECK_EQ(r.write(in.data(), 1), 0u);   // full: short write, not an overwrite
}

TEST(ring_short_read_and_write_at_the_edges)
{
    uint8_t buf[8];
    SpscRing r;
    r.init(buf, 8);

    const auto in = ramp(12);
    CHECK_EQ(r.write(in.data(), 12), 8u);  // clipped to capacity

    uint8_t out[12];
    std::memset(out, 0xAA, sizeof(out));
    CHECK_EQ(r.read(out, 12), 8u);         // clipped to what is there
    CHECK_EQ(std::memcmp(out, in.data(), 8), 0);
    CHECK_EQ(out[8], 0xAA);                // read() does NOT zero-fill; that is PlayStream's job
    CHECK_EQ(r.readable(), 0u);
}

// The split copy: a write that starts near the end of the buffer wraps into the front half. This is
// the case a naive single-memcpy implementation gets wrong.
TEST(ring_transfer_splits_across_the_buffer_end)
{
    uint8_t buf[8];
    SpscRing r;
    r.init(buf, 8);

    // Advance head/tail to offset 6, leaving 2 bytes before the wrap.
    const auto seed = ramp(6, 200);
    CHECK_EQ(r.write(seed.data(), 6), 6u);
    uint8_t drain[6];
    CHECK_EQ(r.read(drain, 6), 6u);
    CHECK_EQ(r.readable(), 0u);

    const auto in = ramp(8, 1);            // 1..8, straddling the end of the buffer
    CHECK_EQ(r.write(in.data(), 8), 8u);
    uint8_t out[8] = {};
    CHECK_EQ(r.read(out, 8), 8u);
    CHECK_EQ(std::memcmp(out, in.data(), 8), 0);
}

// The property the free-running-index scheme exists for: correctness is preserved across the point
// where the raw uint32_t counters wrap. Driving 2^32 bytes through the ring one chunk at a time is
// too slow, so this drives it far enough to wrap several times at a chunk size that is coprime with
// the capacity - which walks every possible head/tail alignment.
TEST(ring_survives_many_wraps_with_misaligned_chunks)
{
    uint8_t buf[64];
    SpscRing r;
    r.init(buf, 64);

    uint8_t next_write = 0, next_read = 0;
    uint8_t chunk[7], got[7];
    for (int round = 0; round < 5000; round++) {
        for (int i = 0; i < 7; i++) chunk[i] = next_write++;
        const uint32_t put = r.write(chunk, 7);
        CHECK_EQ(put, 7u);                       // 7 always fits: we drain 7 each round
        const uint32_t take = r.read(got, 7);
        CHECK_EQ(take, 7u);
        for (uint32_t i = 0; i < take; i++) CHECK_EQ(got[i], next_read++);
    }
    CHECK_EQ(r.readable(), 0u);
}

TEST(ring_reset_empties_it)
{
    uint8_t buf[16];
    SpscRing r;
    r.init(buf, 16);
    const auto in = ramp(10);
    CHECK_EQ(r.write(in.data(), 10), 10u);
    CHECK_EQ(r.readable(), 10u);
    r.reset();
    CHECK_EQ(r.readable(), 0u);
    CHECK_EQ(r.writable(), 16u);
}

// --- PlayStream / RecordStream -------------------------------------------------------------------

namespace {

// An IChunkSource over a byte vector, with a settable per-read cap so a test can reproduce the
// "chunked source returns fewer bytes than asked without being at EOF" case that pump() must NOT
// treat as end-of-file.
class MemSource : public IChunkSource {
public:
    explicit MemSource(std::vector<uint8_t> data, uint32_t max_read = 0xFFFFFFFFu)
    : _data(std::move(data)), _max_read(max_read) {}

    uint32_t read(uint8_t* dst, uint32_t n) override
    {
        if (n > _max_read) n = _max_read;
        const uint32_t left = static_cast<uint32_t>(_data.size()) - _pos;
        if (n > left) n = left;
        // memcpy's arguments are declared non-null even for a zero length, and an empty vector's
        // data() is null - so the empty-source case has to short-circuit here.
        if (n) std::memcpy(dst, _data.data() + _pos, n);
        _pos += n;
        _reads++;
        return n;
    }
    bool eof() const override { return _pos >= _data.size(); }
    void rewind() override { _pos = 0; _rewinds++; }

    int rewinds() const { return _rewinds; }
    int reads()   const { return _reads; }

private:
    std::vector<uint8_t> _data;
    uint32_t             _max_read;
    uint32_t             _pos = 0;
    int                  _rewinds = 0;
    int                  _reads = 0;
};

// An IChunkSink collecting into a vector, recording how many times finalize() ran.
class MemSink : public IChunkSink {
public:
    uint32_t write(const uint8_t* src, uint32_t n) override
    {
        _data.insert(_data.end(), src, src + n);
        return n;
    }
    void finalize() override { _finalized++; }

    const std::vector<uint8_t>& data() const { return _data; }
    int finalized() const { return _finalized; }

private:
    std::vector<uint8_t> _data;
    int                  _finalized = 0;
};

} // namespace

TEST(play_stream_delivers_the_whole_source_then_finishes)
{
    uint8_t ring_buf[64], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 64);

    PlayStream play;
    play.init(&ring, scratch, sizeof(scratch));
    MemSource src(ramp(100));
    play.start(&src);

    std::vector<uint8_t> out;
    uint8_t block[8];
    for (int i = 0; i < 40 && !play.finished(); i++) {
        play.pump();                             // main loop
        const uint32_t got = play.consume(block, 8);   // ISR
        out.insert(out.end(), block, block + got);
    }

    CHECK(play.finished());
    CHECK_EQ(out.size(), 100u);
    CHECK_EQ(std::memcmp(out.data(), ramp(100).data(), 100), 0);
    // Running dry at the true end of the file is the natural finish, not an underrun.
    CHECK_EQ(play.underruns(), 0u);
}

TEST(play_stream_counts_an_underrun_but_not_an_end_of_stream)
{
    uint8_t ring_buf[64], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 64);

    PlayStream play;
    play.init(&ring, scratch, sizeof(scratch));
    MemSource src(ramp(32));
    play.start(&src);

    // Consume before ever pumping: the ring is empty and the source is NOT exhausted, so this is a
    // genuine underrun (the SD pump fell behind) and must be counted.
    uint8_t block[8];
    std::memset(block, 0xAA, sizeof(block));
    CHECK_EQ(play.consume(block, 8), 0u);
    CHECK_EQ(play.underruns(), 1u);
    for (int i = 0; i < 8; i++) CHECK_EQ(block[i], 0u);   // shortfall is zero-filled = silence

    // Drain the file properly; the trailing short read must NOT add an underrun.
    std::vector<uint8_t> out;
    for (int i = 0; i < 20 && !play.finished(); i++) {
        play.pump();
        const uint32_t got = play.consume(block, 8);
        out.insert(out.end(), block, block + got);
    }
    CHECK(play.finished());
    CHECK_EQ(out.size(), 32u);
    CHECK_EQ(play.underruns(), 1u);
}

// A source that returns a short read without being at EOF must not be mistaken for end-of-file -
// pump() only ends the round on a ZERO-byte read.
TEST(play_stream_tolerates_short_reads_from_a_chunked_source)
{
    uint8_t ring_buf[64], scratch[32];
    SpscRing ring;
    ring.init(ring_buf, 64);

    PlayStream play;
    play.init(&ring, scratch, sizeof(scratch));
    MemSource src(ramp(64), /*max_read=*/3);   // never returns more than 3 bytes at a time
    play.start(&src);

    play.pump();
    CHECK(!play.finished());
    CHECK_EQ(ring.readable(), 64u);            // one pump still filled the ring
}

TEST(play_stream_loops_by_rewinding_at_eof)
{
    uint8_t ring_buf[64], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 64);

    PlayStream play;
    play.init(&ring, scratch, sizeof(scratch));
    MemSource src(ramp(10));
    play.set_loop(true);
    play.start(&src);

    play.pump();
    CHECK(!play.finished());                   // looping: EOF is not the end
    CHECK(src.rewinds() > 0);
    CHECK_EQ(ring.readable(), 64u);            // filled by repeating the 10-byte source

    uint8_t out[20] = {};
    CHECK_EQ(play.consume(out, 20), 20u);
    for (int i = 0; i < 20; i++) CHECK_EQ(out[i], static_cast<uint8_t>(i % 10));
}

// An EMPTY looping source would rewind forever; pump() bounds the rewinds per call.
TEST(play_stream_does_not_spin_on_an_empty_looping_source)
{
    uint8_t ring_buf[64], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 64);

    PlayStream play;
    play.init(&ring, scratch, sizeof(scratch));
    MemSource src({});                         // zero bytes
    play.set_loop(true);
    play.start(&src);

    play.pump();                               // must return, not hang
    CHECK(src.rewinds() <= 2);
    CHECK(play.finished());                    // gave up and latched EOF
}

TEST(record_stream_drains_to_the_sink_and_finalizes_once)
{
    uint8_t ring_buf[64], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 64);

    RecordStream rec;
    rec.init(&ring, scratch, sizeof(scratch));
    MemSink sink;
    rec.start(&sink);

    const auto in = ramp(50);
    for (uint32_t off = 0; off < 50; off += 10) {
        CHECK_EQ(rec.produce(in.data() + off, 10), 10u);   // ISR
        rec.pump();                                        // main loop
    }
    CHECK_EQ(sink.finalized(), 0);             // not stopped yet

    rec.stop();
    rec.pump();
    CHECK(rec.finished());
    CHECK_EQ(sink.finalized(), 1);
    CHECK_EQ(sink.data().size(), 50u);
    CHECK_EQ(std::memcmp(sink.data().data(), in.data(), 50), 0);

    rec.pump();                                // extra pumps must not re-finalize
    CHECK_EQ(sink.finalized(), 1);
    CHECK_EQ(rec.overruns(), 0u);
}

TEST(record_stream_counts_dropped_bytes_when_the_pump_falls_behind)
{
    uint8_t ring_buf[16], scratch[16];
    SpscRing ring;
    ring.init(ring_buf, 16);

    RecordStream rec;
    rec.init(&ring, scratch, sizeof(scratch));
    MemSink sink;
    rec.start(&sink);

    const auto in = ramp(40);
    CHECK_EQ(rec.produce(in.data(), 40), 16u); // only the ring's worth fits
    CHECK_EQ(rec.overruns(), 24u);             // the rest is dropped and counted, never blocks

    rec.stop();
    rec.pump();
    CHECK_EQ(sink.data().size(), 16u);
    CHECK_EQ(sink.finalized(), 1);
}

int main() { return daisyapps::test::run_all(); }
