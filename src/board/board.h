#pragma once

// Compile-time target selection for the control/UI abstraction. Each board wraps its libDaisy BSP
// (DaisyPod / DaisyPatchSM / DaisyPatch) behind a uniform surface:
//
//   void  Init(int block_size);                       // bring up the BSP + audio config + ADC
//   void  StartAudio(daisy::AudioHandle::AudioCallback cb);
//   float SampleRate();
//   float Analog(int i);                              // ISR-safe normalized read of analog control i
//   void  Poll(Controls& c);                          // main loop: refresh + fill the snapshot
//   void  StartMidi();                                // no-op where a board has no MIDI input
//   template <typename Sink> void PollMidi(Sink&&);   // main loop: drain MIDI -> sink(st, d1, d2)
//   void  SetIndicator(int idx, float r, float g, float b);  // no-op where a board lacks LEDs
//   void  SetUserLed(bool on);                        // onboard MCU LED (present on every Daisy)
//   void  SetCvOut(int ch, float norm);               // no-op where a board lacks CV outputs
//   void  SetGateOut(bool on);                        // no-op where a board lacks a gate output
//   void  ScreenClear() / ScreenText(x,y,s,small) / ScreenRect(x,y,w,h,fill) / ScreenUpdate();
//   static constexpr int  kAnalogCount / kButtonCount / kGateCount / kIndicatorCount / kCvOutCount;
//   static constexpr int  kAudioChannels;             // 2 on Pod / patch.init(), 4 on Daisy Patch
//   static constexpr bool kHasScreen;  static constexpr int kScreenWidth / kScreenHeight;
//
// EVERY board implements the whole surface; a board without a given peripheral implements those calls
// as no-ops (and reports a zero count / kHasScreen = false) rather than omitting them. That is what
// lets one harness drive all three targets with no #ifdef and no `if constexpr` around the UI code:
// the Patch's OLED calls simply do nothing on a Pod. Query the counts to skip work, not to compile it.
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
