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
    CHECK(board.drew(">pos"));                      // knob below the value: turn up

    hold(ui, e, controls_of({0.80f, 0, 0, 0}));     // caught
    board.lines.clear();
    ui.render(board, e, "test", 120.f, 2000);
    CHECK(board.drew(" pos"));
    CHECK(!board.drew(">pos"));
    CHECK(!board.drew("<pos"));
}

// --- the engine page in the rotation ------------------------------------------------------------
//
// On a board with a display, an engine that draws its own panel gets a page of its own at the end of
// the rotation; the display adapter draws it. ParamUI's job is only to make room for it and to keep
// out of the way.

TEST(the_engine_page_adds_one_to_the_rotation)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size});      // 5 params -> 2 parameter pages

    UI ui;
    ui.init(e, 4);
    CHECK_EQ(ui.pages(), 2);
    CHECK(!ui.has_engine_page());

    ui.set_engine_page(true);
    CHECK(ui.has_engine_page());
    CHECK_EQ(ui.pages(), 3);
}

TEST(the_engine_page_is_the_last_index)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size});
    UI ui;
    ui.init(e, 4);
    ui.set_engine_page(true);

    ui.set_page(0, e);
    CHECK(!ui.on_engine_page());
    ui.set_page(1, e);
    CHECK(!ui.on_engine_page());
    ui.set_page(2, e);
    CHECK(ui.on_engine_page());
    ui.set_page(3, e);                            // wraps back to the start
    CHECK_EQ(ui.page(), 0);
    CHECK(!ui.on_engine_page());
}

TEST(an_engine_page_works_even_with_no_live_params)
{
    FakeEngine e;
    e.param_mask = 0;                             // nothing to page through
    UI ui;
    ui.init(e, 4);
    ui.set_engine_page(true);

    CHECK_EQ(ui.pages(), 1);
    CHECK(ui.on_engine_page());                   // the engine page is the only page
    ui.set_page(1, e);                            // must not divide by zero or run away
    CHECK_EQ(ui.page(), 0);
}

// Flipping to the engine view to watch what a control is doing must not take the control away.
TEST(the_knobs_keep_their_params_on_the_engine_page)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    ui.set_engine_page(true);
    hold(ui, e, controls_of({0.50f, 0, 0, 0}), 2);   // catch it on the parameter page
    e.clear_calls();

    ui.set_page(1, e);                               // -> the engine page
    CHECK(ui.on_engine_page());
    hold(ui, e, controls_of({0.70f, 0, 0, 0}));      // the knob still owns Pos
    CHECK_EQ(e.writes.size(), 1u);
    CHECK_NEAR(e.param(ParamId::Pos, DeckRef::A), 0.70f, 1e-6);
}

// ...but coming back to a parameter page re-seeds, exactly as any other page turn does.
TEST(returning_from_the_engine_page_re_seeds_the_knobs)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos, ParamId::FluxFb, ParamId::Env, ParamId::EnvSize,
                            ParamId::Size});         // 2 parameter pages
    e.set_value(ParamId::Size, DeckRef::A, 0.9f);    // page 1's only param

    UI ui;
    ui.init(e, 4);
    ui.set_engine_page(true);
    hold(ui, e, controls_of({0.3f, 0.3f, 0.3f, 0.3f}), 3);   // catch page 0
    e.clear_calls();

    ui.set_page(2, e);                               // engine page
    ui.set_page(1, e);                               // back to parameter page 1
    CHECK(!ui.on_engine_page());
    hold(ui, e, controls_of({0.3f, 0.3f, 0.3f, 0.3f}), 3);
    CHECK_EQ(e.writes.size(), 0u);                   // must NOT have jumped Size to 0.3
    CHECK_NEAR(e.param(ParamId::Size, DeckRef::A), 0.9f, 1e-6);
}

// Two owners, one panel: ParamUI must draw nothing while the engine page is showing, or it would
// fight the adapter for the screen.
TEST(param_ui_draws_nothing_on_the_engine_page)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    UI ui;
    ui.init(e, 4);
    ui.set_engine_page(true);

    FakeBoard board;
    ui.render(board, e, "chorus", 120.f, 1000);
    CHECK_EQ(board.updates, 1);                      // parameter page 0 drew

    ui.set_page(1, e);                               // -> engine page
    ui.render(board, e, "chorus", 120.f, 2000);
    CHECK_EQ(board.updates, 1);                      // ...and nothing more
}

// The action list is reachable from anywhere, including the engine page, and wins the panel while it
// is open - otherwise the engine page would strand the only route to the pads on a Daisy Patch.
TEST(the_action_screen_still_draws_over_the_engine_page)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    UI ui;
    ui.init(e, 4, PadPlay);
    ui.set_engine_page(true);
    ui.set_page(1, e);
    CHECK(ui.on_engine_page());

    ui.open_actions(e, 1000);
    FakeBoard board;
    ui.render(board, e, "chorus", 120.f, 2000);
    CHECK(board.drew("play"));
    CHECK_EQ(board.updates, 1);
}

// --- IEngine::config, the categorical read-back --------------------------------------------------
//
// Without it the platform's only honest source for a switch position is what it last WROTE, so before
// the user touches a switch its position is unknown and the row reads `-/3`. An engine that answers
// gets an honest display from boot, and its switch cycles from where it actually is.

TEST(a_reporting_engine_shows_its_switch_position_from_boot)
{
    FakeEngine e;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Mode});
    e.reports_config = true;
    e.set_config_value(ConfigId::Mode, DeckRef::A, 2);      // booted at the third position

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);

    FakeBoard board;
    ui.render(board, e, "reverb", 120.f, 1000);
    CHECK(board.drew("3/3"));           // ...and says so, before anything has been clicked
    CHECK(!board.drew("-/3"));
}

// The behaviour half, and the one that matters more than the display: several engines boot at
// Route::DoubleMono, which is selector position 2. Advancing from the platform's old guess moved them
// somewhere else entirely on the very first click.
TEST(a_reporting_switch_cycles_from_where_it_actually_is)
{
    FakeEngine e;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Route});
    e.reports_config = true;
    e.set_config_value(ConfigId::Route, DeckRef::A, 1);     // DoubleMono, i.e. position 2 of 3

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);
    ui.move_cursor(1, 0);                                   // onto the route row
    CHECK_EQ(static_cast<int>(ui.fire(e, 0)), static_cast<int>(ActionRow::Config));

    CHECK_EQ(e.configs.size(), 1u);
    CHECK_EQ(e.configs[0].value, 2);    // 1 -> 2, not the old "select position 1"
}

TEST(a_reporting_engine_wraps_at_the_top_of_the_range)
{
    FakeEngine e;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Mode});
    e.reports_config = true;
    e.set_config_value(ConfigId::Mode, DeckRef::A, 2);      // last position of a ternary switch

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);
    ui.move_cursor(1, 0);
    ui.fire(e, 0);
    CHECK_EQ(e.configs.back().value, 0);
}

// An engine that does not report keeps exactly the old behaviour: unknown until written, and the
// first click SELECTS position 1 rather than advancing from a guess.
TEST(a_silent_engine_keeps_the_unknown_and_the_select_first_rule)
{
    FakeEngine e;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Mode});
    e.reports_config = false;
    e.set_config_value(ConfigId::Mode, DeckRef::A, 2);      // true state, which it will not admit to

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);

    FakeBoard board;
    ui.render(board, e, "granular", 120.f, 1000);
    CHECK(board.drew("-/3"));

    ui.move_cursor(1, 1000);
    ui.fire(e, 1000);
    CHECK_EQ(e.configs.size(), 1u);
    CHECK_EQ(e.configs[0].value, 0);    // select, do not advance
}

// The reader is per-deck for a per-deck switch, and the row follows the focused deck.
TEST(a_reporting_switch_is_read_per_deck)
{
    FakeEngine e;
    e.caps           = CapDualDeck;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Mode});
    e.reports_config = true;
    e.set_config_value(ConfigId::Mode, DeckRef::A, 0);
    e.set_config_value(ConfigId::Mode, DeckRef::B, 2);

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);

    FakeBoard a;
    ui.render(a, e, "x", 120.f, 1000);
    CHECK(a.drew("1/3"));

    ui.set_deck(DeckRef::B, e);
    FakeBoard b;
    ui.render(b, e, "x", 120.f, 2000);
    CHECK(b.drew("3/3"));
}

// ...and once it HAS written, a silent engine still cycles from its own cache rather than resetting.
TEST(a_silent_engine_cycles_from_its_own_write_cache)
{
    FakeEngine e;
    e.param_mask     = mask_of({ParamId::Pos});
    e.config_mask    = config_mask_of({ConfigId::Mode});
    e.reports_config = false;

    UI ui;
    ui.init(e, 4, 0);
    ui.open_actions(e, 0);
    ui.move_cursor(1, 0);                       // onto the mode row

    ui.fire(e, 0);                              // first click SELECTS position 1
    CHECK_EQ(e.configs.back().value, 0);
    ui.fire(e, 0);                              // thereafter it advances...
    CHECK_EQ(e.configs.back().value, 1);
    ui.fire(e, 0);
    CHECK_EQ(e.configs.back().value, 2);
    ui.fire(e, 0);                              // ...and wraps
    CHECK_EQ(e.configs.back().value, 0);
}

// The wire mapping ConfigId::Route carries is NOT the Route enum's own numbering - the enum starts at
// 1 and in a different order. Every engine hand-rolls the int->Route direction inside its set_config;
// route_to_config is the inverse they use to answer config(), so the two must agree exactly or a
// reported position would name the wrong topology.
TEST(route_and_its_selector_index_round_trip)
{
    CHECK_EQ(route_to_config(Route::Stereo),           0);
    CHECK_EQ(route_to_config(Route::DoubleMono),       1);
    CHECK_EQ(route_to_config(Route::GenerativeStereo), 2);

    for (int v = 0; v < 3; v++) CHECK_EQ(route_to_config(config_to_route(v)), v);

    // ...and the int->Route half matches what the engines actually write (2 = generative, 1 = double
    // mono, anything else = stereo).
    CHECK(config_to_route(0) == Route::Stereo);
    CHECK(config_to_route(1) == Route::DoubleMono);
    CHECK(config_to_route(2) == Route::GenerativeStereo);
    CHECK(config_to_route(99) == Route::Stereo);    // out of range falls back, never wraps
}

// --- values pinned against an endpoint -------------------------------------------------------------
//
// Found on hardware, on `delay`: `division` and `tone` read 100 and did nothing. Both are booted at
// 1.0 on purpose by DelayEngine::init(), and the crossing test cannot fire for a value of 1.0 -
// crossing needs `knob >= value`, which an ADC never reads. Before kEndCatchWindow the only route was
// the 0.02 proximity window, so those two knobs were dead across 98% of their travel.

TEST(a_param_pinned_at_maximum_is_catchable_without_hunting)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Env});
    e.set_value(ParamId::Env, DeckRef::A, 1.0f);        // exactly what DelayEngine::init() does

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    // The old behaviour: a full sweep of the range writes nothing at all.
    for (float k : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 0.85f})
        ui.poll_knobs(e, controls_of({k, 0, 0, 0}));
    CHECK_EQ(e.writes.size(), 0u);

    // The fix: the knob takes over on entering the widened endpoint window, well before the very end.
    hold(ui, e, controls_of({0.92f, 0, 0, 0}));
    CHECK(e.writes.size() >= 1u);
    CHECK_NEAR(e.param(ParamId::Env, DeckRef::A), 0.92f, 1e-6);

    // ...and it behaves normally over the whole range afterwards.
    hold(ui, e, controls_of({0.30f, 0, 0, 0}));
    CHECK_NEAR(e.param(ParamId::Env, DeckRef::A), 0.30f, 1e-6);
}

TEST(a_param_pinned_at_minimum_is_catchable_too)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.0f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();

    for (float k : {1.0f, 0.8f, 0.6f, 0.4f, 0.2f, 0.15f})
        ui.poll_knobs(e, controls_of({k, 0, 0, 0}));
    CHECK_EQ(e.writes.size(), 0u);

    hold(ui, e, controls_of({0.08f, 0, 0, 0}));
    CHECK(e.writes.size() >= 1u);
}

// The widened window is ONLY for endpoints. A mid-range value must keep the tight one, or takeover
// would jump by up to 10% everywhere and the mechanism would stop doing its job.
TEST(a_mid_range_param_keeps_the_tight_catch_window)
{
    CHECK_NEAR(UI::catch_window(0.5f),  UI::kCatchWindow,    1e-6);
    CHECK_NEAR(UI::catch_window(0.25f), UI::kCatchWindow,    1e-6);
    CHECK_NEAR(UI::catch_window(1.0f),  UI::kEndCatchWindow, 1e-6);
    CHECK_NEAR(UI::catch_window(0.0f),  UI::kEndCatchWindow, 1e-6);

    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.50f);

    UI ui;
    ui.init(e, 4);
    e.clear_calls();
    hold(ui, e, controls_of({0.60f, 0, 0, 0}), 2);      // 0.10 away - inside the WIDE window
    CHECK_EQ(e.writes.size(), 0u);                      // ...but must not catch, because 0.5 is not pinned
}

// --- the uncaught marker says which way to turn ----------------------------------------------------

TEST(an_uncaught_row_points_toward_the_value)
{
    FakeEngine e;
    e.param_mask = mask_of({ParamId::Pos});
    e.set_value(ParamId::Pos, DeckRef::A, 0.80f);

    UI ui;
    ui.init(e, 4);

    hold(ui, e, controls_of({0.20f, 0, 0, 0}));        // knob below the value -> turn up
    FakeBoard up;
    ui.render(up, e, "test", 120.f, 1000);
    CHECK(up.drew(">pos"));

    hold(ui, e, controls_of({0.95f, 0, 0, 0}));        // knob above the value... but 0.95 crosses 0.80
    // ...so re-seed onto a fresh slot to test the downward marker without catching.
    FakeEngine f;
    f.param_mask = mask_of({ParamId::Pos});
    f.set_value(ParamId::Pos, DeckRef::A, 0.20f);
    UI dn_ui;
    dn_ui.init(f, 4);
    dn_ui.poll_knobs(f, controls_of({0.90f, 0, 0, 0}));
    FakeBoard dn;
    dn_ui.render(dn, f, "test", 120.f, 1000);
    CHECK(dn.drew("<pos"));
}

int main() { return daisyapps::test::run_all(); }
