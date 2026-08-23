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
#include "app/engine_pads.h"   // PadMask: which pads the built engine actually implements
#include "engine/iengine.h"

namespace daisyapps {

// One knob's pickup state for the param it currently addresses.
struct ParamSlot {
    ParamId id        = ParamId::Count;
    bool    caught    = false;   // knob has crossed the value and now writes
    float   last_knob = 0.f;     // previous poll's knob position (for crossing detection)
    float   written   = 0.f;     // last value written (deadband reference)
};

// One row of the action screen (see ParamUI below). At namespace scope rather than nested in the class
// template, so naming it costs no `typename` - the row set depends on the engine, never on the board.
struct ActionRow {
    enum Kind : uint8_t {
        Back,
        Play, Alt, Rec,                     // the transport pads (Alt is Play with `reverse`)
        Stop, ClearBuf,                     // buffer control
        SeqArm, SeqTrig, SeqClear, Disarm,  // the step sequencer
        Fx, FxLock,                         // the two end-of-chain effects (cfx says which)
        Deck,
        Config,
        ClockIn,                            // platform row: what one pulse at the clock input means
    };
    Kind     kind = Back;
    ConfigId cfg  = ConfigId::Count;   // Config rows only
    uint8_t  span = 0;                 // how many values that switch takes
    FxKind   fx   = FxKind::Flux;      // Fx / FxLock rows only
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
    // How long the action screen stays open with no interaction. Long enough to read the list and
    // think about it; short enough that a menu left open does not quietly keep the encoder off the
    // parameter pages.
    static constexpr uint32_t kActionIdleMs = 6000;

    // Build the page list from the engine's declared live params. `knobs` is how many of the board's
    // analog controls to spend on params - the Patch's four; a board with dedicated CV jacks would
    // pass only its pot count.
    // `pads` is which of IEngine's optional pads this build's engine implements - see engine_pads.h,
    // and app/harness.cpp for where it comes from. The action screen lists only those, so an engine
    // is never offered a control it does not have.
    void init(IEngine& engine, int knobs, PadMask pads = 0)
    {
        _knobs = knobs > kMaxSlots ? kMaxSlots : (knobs < 1 ? 1 : knobs);
        _pads  = pads;
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
    int  pages() const { return total_pages(); }

    // Give the engine a page of its own at the end of the rotation, for a board whose display can
    // show one (app/display_adapter.h draws it). Set from engine_draws<E>() - an engine that draws
    // nothing has no page to offer.
    //
    // The knobs deliberately KEEP the params of the page you came from while the engine page is
    // showing: seed_page() runs only when landing on a parameter page, so flipping to the engine view
    // to watch what a control is doing does not take the control away. Coming back re-seeds as any
    // page turn does.
    void set_engine_page(bool on) { _engine_page = on; }
    bool has_engine_page() const  { return _engine_page; }
    bool on_engine_page() const   { return _engine_page && _page >= _pages; }
    void set_deck(DeckRef::Ref deck, IEngine& engine) { _deck = deck; seed_page(engine); }
    DeckRef::Ref deck() const { return _deck; }

    void set_page(int page, IEngine& engine)
    {
        const int total = total_pages();
        if (total <= 0) return;
        while (page < 0)      page += total;
        while (page >= total) page -= total;
        if (page == _page) return;
        _page = page;
        // Only a PARAMETER page repoints the knobs. Landing on the engine page leaves them where they
        // were (see set_engine_page); landing back on a parameter page re-seeds, so they must re-catch
        // because they now address different params.
        if (!on_engine_page()) seed_page(engine);
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
                // The crossing test needs a PREVIOUS reading, and on the very first poll after init()
                // there has not been one: `last_knob` is still its initial 0.0, which reads as "the
                // knob was at the bottom". Every param whose value sits at or below the pot then looks
                // like it was just swept across, so at BOOT roughly half of them are captured instantly
                // and slammed to the knob positions - precisely the jump soft-takeover exists to
                // prevent, at the one moment the user has not touched anything.
                //
                // So the crossing half is suppressed until a real reading has been recorded. The `at`
                // half still applies on that first poll: "the knob is already where the value is" needs
                // no history and is true or false on the spot.
                const bool crossed =
                    _primed && ((slot.last_knob < value && knob >= value) ||
                                (slot.last_knob > value && knob <= value));
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
        // From here on `last_knob` holds a reading that actually happened, so crossing detection is
        // meaningful. seed_page() deliberately does NOT clear this on a page turn - there the previous
        // reading is real, which is the whole point of the comment there.
        _primed = true;
    }

    // An engine whose deck->value mapping changed under us (IEngine::take_param_reseed) invalidates
    // the pickup cache: the knobs now address different values and must re-catch.
    void reseed(IEngine& engine) { seed_page(engine); }

    // Has every knob on the current page taken control of its param? False while any of them still has
    // to be swept across its value first. The screen shows this per row (the '.' marker and the caret);
    // this is the same answer as one bool, for a board with no screen to show on an LED instead - a
    // knob that writes nothing and does not say why is indistinguishable from a broken knob.
    // Slots with no param assigned (a partly-filled last page) do not count against it.
    bool all_caught() const
    {
        for (int s = 0; s < _knobs; s++)
            if (_slot[s].id != ParamId::Count && !_slot[s].caught) return false;
        return true;
    }

    // --- The action screen ---------------------------------------------------------------------
    // Everything an engine exposes that is NOT a knob: the play / record pads and the categorical
    // switches. A pad is momentary and a switch is a small enum, so neither fits the knob-per-param
    // model - and a Daisy Patch has exactly one button, the encoder click, to spend on all of it.
    // So the click opens a LIST instead of firing one hard-coded action: turning moves the cursor,
    // clicking fires the highlighted row and stays there, and the rows are GENERATED from what the
    // engine declares (capabilities(), live_configs()) exactly as the pages come from live_params().
    //
    // Why a list and not a timed gesture. The alternative was a hold ladder - 0.6 s play, 1.6 s
    // record - which is invisible: nothing on screen says the second rung exists and the only way to
    // find it is to be told. Every row here names itself, and none of it is timing-sensitive.
    //
    // Screen-only by construction. A board with no display (Pod, patch.init()) would be entering an
    // invisible mode, so open_actions() refuses there and the harness keeps its direct click mapping.
    bool in_actions() const { return _actions; }

    // The clock-input row's selection, as an index. The harness polls this and pushes the matching
    // pulses-per-quarter into the transport; the UI deliberately does not know what a transport is.
    // kClockInNames' ORDER is the contract between the two - see the static_assert in harness.cpp.
    static constexpr int  kClockInCount    = 4;
    static constexpr const char* kClockInNames[kClockInCount] = { "1/4", "1/8", "1/16", "24p" };
    int clock_in() const { return _clock_in; }

    // Enter the action screen, rebuilding the rows for this engine. The cursor is KEPT from the last
    // visit, so the action you use most stays two clicks away: one to open, one to fire.
    void open_actions(IEngine& engine, uint32_t now_ms)
    {
        if (!Board::kHasScreen) return;
        build_actions(engine);
        if (_act_count == 0) return;
        // Land on the first row that DOES something. `back` is always row 0, so the useful default is
        // row 1 - but only if row 1 exists; and on a first visit that row is `play` only for an engine
        // with a play pad. For an engine without one (reverb, chorus, filter, delay, ...) row 1 is a
        // config switch, and defaulting the cursor onto a mode selector means the second click of a
        // first visit CHANGES THE MODE. So the initial position is computed from the rows that were
        // actually built, and only the FIRST time - after that the cursor is remembered, which is what
        // keeps a repeated action two clicks away.
        if (!_act_seen) {
            _act_cursor = first_useful_row();
            _act_seen   = true;
        }
        if (_act_cursor >= _act_count) _act_cursor = 0;
        _actions = true;
        _act_ms  = now_ms;
    }

    void close_actions() { _actions = false; }

    void move_cursor(int inc, uint32_t now_ms)
    {
        if (!_actions || _act_count == 0) return;
        _act_ms = now_ms;
        int c = _act_cursor + inc;
        while (c < 0)           c += _act_count;
        while (c >= _act_count) c -= _act_count;
        _act_cursor = c;
    }

    // Fire the highlighted row and stay in the list, so a repeated action is one click. Returns which
    // KIND fired, so the harness can count play presses for its header without this class having to
    // know what a play press means.
    ActionRow::Kind fire(IEngine& engine, uint32_t now_ms)
    {
        _act_ms = now_ms;
        if (!_actions || _act_count == 0) return ActionRow::Back;
        const ActionRow r = _act[_act_cursor];
        switch (r.kind) {
            case ActionRow::Back:     _actions = false; break;
            case ActionRow::Play:     engine.on_play_pad(_deck, false);   break;
            case ActionRow::Alt:      engine.on_play_pad(_deck, true);    break;  // the pads' `reverse` half
            case ActionRow::Rec:      engine.on_record_pad(_deck, false); break;
            case ActionRow::Stop:     engine.stop_if_generating(_deck);   break;
            case ActionRow::ClearBuf: engine.clear_buffer(_deck);         break;
            case ActionRow::SeqArm:   engine.on_seq_toggle_arm(_deck);    break;
            case ActionRow::SeqTrig:  engine.on_seq_trigger(_deck);       break;
            case ActionRow::SeqClear: engine.clear_sequence(_deck);       break;
            case ActionRow::Disarm:   engine.disarm_track(_deck);         break;
            case ActionRow::FxLock:   engine.toggle_fx_lock(_deck, r.fx); break;
            case ActionRow::Fx: {
                // set_fx is a pad HELD, not a pad pressed - there is no "toggle" in the contract, so
                // the on/off state is kept here and sent explicitly. Unknown until first used, for
                // the same reason the switches are: the engine booted at its own state and nothing
                // here can read it back, so the first click asserts ON.
                const int f = (r.fx == FxKind::Flux) ? 0 : 1;
                const int d = (_deck == DeckRef::A) ? 0 : 1;
                const bool on = _fx_known[f][d] ? !_fx_on[f][d] : true;
                _fx_on[f][d]    = on;
                _fx_known[f][d] = true;
                engine.set_fx(_deck, r.fx, on);
                break;
            }
            case ActionRow::Deck:
                set_deck(_deck == DeckRef::A ? DeckRef::B : DeckRef::A, engine);
                break;
            case ActionRow::ClockIn:
                // No `-` state here, unlike the engine switches: the harness owns this value and knows
                // what it booted at, so the row can be honest from the first draw.
                _clock_in = (_clock_in + 1) % kClockInCount;
                break;
            case ActionRow::Config: {
                const int     c    = static_cast<int>(r.cfg);
                const int     d    = (_deck == DeckRef::A) ? 0 : 1;
                const uint8_t span = r.span ? r.span : 1;
                // A switch this UI has not written yet is UNKNOWN, not zero - the engine booted at
                // whatever default it chose and the contract has no reader. So the first click
                // SELECTS position 1 rather than advancing from a guess; every click after that
                // cycles. From here on the screen and the engine agree by construction.
                const uint8_t v = _cfg_known[c][d] ? static_cast<uint8_t>((_cfg[c][d] + 1) % span) : 0;
                _cfg[c][d]       = v;
                _cfg_known[c][d] = true;
                if (r.cfg == ConfigId::Route) {                     // Route is global, not per-deck
                    _cfg[c][d ^ 1]       = v;
                    _cfg_known[c][d ^ 1] = true;
                }
                engine.set_config(r.cfg, _deck, static_cast<int>(v));
                // A switch can repoint what the knobs mean (granular's deck_layout follows Mode), so
                // drop them out of catch rather than let a stale pickup write the old value.
                seed_page(engine);
                break;
            }
        }
        return r.kind;
    }

    // Main loop: close the screen after a spell with no interaction. Without this a menu left open
    // keeps the encoder off the parameter pages until someone notices.
    void tick(uint32_t now_ms)
    {
        if (_actions && now_ms - _act_ms >= kActionIdleMs) _actions = false;
    }

    // Draw the page. Rate-limited internally; safe to call every main-loop pass. `engine_name` is the
    // build's engine (SPK_ENGINE_STR), `bpm` and `deck` come from the harness.
    // `status` is an optional short string drawn right-aligned in the header instead of the tempo.
    // The harness uses it for state the engine does not expose as a param - for a streaming engine,
    // whether the stream deck is actually playing, which is the difference between "the engine is
    // silent" and "the engine never started a file".
    void render(Board& board, IEngine& engine, const char* engine_name, float bpm, uint32_t now_ms,
                const char* status = nullptr)
    {
        if (!Board::kHasScreen) return;
        // The engine page is drawn by the display adapter, which owns the DisplayModel; this class
        // deliberately knows nothing about one. Drawing nothing here leaves the panel to it.
        if (on_engine_page() && !_actions) return;
        if (now_ms - _last_draw_ms < static_cast<uint32_t>(1000 / kScreenHz)) return;
        _last_draw_ms = now_ms;

        if (_actions) { render_actions(board, engine_name, status); return; }

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
        if (status && *status) {
            const int w = static_cast<int>(std::strlen(status)) * 6;
            board.ScreenText(Board::kScreenWidth - w - 1, 0, status, true);
        } else {
            std::snprintf(line, sizeof(line), "%3d", static_cast<int>(bpm + 0.5f));
            board.ScreenText(Board::kScreenWidth - 18, 0, line, true);
        }

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

    // Parameter pages plus the engine's own page, where there is one.
    int total_pages() const { return _pages + (_engine_page ? 1 : 0); }

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

    // --- Action screen internals ---------------------------------------------------------------
    // The worst case, exactly. DERIVED rather than written as a literal: the old `16 + ConfigId::Count`
    // was correct but had zero slack, and build_actions() drops a row that does not fit SILENTLY - so
    // adding one bit to PadBit would have made the `clk in` row (added last) vanish with no diagnostic.
    //
    // Rows, in build_actions() order: `back`, then one row per pad bit plus one extra for `alt` (which
    // rides on the play bit) and one extra for each of the two FX bits (one row per effect), then
    // `deck`, then one row per config, then the platform `clk in` row.
    static constexpr int kPadRows =
        kPadBitCount   // one row per implemented pad
        + 1            // `alt`: on_play_pad's `reverse` half, which has no bit of its own
        + 2;           // set_fx / toggle_fx_lock each yield a row per FxKind, so one extra apiece
    static constexpr int kMaxActions =
        1                                      // back
        + kPadRows
        + 1                                    // deck
        + static_cast<int>(ConfigId::Count)    // one per categorical switch
        + 1;                                   // clk in
    static_assert(kMaxActions >= 1 + kPadRows + 1 + static_cast<int>(ConfigId::Count) + 1,
                  "kMaxActions no longer covers every row build_actions() can emit");

    // How many values a switch takes. The wire values are the contract's own
    // (engine/engine_params.h): Route and Mode are ternary, the rest are 0/1 flags.
    static uint8_t config_span(ConfigId id)
    {
        return (id == ConfigId::Route || id == ConfigId::Mode) ? 3 : 2;
    }

    // The rows, from what the engine says about itself. Pads come from the compile-time mask
    // (engine_pads.h), configs from live_configs(), the deck row from capabilities() - three
    // different questions, each answered by whichever channel can answer it honestly.
    //
    // The mask measures the engine's implementation rather than a declaration about it, so a row is
    // present exactly when the method behind it is. That is what makes it safe to list the whole pad
    // surface here: the sequencer and FX rows appear on the four engines that have them and nowhere
    // else, instead of every engine carrying eight rows that do nothing.
    void build_actions(IEngine& engine)
    {
        _act_count = 0;
        auto add = [&](ActionRow::Kind k, ConfigId c = ConfigId::Count, FxKind f = FxKind::Flux) {
            // kMaxActions is derived from the pad/config counts above, so this can only fire if one of
            // those derivations has drifted - a bug here, never a fact about an engine.
            if (_act_count >= kMaxActions) { _act_overflow = true; return; }
            ActionRow& r = _act[_act_count++];
            r.kind = k;
            r.cfg  = c;
            r.span = (k == ActionRow::Config) ? config_span(c) : 0;
            r.fx   = f;
        };

        add(ActionRow::Back);

        if (_pads & PadPlay) {
            add(ActionRow::Play);
            // The same pad with `reverse` set - upstream's Rev. It has no separate method, so it
            // rides on the play bit: bard jumps back 15 s, shuttle and softcut swap track, edrums
            // swaps drum, granular plays backwards.
            add(ActionRow::Alt);
        }
        if (_pads & PadRecord)   add(ActionRow::Rec);
        if (_pads & PadStop)     add(ActionRow::Stop);
        if (_pads & PadClearBuf) add(ActionRow::ClearBuf);
        if (_pads & PadSeqArm)   add(ActionRow::SeqArm);
        if (_pads & PadSeqTrig)  add(ActionRow::SeqTrig);
        if (_pads & PadSeqClear) add(ActionRow::SeqClear);
        if (_pads & PadDisarm)   add(ActionRow::Disarm);
        if (_pads & PadFx) {
            add(ActionRow::Fx, ConfigId::Count, FxKind::Flux);
            add(ActionRow::Fx, ConfigId::Count, FxKind::Grit);
        }
        if (_pads & PadFxLock) {
            add(ActionRow::FxLock, ConfigId::Count, FxKind::Flux);
            add(ActionRow::FxLock, ConfigId::Count, FxKind::Grit);
        }

        if (engine.capabilities() & CapDualDeck) add(ActionRow::Deck);

        // One row per declared switch. An engine that narrows nothing keeps the inherited all-live
        // mask and gets all six - the same "I did not say" the param pages inherit, and the same
        // consequence: turning one does nothing rather than something wrong.
        const IEngine::ConfigMask live = engine.live_configs();
        for (uint8_t i = 0; i < static_cast<uint8_t>(ConfigId::Count); i++)
            if (live & (IEngine::ConfigMask{1} << i)) add(ActionRow::Config, static_cast<ConfigId>(i));

        // The one PLATFORM row: what a pulse at the clock input means. It is not the engine's to
        // answer - the transport owns external sync - but it has to be settable on the box, because
        // whether a Eurorack clock sends quarters or sixteenths is a property of the rack, not of the
        // build. Only where there is a clock input to configure.
        if (Board::kGateCount >= 2) add(ActionRow::ClockIn);
    }

    static const char* action_name(const ActionRow& r)
    {
        const bool flux = (r.fx == FxKind::Flux);
        switch (r.kind) {
            case ActionRow::Back:     return "back";
            case ActionRow::Play:     return "play";
            case ActionRow::Alt:      return "alt";
            case ActionRow::Rec:      return "rec";
            case ActionRow::Stop:     return "stop";
            case ActionRow::ClearBuf: return "clear buf";
            case ActionRow::SeqArm:   return "arm seq";
            case ActionRow::SeqTrig:  return "trig";
            case ActionRow::SeqClear: return "clr seq";
            case ActionRow::Disarm:   return "disarm";
            case ActionRow::Fx:       return flux ? "flux" : "grit";
            case ActionRow::FxLock:   return flux ? "flux lock" : "grit lock";
            case ActionRow::Deck:     return "deck";
            case ActionRow::Config:   return kConfigNames[static_cast<uint8_t>(r.cfg)];
            case ActionRow::ClockIn:  return "clk in";
        }
        return "";
    }

    // The list, four rows at a time, scrolled to keep the cursor visible with a row of context above
    // it where there is one. A switch shows its position (1-based, as a player counts) so a mode is
    // read off the screen instead of inferred from what changed in the sound.
    void render_actions(Board& board, const char* engine_name, const char* status)
    {
        char line[32];
        board.ScreenClear();

        // The trailing '!' only ever appears if build_actions() had to drop a row, which is a
        // kMaxActions derivation bug rather than anything an engine can cause - but a silently short
        // list is exactly the failure that would otherwise be blamed on the engine.
        std::snprintf(line, sizeof(line), "%s %c act%s", engine_name,
                      _deck == DeckRef::A ? 'A' : 'B', _act_overflow ? "!" : "");
        board.ScreenText(0, 0, line, true);
        if (status && *status) {
            const int w = static_cast<int>(std::strlen(status)) * 6;
            board.ScreenText(Board::kScreenWidth - w - 1, 0, status, true);
        }

        constexpr int kRows = 4;
        int first = _act_cursor - 1;
        if (first > _act_count - kRows) first = _act_count - kRows;
        if (first < 0) first = 0;

        for (int i = 0; i < kRows && first + i < _act_count; i++) {
            const ActionRow& r = _act[first + i];
            const int        y = 14 + i * 12;

            char value[8] = {0};
            if (r.kind == ActionRow::Deck) {
                std::snprintf(value, sizeof(value), "%c", _deck == DeckRef::A ? 'A' : 'B');
            } else if (r.kind == ActionRow::ClockIn) {
                std::snprintf(value, sizeof(value), "%s", kClockInNames[_clock_in]);
            } else if (r.kind == ActionRow::Fx) {
                const int f = (r.fx == FxKind::Flux) ? 0 : 1;
                const int d = (_deck == DeckRef::A) ? 0 : 1;
                std::snprintf(value, sizeof(value), "%s",
                              !_fx_known[f][d] ? "-" : (_fx_on[f][d] ? "on" : "off"));
            } else if (r.kind == ActionRow::Config) {
                const int c = static_cast<int>(r.cfg);
                const int d = (_deck == DeckRef::A) ? 0 : 1;
                // '-' means "not known", not "off": until this row is used, the engine's switch is
                // wherever the engine put it and nothing here can read it back. Printing a position
                // would be a guess, and a plausible number is worse than a visible blank.
                if (_cfg_known[c][d]) std::snprintf(value, sizeof(value), "%d/%d", _cfg[c][d] + 1, r.span);
                else                  std::snprintf(value, sizeof(value), "-/%d", r.span);
            }

            std::snprintf(line, sizeof(line), "%c%-10.10s %s",
                          (first + i == _act_cursor) ? '>' : ' ', action_name(r), value);
            board.ScreenText(0, y, line, true);
        }

        board.ScreenUpdate();
    }

    // The cursor a first visit should land on.
    //
    // The governing rule: a first visit is two clicks (one to open, one to fire), and those two clicks
    // must never silently change a setting. So the cursor lands on a MOMENTARY row - `play` if there
    // is one, else any other pad - and otherwise on `back`, where the second click just closes the
    // list again. It never lands on a `Config` switch (whose click changes the engine's mode) or on
    // `clk in` (whose click changes what the rack's clock means).
    //
    // The old rule was the literal row 1, with a comment asserting that row is `play`. It is `play`
    // only for an engine that HAS a play pad; for `reverb`, `chorus`, `filter`, `delay`, `qdelay` and
    // `gigaverb` row 1 is the first config switch, so opening the list and clicking once changed the
    // reverb algorithm.
    //
    // Only consulted on the first visit; after that the cursor is remembered, which is what keeps a
    // repeated action two clicks away.
    static bool is_momentary(ActionRow::Kind k)
    {
        switch (k) {
            case ActionRow::Back:
            case ActionRow::Config:
            case ActionRow::ClockIn:
            case ActionRow::Deck:
                return false;   // these change state that persists past the click
            default:
                return true;    // the pads: play/alt/rec/stop/clear/seq/fx
        }
    }

    int first_useful_row() const
    {
        for (int i = 1; i < _act_count; i++)
            if (_act[i].kind == ActionRow::Play) return i;
        for (int i = 1; i < _act_count; i++)
            if (is_momentary(_act[i].kind)) return i;
        return 0;   // nothing momentary to offer: `back`, which is harmless to click
    }

    ActionRow    _act[kMaxActions];
    int          _act_count  = 0;
    int          _act_cursor = 0;       // seeded on the first open (see first_useful_row)
    bool         _act_seen   = false;   // has the action screen been opened at least once
    bool         _act_overflow = false; // a row did not fit - a kMaxActions bug, surfaced in the header
    bool         _actions    = false;
    uint32_t     _act_ms     = 0;
    // The switch positions, as WRITTEN by this UI - the contract has no reader for a config, so the
    // only honest source is what we last sent. Per deck, because every switch but Route is per-deck.
    // _cfg_known says whether we have written it AT ALL: before that the engine sits at its own
    // default (tape, shuttle, softcut and radio boot at Route = DoubleMono; bard at mode = Read), so
    // the row reads `-` and the first click selects position 1. A `config(ConfigId)` reader on
    // IEngine would remove the unknown entirely, at the cost of diverging from the upstream contract.
    uint8_t      _cfg[static_cast<int>(ConfigId::Count)][2]       = {};
    bool         _cfg_known[static_cast<int>(ConfigId::Count)][2] = {};
    // The two end-of-chain effects, [flux|grit][deck], held for the same reason as the switches.
    bool         _fx_on[2][2]    = {};
    bool         _fx_known[2][2] = {};
    PadMask      _pads           = 0;    // which pads this build's engine implements (engine_pads.h)
    int          _clock_in       = 0;    // index into kClockInNames; 1/4 is the Eurorack default

    ParamId      _params[kMaxParams] = {};
    int          _count              = 0;
    int          _knobs              = 4;
    int          _pages              = 0;   // PARAMETER pages only; see total_pages()
    bool         _engine_page        = false;
    int          _page               = 0;
    DeckRef::Ref _deck               = DeckRef::A;
    ParamSlot    _slot[kMaxSlots];
    bool         _primed             = false;   // has poll_knobs seen a real reading yet
    uint32_t     _last_draw_ms       = 0;
};

} // namespace daisyapps
