#pragma once

// Projects an engine's DisplayModel onto whatever indicators the board actually has.
//
// The gap this closes. IEngine has a whole panel-drawing surface - render(DisplayModel&), the
// engine/indicators.h vocabulary of palettes, breathe/blink motion and ring pictures, and LEDRing as
// the canvas - and twelve files in src/engine/ implement it. None of it ever ran: app/harness.cpp
// never called render(), and no board's SetIndicator was called from anywhere. On a Pod, the only
// hardware-validated target, that meant an engine was a two-knob black box with two dark LEDs.
//
// Why a PROJECTION and not a blit. DisplayModel describes sk-engines' panel: two 32-LED rings plus
// about nineteen named indicators. No board here has that. What they have is:
//
//   Pod           2 RGB LEDs
//   patch.init()  1 mono LED (its driver collapses any colour to on/off)
//   Daisy Patch   0 discrete LEDs - it has the OLED instead
//
// So this maps the model down rather than across, and the mapping is a legibility choice, not a
// faithful rendering: one LED cannot represent thirty-two. The choices are gathered in
// ring_summary() and project() below so they are in one place and can be argued with - and, since
// nothing here touches hardware, asserted on in host/test_display_adapter.cpp.
//
// What goes where:
//
//   indicator 0   the deck's TRANSPORT state (DisplayModel::play), which engines already fill with a
//                 direction/state-coded colour via indicators.h's transport_view(): red recording,
//                 green rolling forward, cyan reverse, dim white engaged-but-stopped, amber error.
//                 That is the single most useful thing a player can see, and it needs no projection -
//                 DisplayModel::Indicator is {rgb, brightness} and SetIndicator takes exactly that.
//   indicator 1   the deck's RING, collapsed to one colour + brightness (ring_summary). For a level
//                 meter that reads as an activity lamp; for a selector or slot ring it reads as
//                 "something is selected", which is less information but not misleading.
//
// PLATFORM FALLBACK. An engine that does not draw (most of them - engine_draws<E>() measures it, see
// app/engine_pads.h) would leave the panel dark, which is the state this file exists to fix. So the
// indicators fall back to what the PLATFORM knows: indicator 0 shows the parameter page as a hue, and
// indicator 1 shows knob pickup state - red while a visible knob has not yet caught its value, green
// once they all have. On a screenless board that is the only feedback the paged UI has ever had; the
// page and the catch state are precisely what its own comments say a player cannot otherwise see.
//
// A board with no indicators (the Patch) compiles this away to nothing: every call is guarded by
// `Board::kIndicatorCount`, which is a compile-time constant.

#include <cmath>
#include <cstdint>
#include <type_traits>

#include <cstdio>
#include <cstring>

#include "app/engine_pads.h"        // engine_draws<E>() - measured, not declared
#include "engine/display_model.h"
#include "engine/iengine.h"
#include "engine/indicators.h"      // pal:: - the palette transport_word() inverts

namespace daisyapps {

// One indicator's worth of colour, already scaled by brightness and ready for SetIndicator.
struct IndicatorRgb {
    float r = 0.f, g = 0.f, b = 0.f;

    bool dark() const { return r <= 0.f && g <= 0.f && b <= 0.f; }
};

// Below this, an indicator is emitted as fully dark rather than as a very dim colour.
//
// Not cosmetic. A mono-LED board (patch.init()) collapses colour to on/off with
// `r > 0 || g > 0 || b > 0`, so ANY non-zero channel reads as fully lit - a 2%-brightness glow and a
// 100% one are the same lamp. Without a floor, such a board's LED would be on permanently and carry
// no information at all. On an RGB board the floor costs nothing: 2% of full is not visible anyway.
inline constexpr float kIndicatorFloor = 0.04f;

// Scale a DisplayModel::Indicator (hex colour + separate brightness) into channel values.
inline IndicatorRgb to_rgb(const DisplayModel::Indicator& ind)
{
    if (ind.brightness < kIndicatorFloor) return {};
    const float br = ind.brightness > 1.f ? 1.f : ind.brightness;
    return { static_cast<float>((ind.rgb >> 16) & 0xFF) / 255.f * br,
             static_cast<float>((ind.rgb >> 8) & 0xFF) / 255.f * br,
             static_cast<float>(ind.rgb & 0xFF) / 255.f * br };
}

// Collapse a 32-pixel ring to a single indicator.
//
// COLOUR is the brightness-weighted mean of the lit pixels, so a uniform arc keeps its own hue and a
// mixed ring (indicators.h's ring::value draws the value bar in one colour and the not-yet-caught
// deviation in red) reads as a blend that shifts as the gap closes.
//
// BRIGHTNESS is the mean over the WHOLE ring - i.e. how much of it is lit - passed through a square
// root. The mean alone is the honest signal for a level meter (ring::level fills 0..level, so the
// mean IS the level) but it buries a sparse picture: ring::selector draws one bright dot and a
// handful at 0.15, which averages to about 0.06 and would read as off. The square root lifts that to
// roughly 0.25 - visible - while leaving a half-full meter near 0.63 rather than 0.5. It is
// monotonic, so brighter always means more, and it is the single knob most likely to want adjusting
// once someone has actually looked at a Pod.
//
// Takes the ring by reference and not by const reference because LEDRing::apply() is the only way to
// read it, and apply() also consumes the dirty flag (it is the blit). That is the intended use: this
// IS the blit for these boards.
inline DisplayModel::Indicator ring_summary(LEDRing& ring)
{
    float sum_b = 0.f, wr = 0.f, wg = 0.f, wb = 0.f;
    int   count = 0;

    ring.apply([&](uint8_t, uint32_t hex, float brightness) {
        count++;
        if (brightness <= 0.f) return;
        sum_b += brightness;
        wr += static_cast<float>((hex >> 16) & 0xFF) * brightness;
        wg += static_cast<float>((hex >> 8) & 0xFF) * brightness;
        wb += static_cast<float>(hex & 0xFF) * brightness;
    });

    if (count == 0 || sum_b <= 0.f) return { 0u, 0.f };

    const uint32_t rgb = (static_cast<uint32_t>(wr / sum_b + 0.5f) << 16)
                       | (static_cast<uint32_t>(wg / sum_b + 0.5f) << 8)
                       | (static_cast<uint32_t>(wb / sum_b + 0.5f));

    const float mean = sum_b / static_cast<float>(count);
    return { rgb, std::sqrt(mean > 1.f ? 1.f : mean) };
}

// A one-word name for a transport indicator's COLOUR.
//
// The Daisy Patch's OLED is one bit per pixel, so DisplayModel's colour vocabulary cannot be painted
// there at all - and colour is where the transport state lives (indicators.h's transport_view() maps
// recording/forward/reverse/frozen/error onto exact palette constants). Dropping it would make the
// screen strictly worse than the Pod's two LEDs.
//
// But a screen can do something an LED cannot: say the word. Inverting transport_view()'s mapping
// turns the lost dimension into text, which is the one representation a monochrome panel renders
// perfectly. Matching is on the exact palette values, because that is what transport_view() emits;
// anything else is an engine's own hue and gets the generic "on".
inline const char* transport_word(uint32_t rgb)
{
    switch (rgb) {
        case pal::kRed:    return "rec";
        case pal::kGreen:  return "fwd";
        case pal::kCyan:   return "rev";
        case pal::kFrozen: return "hold";
        case pal::kErr:    return "err";
        case pal::kAmber:  return "busy";
        default:           return "on";
    }
}

// The platform's own fallback picture, for an engine that draws nothing.
namespace platform_leds {

// Page hue. Distinct, well-separated hues rather than a gradient, because the question a player asks
// is "which page am I on", not "how far through the pages am I" - and on two LEDs a gradient is
// unreadable. Wraps, so a build with more pages than hues repeats rather than going dark.
inline constexpr uint32_t kPageHues[6] = {
    0x00a0ff,  // blue
    0xf7941d,  // orange
    0x00ff00,  // green
    0xc850ff,  // purple
    0x00FFEF,  // turquoise
    0xffDE21,  // yellow
};
inline constexpr int kPageHueCount = 6;

inline DisplayModel::Indicator page(int page_index, int page_count)
{
    if (page_count <= 0) return { 0u, 0.f };
    const int idx = ((page_index % kPageHueCount) + kPageHueCount) % kPageHueCount;
    return { kPageHues[idx], 0.55f };
}

// Knob pickup state: red while any visible knob still has to be swept across its value before it
// takes control, green once they all have. This is the one piece of feedback the paged UI is missing
// on a screenless board - a knob that writes nothing and does not say why is indistinguishable from a
// broken knob.
inline DisplayModel::Indicator pickup(bool all_caught)
{
    return all_caught ? DisplayModel::Indicator{ 0x00ff00, 0.35f }
                      : DisplayModel::Indicator{ 0xff0000, 0.75f };
}

} // namespace platform_leds

// Templated on BOTH axes that decide whether there is anything to do, so both answers are settled at
// compile time and neither costs a byte when it is "no":
//
//   Board   what the target physically has - indicators, a screen, or neither. Whatever board/board.h
//           selected. The Screen*/SetIndicator facades already compile away where absent.
//   Draws   whether this build's ENGINE overrides render(), from engine_draws<E>() in engine_pads.h.
//           An engine that draws nothing needs no DisplayModel at all - it gets the platform's
//           page/pickup fallback, which is computed from two ints and a bool.
//
// The pair is what keeps the ~2.3 KB model off the eight-odd engines that do not draw, on a target
// where several of the ones that do are already under real static-data pressure.
template <typename Board, bool Draws = true>
class DisplayAdapter {
public:
    // LED refresh cap. Upstream services its panel on a 62 Hz tick; the same rate here keeps
    // breathe/blink motion smooth (indicators.h's motion:: helpers are time-based) without running an
    // engine's render() at main-loop speed, which on this hardware is tens of kHz.
    static constexpr int      kLedHz    = 60;
    static constexpr uint32_t kPeriodMs = 1000 / kLedHz;
    // The OLED blit is ~1 KB over SPI, so the engine page redraws at the same rate ParamUI uses for
    // the parameter pages rather than at the LED rate.
    static constexpr int      kScreenHz = 20;

    // Main loop. Ask the engine to draw (if it does), project the result, push it to the board.
    // `page`/`pages`/`all_caught` are the platform's fallback state, used when the engine draws
    // nothing. Rate-limited internally, so this is safe to call every pass.
    void tick(Board& board, IEngine& engine, DeckRef::Ref deck,
              int page, int pages, bool all_caught, uint32_t now_ms)
    {
        if constexpr (Board::kIndicatorCount <= 0) {
            (void)board; (void)engine; (void)deck;
            (void)page; (void)pages; (void)all_caught; (void)now_ms;
            return;                       // no discrete LEDs on this target (the Patch): nothing to do
        } else {
            if (now_ms - _last_ms < kPeriodMs) return;
            _last_ms = now_ms;
            refresh(engine, now_ms);

            DisplayModel::Indicator ind0, ind1;

            if constexpr (Draws) {
                const int d = (deck == DeckRef::A) ? 0 : 1;
                ind0 = _model.play[d];
                ind1 = ring_summary(_model.ring[d]);
            } else {
                ind0 = platform_leds::page(page, pages);
                ind1 = platform_leds::pickup(all_caught);
            }

            push(board, 0, ind0);
            if constexpr (Board::kIndicatorCount > 1) push(board, 1, ind1);
        }
    }

    // --- The engine page, for a board with a display -------------------------------------------
    // Draws the engine's own panel on the OLED: its two ring canvases as bar strips, and its lit
    // named indicators as WORDS. The Daisy Patch has no discrete LEDs at all, so without this a
    // CapOwnDisplay engine has nowhere to draw on the one board with a real display.
    //
    // Not a smaller copy of the Pod's LEDs. A 1-bit panel loses colour, which is where the transport
    // state lives - but it gains text, which an LED does not have. So the rings carry the SHAPE
    // (which pixels, and roughly how bright) and the indicators carry the MEANING as words. That is
    // more information than two RGB lamps, not less.
    //
    // Rate-limited separately from the LEDs: an OLED blit is ~1 KB over SPI.
    void render_screen(Board& board, IEngine& engine, DeckRef::Ref deck,
                       const char* engine_name, uint32_t now_ms)
    {
        if constexpr (!Board::kHasScreen || !Draws) {
            // No display, or nothing to draw on it. Either way there is no engine page.
            (void)board; (void)engine; (void)deck; (void)engine_name; (void)now_ms;
            return;
        } else {
            if (now_ms - _screen_ms < static_cast<uint32_t>(1000 / kScreenHz)) return;
            _screen_ms = now_ms;
            refresh(engine, now_ms);

            const int focus = (deck == DeckRef::A) ? 0 : 1;
            char line[32];

            board.ScreenClear();
            std::snprintf(line, sizeof(line), "%s %c eng", engine_name, focus ? 'B' : 'A');
            board.ScreenText(0, 0, line, true);

            // The two ring canvases, focused deck first so it is always on the same row.
            draw_ring(board, _model.ring[focus],     focus == 0 ? 'A' : 'B', 14, true);
            draw_ring(board, _model.ring[focus ^ 1], focus == 0 ? 'B' : 'A', 26, false);

            // The lit named indicators, as words. Two lines; anything past that is dropped rather
            // than wrapped mid-name.
            char words[64];
            words[0] = '\0';
            int n = 0;
            append_word(words, sizeof(words), n, "play", transport_word(_model.play[focus].rgb),
                        _model.play[focus].brightness);
            append_word(words, sizeof(words), n, "rev",  nullptr, _model.rev[focus].brightness);
            append_word(words, sizeof(words), n, "flux", nullptr, _model.flux[focus].brightness);
            append_word(words, sizeof(words), n, "grit", nullptr, _model.grit[focus].brightness);
            append_word(words, sizeof(words), n, "gate", nullptr, _model.gate_in[focus].brightness);
            append_word(words, sizeof(words), n, "cyc",  nullptr, _model.cycle[focus].brightness);
            append_word(words, sizeof(words), n, "alt",  nullptr, _model.alt[focus].brightness);
            append_word(words, sizeof(words), n, "clk",  nullptr, _model.clock_in.brightness);

            // 21 characters fit on a 128 px row in the 6x8 font.
            constexpr int kCols = 21;
            char row[kCols + 1];
            std::snprintf(row, sizeof(row), "%.*s", kCols, words);
            board.ScreenText(0, 40, row, true);
            if (std::strlen(words) > kCols) {
                std::snprintf(row, sizeof(row), "%.*s", kCols, words + kCols);
                board.ScreenText(0, 50, row, true);
            }

            board.ScreenUpdate();
        }
    }

    // Turn every indicator off. For a clean panel at shutdown or when handing the LEDs to something
    // else; not used by the harness today, but a dark panel should be one call and not a guess.
    void clear(Board& board)
    {
        if constexpr (Board::kIndicatorCount > 0) {
            for (int i = 0; i < Board::kIndicatorCount; i++) board.SetIndicator(i, 0.f, 0.f, 0.f);
        } else {
            (void)board;
        }
    }

private:
    // Fill _model from the engine, at most once per LED period. Both consumers (the LEDs and the
    // engine page) call this, so a board that had both would still render once per frame rather than
    // twice - and an engine's render() is the expensive part.
    void refresh(IEngine& engine, uint32_t now_ms)
    {
        if constexpr (!Draws) { (void)engine; (void)now_ms; return; }
        else {
        if (_have_frame && (now_ms - _frame_ms) < kPeriodMs) return;
        _frame_ms   = now_ms;
        _have_frame = true;

        // clear() first: render() implementations are written against a blank canvas (they call
        // m.clear() themselves by convention, but an engine that forgets would otherwise inherit the
        // previous frame).
        _model.clear();
        engine.render(_model);
        }
    }

    // One ring as a 32-cell bar strip. A 1-bit panel cannot show 32 brightnesses, but it can show
    // three states per cell, which is enough to read a level arc, a playhead dot and a selector:
    // full-height for a lit pixel, a short stub for a dim one, nothing for an unlit one. `focused`
    // draws the deck letter in a box so it is obvious which row the knobs belong to.
    static void draw_ring(Board& board, LEDRing& ring, char label, int y, bool focused)
    {
        char tag[2] = { label, '\0' };
        board.ScreenText(0, y, tag, true);
        if (focused) board.ScreenRect(8, y - 1, 2, 9, true);

        constexpr int kX0 = 14, kPitch = 3, kW = 2, kH = 7;
        ring.apply([&](uint8_t i, uint32_t, float brightness) {
            const int x = kX0 + static_cast<int>(i) * kPitch;
            if (brightness >= 0.5f)       board.ScreenRect(x, y, kW, kH, true);
            else if (brightness > 0.04f)  board.ScreenRect(x, y + kH - 2, kW, 2, true);
        });
    }

    // Append " name" (or " name=value") to `buf` if the indicator is lit and it still fits.
    static void append_word(char* buf, size_t cap, int& count, const char* name, const char* value,
                            float brightness)
    {
        if (brightness < kIndicatorFloor) return;
        const size_t used = std::strlen(buf);
        char piece[16];
        if (value) std::snprintf(piece, sizeof(piece), "%s%s:%s", count ? " " : "", name, value);
        else       std::snprintf(piece, sizeof(piece), "%s%s",    count ? " " : "", name);
        const size_t len = std::strlen(piece);
        if (used + len + 1 > cap) return;      // drop rather than truncate a name mid-word
        std::memcpy(buf + used, piece, len + 1);
        count++;
    }

    static void push(Board& board, int idx, const DisplayModel::Indicator& ind)
    {
        const IndicatorRgb c = to_rgb(ind);
        board.SetIndicator(idx, c.r, c.g, c.b);
    }

    // A DisplayModel is ~2.3 KB (two 32-pixel LEDRing canvases, each double-buffered, plus the named
    // indicators). It is a member rather than a local because it is far too big for the stack on a
    // target whose stack lives in the 128 KB DTCMRAM, and it is reused every frame anyway.
    //
    // On a board with NO indicators (the Daisy Patch) every use of it is already compiled out by the
    // `if constexpr` in tick(), but an unconditional member would still occupy those 2.3 KB of .bss -
    // paid for by every engine on that target, several of which are the ones under real static-data
    // pressure. So the member itself is conditional: present where there is something to drive,
    // absent where there is not.
    struct NoModel {};
    static constexpr bool kNeedsModel = Draws && ((Board::kIndicatorCount > 0) || Board::kHasScreen);
    using ModelStorage = std::conditional_t<kNeedsModel, DisplayModel, NoModel>;

    ModelStorage _model;
    uint32_t     _last_ms   = 0;   // LED tick
    uint32_t     _screen_ms = 0;   // OLED tick
    uint32_t     _frame_ms  = 0;   // when _model was last filled
    bool         _have_frame = false;
};

} // namespace daisyapps
