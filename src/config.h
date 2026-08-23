#pragma once

#include <string.h>
#include <stdint.h>
#include <algorithm>

namespace daisyapps {

class Config
{
public:
    struct Values {
        uint8_t midi_channel_a = 0; // Actual 1
        uint8_t midi_channel_b = 1; // Actual 2
        uint8_t midi_play_stop_a = 0;
        uint8_t midi_play_stop_b = 0;
        bool is_preload_on = true;
    };

    uint8_t midi_channel_a() const { return _vals.midi_channel_a; }
    uint8_t midi_channel_b() const { return _vals.midi_channel_b; }
    uint8_t midi_play_stop_a() const { return _vals.midi_play_stop_a; }
    uint8_t midi_play_stop_b() const { return _vals.midi_play_stop_b; }
    bool is_preload_on() const { return _vals.is_preload_on; }

    // NOTE: this used to carry a fill(const uint8_t*, size_t) that parsed a key/value text blob off
    // the SD card into _vals, plus an is_loaded() flag. Both were removed: nothing in the repo ever
    // called either (so every engine has always run on the defaults above), and the parser was not
    // safe to keep as latent code - it built its line buffers as VLAs from a non-constant `int`, read
    // past the 8-byte line buffer for any line longer than 8 characters, overflowed its int8_t index
    // on a long line, and memcmp'd an uninitialised `prop` on a leading numeric line.
    //
    // If per-card configuration is wanted later, write it fresh against IStreamDeck::read_text (which
    // already hands back a NUL-terminated, length-bounded buffer) and cover it in host/ - it is pure
    // string handling, so there is no reason for it to be untested.

    static Config& dynamic()
    {
        static Config instance;
        return instance;
    }
    Config(Config const&)           = delete;
    void operator=(Config const&)   = delete;

private:
    Config() {}
    Values _vals;
};


// STATIC CONFIG ///////////////////////////////////////////////
// Clock ........................................
static constexpr uint8_t kPPQNIntern = 48;

// Buffer
static constexpr size_t kRecordFade = 192; // 4ms

// Grain ........................................
static constexpr size_t kWindowSlope = 960; //20ms @ 48K 1x
static constexpr size_t kMinimumWindowSize = 2 * kWindowSlope; //40ms @ 48k 1x
static constexpr size_t kDefaultWindowSize = 2880; //60ms @ 48k 1x

// Slice ........................................
static constexpr size_t kSliceSlope = 192; //4ms
static constexpr size_t kSliceMinSize = 2 * kSliceSlope + 960; //+20ms sustain @ 48K 1x

// LFO ..........................................
static constexpr float kLFOFreqMin = .01f;
static constexpr float kLFOFreqRange = 11.99f;

// Overdub ......................................
static constexpr float kDefaultFeedback = 0.95f; //-3db at -60...0dB scale

// Drift .........................................
static constexpr float kDriftStartOffsetL1  = .08f;
static constexpr float kDriftStartOffsetL2  = .15f;
static constexpr float kDriftStartKofL1     = 1.42f;
static constexpr float kDriftStartKofL2     = 1.85f;
static constexpr float kDriftSizeKofL1      = .62f;
static constexpr float kDriftSizeKofL2      = .38f;

// Slice points ..................................
static constexpr uint8_t kMaxSlicePointCount = 32;

// Loop buffer length, in seconds (the granular engine sizes its per-deck source buffer from this and
// the sample rate). Moved here from the SDRAM pool so the engine owns its buffer sizing, not the HAL.
// 16-bit storage halves bytes/frame, so the same SDRAM holds twice the seconds.
#if LOFI_INT16
static constexpr unsigned kSourceMaxSeconds = 84;
#else
static constexpr unsigned kSourceMaxSeconds = 42;
#endif

// Tempo control range (BPM). The platform's tempo knob spans this range; the granular Tempo class and
// the UI's tempo MValue display both convert through the helpers below. Lives here (not on Tempo) so
// the platform has no granular dependency for tempo display - Phase 5 R4.
static constexpr float kTempoMinBpm = 20.f;
static constexpr float kTempoMaxBpm = 250.f;
inline float tempo_abs_to_norm(const float bpm)
{
    return (bpm - kTempoMinBpm) / (kTempoMaxBpm - kTempoMinBpm);
}
inline float tempo_norm_to_abs(const float norm)
{
    return kTempoMinBpm + std::clamp(norm, 0.f, 1.f) * (kTempoMaxBpm - kTempoMinBpm);
}

}