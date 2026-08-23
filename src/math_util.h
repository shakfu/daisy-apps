#pragma once

// The handful of scalar helpers the ENGINE CONTRACT needs, with no HAL dependency.
//
// Why this file exists. `common.h` is a general-purpose math/logging grab bag that includes
// <daisy.h> and <daisysp.h>. `engine/color.h` used to reach for it to get one function -
// unitclamp() - which meant the whole contract chain
//
//     engine/iengine.h -> engine/display_model.h -> engine/led.ring.h -> engine/color.h -> common.h
//
// dragged libDaisy and DaisySP into every translation unit that included iengine.h (measured: 339
// headers, ~101k preprocessed lines), and made it impossible to compile the contract - or anything
// that includes it, such as app/param_ui.h - without a cross toolchain. That in turn blocked the
// host test build the streaming layer was explicitly designed for (see memory/audio_stream.h).
//
// Splitting the one needed function out costs nothing and makes the contract header-independent of
// the HAL again: `#include "engine/iengine.h"` now compiles under a plain host g++ with only the
// standard library. `common.h` is unchanged and still available to engine code that wants the rest
// of it; it includes this header so the definitions stay single-sourced.
//
// Keep this file HAL-free. Anything added here must compile with nothing but <algorithm>/<cmath>.

#include <algorithm>

namespace infrasonic
{

/** True if `in` lies within the normalized 0..1 range. */
constexpr bool is_in_unit_range(float in)
{
    return in >= 0.0f && in <= 1.0f;
}

/** Sign of `in` as +/-1. Zero is treated as negative, matching the original behaviour. */
constexpr float sgn(float in)
{
    return in > 0 ? 1.0f : -1.0f;
}

/** Clamp to the normalized 0..1 range. */
constexpr float unitclamp(float in)
{
    return std::clamp(in, 0.0f, 1.0f);
}

/** Linear interpolation between `a` and `b` by `t` (unclamped). */
constexpr float lerp(float a, float b, float t)
{
    return (1 - t) * a + t * b;
}

/** Three-point linear interpolation: `t` in 0..0.5 spans value1->value2, 0.5..1 spans value2->value3. */
constexpr float lerp3(float value1, float value2, float value3, float t)
{
    return t < 0.5f ? lerp(value1, value2, t * 2.0f)
                    : lerp(value2, value3, 2.0f * t - 1.0f);
}

/** Rescale `x` from the range [in_min, in_max] onto [out_min, out_max]. */
template <typename T>
constexpr T map(const T& x,
                const T& in_min,
                const T& in_max,
                const T& out_min,
                const T& out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

} // namespace infrasonic
