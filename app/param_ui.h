#pragma once

// The paged parameter UI: the Daisy Patch's four knobs and one encoder, driving an arbitrary engine.
//
// The problem this solves. sk-engines drives its engines from a large fixed panel - a knob per role,
// pads, two LED rings - and each engine's meaning for those knobs is baked into the platform. A Patch
// has four knobs and a screen, and this repo wants ONE harness that hosts any engine. So the pages are
// not hand-written per engine: the engine declares which ParamIds it actually consumes (live_params())
// and what it calls them (param_label()), and this builds the pages from that. Port an engine, get a
// working control surface with correct names on the screen, write nothing.
//
// Layout: the live params, in ParamId order, four to a page. Turning the encoder moves between pages;
// the knobs address whichever four are showing.
//
// Value pickup. Four knobs standing in for up to 24 params means a knob is almost always physically
// somewhere other than the value it now addresses, and letting it write immediately would jump that
// param on every page turn. So a knob is "caught" only once it crosses (or already sits at) the
// engine's current value for its param - the standard soft-takeover from hardware synths. Until then
// the screen shows the value with a caret marking where the knob is, and the knob writes nothing.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board/controls.h"
#include "app/param_names.h"
#include "engine/iengine.h"

namespace daisyapps {

// One knob's pickup state for the param it currently addresses.
struct ParamSlot {
    ParamId id        = ParamId::Count;
    bool    caught    = false;   // knob has crossed the value and now writes
    float   last_knob = 0.f;     // previous poll's knob position (for crossing detection)
    float   written   = 0.f;     // last value written (deadband reference)
};

// Board-templated so the no-op screen calls on a Pod / patch.init() compile away, and so this needs no
// virtual dispatch. `Board` is whatever board/board.h selected.
template <typename Board>
class ParamUI {
public:
    static constexpr int   kMaxSlots     = Controls::kMaxAnalog;   // knobs available for params
    static constexpr float kCatchWindow  = 0.02f;   // |knob - value| that counts as already caught
    static constexpr float kDeadband     = 0.004f;  // ignore pot jitter below this (porting guide 4.3)
    static constexpr int   kScreenHz     = 20;      // OLED refresh cap (the blit is not free)

    // Build the page list from the engine's declared live params. `knobs` is how many of the board's
    // analog controls to spend on params - the Patch's four; a board with dedicated CV jacks would
    // pass only its pot count.
    void init(IEngine& engine, int knobs)
    {
        _knobs = knobs > kMaxSlots ? kMaxSlots : (knobs < 1 ? 1 : knobs);
        _count = 0;

        const IEngine::ParamMask live = engine.live_params();
        for (uint8_t i = 0; i < static_cast<uint8_t>(ParamId::Count) && _count < kMaxParams; i++) {
            if (live & (IEngine::ParamMask{1} << i)) _params[_count++] = static_cast<ParamId>(i);
        }
        // An engine that narrows nothing keeps the inherited all-live mask, which is honest ("I did not
        // say") rather than a claim that all 24 do something. The pages then list every slot; the ones
        // the engine ignores simply do nothing when turned.

        _pages = _count ? ((_count + _knobs - 1) / _knobs) : 0;
        _page  = 0;
        seed_page(engine);
    }

    int  page() const  { return _page; }
    int  pages() const { return _pages; }
    void set_deck(DeckRef::Ref deck, IEngine& engine) { _deck = deck; seed_page(engine); }
    DeckRef::Ref deck() const { return _deck; }

    void set_page(int page, IEngine& engine)
    {
        if (_pages <= 0) return;
        while (page < 0)       page += _pages;
        while (page >= _pages) page -= _pages;
        if (page == _page) return;
        _page = page;
        seed_page(engine);   // knobs must re-catch: they now address different params
    }

    // Main loop: push caught knobs into the engine. `analog` is the board's normalized snapshot.
    void poll_knobs(IEngine& engine, const Controls& c)
    {
        for (int s = 0; s < _knobs; s++) {
            ParamSlot& slot = _slot[s];
            if (slot.id == ParamId::Count || s >= c.analog_count) continue;

            const float knob = c.analog[s];

            if (!slot.caught) {
                const float value = engine.param(slot.id, _deck);
                const bool  at    = std::fabs(knob - value) <= kCatchWindow;
                const bool  crossed =
                    (slot.last_knob < value && knob >= value) || (slot.last_knob > value && knob <= value);
                if (at || crossed) {
                    slot.caught  = true;
                    slot.written = value;
                }
            }

            if (slot.caught && std::fabs(knob - slot.written) > kDeadband) {
                engine.set_param(slot.id, _deck, knob);
                slot.written = knob;
            }
            slot.last_knob = knob;
        }
    }

    // An engine whose deck->value mapping changed under us (IEngine::take_param_reseed) invalidates
    // the pickup cache: the knobs now address different values and must re-catch.
    void reseed(IEngine& engine) { seed_page(engine); }

    // Draw the page. Rate-limited internally; safe to call every main-loop pass. `engine_name` is the
    // build's engine (SPK_ENGINE_STR), `bpm` and `deck` come from the harness.
    void render(Board& board, IEngine& engine, const char* engine_name, float bpm, uint32_t now_ms)
    {
        if (!Board::kHasScreen) return;
        if (now_ms - _last_draw_ms < static_cast<uint32_t>(1000 / kScreenHz)) return;
        _last_draw_ms = now_ms;

        char line[32];
        board.ScreenClear();

        // Header: engine, deck (dual-deck engines only), page, tempo.
        const bool dual = (engine.capabilities() & CapDualDeck) != 0;
        if (dual)
            std::snprintf(line, sizeof(line), "%s %c %d/%d", engine_name,
                          _deck == DeckRef::A ? 'A' : 'B', _page + 1, _pages ? _pages : 1);
        else
            std::snprintf(line, sizeof(line), "%s %d/%d", engine_name, _page + 1, _pages ? _pages : 1);
        board.ScreenText(0, 0, line, true);
        std::snprintf(line, sizeof(line), "%3d", static_cast<int>(bpm + 0.5f));
        board.ScreenText(Board::kScreenWidth - 18, 0, line, true);

        // One row per knob: name, value, and a bar. An uncaught knob is marked with '.' before its
        // name and gets a caret on the bar at the knob's physical position, so the gap you have to
        // close to take control is visible rather than guessed.
        for (int s = 0; s < _knobs; s++) {
            const ParamSlot& slot = _slot[s];
            const int        y    = 14 + s * 12;
            if (slot.id == ParamId::Count) continue;

            const char* label = engine.param_label(slot.id);
            if (!label) label = kParamNames[static_cast<uint8_t>(slot.id)];

            const float value = engine.param(slot.id, _deck);
            std::snprintf(line, sizeof(line), "%c%-9.9s%3d", slot.caught ? ' ' : '.', label,
                          static_cast<int>(value * 100.f + 0.5f));
            board.ScreenText(0, y, line, true);

            // Bar occupies the right of the row.
            const int bx = 74, bw = Board::kScreenWidth - bx - 2, bh = 7;
            board.ScreenRect(bx, y, bw, bh, false);
            const int fill = static_cast<int>(value * static_cast<float>(bw - 2) + 0.5f);
            if (fill > 0) board.ScreenRect(bx + 1, y + 1, fill, bh - 2, true);
            if (!slot.caught) {
                const int cx = bx + 1 + static_cast<int>(slot.last_knob * static_cast<float>(bw - 3) + 0.5f);
                board.ScreenRect(cx, y, 1, bh, true);
            }
        }

        board.ScreenUpdate();
    }

private:
    static constexpr int kMaxParams = static_cast<int>(ParamId::Count);

    // Point the knobs at this page's params and drop them out of catch, seeding the crossing detector
    // with where each knob physically is right now.
    void seed_page(IEngine& engine)
    {
        for (int s = 0; s < kMaxSlots; s++) {
            const int idx = _page * _knobs + s;
            _slot[s].id        = (s < _knobs && idx < _count) ? _params[idx] : ParamId::Count;
            _slot[s].caught    = false;
            _slot[s].written   = (_slot[s].id != ParamId::Count) ? engine.param(_slot[s].id, _deck) : 0.f;
            // last_knob is left as the previous poll's reading: it is the "where the knob was" half of
            // the crossing test, and zeroing it here would fake a sweep from 0 on the next poll.
        }
    }

    ParamId      _params[kMaxParams] = {};
    int          _count              = 0;
    int          _knobs              = 4;
    int          _pages              = 0;
    int          _page               = 0;
    DeckRef::Ref _deck               = DeckRef::A;
    ParamSlot    _slot[kMaxSlots];
    uint32_t     _last_draw_ms       = 0;
};

} // namespace daisyapps
