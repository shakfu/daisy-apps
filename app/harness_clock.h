#pragma once

// The harness's clock glue: a libDaisy-backed ITimeSource and a minimal free-running ITransport.
//
// Upstream (sk-engines) the platform owns a full Transport service - internal / tap / TS4 / MIDI-clock
// sources, a Divider, key intervals, clock output. Engines only ever see the read-only ITransport view,
// and most of them read exactly one thing from it: tempo(). So the harness implements that view over a
// simple sample-counting clock, which is enough for every tempo-synced engine (delay, qdelay, edrums)
// and cheap enough to leave running on the boards that have no clock input at all.
//
// Two things drive the tempo: the BPM the harness sets (a knob, a default), and an external clock at
// the board's gate input. External sync is a tap-style measurement - each rising edge times the
// interval since the last one - and a run of edges more than a second apart is treated as the clock
// having stopped, at which point the transport falls back to its internal BPM.

#include <cstdint>
#include <functional>

#include "daisy.h"
#include "engine/itimesource.h"
#include "engine/itransport.h"
#include "engine/mode.h"

namespace daisyapps {

// ITimeSource over daisy::System. now_ms/now_us are what engines use for UI-rate motion (breathe,
// blink, tap timing); nothing in the audio path depends on them.
class SystemTime : public ITimeSource {
public:
    uint32_t now_ms() const override { return daisy::System::GetNow(); }
    uint32_t now_us() const override { return daisy::System::GetUs(); }
};

// A free-running musical clock: fixed internal BPM until an external clock arrives at a gate input,
// then locked to that. Ticks are generated on the main loop from the wall clock, at the same 48 PPQN
// resolution sk-engines' Divider uses, and fanned out to whichever engine subscribed.
class HarnessTransport : public ITransport {
public:
    static constexpr uint8_t  kPPQN            = 48;     // internal tick resolution (upstream's kPPQNIntern)
    static constexpr uint32_t kExtClockTimeout = 1000;   // ms without an edge -> external clock is gone
    static constexpr float    kMinBpm          = 20.f;
    static constexpr float    kMaxBpm          = 250.f;

    explicit HarnessTransport(const ITimeSource& time) : _time(time) {}

    // --- ITransport (the read-only view an engine gets) -----------------------------------------
    float tempo() const override { return _tempo; }

    // Always `internal`, even when a gate clock is steering the tempo. ClockSource's enumerators ARE
    // the external PPQN upstream (ts4 = 4, midi = 24), and this harness's gate clock is a 1-PPQN
    // quarter-note pulse that matches none of them - so reporting one would misstate the resolution to
    // an engine that divides by it. is_external_sync() below carries the honest answer.
    ClockSource::Source source() const override { return ClockSource::internal; }

    bool    is_external_sync() const override   { return _external; }
    uint8_t key_interval() const override       { return _key_interval; }
    bool    is_key_sub_quarter() const override { return _key_interval < k1_4; }

    void set_on_tick(std::function<void(const TransportTick&)> on_tick) override
    {
        _on_tick = std::move(on_tick);
    }

    // --- Harness-side control (not visible to the engine) ---------------------------------------
    void set_tempo(float bpm)
    {
        if (bpm < kMinBpm) bpm = kMinBpm;
        if (bpm > kMaxBpm) bpm = kMaxBpm;
        _internal_bpm = bpm;
        if (!_external) _tempo = bpm;
    }

    float internal_tempo() const { return _internal_bpm; }

    // One rising edge of an external clock at a gate input. Assumed to be a quarter-note pulse (the
    // eurorack norm), so the measured interval IS the beat: bpm = 60000 / ms_between_edges. The first
    // edge only starts the measurement; the second one sets a tempo.
    void on_external_clock_edge()
    {
        const uint32_t now = _time.now_ms();
        if (_last_edge_ms != 0) {
            const uint32_t delta = now - _last_edge_ms;
            if (delta > 0 && delta < kExtClockTimeout) {
                const float bpm = 60000.f / static_cast<float>(delta);
                if (bpm >= kMinBpm && bpm <= kMaxBpm) {
                    _tempo    = bpm;
                    _external = true;
                    // Realign the grid to the incoming pulse so a downbeat lands on the edge.
                    _tick_acc_ms = 0.f;
                    _last_tick_ms = now;
                    _reset_pending = true;
                }
            }
        }
        _last_edge_ms = now;
    }

    // Main loop: advance the tick grid and fan out any ticks that fell due since the last call. Cheap
    // (one float compare in the common case) and deliberately main-loop rather than ISR - engines
    // subscribing to ticks do sequencing work that has no business in the audio callback.
    void poll()
    {
        const uint32_t now = _time.now_ms();

        // External clock gone quiet -> fall back to the internal BPM.
        if (_external && _last_edge_ms != 0 && (now - _last_edge_ms) > kExtClockTimeout) {
            _external = false;
            _tempo    = _internal_bpm;
        }

        if (_last_tick_ms == 0) { _last_tick_ms = now; return; }

        const float ms_per_tick = 60000.f / (_tempo * static_cast<float>(kPPQN));
        _tick_acc_ms += static_cast<float>(now - _last_tick_ms);
        _last_tick_ms = now;

        while (_tick_acc_ms >= ms_per_tick) {
            _tick_acc_ms -= ms_per_tick;
            emit_tick();
        }
    }

private:
    void emit_tick()
    {
        const uint32_t ppq     = kPPQN;
        const bool     quarter = (_tick_in_beat == 0);
        // key_interval is in 1/16 units (kKeyInterval: k4_4 = 16 = one bar), so a key boundary falls
        // every interval/4 quarter notes.
        const uint32_t beats_per_key = _key_interval >= k1_4 ? (_key_interval / 4u) : 1u;

        TransportTick t;
        t.index   = _tick_index;
        t.tick    = true;
        t.quarter = quarter;
        t.key     = quarter && (_beat_index % beats_per_key) == 0;
        t.tempo   = _tempo;
        t.reset   = _reset_pending;
        _reset_pending = false;

        if (_on_tick) _on_tick(t);

        _tick_index++;
        if (++_tick_in_beat >= ppq) { _tick_in_beat = 0; _beat_index++; }
    }

    const ITimeSource& _time;

    std::function<void(const TransportTick&)> _on_tick;

    float    _internal_bpm  = 120.f;
    float    _tempo         = 120.f;
    bool     _external      = false;
    uint8_t  _key_interval  = k4_4;   // one bar, in 1/16 units (16)

    uint32_t _last_edge_ms  = 0;
    uint32_t _last_tick_ms  = 0;
    float    _tick_acc_ms   = 0.f;
    uint32_t _tick_index    = 0;
    uint32_t _tick_in_beat  = 0;
    uint32_t _beat_index    = 0;
    bool     _reset_pending = false;
};

} // namespace daisyapps
