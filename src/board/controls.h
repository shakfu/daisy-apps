#pragma once

namespace daisyapps {

// Board-agnostic snapshot of a target's control surface, filled by Board::Poll() once per main-loop
// pass. The *_count fields say how many entries the active board actually populates - a Daisy Pod
// fills 2 analog + 2 buttons + 0 gates; patch.init() fills up to 8 analog + 2 gates; etc. Consumers
// (the harness / a future platform layer) map analog[i] onto engine ParamIds without knowing which
// board produced them. Counts let generic code iterate only the live controls.
//
// "analog" unifies pots/knobs and CV inputs into one normalized 0..1 array: a board lists its pots
// first (analog[0..pot_count-1]), then any CV inputs. The minimal-case Pod has pots only.
struct Controls {
    static constexpr int kMaxAnalog  = 8;   // pots + CV inputs, unified, normalized 0..1
    static constexpr int kMaxButtons = 4;
    static constexpr int kMaxGates   = 2;

    int   analog_count = 0;
    float analog[kMaxAnalog] = {0.f};

    int   enc_inc   = 0;       // encoder detents since the last Poll() (signed; 0 if no encoder)
    bool  enc_press = false;   // encoder switch currently held

    int   button_count = 0;
    bool  button[kMaxButtons] = {false};

    int   gate_count = 0;
    bool  gate[kMaxGates] = {false};
};

} // namespace daisyapps
