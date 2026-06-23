#pragma once

// Compile-time target selection for the control/UI abstraction. Each board wraps its libDaisy BSP
// (DaisyPod / DaisyPatchSM / DaisyPatch) behind a uniform surface:
//
//   void  Init(int block_size);                       // bring up the BSP + audio config + ADC
//   void  StartAudio(daisy::AudioHandle::AudioCallback cb);
//   float SampleRate();
//   float Analog(int i);                              // ISR-safe normalized read of analog control i
//   void  Poll(Controls& c);                          // main loop: refresh + fill the snapshot
//   void  SetIndicator(int idx, float r, float g, float b);  // no-op where a board lacks LEDs
//   void  SetUserLed(bool on);                        // onboard MCU LED (present on every Daisy)
//   static constexpr int kAnalogCount / kButtonCount / kGateCount / kIndicatorCount;
//
// Define exactly one TARGET_* (the Makefiles pass -DTARGET_POD). `Board` then aliases the concrete
// class, so the harness writes `daisyapps::Board board;` and stays board-agnostic. No virtual
// dispatch: the selection is resolved at compile time, and a build links only its board's driver.
#if defined(TARGET_POD)
#include "board/pod_board.h"
namespace daisyapps { using Board = PodBoard; }
#elif defined(TARGET_PATCH_INIT)
#include "board/patch_init_board.h"
namespace daisyapps { using Board = PatchInitBoard; }
#elif defined(TARGET_PATCH)
#include "board/patch_board.h"
namespace daisyapps { using Board = PatchBoard; }
#else
#error "daisy-apps board: define exactly one of TARGET_POD / TARGET_PATCH_INIT / TARGET_PATCH"
#endif
