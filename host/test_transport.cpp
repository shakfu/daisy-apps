// HarnessTransport: the tick grid every tempo-synced engine reads (delay, qdelay, edrums).
//
// Why this suite exists, in the file's own words: "The two rates are NOT the same thing, and
// conflating them is a bug with a very loud symptom... Marking every sub-tick as common ran that
// sequencer twelve times too fast, which presented as 'the clock is broken' rather than as a
// resolution mismatch." That is a defect class a host test settles in milliseconds and a device
// session settles in an afternoon.
//
// The transport takes its clock by injection (const ITimeSource&), so a fake clock drives the whole
// state machine deterministically - no sleeping, no wall-clock flakiness.

#include "check.h"

#include "app/harness_clock.h"

#include <cstdint>
#include <vector>

using namespace daisyapps;

namespace {

// A settable ITimeSource. Everything the transport does is a function of now_ms(), so advancing this
// IS advancing time.
class FakeTime : public ITimeSource {
public:
    uint32_t now_ms() const override { return _ms; }
    uint32_t now_us() const override { return _ms * 1000u; }

    void set(uint32_t ms) { _ms = ms; }
    void advance(uint32_t ms) { _ms += ms; }

private:
    uint32_t _ms = 0;
};

// Collects the ticks the transport fans out, so a test can assert on the shape of the grid rather
// than on one flag at one instant.
struct TickLog {
    std::vector<TransportTick> ticks;

    void attach(HarnessTransport& t)
    {
        t.set_on_tick([this](const TransportTick& tk) { ticks.push_back(tk); });
    }
    int count_common()  const { int n = 0; for (auto& t : ticks) if (t.tick) n++;    return n; }
    int count_quarter() const { int n = 0; for (auto& t : ticks) if (t.quarter) n++; return n; }
    int count_reset()   const { int n = 0; for (auto& t : ticks) if (t.reset) n++;   return n; }
    void clear() { ticks.clear(); }
};

// Run the main-loop poll for `ms` of simulated time, stepping in 1 ms slices the way the real loop
// samples the clock.
void run_for(HarnessTransport& t, FakeTime& clk, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) { clk.advance(1); t.poll(); }
}

} // namespace

// --- internal clock ------------------------------------------------------------------------------

TEST(transport_defaults_to_120_bpm_internal)
{
    FakeTime clk;
    HarnessTransport t(clk);
    CHECK_NEAR(t.tempo(), 120.f, 1e-4);
    CHECK_NEAR(t.internal_tempo(), 120.f, 1e-4);
    CHECK(!t.is_external_sync());
    CHECK_EQ(static_cast<int>(t.source()), static_cast<int>(ClockSource::internal));
}

TEST(transport_clamps_the_settable_tempo)
{
    FakeTime clk;
    HarnessTransport t(clk);
    t.set_tempo(1.f);
    CHECK_NEAR(t.tempo(), HarnessTransport::kMinBpm, 1e-4);
    t.set_tempo(10000.f);
    CHECK_NEAR(t.tempo(), HarnessTransport::kMaxBpm, 1e-4);
    t.set_tempo(140.f);
    CHECK_NEAR(t.tempo(), 140.f, 1e-4);
}

// THE distinction this file's header comment is about: the callback fires at 48 PPQN, but only one
// sub-tick in twelve is a COMMON tick (a 1/16 note). An engine that steps on every callback runs 12x
// fast; edrums' {1,2,4} clock divider assumes exactly this ratio.
TEST(transport_emits_twelve_sub_ticks_per_common_tick)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);

    t.set_tempo(120.f);              // 120 BPM -> 500 ms per quarter, 48 sub-ticks per quarter
    run_for(t, clk, 4000);           // 8 quarters

    CHECK(!log.ticks.empty());
    const int subs    = static_cast<int>(log.ticks.size());
    const int commons = log.count_common();
    CHECK(commons > 0);
    // 12 sub-ticks per common tick, within one tick of rounding at the ends.
    CHECK(subs >= commons * 12 - 12);
    CHECK(subs <= commons * 12 + 12);
}

TEST(transport_emits_four_common_ticks_per_quarter)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);

    t.set_tempo(120.f);
    run_for(t, clk, 4000);           // 8 quarters at 500 ms

    const int quarters = log.count_quarter();
    const int commons  = log.count_common();
    CHECK(quarters >= 7 && quarters <= 9);          // ~8 quarters in 4 s at 120 BPM
    // A common tick is a 1/16, so four per quarter.
    CHECK(commons >= quarters * 4 - 4);
    CHECK(commons <= quarters * 4 + 4);
}

// Every quarter is also a common tick (the grid positions coincide at tick_in_beat == 0); the
// converse is not true.
TEST(transport_every_quarter_is_also_a_common_tick)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);
    t.set_tempo(180.f);
    run_for(t, clk, 3000);

    for (const auto& tk : log.ticks)
        if (tk.quarter) CHECK(tk.tick);
}

TEST(transport_common_tick_rate_scales_with_tempo)
{
    FakeTime slow_clk, fast_clk;
    HarnessTransport slow(slow_clk), fast(fast_clk);
    TickLog slow_log, fast_log;
    slow_log.attach(slow);
    fast_log.attach(fast);

    slow.set_tempo(60.f);
    fast.set_tempo(120.f);
    run_for(slow, slow_clk, 4000);
    run_for(fast, fast_clk, 4000);

    // Double the tempo, double the ticks in the same wall-clock window (+/- one tick of rounding).
    const int s = slow_log.count_common(), f = fast_log.count_common();
    CHECK(s > 0);
    CHECK(f >= 2 * s - 2);
    CHECK(f <= 2 * s + 2);
}

TEST(transport_tick_index_counts_common_ticks_only)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);
    t.set_tempo(120.f);
    run_for(t, clk, 2000);

    // index is documented as "a monotonic counter of common (divided) ticks", so it must advance on
    // common ticks and hold still on the sub-ticks between them.
    uint32_t prev_index = 0;
    bool     first      = true;
    for (const auto& tk : log.ticks) {
        if (!first) {
            CHECK(tk.index >= prev_index);
            CHECK(tk.index - prev_index <= 1);
        }
        first      = false;
        prev_index = tk.index;
    }
    CHECK(prev_index > 0);
}

// --- external sync -------------------------------------------------------------------------------

TEST(transport_ext_ppq_accepts_only_the_offered_choices)
{
    FakeTime clk;
    HarnessTransport t(clk);
    CHECK_EQ(t.ext_ppq(), 1);                 // Eurorack default: one pulse per quarter

    for (uint8_t c : HarnessTransport::kExtPpqChoices) {
        t.set_ext_ppq(c);
        CHECK_EQ(t.ext_ppq(), c);
    }
    // A value outside the table is REFUSED, not clamped: silently misreading every interval is worse
    // than an ignored write.
    t.set_ext_ppq(4);
    t.set_ext_ppq(7);
    CHECK_EQ(t.ext_ppq(), 4);
    t.set_ext_ppq(0);
    CHECK_EQ(t.ext_ppq(), 4);
}

TEST(transport_derives_tempo_from_the_clock_interval)
{
    FakeTime clk;
    HarnessTransport t(clk);
    clk.set(1000);

    // 500 ms between quarter-note pulses = 120 BPM. The first edge only starts the measurement.
    t.on_external_clock_edge();
    CHECK(!t.is_external_sync());
    clk.advance(500);
    t.on_external_clock_edge();

    CHECK(t.is_external_sync());
    CHECK_NEAR(t.tempo(), 120.f, 0.5);

    clk.advance(500);
    t.on_external_clock_edge();
    CHECK_NEAR(t.tempo(), 120.f, 0.5);
}

// A pulse means whatever ext_ppq says it means. At 4 PPQ the same 125 ms interval is a 16th, not a
// quarter, so the same pulse rate yields the same 120 BPM.
TEST(transport_scales_the_interval_by_pulses_per_quarter)
{
    FakeTime clk;
    HarnessTransport t(clk);
    t.set_ext_ppq(4);
    clk.set(1000);

    t.on_external_clock_edge();
    for (int i = 0; i < 8; i++) { clk.advance(125); t.on_external_clock_edge(); }

    CHECK(t.is_external_sync());
    CHECK_NEAR(t.tempo(), 120.f, 1.0);        // 125 ms * 4 = 500 ms per quarter
}

// The pulse IS a grid position: acquiring sync lands a quarter (and therefore a common tick) on the
// edge rather than wherever the free-running interpolator happened to be.
TEST(transport_lands_a_tick_on_the_pulse)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);
    clk.set(1000);

    t.on_external_clock_edge();
    log.clear();
    clk.advance(500);
    t.on_external_clock_edge();

    CHECK_EQ(log.ticks.size(), 1u);           // exactly the tick belonging to this pulse
    CHECK(log.ticks[0].tick);                 // a 1/4 pulse is a common tick...
    CHECK(log.ticks[0].quarter);              // ...and a quarter
}

// `reset` tells a sequencer to rewind its pattern. It belongs to ACQUIRING sync, not to every pulse
// of a clock already running - edrums zeroes its step counter on one, so a reset per quarter would
// mean no pattern ever reaches its fifth step.
TEST(transport_resets_only_when_acquiring_sync)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);
    clk.set(1000);

    t.on_external_clock_edge();
    for (int i = 0; i < 8; i++) { clk.advance(500); t.on_external_clock_edge(); }

    CHECK(t.is_external_sync());
    CHECK_EQ(log.count_reset(), 1);           // exactly once, on the pulse that acquired sync
}

// A clock that stops must not leave the transport locked to a tempo nothing is driving.
TEST(transport_falls_back_to_internal_when_the_clock_stops)
{
    FakeTime clk;
    HarnessTransport t(clk);
    clk.set(1000);
    t.set_tempo(90.f);                        // the internal fallback

    t.on_external_clock_edge();
    clk.advance(500);
    t.on_external_clock_edge();
    CHECK(t.is_external_sync());
    CHECK_NEAR(t.tempo(), 120.f, 0.5);

    run_for(t, clk, HarnessTransport::kExtClockTimeout + 50);
    CHECK(!t.is_external_sync());
    CHECK_NEAR(t.tempo(), 90.f, 1e-4);        // back to the BPM the harness set
}

// set_tempo while externally synced must update the FALLBACK without disturbing the live tempo -
// otherwise the encoder gesture would fight the incoming clock.
TEST(transport_set_tempo_does_not_override_a_live_external_clock)
{
    FakeTime clk;
    HarnessTransport t(clk);
    clk.set(1000);

    t.on_external_clock_edge();
    clk.advance(500);
    t.on_external_clock_edge();
    CHECK_NEAR(t.tempo(), 120.f, 0.5);

    t.set_tempo(75.f);
    CHECK_NEAR(t.tempo(), 120.f, 0.5);        // still following the clock
    CHECK_NEAR(t.internal_tempo(), 75.f, 1e-4);

    run_for(t, clk, HarnessTransport::kExtClockTimeout + 50);
    CHECK_NEAR(t.tempo(), 75.f, 1e-4);        // ...and the new fallback takes over when it stops
}

// An interval implying a tempo outside the accepted range is ignored rather than acted on.
TEST(transport_ignores_an_out_of_range_clock_interval)
{
    FakeTime clk;
    HarnessTransport t(clk);
    clk.set(1000);

    t.on_external_clock_edge();
    clk.advance(20);                          // 20 ms per quarter = 3000 BPM
    t.on_external_clock_edge();
    CHECK(!t.is_external_sync());
    CHECK_NEAR(t.tempo(), 120.f, 1e-4);

    t.on_external_clock_edge();
    clk.advance(900);                         // 900 ms per quarter = ~67 BPM, in range
    t.on_external_clock_edge();
    CHECK(t.is_external_sync());
    CHECK_NEAR(t.tempo(), 66.67f, 1.0);
}

// Between pulses the interpolator fills in the sub-ticks, which is what lets a quarter-note clock
// drive a 1/16 sequencer.
TEST(transport_interpolates_sub_ticks_between_pulses)
{
    FakeTime clk;
    HarnessTransport t(clk);
    TickLog log;
    log.attach(t);
    clk.set(1000);

    t.on_external_clock_edge();
    clk.advance(500);
    t.on_external_clock_edge();               // acquires sync at 120 BPM
    log.clear();

    run_for(t, clk, 480);                     // just short of the next pulse
    // One quarter at 120 BPM is four 1/16 ticks; the pulse supplied one, so ~3 are interpolated.
    CHECK(log.count_common() >= 2);
    CHECK(log.count_common() <= 4);
}

int main() { return daisyapps::test::run_all(); }
