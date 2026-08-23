#pragma once

// Which of IEngine's optional PAD methods does the engine in this build actually implement?
//
// The problem. IEngine's pads (play, record, the sequencer, the FX toggles) have no-op default
// bodies, so an engine that does not want them simply says nothing - which is what makes the contract
// pleasant to port against, and also means the platform cannot ask. capabilities() is the declaration
// channel, but it has no bit for most of the pads and it is a hand-maintained claim: `tape` and
// `shuttle` implemented on_record_pad for their whole life here while declaring no CapRecording. A UI
// built on that either hides a feature or offers one that does not exist.
//
// The answer is a language rule rather than a declaration. For an INHERITED member, `&Derived::f` has
// type `R (Base::*)(...)`; the type only becomes `R (Derived::*)(...)` once the derived class (or an
// intermediate wrapper, which counts - it really does declare it) declares f. Comparing those two
// types answers "did this engine override it" at COMPILE TIME: no runtime cost, no vtable or ABI
// assumptions, and nothing to keep in sync, so it cannot drift the way a capability bit can.
//
// It needs the concrete engine type, which the firmware has exactly where it matters - ActiveEngine
// in app/harness.cpp, since the engine is chosen at build time. The harness folds the answer to a
// mask and hands it to the UI, which never learns the engine's type.
//
// What it does NOT tell you: whether the override does anything useful. An engine that declares a pad
// and returns immediately still reads as implementing it. That error is a spare row on a screen; the
// error it replaces was offering `rec` on the thirteen engines that cannot record.

#include <cstdint>
#include <type_traits>

#include "engine/iengine.h"

namespace daisyapps {

// One bit per pad. Order is IEngine's own declaration order, which is also roughly the order a player
// would look for them.
enum PadBit : uint16_t {
    PadPlay     = 1u << 0,   // on_play_pad - also gates the `alt` row, which is the same pad reversed
    PadRecord   = 1u << 1,   // on_record_pad
    PadClearBuf = 1u << 2,   // clear_buffer
    PadStop     = 1u << 3,   // stop_if_generating
    PadFx       = 1u << 4,   // set_fx
    PadFxLock   = 1u << 5,   // toggle_fx_lock
    PadSeqArm   = 1u << 6,   // on_seq_toggle_arm
    PadSeqTrig  = 1u << 7,   // on_seq_trigger
    PadSeqClear = 1u << 8,   // clear_sequence
    PadDisarm   = 1u << 9,   // disarm_track
};
using PadMask = uint16_t;

// How many pad bits exist. Consumers size their row tables from this rather than from a literal, so
// adding a pad here cannot silently overflow a UI list (see ParamUI::kMaxActions). The assert keeps
// the count and the highest bit in step.
inline constexpr int kPadBitCount = 10;
static_assert(PadDisarm == (1u << (kPadBitCount - 1)),
              "kPadBitCount is out of step with the PadBit enumerators above");

// declares_<name><E>() - true iff E (or something between E and IEngine) declares that member.
#define SPK_DECLARES(name, ret, ...)                                                      \
    template <class E>                                                                    \
    constexpr bool declares_##name()                                                      \
    {                                                                                     \
        return !std::is_same<decltype(&E::name), ret (IEngine::*)(__VA_ARGS__)>::value;    \
    }

SPK_DECLARES(on_play_pad,        bool, DeckRef::Ref, bool)
SPK_DECLARES(on_record_pad,      void, DeckRef::Ref, bool)
SPK_DECLARES(clear_buffer,       void, DeckRef::Ref)
SPK_DECLARES(stop_if_generating, void, DeckRef::Ref)
SPK_DECLARES(set_fx,             void, DeckRef::Ref, FxKind, bool)
SPK_DECLARES(toggle_fx_lock,     void, DeckRef::Ref, FxKind)
SPK_DECLARES(on_seq_toggle_arm,  void, DeckRef::Ref)
SPK_DECLARES(on_seq_trigger,     void, DeckRef::Ref)
SPK_DECLARES(clear_sequence,     void, DeckRef::Ref)
SPK_DECLARES(disarm_track,       void, DeckRef::Ref)

#undef SPK_DECLARES

template <class E>
constexpr PadMask pad_mask()
{
    return static_cast<PadMask>(
          (declares_on_play_pad<E>()        ? PadPlay     : 0)
        | (declares_on_record_pad<E>()      ? PadRecord   : 0)
        | (declares_clear_buffer<E>()       ? PadClearBuf : 0)
        | (declares_stop_if_generating<E>() ? PadStop     : 0)
        | (declares_set_fx<E>()             ? PadFx       : 0)
        | (declares_toggle_fx_lock<E>()     ? PadFxLock   : 0)
        | (declares_on_seq_toggle_arm<E>()  ? PadSeqArm   : 0)
        | (declares_on_seq_trigger<E>()     ? PadSeqTrig  : 0)
        | (declares_clear_sequence<E>()     ? PadSeqClear : 0)
        | (declares_disarm_track<E>()       ? PadDisarm   : 0));
}

// Drift guard. If one of the signatures above ever stops matching IEngine's - a parameter added, a
// return type changed - the comparison stops matching for EVERY class and every engine silently reads
// as implementing that pad. The base must therefore report a mask of zero; anything else is a typo in
// this file rather than a fact about an engine.
static_assert(pad_mask<IEngine>() == 0,
              "a signature in engine_pads.h no longer matches IEngine - fix it here, not in an engine");

} // namespace daisyapps
