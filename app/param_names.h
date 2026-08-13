#pragma once

// Generic slot names for ParamId / ConfigId - the fallback the OLED prints above a knob when the
// engine's own param_label() returns nullptr for that slot. Lifted from sk-engines' terminal/names.cpp
// (its `describe` reply prints the same words), and kept in the same order so the two stay diffable.
//
// The distinction matters on screen: `speed` is the SLOT, "station" is what the radio engine does with
// it. An engine that overrides param_label wins; this table is only what a slot is called when nothing
// more specific exists.

#include "engine/engine_params.h"

namespace daisyapps {

// index == (uint8_t)ParamId (engine/engine_params.h). Keep in lockstep with the enum.
inline const char* const kParamNames[] = {
    "pos", "fluxfb", "env", "envsize", "size", "win", "polyslice", "speed",
    "fluxint", "gritint", "fluxmix", "gritmix", "feedback", "mix", "modspeed", "modamp",
    "tempo", "clickmix", "panspeed", "panrange", "keyinterval", "crossfade", "altpos", "aux",
};
static_assert(sizeof(kParamNames) / sizeof(kParamNames[0]) == static_cast<size_t>(ParamId::Count),
              "kParamNames out of sync with ParamId");

// index == (uint8_t)ConfigId.
inline const char* const kConfigNames[] = {
    "route", "modtype", "lfoshape", "mode", "startmodon", "sizemodon",
};
static_assert(sizeof(kConfigNames) / sizeof(kConfigNames[0]) == static_cast<size_t>(ConfigId::Count),
              "kConfigNames out of sync with ConfigId");

} // namespace daisyapps
