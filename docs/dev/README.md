# Developer notes

Documents that live in this repo:

- [`porting-sk-engines.md`](porting-sk-engines.md) — the procedure for bringing an engine across from
  sk-engines, and the rules that keep a port a copy rather than a rewrite.
- [`chuck-midi-in.md`](chuck-midi-in.md) — how MIDI reaches a `.ck` patch's `MidiIn`.
- [`chuck-midi-in-porting.md`](chuck-midi-in-porting.md) — what had to change in the ChucK core to get
  it there, and the patch that does it (`scripts/patches/midi_daisy.patch`).
- [`hardware-bringup.md`](hardware-bringup.md) — the ordered bench plan for validating `app/` on a
  Daisy Patch, and the running record of what has actually been observed on a device.

## References to documents that are not here

Engine sources and the contract headers under `src/engine/` are ported from sk-engines **verbatim
apart from the namespace** — that is the whole reason a port costs a Makefile block and no source
edits (see [`porting-sk-engines.md`](porting-sk-engines.md)). Those files carry comments citing
sk-engines' own design notes, and the notes were not ported with them. Rewriting the citations would
mean editing ported sources, which breaks the property that makes porting cheap, so the references
are left alone and catalogued here instead.

If you follow one of these and find nothing, that is why. The design decision each one records is
generally also stated in the code at the point of use — the citation is provenance, not the only
copy.

| Referenced | Subject | Where it is cited from |
|---|---|---|
| `docs/dev/terminal-impl.md` | the serial command channel's implementation | `src/abi_tag.h` |
| `docs/dev/terminal-dispatch.md` | `describe` and the liveness masks (`live_params` / `live_configs`) | most engine headers |
| `docs/dev/terminal-target-b.md` | declared-rather-than-parsed engine state (`engine_queries`) | `src/engine/iengine.h`, `terminal_io.h`, `tape_engine.h` |
| `docs/dev/terminal-osc.md` | the OSC form of `describe` and where `param_label` is load-bearing | `src/engine/iengine.h`, `terminal_io.h`, several engines |
| `docs/dev/indicator-grammar.md` | the LED/indicator vocabulary | `src/engine/indicators.h`, `tape_engine.cpp` |
| `docs/dev/indicator-comparison.md` | how engines' hand-rolled indicator code compared before it was unified | `src/engine/indicators.h`, `reverb`, `reso`, `pstretch` |
| `docs/dev/csound-impl.md` | the Csound engine's roadmap and build recipe | `src/engine/csound/*` |
| `docs/dev/chuck-impl.md` | the ChucK engine's roadmap (M1–M4) | `src/engine/chuck/chuck_engine.h`, `pod/Makefile.chuck` |
| `docs/dev/chuck-pod-poc.md` | the ChucK Pod proof of concept | `pod/harness_chuck.cpp` |
| `docs/dev/radio-impl.md` | the radio engine's seek/bank design | `src/engine/istreamdeck.h` |
| `docs/dev/pstretch-impl.md` | the PaulStretch port | `src/engine/pstretch/*` |
| `docs/dev/softcut-spike.md` | the softcut-lib evaluation | `src/engine/softcut/*` |
| `docs/dev/graincloud-impl.md` | the GrainflowLib variant of the granular engine | `src/engine/graincloud/thirdparty/grainflow/gfGrain.h` |
| `docs/lofi-int16-scope.md` | the 16-bit sample-storage option (`LOFI_INT16`) | `src/memory/wav.h` |
| `docs/engine-ideas.md`, `docs/item3b-plan.md` | sk-engines planning notes | ported engine comments |

`scripts/check_docs.sh` enforces this list: a reference to a document that is neither present nor
named above fails, so a NEW dead link cannot be added quietly while the inherited ones stay
documented.
