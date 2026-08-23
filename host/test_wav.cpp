// WavStreamReader's chunk walk, WavStreamWriter's placeholder/finalize round-trip, and
// find_cue_points' bounds handling.
//
// The reader is the piece that decides whether a card's file plays at all, and its failure mode on
// hardware is silence with no diagnostic - which is exactly the sort of thing that should be settled
// on a host. The suite runs under ASan/UBSan, so a chunk walk that steps past the end of a truncated
// file fails here rather than reading whatever happened to follow it in SDRAM.

#include "check.h"

#include "memory/byte_file.h"
#include "memory/wav.h"
#include "memory/wav_stream.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace daisyapps;

namespace {

// IByteFile over a byte vector. Reads/writes are exact; seek past the end fails, as FatFs' would.
class MemFile : public IByteFile {
public:
    MemFile() = default;
    explicit MemFile(std::vector<uint8_t> d) : _d(std::move(d)) {}

    uint32_t read(void* dst, uint32_t n) override
    {
        const uint32_t left = static_cast<uint32_t>(_d.size()) - _pos;
        if (n > left) n = left;
        if (n) std::memcpy(dst, _d.data() + _pos, n);
        _pos += n;
        return n;
    }
    uint32_t write(const void* src, uint32_t n) override
    {
        if (_pos + n > _d.size()) _d.resize(_pos + n);
        if (n) std::memcpy(_d.data() + _pos, src, n);
        _pos += n;
        return n;
    }
    bool seek(uint32_t pos) override
    {
        if (pos > _d.size()) return false;
        _pos = pos;
        return true;
    }

    const std::vector<uint8_t>& bytes() const { return _d; }

private:
    std::vector<uint8_t> _d;
    uint32_t             _pos = 0;
};

void push_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
void push_u16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void push_id(std::vector<uint8_t>& v, const char* id)
{
    v.insert(v.end(), id, id + 4);
}

// A chunk with an explicit id and body, word-padded as the spec requires.
void push_chunk(std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& body)
{
    push_id(v, id);
    push_u32(v, static_cast<uint32_t>(body.size()));
    v.insert(v.end(), body.begin(), body.end());
    if (body.size() & 1u) v.push_back(0);
}

std::vector<uint8_t> fmt_body(uint16_t fmt, uint16_t channels, uint32_t rate, uint16_t bits)
{
    std::vector<uint8_t> b;
    push_u16(b, fmt);
    push_u16(b, channels);
    push_u32(b, rate);
    push_u32(b, rate * channels * (bits / 8u));   // BytePerSec
    push_u16(b, static_cast<uint16_t>(channels * (bits / 8u)));
    push_u16(b, bits);
    return b;
}

// The format the streaming path actually accepts, per WavStreamReader's engine-capability gates.
std::vector<uint8_t> native_fmt()
{
    return fmt_body(kWavAudioFormat, 1, WavStreamReader::kPlaybackSampleRate, kWavBitsPerSample);
}

// Assemble a RIFF/WAVE file from a list of already-built chunks.
std::vector<uint8_t> riff(const std::vector<std::vector<uint8_t>>& chunks)
{
    std::vector<uint8_t> body;
    for (const auto& c : chunks) body.insert(body.end(), c.begin(), c.end());

    std::vector<uint8_t> f;
    push_id(f, "RIFF");
    push_u32(f, static_cast<uint32_t>(4 + body.size()));   // "WAVE" + chunks
    push_id(f, "WAVE");
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> c;
    push_chunk(c, id, body);
    return c;
}

std::vector<uint8_t> ramp(uint32_t n)
{
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i);
    return v;
}

} // namespace

// --- WavStreamReader: the happy path -------------------------------------------------------------

TEST(reader_accepts_a_minimal_native_file)
{
    const auto audio = ramp(64);
    MemFile f(riff({chunk("fmt ", native_fmt()), chunk("data", audio)}));

    WavStreamReader r;
    CHECK(r.begin(&f));
    CHECK_EQ(r.data_bytes(), 64u);
    CHECK_EQ(r.body_remaining(), 64u);
    CHECK(!r.eof());

    std::vector<uint8_t> out(64);
    CHECK_EQ(r.read(out.data(), 64), 64u);
    CHECK_EQ(std::memcmp(out.data(), audio.data(), 64), 0);
    CHECK(r.eof());
}

// A conformant reader must SKIP metadata chunks by their size and keep walking to `data`. An
// externally-authored file (anything that has been through a DAW) routinely has several.
TEST(reader_skips_metadata_chunks_before_data)
{
    const auto audio = ramp(32);
    MemFile f(riff({
        chunk("JUNK", ramp(30)),           // odd size: exercises the word-alignment pad
        chunk("fmt ", native_fmt()),
        chunk("LIST", ramp(64)),
        chunk("bext", ramp(16)),
        chunk("data", audio),
    }));

    WavStreamReader r;
    CHECK(r.begin(&f));
    CHECK_EQ(r.data_bytes(), 32u);

    std::vector<uint8_t> out(32);
    CHECK_EQ(r.read(out.data(), 32), 32u);
    CHECK_EQ(std::memcmp(out.data(), audio.data(), 32), 0);
}

// Trailing chunks after `data` must not be streamed as audio - the reader stops at DataSize.
TEST(reader_stops_at_data_size_and_ignores_trailing_chunks)
{
    const auto audio = ramp(16);
    MemFile f(riff({chunk("fmt ", native_fmt()), chunk("data", audio), chunk("cue ", ramp(28))}));

    WavStreamReader r;
    CHECK(r.begin(&f));
    std::vector<uint8_t> out(64);
    CHECK_EQ(r.read(out.data(), 64), 16u);   // clipped to the data chunk, not the file
    CHECK(r.eof());
}

TEST(reader_rewinds_to_the_data_chunk_for_looping)
{
    const auto audio = ramp(16);
    MemFile f(riff({chunk("LIST", ramp(20)), chunk("fmt ", native_fmt()), chunk("data", audio)}));

    WavStreamReader r;
    CHECK(r.begin(&f));
    std::vector<uint8_t> out(16);
    CHECK_EQ(r.read(out.data(), 16), 16u);
    CHECK(r.eof());

    r.rewind();
    CHECK(!r.eof());
    CHECK_EQ(r.body_remaining(), 16u);
    std::vector<uint8_t> again(16);
    CHECK_EQ(r.read(again.data(), 16), 16u);
    CHECK_EQ(std::memcmp(again.data(), audio.data(), 16), 0);   // back to the body, not the header
}

// WAVE_FORMAT_EXTENSIBLE (0xFFFE) carries the real tag in the SubFormat GUID's first two bytes.
TEST(reader_resolves_wave_format_extensible)
{
    std::vector<uint8_t> ext = fmt_body(0xFFFE, 1, WavStreamReader::kPlaybackSampleRate,
                                        kWavBitsPerSample);
    push_u16(ext, 22);                       // cbSize
    push_u16(ext, kWavBitsPerSample);        // wValidBitsPerSample
    push_u32(ext, 0x4);                      // dwChannelMask
    push_u16(ext, kWavAudioFormat);          // SubFormat GUID: real tag in the first two bytes
    while (ext.size() < 40) ext.push_back(0);

    MemFile f(riff({chunk("fmt ", ext), chunk("data", ramp(8))}));
    WavStreamReader r;
    CHECK(r.begin(&f));
    CHECK_EQ(r.data_bytes(), 8u);
}

// --- WavStreamReader: the rejections -------------------------------------------------------------
//
// These are engine-capability gates, not spec strictness: the streaming path hands raw body bytes
// straight to the engine's frames with no conversion, so a mismatched file would be reinterpreted as
// noise. A reject becomes the deck's error flash; a mis-play would not.

TEST(reader_rejects_wrong_channel_count)
{
    MemFile f(riff({chunk("fmt ", fmt_body(kWavAudioFormat, 2,
                                           WavStreamReader::kPlaybackSampleRate, kWavBitsPerSample)),
                    chunk("data", ramp(8))}));
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

TEST(reader_rejects_wrong_sample_rate)
{
    MemFile f(riff({chunk("fmt ", fmt_body(kWavAudioFormat, 1, 44100, kWavBitsPerSample)),
                    chunk("data", ramp(8))}));
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

TEST(reader_rejects_wrong_bit_depth)
{
    const uint16_t wrong_bits = (kWavBitsPerSample == 32) ? 16 : 32;
    MemFile f(riff({chunk("fmt ", fmt_body(kWavAudioFormat, 1,
                                           WavStreamReader::kPlaybackSampleRate, wrong_bits)),
                    chunk("data", ramp(8))}));
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

TEST(reader_rejects_data_before_fmt)
{
    MemFile f(riff({chunk("data", ramp(8)), chunk("fmt ", native_fmt())}));
    WavStreamReader r;
    CHECK(!r.begin(&f));   // `fmt ` must precede `data`; nothing to validate against otherwise
}

TEST(reader_rejects_a_short_fmt_chunk)
{
    MemFile f(riff({chunk("fmt ", ramp(8)), chunk("data", ramp(8))}));   // WAVEFORMAT is >= 16 bytes
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

TEST(reader_rejects_a_non_riff_file)
{
    MemFile f(std::vector<uint8_t>{'N', 'O', 'P', 'E', 0, 0, 0, 0, 'W', 'A', 'V', 'E'});
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

TEST(reader_rejects_a_file_too_short_to_hold_a_header)
{
    MemFile f(std::vector<uint8_t>{'R', 'I', 'F', 'F'});
    WavStreamReader r;
    CHECK(!r.begin(&f));
}

// A file whose chunk list runs off the end before `data` must fail rather than walk past it. Under
// ASan an over-read here is a hard failure, which is the point of running this on a host.
TEST(reader_rejects_a_truncated_file_without_over_reading)
{
    auto bytes = riff({chunk("fmt ", native_fmt()), chunk("data", ramp(64))});
    bytes.resize(bytes.size() - 40);       // chop into the data body... still fine (size is declared)
    MemFile f(bytes);
    WavStreamReader r;
    CHECK(r.begin(&f));                    // header intact: the reader trusts DataSize
    std::vector<uint8_t> out(64);
    CHECK(r.read(out.data(), 64) < 64u);   // ...but the actual read comes up short, safely

    auto cut = riff({chunk("fmt ", native_fmt())});
    cut.resize(cut.size() - 6);            // truncate mid-fmt: no `data` will ever be found
    MemFile g(cut);
    WavStreamReader s;
    CHECK(!s.begin(&g));
}

// A chunk whose declared size is absurd must not send the walk into an infinite or wrapping loop.
TEST(reader_rejects_a_chunk_with_an_overflowing_size)
{
    std::vector<uint8_t> f;
    push_id(f, "RIFF");
    push_u32(f, 0xFFFFFFFFu);
    push_id(f, "WAVE");
    push_id(f, "JUNK");
    push_u32(f, 0xFFFFFFF0u);              // would wrap `pos + 8 + size`
    f.insert(f.end(), 16, 0);

    MemFile mf(f);
    WavStreamReader r;
    CHECK(!r.begin(&mf));                  // terminates, and says no
}

// --- WavStreamWriter -----------------------------------------------------------------------------

TEST(writer_round_trips_through_the_reader)
{
    MemFile f;
    WavStreamWriter w;
    CHECK(w.begin(&f, 1));                  // mono, as the tape engine records
    CHECK_EQ(f.bytes().size(), 44u);        // placeholder header written up front

    const auto audio = ramp(128);
    CHECK_EQ(w.write(audio.data(), 128), 128u);
    CHECK_EQ(w.body_bytes(), 128u);
    w.finalize();                           // seeks back and patches the real sizes

    CHECK_EQ(f.bytes().size(), 44u + 128u);

    // The file the writer produced must be one the reader accepts, byte for byte.
    MemFile back(f.bytes());
    WavStreamReader r;
    CHECK(r.begin(&back));
    CHECK_EQ(r.data_bytes(), 128u);
    std::vector<uint8_t> out(128);
    CHECK_EQ(r.read(out.data(), 128), 128u);
    CHECK_EQ(std::memcmp(out.data(), audio.data(), 128), 0);
}

TEST(writer_finalizes_an_empty_recording)
{
    MemFile f;
    WavStreamWriter w;
    CHECK(w.begin(&f, 1));
    w.finalize();
    CHECK_EQ(w.body_bytes(), 0u);
    CHECK_EQ(f.bytes().size(), 44u);

    MemFile back(f.bytes());
    WavStreamReader r;
    CHECK(r.begin(&back));                  // a valid zero-length file, not a corrupt one
    CHECK_EQ(r.data_bytes(), 0u);
    CHECK(r.eof());
}

TEST(wav_header_builder_is_44_bytes_and_self_consistent)
{
    const WavHeader h = wav_header(1000, 1);
    CHECK_EQ(sizeof(h), 44u);
    CHECK_EQ(h.DataSize, 1000u);
    CHECK_EQ(h.size, 1000u + 44u - 8u);     // RIFF size excludes the id and the size field itself
    CHECK_EQ(h.NbrChannels, 1u);
    CHECK_EQ(h.SampleRate, 48000u);
    CHECK_EQ(h.BitsPerSample, kWavBitsPerSample);
    CHECK_EQ(h.BytePerBloc, static_cast<uint16_t>(kWavBytesPerSample * 1));
    CHECK_EQ(h.BytePerSec, 48000u * kWavBytesPerSample);
}

// --- find_cue_points -----------------------------------------------------------------------------

TEST(cue_points_are_parsed_and_bounded)
{
    std::vector<uint8_t> cue;
    push_u32(cue, 3);                       // numCuePoints
    for (uint32_t i = 0; i < 3; i++) {
        std::vector<uint8_t> pt(24, 0);
        const uint32_t frame = 100u * (i + 1);
        pt[20] = static_cast<uint8_t>(frame);
        pt[21] = static_cast<uint8_t>(frame >> 8);
        pt[22] = static_cast<uint8_t>(frame >> 16);
        pt[23] = static_cast<uint8_t>(frame >> 24);
        cue.insert(cue.end(), pt.begin(), pt.end());
    }

    const auto f = riff({chunk("fmt ", native_fmt()), chunk("cue ", cue), chunk("data", ramp(8))});

    WavCues out;
    find_cue_points(f.data(), f.size(), /*frame_limit=*/1000, out);
    CHECK_EQ(out.count, 3);
    CHECK_EQ(out.frames[0], 100u);
    CHECK_EQ(out.frames[1], 200u);
    CHECK_EQ(out.frames[2], 300u);

    // Markers at or past the end of the audio are dropped, not clamped.
    find_cue_points(f.data(), f.size(), /*frame_limit=*/250, out);
    CHECK_EQ(out.count, 2);

    // No `cue ` chunk -> no markers, and no complaint.
    const auto g = riff({chunk("fmt ", native_fmt()), chunk("data", ramp(8))});
    find_cue_points(g.data(), g.size(), 1000, out);
    CHECK_EQ(out.count, 0);
}

// A cue chunk claiming more points than it contains must stop at the buffer, not read past it.
// (ASan is the real assertion here.)
TEST(cue_points_survive_a_lying_count)
{
    std::vector<uint8_t> cue;
    push_u32(cue, 999);                     // claims 999 points...
    std::vector<uint8_t> pt(24, 0);
    pt[20] = 5;
    cue.insert(cue.end(), pt.begin(), pt.end());   // ...and carries exactly one

    const auto f = riff({chunk("cue ", cue), chunk("fmt ", native_fmt()), chunk("data", ramp(8))});
    WavCues out;
    find_cue_points(f.data(), f.size(), 1000, out);
    CHECK_EQ(out.count, 1);
    CHECK_EQ(out.frames[0], 5u);
}

TEST(cue_points_ignore_a_non_riff_buffer)
{
    const uint8_t junk[12] = {'N', 'O', 'P', 'E', 1, 2, 3, 4, 'W', 'A', 'V', 'E'};
    WavCues out;
    out.count = 7;                          // must be reset even on the reject path
    find_cue_points(junk, sizeof(junk), 1000, out);
    CHECK_EQ(out.count, 0);
}

int main() { return daisyapps::test::run_all(); }
