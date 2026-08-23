// DisplayAdapter: projecting an engine's DisplayModel onto a board's indicator LEDs.
//
// Worth stating what this can and cannot establish. It proves the MAPPING: that a recording deck
// drives indicator 0 red, that a half-lit ring is brighter than a nearly-dark one, that a mono board
// is not left permanently on, that a board with no LEDs is never written to. It cannot tell you
// whether a 32-pixel ring collapsed to one lamp actually reads well on a Pod - that is a visual
// judgement and it needs the hardware. The collapse policy is deliberately gathered into
// ring_summary() so that when someone does look at a Pod, there is one function to argue with.

#include "check.h"
#include "fakes.h"

#include "app/display_adapter.h"
#include "engine/indicators.h"

using namespace daisyapps;
using namespace daisyapps::test;

namespace {

// An engine that draws a caller-supplied picture, so a test can set up an exact DisplayModel and
// assert what reaches the board.
class DrawingEngine : public FakeEngine {
public:
    void render(DisplayModel& m) override
    {
        m.clear();
        if (on_render) on_render(m);
    }
    void (*on_render)(DisplayModel&) = nullptr;
};

// Advance past the adapter's rate limit.
constexpr uint32_t kStep = DisplayAdapter<FakeBoard, true>::kPeriodMs + 1;

} // namespace

// --- to_rgb: DisplayModel::Indicator -> channel values --------------------------------------------

TEST(to_rgb_splits_the_hex_and_scales_by_brightness)
{
    const IndicatorRgb full = to_rgb({ 0xff0000, 1.f });
    CHECK_NEAR(full.r, 1.f, 1e-4);
    CHECK_NEAR(full.g, 0.f, 1e-4);
    CHECK_NEAR(full.b, 0.f, 1e-4);

    const IndicatorRgb half = to_rgb({ 0x00ff00, 0.5f });
    CHECK_NEAR(half.g, 0.5f, 1e-3);
    CHECK_NEAR(half.r, 0.f, 1e-4);

    const IndicatorRgb white = to_rgb({ 0xffffff, 1.f });
    CHECK_NEAR(white.r, 1.f, 1e-4);
    CHECK_NEAR(white.g, 1.f, 1e-4);
    CHECK_NEAR(white.b, 1.f, 1e-4);
}

TEST(to_rgb_clamps_brightness_above_one)
{
    const IndicatorRgb c = to_rgb({ 0xffffff, 4.f });
    CHECK_NEAR(c.r, 1.f, 1e-4);
    CHECK_NEAR(c.g, 1.f, 1e-4);
    CHECK_NEAR(c.b, 1.f, 1e-4);
}

// The floor exists for mono-LED boards, whose driver reads ANY non-zero channel as fully lit. Without
// it their LED would be on permanently and carry no information.
TEST(to_rgb_floors_a_barely_visible_brightness_to_black)
{
    CHECK(to_rgb({ 0xffffff, 0.001f }).dark());
    CHECK(to_rgb({ 0xffffff, kIndicatorFloor * 0.5f }).dark());
    CHECK(!to_rgb({ 0xffffff, kIndicatorFloor * 2.f }).dark());
    CHECK(to_rgb({ 0xffffff, 0.f }).dark());
}

// --- ring_summary: 32 pixels -> one lamp -----------------------------------------------------------

TEST(an_empty_ring_summarises_to_black)
{
    LEDRing ring;
    ring.clear();
    ring.set_updated();
    const DisplayModel::Indicator s = ring_summary(ring);
    CHECK_NEAR(s.brightness, 0.f, 1e-6);
}

TEST(a_uniform_arc_keeps_its_own_hue)
{
    LEDRing ring;
    ring.clear();
    ring::level(ring, 0.5f, 0x00ff00);   // half-full green meter
    ring.set_updated();

    const DisplayModel::Indicator s = ring_summary(ring);
    CHECK(s.brightness > 0.f);
    // Green channel dominant, the other two essentially absent.
    CHECK_EQ((s.rgb >> 8) & 0xFFu, 0xFFu);
    CHECK_EQ((s.rgb >> 16) & 0xFFu, 0x00u);
    CHECK_EQ(s.rgb & 0xFFu, 0x00u);
}

// The property that makes the collapse useful at all: more of the ring lit must mean a brighter lamp.
TEST(ring_brightness_is_monotonic_in_how_much_is_lit)
{
    float prev = -1.f;
    for (float level : {0.1f, 0.25f, 0.5f, 0.75f, 0.999f}) {
        LEDRing ring;
        ring.clear();
        ring::level(ring, level, 0xffffff);
        ring.set_updated();
        const float b = ring_summary(ring).brightness;
        CHECK(b > prev);
        prev = b;
    }
    CHECK(prev <= 1.f);
}

// A sparse picture - indicators.h's selector ring is one bright dot plus a few dim ones - must not
// average away to something indistinguishable from off. This is what the square root is for.
TEST(a_sparse_selector_ring_stays_visible)
{
    LEDRing ring;
    ring.clear();
    ring::selector(ring, 8, 3, 0xffffff);
    ring.set_updated();

    const DisplayModel::Indicator s = ring_summary(ring);
    CHECK(s.brightness > kIndicatorFloor * 2.f);   // comfortably above the black floor
    CHECK(s.brightness < 0.6f);                    // ...but still clearly dimmer than a full ring
}

TEST(a_full_bright_ring_is_brighter_than_a_sparse_one)
{
    LEDRing full;
    full.clear();
    ring::level(full, 0.999f, 0xffffff);
    full.set_updated();

    LEDRing sparse;
    sparse.clear();
    ring::selector(sparse, 8, 0, 0xffffff);
    sparse.set_updated();

    CHECK(ring_summary(full).brightness > ring_summary(sparse).brightness);
}

// A ring drawn in two colours blends, weighted by brightness - which is what ring::value produces
// while a knob has not yet caught its value (the value bar in one hue, the deviation in red).
TEST(a_two_colour_ring_blends_toward_the_brighter_half)
{
    LEDRing ring;
    ring.clear();
    ring::value(ring, /*value=*/0.5f, /*knob=*/0.9f, /*picked_up=*/false, 0x0000ff);
    ring.set_updated();

    const DisplayModel::Indicator s = ring_summary(ring);
    const uint32_t r = (s.rgb >> 16) & 0xFF, b = s.rgb & 0xFF;
    CHECK(r > 0u);       // the red deviation overlay is present...
    CHECK(b > 0u);       // ...alongside the blue value bar
}

// --- the adapter: an engine that draws --------------------------------------------------------------

TEST(a_drawing_engine_reaches_indicator_zero)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        m.play[0] = { 0xff0000, 1.f };            // deck A: recording
    };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

    CHECK_EQ(board.led[0].writes, 1);
    CHECK_NEAR(board.led[0].r, 1.f, 1e-3);
    CHECK_NEAR(board.led[0].g, 0.f, 1e-3);
}

TEST(the_adapter_follows_the_focused_deck)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        m.play[0] = { 0xff0000, 1.f };            // A red
        m.play[1] = { 0x0000ff, 1.f };            // B blue
    };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;

    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);
    CHECK_NEAR(board.led[0].r, 1.f, 1e-3);
    CHECK_NEAR(board.led[0].b, 0.f, 1e-3);

    leds.tick(board, e, DeckRef::B, 0, 1, true, kStep * 2);
    CHECK_NEAR(board.led[0].b, 1.f, 1e-3);
    CHECK_NEAR(board.led[0].r, 0.f, 1e-3);
}

TEST(the_engines_ring_reaches_indicator_one)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        ring::level(m.ring[0], 0.8f, 0x00ff00);
        m.ring[0].set_updated();
    };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

    CHECK_EQ(board.led[1].writes, 1);
    CHECK(!board.led[1].dark());
    CHECK(board.led[1].g > board.led[1].r);
}

// transport_view() is the shared state-colour helper engines use; the whole chain from it to a lit LED
// should hold, because that is the single most useful thing a player can see on a Pod.
TEST(the_transport_state_colours_survive_the_whole_chain)
{
    struct Case { bool rolling, recording; float speed; uint32_t want; };
    const Case cases[] = {
        { true,  true,  1.f, pal::kRed   },   // recording
        { true,  false, 1.f, pal::kGreen },   // rolling forward
        { true,  false, -1.f, pal::kCyan },   // reverse
    };

    for (const Case& c : cases) {
        static Case s_case;                    // the render hook is a plain function pointer
        s_case = c;
        DrawingEngine e;
        e.on_render = [](DisplayModel& m) {
            const TransportView tv = transport_view(s_case.rolling, s_case.recording, s_case.speed, false);
            led::transport(m, 0, tv);
        };

        DisplayAdapter<FakeBoard, true> leds;
        FakeBoard board;
        leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

        const IndicatorRgb want = to_rgb({ c.want, 1.f });
        CHECK_NEAR(board.led[0].r, want.r, 1e-3);
        CHECK_NEAR(board.led[0].g, want.g, 1e-3);
        CHECK_NEAR(board.led[0].b, want.b, 1e-3);
    }
}

// --- the adapter: the platform fallback --------------------------------------------------------------

// Most engines draw nothing. Leaving the panel dark for them is the state this whole file exists to
// fix, so the platform shows what IT knows instead.
TEST(a_non_drawing_engine_still_lights_the_panel)
{
    FakeEngine e;
    DisplayAdapter<FakeBoard, false> leds;                          // engine_draws<E>() == false
    FakeBoard board;
    leds.tick(board, e, DeckRef::A, 0, 3, true, kStep);

    CHECK(!board.led[0].dark());               // page hue
    CHECK(!board.led[1].dark());               // pickup state
}

TEST(the_page_indicator_changes_hue_per_page)
{
    FakeEngine e;
    DisplayAdapter<FakeBoard, false> leds;
    FakeBoard board;

    leds.tick(board, e, DeckRef::A, 0, 4, true, kStep);
    const FakeBoard::Led p0 = board.led[0];
    leds.tick(board, e, DeckRef::A, 1, 4, true, kStep * 2);
    const FakeBoard::Led p1 = board.led[0];

    const bool differs = (p0.r != p1.r) || (p0.g != p1.g) || (p0.b != p1.b);
    CHECK(differs);
}

TEST(the_page_hue_wraps_rather_than_going_dark)
{
    FakeEngine e;
    DisplayAdapter<FakeBoard, false> leds;
    FakeBoard board;

    // More pages than there are hues: every one must still light the LED.
    for (int page = 0; page < platform_leds::kPageHueCount * 2 + 1; page++) {
        leds.tick(board, e, DeckRef::A, page, 20, true, kStep * static_cast<uint32_t>(page + 1));
        CHECK(!board.led[0].dark());
    }
}

// The paged UI's missing feedback on a screenless board: a knob that writes nothing and does not say
// why is indistinguishable from a broken knob.
TEST(the_pickup_indicator_distinguishes_caught_from_uncaught)
{
    FakeEngine e;
    DisplayAdapter<FakeBoard, false> leds;
    FakeBoard board;

    leds.tick(board, e, DeckRef::A, 0, 1, /*all_caught=*/false, kStep);
    CHECK(board.led[1].r > board.led[1].g);        // red: still has to be swept across

    leds.tick(board, e, DeckRef::A, 0, 1, /*all_caught=*/true, kStep * 2);
    CHECK(board.led[1].g > board.led[1].r);        // green: the knobs own their values
}

// --- board shapes -------------------------------------------------------------------------------------

// The Daisy Patch has no discrete LEDs. The adapter must compile away rather than write anywhere.
TEST(a_board_with_no_indicators_is_never_written_to)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) { m.play[0] = { 0xffffff, 1.f }; };

    DisplayAdapter<FakeLedlessBoard, true> leds;
    FakeLedlessBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

    CHECK_EQ(board.led[0].writes, 0);
    CHECK_EQ(board.led[1].writes, 0);
}

// patch.init() has one LED. Indicator 1 has nowhere to go and must not be written.
TEST(a_single_indicator_board_gets_only_indicator_zero)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        m.play[0] = { 0x00ff00, 1.f };
        ring::level(m.ring[0], 0.9f, 0xff0000);
        m.ring[0].set_updated();
    };

    DisplayAdapter<FakeMonoBoard, true> leds;
    FakeMonoBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

    CHECK_EQ(board.led[0].writes, 1);
    CHECK_EQ(board.led[1].writes, 0);
}

// The reason the floor exists: a mono driver reads any non-zero channel as fully lit, so an idle
// engine drawing a very dim glow must reach the board as genuinely black, not as "on".
TEST(a_dim_glow_reaches_a_mono_board_as_off_not_as_on)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) { m.play[0] = { 0xffffff, 0.01f }; };

    DisplayAdapter<FakeMonoBoard, true> leds;
    FakeMonoBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);

    CHECK_EQ(board.led[0].writes, 1);
    CHECK(board.led[0].dark());
}

// --- rate limiting ------------------------------------------------------------------------------------

// The main loop runs at tens of kHz. Calling an engine's render() and blitting the panel at that rate
// would be pure waste, and the motion helpers in indicators.h are time-based anyway.
TEST(the_adapter_is_rate_limited)
{
    DrawingEngine e;
    int renders = 0;
    static int* s_renders;
    s_renders = &renders;
    e.on_render = [](DisplayModel& m) { (*s_renders)++; m.play[0] = { 0xffffff, 1.f }; };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;

    leds.tick(board, e, DeckRef::A, 0, 1, true, 1000);
    CHECK_EQ(renders, 1);

    for (uint32_t t = 1001; t < 1000 + DisplayAdapter<FakeBoard, true>::kPeriodMs; t++)
        leds.tick(board, e, DeckRef::A, 0, 1, true, t);
    CHECK_EQ(renders, 1);                       // still the one frame

    leds.tick(board, e, DeckRef::A, 0, 1, true, 1000 + DisplayAdapter<FakeBoard, true>::kPeriodMs);
    CHECK_EQ(renders, 2);
}

TEST(clear_darkens_every_indicator)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) { m.play[0] = { 0xffffff, 1.f }; };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.tick(board, e, DeckRef::A, 0, 1, true, kStep);
    CHECK(!board.led[0].dark());

    leds.clear(board);
    CHECK(board.led[0].dark());
    CHECK(board.led[1].dark());
}

// --- the engine page (a board with a display) --------------------------------------------------------
//
// The Daisy Patch has NO discrete LEDs, so this is the only place a CapOwnDisplay engine can draw on
// the one board with a real display. A 1-bit panel cannot show colour - which is where the transport
// state lives - so the rings carry shape and the indicators carry meaning as words.

TEST(transport_colours_map_to_words)
{
    // The exact palette values transport_view() emits, inverted.
    CHECK_EQ(transport_word(pal::kRed),    std::string("rec"));
    CHECK_EQ(transport_word(pal::kGreen),  std::string("fwd"));
    CHECK_EQ(transport_word(pal::kCyan),   std::string("rev"));
    CHECK_EQ(transport_word(pal::kFrozen), std::string("hold"));
    CHECK_EQ(transport_word(pal::kErr),    std::string("err"));
    CHECK_EQ(transport_word(pal::kAmber),  std::string("busy"));
    // An engine's own hue is not a transport state; it gets the generic word rather than a wrong one.
    CHECK_EQ(transport_word(0x123456),     std::string("on"));
}

TEST(the_engine_page_names_the_engine_and_the_deck)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel&) {};

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.render_screen(board, e, DeckRef::A, "chorus", 1000);

    CHECK(board.drew("chorus"));
    CHECK(board.drew("eng"));
    CHECK_EQ(board.updates, 1);
}

// The state an LED can only imply, spelled out. This is the whole reason the page is worth having on
// a monochrome panel.
TEST(the_engine_page_spells_out_the_transport_state)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        led::transport(m, 0, transport_view(true, true, 1.f, false));   // deck A recording
    };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.render_screen(board, e, DeckRef::A, "tape", 1000);

    CHECK(board.drew("play:rec"));
}

TEST(the_engine_page_lists_only_the_lit_indicators)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        led::flux(m, 0, 1.f, 0.f);        // flux lit
        m.grit[0] = { 0xffffff, 0.f };    // grit dark
    };

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;
    leds.render_screen(board, e, DeckRef::A, "qdelay", 1000);

    CHECK(board.drew("flux"));
    CHECK(!board.drew("grit"));
}

TEST(the_engine_page_follows_the_focused_deck)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) {
        led::transport(m, 0, transport_view(true, true,  1.f, false));   // A recording
        led::transport(m, 1, transport_view(true, false, 1.f, false));   // B rolling forward
    };

    DisplayAdapter<FakeBoard, true> leds;

    FakeBoard a;
    leds.render_screen(a, e, DeckRef::A, "tape", 1000);
    CHECK(a.drew("play:rec"));

    FakeBoard b;
    leds.render_screen(b, e, DeckRef::B, "tape", 1000 + 100);
    CHECK(b.drew("play:fwd"));
}

// The ring is drawn as a bar strip: a fuller ring must put more marks on the panel than a sparse one.
TEST(a_fuller_ring_draws_more_bars)
{
    static float s_level;

    auto bars_for = [](float level) {
        s_level = level;
        DrawingEngine e;
        e.on_render = [](DisplayModel& m) {
            ring::level(m.ring[0], s_level, 0xffffff);
            m.ring[0].set_updated();
        };
        DisplayAdapter<FakeBoard, true> leds;
        FakeBoard board;
        leds.render_screen(board, e, DeckRef::A, "x", 1000);
        return board.rects.size();
    };

    const size_t few  = bars_for(0.2f);
    const size_t many = bars_for(0.9f);
    CHECK(many > few);
    CHECK(few > 0u);
}

TEST(the_engine_page_is_rate_limited)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel&) {};

    DisplayAdapter<FakeBoard, true> leds;
    FakeBoard board;

    leds.render_screen(board, e, DeckRef::A, "x", 1000);
    CHECK_EQ(board.updates, 1);
    for (uint32_t t = 1001; t < 1000 + (1000 / DisplayAdapter<FakeBoard, true>::kScreenHz); t++)
        leds.render_screen(board, e, DeckRef::A, "x", t);
    CHECK_EQ(board.updates, 1);
    leds.render_screen(board, e, DeckRef::A, "x", 1000 + (1000 / DisplayAdapter<FakeBoard, true>::kScreenHz));
    CHECK_EQ(board.updates, 2);
}

// A board with no display must not attempt the page at all - the Pod reaches its engine through the
// LEDs instead.
TEST(a_screenless_board_draws_no_engine_page)
{
    DrawingEngine e;
    e.on_render = [](DisplayModel& m) { m.play[0] = { 0xff0000, 1.f }; };

    DisplayAdapter<FakeScreenlessBoard, true> leds;
    FakeScreenlessBoard board;
    leds.render_screen(board, e, DeckRef::A, "x", 1000);

    CHECK_EQ(board.updates, 0);
    CHECK(board.lines.empty());
}

int main() { return daisyapps::test::run_all(); }
