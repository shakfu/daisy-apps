#pragma once

// The harness's ITimeSource: libDaisy's millisecond/microsecond clock behind the contract's
// interface.
//
// Split out of harness_clock.h because it is the ONLY thing in the clock layer that touches the HAL.
// HarnessTransport - the tick grid, the external-sync re-datuming, the common/sub tick division - is
// pure arithmetic over an injected ITimeSource, and that arithmetic is exactly the part worth
// testing: its own comment records that conflating the sub-tick and common-tick rates once ran a
// sequencer twelve times too fast and presented as "the clock is broken". Keeping <daisy.h> out of
// harness_clock.h is what lets host/test_transport.cpp drive it from a fake clock.

#include <cstdint>

#include "daisy.h"
#include "engine/itimesource.h"

namespace daisyapps {

// ITimeSource over daisy::System. now_ms/now_us are what engines use for UI-rate motion (breathe,
// blink, tap timing); nothing in the audio path depends on them.
class SystemTime : public ITimeSource {
public:
    uint32_t now_ms() const override { return daisy::System::GetNow(); }
    uint32_t now_us() const override { return daisy::System::GetUs(); }
};

} // namespace daisyapps
