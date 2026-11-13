# Technical assessment and roadmap

Written at the start of the project. Statements about the game's internals are
marked **unverified** where they come from general knowledge rather than from
inspecting a ROM; none could be checked at the time, because no ROM is
available in the development environment. Facts since established from a real
ROM are recorded in [ROM_SURVEY.md](ROM_SURVEY.md), which takes precedence over
the guesses here.

## 1. What the repository contained

A one-line `README.md` and the GPL-3.0 licence text. No source code, no build
system, no tools, no documentation, no decompiled game code, no emulator. There
was nothing to reuse, modify or preserve; everything has to be developed.

The licence matters for dependencies: anything linked in must be
GPL-3.0-compatible.

## 2. Is a genuine recompilation possible?

A recompilation needs one of two starting points.

**A decompilation** (human-readable source reconstructed from the game). A
search found no public decompilation of this game. Decompilations of other DS
games exist and the tooling for them is maturing (the `dsd` toolkit, used for
example by a Dragon Quest IX project), but producing one for a large RPG is
years of work for a team. Not a realistic starting point.

**A static recompiler** (a tool that translates the game's machine code into C
ahead of time, which is then compiled natively and linked with a runtime that
replaces the console). This is how the N64 recompilation projects work. A search
found no existing static recompiler for DS games, so it has to be written. That
is feasible: the DS main processor is an ARM946E-S (ARMv5TE, ARM and Thumb
instruction sets), a well-documented target.

**Conclusion:** static recompilation is the viable route and is what this
project will pursue. It is a genuine recompilation: the game's logic runs as
native code. It is also a large undertaking; section 5 is honest about that.

**Update.** The project owner's earlier project already contained such a
translator for this game: 14,354 functions translated to C++ that compiles and
links, tested instruction by instruction against reference emulators. It is
now in this repository and its output has been executed on test programs.
Section 5 describes it and what it still lacks.

The recompiled C code is derived from the game and is therefore produced on the
user's machine from the user's ROM. It is never committed to this repository.
Whether binary releases may contain it is a legal decision for the project
owner, not something this document can settle.

## 3. What can be reused, modified, developed

| | |
| --- | --- |
| Reused | Nothing from the repository. Public format documentation (GBATEK and community notes) is the reference for clean-room implementations. |
| Modified | Nothing. |
| Developed | Everything: ROM layer (done), code analysis, recompiler, runtime, renderer, audio, input, asset decoding, mod system, settings UI. |

## 4. Technical obstacles

Ordered by how likely each is to block the intended result.

1. **No ROM in the development environment.** Every game-specific fact has to
   be established by the user running tools and reporting results, or by a
   contributor with a ROM. This slows iteration and is why the first tool built
   is a survey tool.
2. **Overlays.** DS games load code on demand, and several overlays can occupy
   the same address range. A static recompiler must translate each overlay
   separately and dispatch indirect calls according to which overlay is
   currently loaded. Now measured: 366 overlays at 14 load addresses, 297 of
   them sharing a single address (see ROM_SURVEY.md). Overlay handling is
   therefore central to the recompiler, not an edge case.
3. **Indirect control flow.** Jump tables, function pointers and ARM/Thumb
   mode switches must all be resolved or handled by a runtime lookup. Code
   discovery that misses a function produces a runtime failure, not a build
   failure, so coverage has to be measured.
4. **The console underneath the game.** The recompiled code expects the
   Nintendo SDK and DS hardware. The plan is to replace the SDK at function
   level where functions can be identified (threads, file access, sound,
   input) and to capture graphics at the level of 3D command streams and 2D
   layer state. The second processor (ARM7) runs stock SDK firmware and is
   replaced entirely rather than recompiled. Function identification depends on
   the SDK version, which `bdtool info` reports.
5. **Single-screen presentation needs knowledge of the game, not just the
   hardware.** Deciding what to do with each screen in each situation (field,
   battle, menus, dialogue, cutscenes that span both screens) requires
   reverse-engineering the game's state machine. This cannot be done
   generically and is the largest game-specific design task.
6. **One 3D engine, two screens.** The DS can render 3D to only one screen per
   frame; games that show 3D on both alternate frames using display capture. If
   this game does so anywhere (**unverified**), those scenes need dedicated
   handling in the unified viewport.
7. **Widescreen.** Changing the projection is easy; the game's own visibility
   culling, 2D elements positioned for a 256x192 screen, and scripted camera
   framing are not. Each needs targeted patches or presentation rules.
8. **Hardware-specific effects.** Display capture, 2D windows and blending,
   brightness fades, mosaic, edge marking, fog and translucency rules all have
   fixed-resolution or DS-specific semantics. They need a catalogue and
   per-effect replacements.
9. **Asset formats.** Texture and model replacement require decoding the
   originals. Now measured: the game uses the standard Nintendo sound archive
   and the standard 3D model, texture and animation formats by name, mostly
   wrapped in its own compression, alongside several formats of its own that
   must be reverse-engineered (see ROM_SURVEY.md). Model replacement with different skeletons needs an
   animation retargeting pipeline on top.
10. **Timing.** DS game logic is normally tied to the 60 Hz display refresh.
    The simulation will keep its original tick rate; rendering above it would
    need interpolation, which is a later, optional step.
11. **Copy protection.** Late DS releases commonly contain anti-piracy checks
    (**unverified** for this game). A recompilation that models the cartridge
    faithfully should pass them, but they can cost debugging time.

## 5. The translator, and what it lacked

The translator from the owner's earlier project is now in this repository
(`tools/translator`). It finds functions by following the code from its entry
points, then turns each ARM or Thumb instruction into C++ statements that act
on a processor model (16 registers, the flags, and memory access through
callbacks the runtime supplies). A call to a known function becomes a C++
call; a call or jump to an address not known until run time goes through the
runtime.

Its own documentation listed the gaps below. The first four are now dealt
with for the test corpus; the rest are the work ahead.

| Gap | Status |
| --- | --- |
| Calls and returns are native C++ calls and returns, so code that resumes somewhere other than its caller breaks | **Solved.** Call sites are recorded and the runtime re-synchronises whenever control reaches one, by any route. Covers returns through registers, old-style calls, jumps out of nested functions, and stack switches. See FALLBACK_INTERPRETER.md |
| Functions reached only through pointers may be missed; some jump tables are not recognised | **Solved by the fallback interpreter**, which runs what has no translation and reports each address |
| A software interrupt overwrote the return register | **Fixed** in the translator |
| A Thumb-state computed jump lost its instruction set | **Fixed** in the translator |
| The dispatcher cannot tell which overlay is loaded | Open. 297 overlays share one address. Plan: decide by content, comparing guest memory with the overlay images |
| Nothing delivers interrupts | Open. The game's frame logic is driven by the vertical-blank interrupt |
| The processor model has one set of registers, not one per processor mode | Open. Matters for the start-up code and interrupt handling |
| System calls, coprocessor access, hardware registers | Open. The runtime has the entry points; nothing answers them yet |
| ARM7 program | Open. Plan: do not run it; answer the ARM9 side's requests natively |
| Names are addresses | Open. Plan: identify SDK functions by signature against SDK 5.4 |

## 6. Roadmap

Sizing is deliberately frank: the complete vision is a multi-year effort of the
kind normally carried by a team. Milestones are ordered so that each leaves
something usable and testable, and so that the riskiest unknowns are resolved
first. Nothing can be rendered on one screen until code runs, so the visible
features come after the runtime.

One constraint governs the pace: the development environment has no ROM. Every
step that touches the real game is run by the owner and reported back.

| # | Milestone | Done when |
| --- | --- | --- |
| 0 | **ROM ingestion and survey** (done) | Validation, file system, decompression and survey tool pass their tests |
| 1 | **Survey of the real game** (done) | Program code and the standard asset formats are accounted for on the real ROM; see ROM_SURVEY.md for the minor questions left |
| 2 | **Bring in the translator** (done for test programs; whole-game driver pending) | Its source is in this repository; its output is compared with the guest test corpus in CI; a driver translates the whole game from `bdtool extract --code` output |
| 3 | **Runtime core** (dispatcher and interpreter done; memory map and overlays pending) | The guest test corpus gives the reference results under translated code, under the interpreter, and under the two together (done). Guest memory map and overlay selection by content (pending) |
| 4 | **Boot without graphics** | With SDK services answered natively (threads, files, cartridge, timers, the ARM7's duties), the game's start-up runs on the owner's machine and its graphics commands are captured to a log |
| 5 | **Platform shell and reference renderer** | Window, input and audio device via SDL; captured frames drawn correctly at native resolution (accuracy first) |
| 6 | **Unified screen, high resolution, widescreen** | Per-situation layout rules replace the two-screen output; 3D renders at selectable resolution with corrected projection; effects catalogue started |
| 7 | **Audio** | Original music and sound effects play; music and effects volumes are separate |
| 8 | **Controllers** | Hot-plug, remapping, dead zones, button prompts, and replacements for every touch interaction |
| 9 | **Mod framework** | Manifests, load order, fallbacks; texture packs first, then music, then models |
| 10 | **Book-style settings menu** | Built from fonts and UI graphics read from the user's ROM |
| 11 | **Optimisation and release engineering** | Performance, crash reporting, save management, packaging |

Milestone 4 now carries the most technical risk. Translated code has been
executed on test programs, including calls between functions, the dispatcher
and stack switches, but none of the game's own code has run. The first
attempt is where the SDK's start-up, thread and interrupt code meets the
runtime.

## 7. Milestone 0, delivered

- CMake build with no third-party dependencies; warnings-as-errors and
  sanitizer configurations.
- `src/rom`: header parsing and checksum, completeness checks, game
  identification, file system, overlay tables, ARM9 start-up parameter
  discovery, BLZ/LZ10/LZ11 decompression, format detection.
- `bdtool`: `validate`, `info`, `list`, `survey`, `extract`.
- Tests: codec round trips, malformed-input rejection, a randomised
  structural-damage test, and end-to-end runs of the tool, all against
  generated ROM images.

Known limitations:

- Run three times against a real ROM. The first run exposed a bug in locating
  the ARM9 start-up parameters and the second showed the LZ40 decoder failing
  on a quarter of its input; both are fixed and the third run confirms it.
  Details are in ROM_SURVEY.md.
- Game identification uses the cartridge game code only. A table of known-good
  whole-image checksums would detect modified dumps; it is omitted until the
  values can be confirmed against real dumps.
- The whole ROM is read into memory (up to a few hundred MiB). Acceptable for a
  tool; the runtime may move to memory mapping.
- A ROM whose secure area is still encrypted is detected and reported but not
  decrypted.
