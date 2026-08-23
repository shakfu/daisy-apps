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
// the board's gate input. A run of edges more than a second apart is treated as the clock having
// stopped, at which point the transport falls back to its internal BPM.
//
// EXTERNAL SYNC is not a tempo measurement alone. Each rising edge both sets the BPM (from the
// interval, scaled by how many pulses the incoming clock sends per quarter) and LANDS A TICK ON THE
// EDGE: the grid is realigned to the pulse and the divided tick is emitted there and then, so a step
// falls on the pulse rather than wherever a free-running interpolator happened to be. Between pulses
// the interpolator fills in the sub-ticks and any intermediate divided ticks, which is what makes a
// quarter-note clock able to drive a 1/16 sequencer. The consequence worth stating: sync error cannot
// accumulate, because every pulse re-datums the grid.
//
// What a pulse MEANS is a setting, not an assumption - a Eurorack clock might send one pulse per
// quarter, per eighth, per sixteenth, or MIDI's 24 PPQN. See ext_ppq(); the harness puts it on the
// action screen so it can be changed on the box.

#include <cstdint>
#include <functional>

// NO HAL INCLUDE HERE, deliberately. Everything below is arithmetic over an injected ITimeSource;
// the libDaisy-backed implementation of that interface lives in app/system_time.h. That split is
// what makes this file host-compilable, and the tick grid is the part of the harness most worth
// proving off-target (see host/test_transport.cpp).
#include "engine/itimesource.h"
#include "engine/itransport.h"
#include "engine/mode.h"

namespace daisyapps {

// A free-running musical clock: fixed internal BPM until an external clock arrives at a gate input,
// then locked to that. Ticks are generated on the main loop from the wall clock, at the same 48 PPQN
// resolution sk-engines' Divider uses, and fanned out to whichever engine subscribed.
//
// The two rates are NOT the same thing, and conflating them is a bug with a very loud symptom. The
// callback fires at 48 PPQN, but `TransportTick::tick` marks only the COMMON (divided) tick - a 1/16
// note, every 12 sub-ticks - and the sub-ticks in between carry tick=false purely to keep tempo
// tracking smooth. granular says so in its own sink (`deck.cpp`: "common_tick ... it's effectively
// 1/16th") and edrums relies on it: its MODFREQ divider is {1, 2, 4} common ticks per step, i.e.
// 1/16, 1/8, 1/4. Marking every sub-tick as common ran that sequencer twelve times too fast, which
// presented as "the clock is broken" rather than as a resolution mismatch.
class HarnessTransport : public ITransport {
public:
    static constexpr uint8_t  kPPQN            = 48;     // internal tick resolution (upstream's kPPQNIntern)
    static constexpr uint8_t  kSubPerCommon    = 12;     // sub-ticks per COMMON tick -> 1/16 at 48 PPQN
    static constexpr uint32_t kExtClockTimeout = 1000;   // ms without an edge -> external clock is gone
    static constexpr float    kMinBpm          = 20.f;
    static constexpr float    kMaxBpm          = 250.f;

    // What one pulse at the clock input means, as pulses per quarter note. 1 is the Eurorack default
    // (a quarter-note clock); 4 is a 16th clock, which lands a pulse on every step of a sequencer at
    // the common-tick rate; 24 is MIDI clock's resolution for a device that outputs it as gates.
    static constexpr uint8_t kExtPpqChoices[4] = { 1, 2, 4, 24 };

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

    // How many pulses the clock input sends per quarter note (1 = a quarter-note clock). Anything not
    // in kExtPpqChoices is ignored rather than clamped: a bad value here would silently misread every
    // interval, and a refused write is easier to notice than a wrong tempo.
    void set_ext_ppq(uint8_t ppq)
    {
        for (uint8_t c : kExtPpqChoices)
            if (c == ppq) { _ext_ppq = ppq; _pulse_index = 0; return; }
    }
    uint8_t ext_ppq() const { return _ext_ppq; }

    // One rising edge of an external clock at a gate input. Does two jobs.
    //
    // TEMPO: the interval between edges is one pulse, and _ext_ppq pulses make a quarter, so
    // bpm = 60000 / (delta_ms * _ext_ppq). The first edge only starts the measurement.
    //
    // PHASE: the pulse IS a grid position, so rather than let the interpolator drift between edges,
    // the grid is re-datumed here and the tick that belongs on this pulse is emitted immediately. A
    // 1-PPQ clock therefore lands a quarter (and a common tick) exactly on the edge, and the three
    // intervening 1/16 ticks are interpolated; a 4-PPQ clock lands every step on an edge and nothing
    // is interpolated at all. Sync error cannot accumulate, because each pulse resets the datum.
    void on_external_clock_edge()
    {
        const uint32_t now = _time.now_ms();

        if (_last_edge_ms != 0) {
            const uint32_t delta = now - _last_edge_ms;
            if (delta > 0 && delta < kExtClockTimeout) {
                const float bpm = 60000.f / (static_cast<float>(delta) * static_cast<float>(_ext_ppq));
                if (bpm >= kMinBpm && bpm <= kMaxBpm) {
                    // A `reset` tells a sequencer to realign its own divider and restart its pattern,
                    // so it belongs to ACQUIRING sync, not to every pulse of a clock already running -
                    // edrums zeroes its step counter and rewinds all four patterns on one, which at a
                    // quarter-note clock would mean no pattern ever reaches its fifth step.
                    if (!_external) {
                        // Acquiring sync: this pulse becomes the downbeat, so the reset a sequencer
                        // acts on lands on a quarter and a common tick rather than mid-step.
                        _reset_pending = true;
                        _pulse_index   = 0;
                        _beat_index    = 0;
                    } else {
                        _pulse_index++;
                    }
                    _tempo    = bpm;
                    _external = true;

                    // Land on the grid position this pulse represents, and fire it now.
                    const uint32_t sub_per_pulse = kPPQN / _ext_ppq;               // 48, 24, 12, 2
                    _tick_in_beat = (_pulse_index % _ext_ppq) * sub_per_pulse;
                    _tick_acc_ms  = 0.f;
                    _last_tick_ms = now;
                    emit_sub_tick();
                }
            } else if (delta >= kExtClockTimeout) {
                _pulse_index = 0;   // a gap that long is a new take, not a slow beat
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

        // Sub-tick interpolation, at 48 PPQN. Under external sync this fills the gaps BETWEEN pulses;
        // each pulse then re-datums it (see on_external_clock_edge), so this never has to be right for
        // longer than one pulse.
        const float ms_per_tick = 60000.f / (_tempo * static_cast<float>(kPPQN));
        _tick_acc_ms += static_cast<float>(now - _last_tick_ms);
        _last_tick_ms = now;

        while (_tick_acc_ms >= ms_per_tick) {
            _tick_acc_ms -= ms_per_tick;
            emit_sub_tick();
        }
    }

private:
    // Emit the sub-tick at the current grid position and step the position on. `tick` marks the
    // COMMON tick only - one in every kSubPerCommon, i.e. a 1/16 - which is what a sequencing engine
    // steps off; the rest carry the tempo and nothing else. `index` counts common ticks, per the
    // ITransport contract ("monotonic counter of common (divided) ticks").
    void emit_sub_tick()
    {
        const bool common  = (_tick_in_beat % kSubPerCommon) == 0;
        const bool quarter = (_tick_in_beat == 0);
        // key_interval is in 1/16 units (kKeyInterval: k4_4 = 16 = one bar), so a key boundary falls
        // every interval/4 quarter notes.
        const uint32_t beats_per_key = _key_interval >= k1_4 ? (_key_interval / 4u) : 1u;

        TransportTick t;
        t.index   = _tick_index;
        t.tick    = common;
        t.quarter = quarter;
        t.key     = common && quarter && (_beat_index % beats_per_key) == 0;
        t.tempo   = _tempo;
        t.reset   = _reset_pending;
        _reset_pending = false;

        if (_on_tick) _on_tick(t);

        if (common) _tick_index++;
        if (++_tick_in_beat >= kPPQN) { _tick_in_beat = 0; _beat_index++; }
    }

    const ITimeSource& _time;

    std::function<void(const TransportTick&)> _on_tick;

    float    _internal_bpm  = 120.f;
    float    _tempo         = 120.f;
    bool     _external      = false;
    uint8_t  _key_interval  = k4_4;   // one bar, in 1/16 units (16)

    uint8_t  _ext_ppq       = 1;      // pulses per quarter at the clock input (Eurorack default)
    uint32_t _pulse_index   = 0;      // which pulse of the current beat the last edge was

    uint32_t _last_edge_ms  = 0;
    uint32_t _last_tick_ms  = 0;
    float    _tick_acc_ms   = 0.f;
    uint32_t _tick_index    = 0;
    uint32_t _tick_in_beat  = 0;
    uint32_t _beat_index    = 0;
    bool     _reset_pending = false;
};

} // namespace daisyapps
