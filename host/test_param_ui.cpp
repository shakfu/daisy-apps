// ParamUI: the paged parameter surface, its soft-takeover pickup, and the action screen's generated
// row list.
//
// This is the repo's headline feature - pages built from an engine's own live_params(), rows built
// from a compile-time pad mask - and it was entirely unexercised. Soft-takeover in particular is the
// kind of code that is obviously right and subtly wrong: it has to decide, from two floats and a
// history, whether a physical knob has earned the right to move a value.
//
// It is testable at all because the engine contract is HAL-free (src/math_util.h); ParamUI is
// board-templated, so a FakeBoard that records its draw calls stands in for the OLED.

#include "check.h"
#include "fakes.h"

#include "app/param_ui.h"

using namespace daisyapps;
using namespace daisyapps::test;

using UI = ParamUI<FakeBoard>;

namespace {

// Poll the knobs `n` times with the same reading - the main loop's steady state.
void hold(UI& ui, IEngine& e, const Controls& c, int n = 1)
{
    for (int i = 0; i < n; i++) ui.poll_knobs(e, c);
}

} // namespace

// --- page construction ---------------------------------------------------------------------------

TEST(pages_are_built_from_the_engines_live_params)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::Size, ParamId::Mix});

    UI ui;
    ui.init(e, 4);
    CHECK_EQ(ui.pages(), 1);      // three params, four knobs -> one page
    CHECK_EQ(ui.page(), 0);
}

TEST(pages_are_ceil_of_params_over_knobs)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size, ParamId::Win});   // 6 params

    UI four;
    four.init(e, 4);
    CHECK_EQ(four.pages(), 2);    // ceil(6/4)

    UI two;
    two.init(e, 2);
    CHECK_EQ(two.pages(), 3);     // ceil(6/2) - a Pod's two knobs
}

TEST(an_engine_with_no_live_params_has_no_pages)
{
    FakeEngine e;
    e.param_mask = 0;
    UI ui;
    ui.init(e, 4);
    CHECK_EQ(ui.pages(), 0);
    ui.set_page(1, e);            // must not divide by zero or index anything
    CHECK_EQ(ui.page(), 0);
}

TEST(set_page_wraps_in_both_directions)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size, ParamId::Win, ParamId::PolySlice, ParamId::Speed,
                            ParamId::FluxIntensity});   // 9 params -> 3 pages of 4
    UI ui;
    ui.init(e, 4);
    CHECK_EQ(ui.pages(), 3);

    ui.set_page(3, e);
    CHECK_EQ(ui.page(), 0);       // one past the end wraps to the start
    ui.set_page(-1, e);
    CHECK_EQ(ui.page(), 2);       // and below zero wraps to the end
    ui.set_page(7, e);
    CHECK_EQ(ui.page(), 1);       // multiple laps still land correctly
}

// --- value pickup (soft takeover) ----------------------------------------------------------------

// A knob that is physically somewhere other than the value it now addresses must write NOTHING until
// it earns control. Without this, every page turn jumps four params.
TEST(an_uncaught_knob_writes_nothing)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.8f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    hold(ui, e, controls_of({0.2f, 0, 0, 0}), 5);   // knob far below the value
    CHECK_EQ(e.writes.size(), 0u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.8f, 1e-6);
}

// A knob already sitting at (or within kCatchWindow of) the value is caught immediately - there is
// no gap to close.
TEST(a_knob_already_at_the_value_is_caught_at_once)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    hold(ui, e, controls_of({0.505f, 0, 0, 0}));    // inside kCatchWindow (0.02)
    hold(ui, e, controls_of({0.60f, 0, 0, 0}));     // now it writes
    CHECK(e.writes.size() >= 1u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.60f, 1e-6);
}

// The crossing case: sweep the knob THROUGH the stored value and it takes over at the crossing.
TEST(a_knob_takes_over_when_it_crosses_the_value)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    hold(ui, e, controls_of({0.10f, 0, 0, 0}));     // well below: uncaught
    CHECK_EQ(e.writes.size(), 0u);
    hold(ui, e, controls_of({0.30f, 0, 0, 0}));     // still below
    CHECK_EQ(e.writes.size(), 0u);
    hold(ui, e, controls_of({0.70f, 0, 0, 0}));     // crossed 0.50 -> caught, and this poll writes
    CHECK_EQ(e.writes.size(), 1u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.70f, 1e-6);
}

TEST(crossing_works_downward_too)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    hold(ui, e, controls_of({0.95f, 0, 0, 0}));     // primes the crossing detector at 0.95
    hold(ui, e, controls_of({0.95f, 0, 0, 0}));     // still above the value: uncaught
    CHECK_EQ(e.writes.size(), 0u);
    hold(ui, e, controls_of({0.20f, 0, 0, 0}));     // crossed downward
    CHECK_EQ(e.writes.size(), 1u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.20f, 1e-6);
}

// Once caught, a still knob must not chatter: sub-deadband jitter is ignored.
TEST(a_caught_knob_ignores_jitter_below_the_deadband)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    hold(ui, e, controls_of({0.50f, 0, 0, 0}));     // caught
    e.clear_calls();

    hold(ui, e, controls_of({0.5005f, 0, 0, 0}));   // well under kDeadband (0.004)
    hold(ui, e, controls_of({0.4995f, 0, 0, 0}));
    CHECK_EQ(e.writes.size(), 0u);

    hold(ui, e, controls_of({0.52f, 0, 0, 0}));     // a real move does get through
    CHECK_EQ(e.writes.size(), 1u);
}

// Turning the page repoints the knobs at different params, so they must ALL drop out of catch -
// otherwise the first poll slams the new param to wherever the knob happens to be.
TEST(a_page_turn_drops_every_knob_out_of_catch)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size});        // 5 params -> 2 pages
    e.set_value(ParamId::Size, DeckRef::A, 0.9f);   // page 1's only param

    UI ui;
    ui.init(e, 4);
    hold(ui, e, controls_of({0.3f, 0.3f, 0.3f, 0.3f}), 3);   // catch page 0's knobs
    e.clear_calls();

    ui.set_page(1, e);
    hold(ui, e, controls_of({0.3f, 0.3f, 0.3f, 0.3f}), 3);   // knob 0 now addresses Size (0.9)
    CHECK_EQ(e.writes.size(), 0u);                           // must NOT have jumped Size to 0.3
    CHECK_NEAR(e.param(ParamId::Size, DeckRef::A), 0.9f, 1e-6);
}

// take_param_reseed()'s consumer: an engine that repoints a deck's knobs (edrums' drum swap)
// invalidates the pickup cache, and reseed() must drop the knobs out of catch.
TEST(reseed_drops_the_knobs_out_of_catch)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.30f);

    UI ui;
    ui.init(e, 4);
    hold(ui, e, controls_of({0.30f, 0, 0, 0}));     // caught
    e.clear_calls();

    e.set_value(ParamId::Pos, DeckRef::A, 0.90f);   // the engine repointed this knob
    ui.reseed(e);
    hold(ui, e, controls_of({0.31f, 0, 0, 0}));     // a small move must not snap 0.90 -> 0.31
    CHECK_EQ(e.writes.size(), 0u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.90f, 1e-6);
}

// Switching deck re-points the same knobs at the other deck's values, so it must re-catch too.
TEST(a_deck_switch_drops_the_knobs_out_of_catch)
{
    FakeEngine e;
    e.caps       = CapDualDeck;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.20f);
    e.set_value(ParamId::Pos, DeckRef::B, 0.80f);

    UI ui;
    ui.init(e, 4);
    hold(ui, e, controls_of({0.20f, 0, 0, 0}));
    e.clear_calls();

    ui.set_deck(DeckRef::B, e);
    CHECK_EQ(static_cast<int>(ui.deck()), static_cast<int>(DeckRef::B));
    hold(ui, e, controls_of({0.21f, 0, 0, 0}));
    CHECK_EQ(e.writes.size(), 0u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::B), 0.80f, 1e-6);
}

// A board with fewer analog inputs than the UI has slots must not read past what it filled.
TEST(poll_knobs_respects_the_boards_analog_count)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize});

    UI ui;
    ui.init(e, 4);
    Controls c = controls_of({0.5f, 0.5f});   // only two live inputs, though the UI has four slots
    hold(ui, e, c, 3);                        // must not touch analog[2..3]
    CHECK(true);                              // ASan is the assertion
}

// --- the action screen ---------------------------------------------------------------------------

TEST(action_rows_list_only_the_pads_the_engine_implements)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay | PadRecord);
    ui.open_actions(e, 0);
    CHECK(ui.in_actions());

    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    // back / play / alt / rec, four rows visible at a time.
    CHECK(board.drew("play"));
    CHECK(board.drew("rec"));
    CHECK(!board.drew("arm seq"));    // no sequencer pad declared -> no sequencer row
    CHECK(!board.drew("flux"));       // no FX pad declared -> no FX row
}

TEST(action_rows_include_a_row_per_declared_config)
{
    FakeEngine e;
    e.param_mask  = mask_of({ParamId::Pos});
    e.config_mask = config_mask_of({ConfigId::Mode});

    UI ui;
    ui.init(e, 4, 0);                 // no pads at all
    ui.open_actions(e, 0);

    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK(board.drew("mode"));
    CHECK(!board.drew("route"));      // not declared live
}

// A switch this UI has not written is UNKNOWN, not zero: the engine booted at its own default and the
// contract has no reader, so printing a position would be a guess. `-` is the honest answer.
TEST(an_unwritten_config_reads_as_unknown_then_selects_position_one)
{
    FakeEngine e;
    e.param_mask  = mask_of({ParamId::Pos});
    e.config_mask = config_mask_of({ConfigId::Mode});

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);

    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK(board.drew("-/3"));         // Mode is ternary and its position is unknown

    // The first-visit cursor sits on `back` for an engine with no pads, so step to the `mode` row.
    ui.move_cursor(1, 1000);
    const ActionRow::Kind fired = ui.fire(e, 1000);
    CHECK_EQ(static_cast<int>(fired), static_cast<int>(ActionRow::Config));
    CHECK_EQ(e.configs.size(), 1u);
    CHECK_EQ(e.configs[0].value, 0);  // the FIRST click SELECTS position 1 rather than advancing

    ui.render(board, e, "test", 120.f, 1100);
    CHECK(board.drew("1/3"));         // and now the screen and the engine agree by construction
}

TEST(firing_the_play_row_reaches_the_engine)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay);
    ui.open_actions(e, 0);

    // The cursor lands on `play` on a first visit (see first_useful_row).
    const ActionRow::Kind fired = ui.fire(e, 0);
    CHECK_EQ(static_cast<int>(fired), static_cast<int>(ActionRow::Play));
    CHECK_EQ(e.plays, 1);
    CHECK(!e.last_reverse);
    CHECK(ui.in_actions());           // stays in the list, so a repeat is one click
}

// REGRESSION: the initial cursor used to be the literal row 1, with a comment claiming that is
// `play`. Row 1 is `play` only when the engine HAS a play pad; for an engine without one (reverb,
// chorus, filter, delay) row 1 is a config switch, so the second click of a first visit silently
// changed the engine's mode.
TEST(the_first_visit_cursor_never_lands_on_a_config_switch)
{
    FakeEngine e;
    e.param_mask  = mask_of({ParamId::Pos});
    e.config_mask = config_mask_of({ConfigId::Mode});

    UI ui;
    ui.init(e, 4, 0);                 // no pads: rows are back / mode / clk in
    ui.open_actions(e, 0);

    const ActionRow::Kind fired = ui.fire(e, 0);
    CHECK(fired != ActionRow::Config);
    CHECK_EQ(e.configs.size(), 0u);   // nothing was reconfigured by opening and clicking once
}

// ...but when a play pad IS present, `play` is still what a first visit gets.
TEST(the_first_visit_cursor_prefers_play_when_there_is_one)
{
    FakeEngine e;
    e.param_mask  = mask_of({ParamId::Pos});
    e.config_mask = config_mask_of({ConfigId::Mode});

    UI ui;
    ui.init(e, 4, PadPlay | PadRecord);
    ui.open_actions(e, 0);
    CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::Play));
}

TEST(the_cursor_is_remembered_between_visits)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay | PadRecord);
    ui.open_actions(e, 0);
    ui.move_cursor(1, 0);             // play -> alt
    ui.move_cursor(1, 0);             // alt  -> rec
    CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::Rec));
    ui.close_actions();

    ui.open_actions(e, 0);            // second visit: still on `rec`
    CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::Rec));
    CHECK_EQ(e.records, 2);
}

TEST(the_cursor_wraps_around_the_row_list)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay);           // rows: back, play, alt, clk in
    ui.open_actions(e, 0);
    ui.move_cursor(-100, 0);          // must land somewhere valid, not out of bounds
    ui.fire(e, 0);
    ui.move_cursor(100, 0);
    ui.fire(e, 0);
    CHECK(true);                      // ASan is the assertion
}

TEST(the_back_row_closes_the_screen)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay);
    ui.open_actions(e, 0);            // first visit lands on `play` (row 1)
    ui.move_cursor(-1, 0);            // play -> back
    CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::Back));
    CHECK(!ui.in_actions());
}

TEST(an_idle_action_screen_closes_itself)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay);
    ui.open_actions(e, 1000);
    CHECK(ui.in_actions());

    ui.tick(1000 + UI::kActionIdleMs - 1);
    CHECK(ui.in_actions());
    ui.tick(1000 + UI::kActionIdleMs);
    CHECK(!ui.in_actions());
}

TEST(interacting_with_the_action_screen_defers_the_idle_close)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, PadPlay);
    ui.open_actions(e, 1000);
    ui.move_cursor(1, 5000);                        // interaction at t=5000 restarts the clock
    ui.tick(5000 + UI::kActionIdleMs - 1);
    CHECK(ui.in_actions());
    ui.tick(5000 + UI::kActionIdleMs);
    CHECK(!ui.in_actions());
}

// A board with no display would be entering an invisible mode, so the action screen refuses there and
// the harness keeps its direct click mapping.
TEST(a_screenless_board_refuses_the_action_screen)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    ParamUI<FakeScreenlessBoard> ui;
    ui.init(e, 2, PadPlay);
    ui.open_actions(e, 0);
    CHECK(!ui.in_actions());
}

TEST(the_clock_in_row_only_appears_where_there_is_a_clock_input)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;                            // FakeBoard has kGateCount == 2
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);
    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK(board.drew("clk in"));

    ParamUI<FakeScreenlessBoard> none;   // kGateCount == 0, and no screen either
    none.init(e, 2, 0);
    none.open_actions(e, 0);
    CHECK(!none.in_actions());
}

TEST(the_clock_in_row_cycles_its_choices)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4, 0);                 // rows: back, clk in
    ui.open_actions(e, 0);
    CHECK_EQ(ui.clock_in(), 0);       // 1/4 is the Eurorack default

    ui.move_cursor(1, 0);             // `back` -> `clk in` (row 1)
    for (int i = 1; i < UI::kClockInCount; i++) {
        CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::ClockIn));
        CHECK_EQ(ui.clock_in(), i);
    }
    ui.fire(e, 0);
    CHECK_EQ(ui.clock_in(), 0);       // wraps
}

// --- rendering -----------------------------------------------------------------------------------

TEST(the_page_shows_the_engines_own_param_label)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Speed});
    e.labels[static_cast<int>(ParamId::Speed)] = "station";   // what `radio` calls this slot

    UI ui;
    ui.init(e, 4);
    FakeBoard board;
    ui.render(board, e, "radio", 120.f, 1000);
    CHECK(board.drew("station"));
}

TEST(the_page_falls_back_to_the_generic_slot_name)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Speed});   // no label override

    UI ui;
    ui.init(e, 4);
    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK(board.drew("speed"));                 // kParamNames' word for the slot
}

// REGRESSION: the harness used to pass a status string on EVERY frame, which meant the tempo branch
// here was unreachable - and `delay`, `qdelay` and `edrums` are tempo-synced while the only tempo
// control is a blind encoder gesture. A null/empty status must fall through to the BPM.
TEST(the_header_shows_the_tempo_when_there_is_no_status)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4);
    FakeBoard board;
    ui.render(board, e, "delay", 137.f, 1000, nullptr);
    CHECK(board.drew("137"));

    board.lines.clear();
    ui.render(board, e, "delay", 137.f, 2000, "");     // empty is also "nothing to say"
    CHECK(board.drew("137"));
}

TEST(the_header_shows_the_status_when_there_is_one)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4);
    FakeBoard board;
    ui.render(board, e, "bard", 137.f, 1000, "P3");
    CHECK(board.drew("P3"));
    CHECK(!board.drew("137"));                          // the status owns the slot while it is live
}

TEST(the_header_names_the_deck_only_for_a_dual_deck_engine)
{
    FakeEngine mono;
    mono.param_mask = mask_of({ParamId::Pos});
    UI a;
    a.init(mono, 4);
    FakeBoard b1;
    a.render(b1, mono, "chorus", 120.f, 1000);
    CHECK_EQ(b1.line_with("chorus"), std::string("chorus 1/1"));

    FakeEngine dual;
    dual.caps       = CapDualDeck;
    dual.param_mask = mask_of({ParamId::Pos});
    UI d;
    d.init(dual, 4);
    FakeBoard b2;
    d.render(b2, dual, "tape", 120.f, 1000);
    CHECK_EQ(b2.line_with("tape"), std::string("tape A 1/1"));
}

// The screen blit is ~1 KB over SPI, so render() rate-limits itself and is safe to call every pass.
TEST(render_is_rate_limited)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    UI ui;
    ui.init(e, 4);
    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    const int after_first = board.updates;
    CHECK_EQ(after_first, 1);

    for (uint32_t t = 1001; t < 1000 + (1000 / UI::kScreenHz); t++)
        ui.render(board, e, "test", 120.f, t);
    CHECK_EQ(board.updates, after_first);               // still one blit

    ui.render(board, e, "test", 120.f, 1000 + (1000 / UI::kScreenHz));
    CHECK_EQ(board.updates, after_first + 1);
}

// A screenless board must not blit at all - the calls compile away to no-ops on device, and here we
// assert the UI does not even try.
TEST(a_screenless_board_never_blits)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});

    ParamUI<FakeScreenlessBoard> ui;
    ui.init(e, 2);
    FakeScreenlessBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK_EQ(board.updates, 0);
}

// The uncaught marker: a '.' before the name, and a caret on the bar at the knob's physical
// position, so the gap you have to close is visible rather than guessed.
TEST(an_uncaught_knob_is_marked_on_screen)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.80f);

    UI ui;
    ui.init(e, 4);
    hold(ui, e, controls_of({0.20f, 0, 0, 0}));     // far away: uncaught

    FakeBoard board;
    ui.render(board, e, "test", 120.f, 1000);
    CHECK(board.drew(".pos"));

    hold(ui, e, controls_of({0.80f, 0, 0, 0}));     // caught
    board.lines.clear();
    ui.render(board, e, "test", 120.f, 2000);
    CHECK(board.drew(" pos"));
    CHECK(!board.drew(".pos"));
}

int main() { return daisyapps::test::run_all(); }
