#pragma once

#include "engine/istreamdeck.h"
#include "memory/spsc_ring.h"
#include "memory/audio_stream.h"
#include "memory/wav_stream.h"
#include "memory/raw_stream.h"
#include "hw/fat_file.h"

#include <atomic>
#include <cstdint>

namespace daisyapps {

// Platform streaming service: bridges the audio ISR (lock-free rings) and FatFs (main-loop SD I/O) for
// the `tape` engine. Two INDEPENDENT decks (A/B), each its own play-XOR-record state machine, file, and
// ring; the two share one scratch buffer because the main-loop pump services them sequentially (never
// concurrently). Control (start_*/stop) runs in the main loop; the ISR only calls the *_consume/produce.
class StreamDeck : public IStreamDeck {
public:
    struct Mem {
        uint8_t* ring_a;    uint32_t ring_a_bytes;     // power-of-two SDRAM ring, deck A (play XOR record)
        uint8_t* ring_b;    uint32_t ring_b_bytes;     // power-of-two SDRAM ring, deck B
        uint8_t* scratch;   uint32_t scratch_bytes;    // f_read/f_write staging, shared (sequential pumps)
    };
    void init(const Mem& m);

    // Main-loop pump (AppImpl::Loop): does the slow SD I/O for each deck that is active/finalizing.
    void process();

    // --- IStreamDeck (per-deck) -------------------------------------------------------------------
    uint32_t play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) override;       // ISR
    uint32_t record_produce(DeckRef::Ref deck, const uint8_t* src, uint32_t n) override; // ISR
    bool is_playing(DeckRef::Ref deck)   const override { return _d[deck].mode.load(std::memory_order_acquire) == Mode::play; }
    bool is_recording(DeckRef::Ref deck) const override { return _d[deck].mode.load(std::memory_order_acquire) == Mode::record; }
    bool start_play(DeckRef::Ref deck, const char* path)   override;   // main loop
    bool start_record(DeckRef::Ref deck, const char* path) override;   // main loop
    void stop(DeckRef::Ref deck)                           override;   // main loop
    void set_loop(DeckRef::Ref deck, bool loop)            override;   // main loop
    uint32_t loop_frames(DeckRef::Ref deck) const          override;
    bool exists(const char* path) const                    override;   // main loop (f_stat)

    // Raw 16-bit-mono streaming for the radio engine (main loop). start_play_raw seeks-on-open to the
    // free-running playhead position; frames_of/scan_bank build the bank index off the audio path.
    bool start_play_raw(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override;
    bool start_play_wav(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override;
    bool seek_play(DeckRef::Ref deck, uint32_t frame) override;
    uint32_t frames_of(const char* path) const override;
    int  scan_bank(const char* dir, BankEntry* out, int max) const override;
    int  read_text(const char* path, char* buf, int max) const override;
    bool write_text(const char* path, const char* buf, int n) override;

private:
    enum class Mode : uint8_t { idle, play, record };

    // One self-contained streaming unit per deck. The ring is shared between play and record (a deck is
    // only ever doing one); play/record streams bind to it and to the common scratch at init().
    struct Deck {
        std::atomic<Mode> mode{Mode::idle};
        // Seqlock-style generation counter guarding main-loop mutations of the live stream state
        // against a concurrent ISR consume. ODD = a mutation is in flight.
        //
        // `mode` alone is not enough. play_consume() reads mode, finds `play`, and then calls
        // PlayStream::consume() -> SpscRing::read(); those two steps are not atomic, so an ISR that
        // passed the check immediately before seek_play() stores `idle` proceeds into the ring while
        // the main loop is inside SpscRing::reset(). reset() rewrites head and tail with relaxed
        // stores, so the consumer can observe an arbitrary `head - tail` and copy out a ring's worth
        // of stale bytes. It stays in bounds (the mask never changes) - the symptom is a burst of
        // previously-played audio on every seek, not a fault - but it is a real data race, and
        // `bard` seeks a playing file as a matter of course.
        //
        // The consumer samples this before and after the copy and discards the result if it moved
        // (see play_consume). Cost: one block of silence on a seek, which is what a seek sounds like
        // anyway.
        std::atomic<uint32_t> gen{0};
        bool            finalizing = false;  // record stopped; main loop flushing the tail + finalizing
        SpscRing        ring;
        PlayStream      play;
        RecordStream    record;
        WavStreamReader reader;
        RawStreamReader raw;                 // headerless 16-bit-mono source (radio/bard engines)
        bool            raw_src = false;     // the live play source is `raw` (frame-seekable), not `reader`
        WavStreamWriter writer;
        FatFile         file;                // one file handle per deck (play XOR record)
    };
    Deck _d[2];

    void _pump(Deck& d);
};

} // namespace daisyapps
