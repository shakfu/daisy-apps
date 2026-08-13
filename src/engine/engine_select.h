// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Build-time engine selection. The app Makefile's ENGINE variable emits one -DSPK_ENGINE_* define;
// this header maps it to the concrete `ActiveEngine` type that app/harness.cpp instantiates. The
// harness only ever sees IEngine, so this is the single place the firmware names a concrete engine.
//
// The SPK_ENGINE_* spelling is upstream's (sk-engines/src/engine/engine_select.h) and is kept
// deliberately: engine sources are ported verbatim apart from the namespace, and a few of them test
// their own define. Keeping the names identical means a port is a copy, not a search-and-replace.
//
// Engines ported so far are listed below; the rest of the upstream set (mosc, reso, softcut, tape,
// shuttle, radio, pstretch, bard, granular, graincloud, edrums) follows the procedure in
// docs/dev/porting-sk-engines.md. csound/chuck are NOT here - they predate this harness and keep
// their own single-engine harnesses under pod/.

#if defined(SPK_ENGINE_PASSTHROUGH)
  #include "engine/passthrough/passthrough_engine.h"
  namespace daisyapps { using ActiveEngine = PassthroughEngine; }
#elif defined(SPK_ENGINE_DELAY)
  #include "engine/delay/delay_engine.h"
  namespace daisyapps { using ActiveEngine = DelayEngine; }
#elif defined(SPK_ENGINE_QDELAY)
  #include "engine/qdelay/qdelay_engine.h"
  namespace daisyapps { using ActiveEngine = QdelayEngine; }
#elif defined(SPK_ENGINE_GLITCH)
  #include "engine/glitch/glitch_engine.h"
  namespace daisyapps { using ActiveEngine = GlitchEngine; }
#elif defined(SPK_ENGINE_REVERB)
  #include "engine/reverb/reverb_engine.h"
  namespace daisyapps { using ActiveEngine = ReverbEngine; }
#elif defined(SPK_ENGINE_GIGAVERB)
  #include "engine/gigaverb/gigaverb_engine.h"
  namespace daisyapps { using ActiveEngine = GigaverbEngine; }
#elif defined(SPK_ENGINE_CHORUS)
  #include "engine/chorus/chorus_engine.h"
  namespace daisyapps { using ActiveEngine = ChorusEngine; }
#elif defined(SPK_ENGINE_FILTER)
  #include "engine/filter/filter_engine.h"
  namespace daisyapps { using ActiveEngine = FilterEngine; }
#elif defined(SPK_ENGINE_VOICE)
  #include "engine/voice/voice_engine.h"
  namespace daisyapps { using ActiveEngine = VoiceEngine; }
// --- Granular (the original Spotykach engine) and its GrainflowLib variant --------------------
#elif defined(SPK_ENGINE_GRANULAR)
  #include "engine/granular/granular_engine.h"
  namespace daisyapps { using ActiveEngine = GranularEngine; }
#elif defined(SPK_ENGINE_GRAINCLOUD)
  #include "engine/graincloud/graincloud_engine.h"
  namespace daisyapps { using ActiveEngine = GraincloudEngine; }
#elif defined(SPK_ENGINE_EDRUMS)
  #include "engine/edrums/edrums_engine.h"
  namespace daisyapps { using ActiveEngine = EdrumsEngine; }
// --- Mutable Instruments voices (vendored DSP; the two engines that play from MIDI) -----------
#elif defined(SPK_ENGINE_RESO)
  #include "engine/reso/reso_engine.h"
  namespace daisyapps { using ActiveEngine = ResoEngine; }
#elif defined(SPK_ENGINE_MOSC)
  #include "engine/mosc/mosc_engine.h"
  namespace daisyapps { using ActiveEngine = MoscEngine; }
// --- SD-streaming engines (SPK_USE_STREAM; see hw/stream_deck.h) ------------------------------
#elif defined(SPK_ENGINE_RADIO)
  #include "engine/radio/radio_engine.h"
  namespace daisyapps { using ActiveEngine = RadioEngine; }
#elif defined(SPK_ENGINE_TAPE)
  #include "engine/tape/tape_engine.h"
  namespace daisyapps { using ActiveEngine = TapeEngine; }
#elif defined(SPK_ENGINE_SHUTTLE)
  #include "engine/shuttle/shuttle_engine.h"
  namespace daisyapps { using ActiveEngine = ShuttleEngine; }
#elif defined(SPK_ENGINE_PSTRETCH)
  #include "engine/pstretch/pstretch_engine.h"
  namespace daisyapps { using ActiveEngine = PstretchEngine; }
#elif defined(SPK_ENGINE_SOFTCUT)
  #include "engine/softcut/softcut_engine.h"
  namespace daisyapps { using ActiveEngine = SoftcutEngine; }
#elif defined(SPK_ENGINE_BARD)
  #include "engine/bard/bard_engine.h"
  namespace daisyapps { using ActiveEngine = BardEngine; }
#else
  #error "No engine selected: build with one of: passthrough delay qdelay glitch reverb chorus filter voice gigaverb radio tape shuttle pstretch softcut bard reso mosc granular graincloud edrums"
#endif
