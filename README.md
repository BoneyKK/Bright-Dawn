# Bright Dawn

An effort to build a native PC version of *Golden Sun: Dark Dawn* (Nintendo DS,
2010) by static recompilation: one unified screen, high-resolution and
widescreen rendering, controller-first input, and data-driven texture, model
and music packs.

**Status: early foundation. The game does not run.** Its own start-up code
now executes, in the interpreter, on a real ROM: it sets up the machine, takes
interrupts, reads from the cartridge and from its save memory, and reaches its
frame loop, where it reads the buttons and the touch screen once a frame and
keeps running. Its screens are made, 3D and 2D, and `bright-dawn` shows them
in a window, with the keyboard, controllers and the mouse (as the stylus) as
input: the title screen can be passed. It is not playable yet: everything is
interpreted, at about a third of the console's speed, and much of the game is
untried. Its sound
commands reach a model of the sound hardware, but the music player behind
them is not there, so no sound comes out
([docs/BOOT.md](docs/BOOT.md)). See
[docs/ASSESSMENT.md](docs/ASSESSMENT.md) for what is feasible, what stands in
the way, and the roadmap, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for
the module design.

## You must supply your own ROM

This repository contains no game code, no game assets and no ROM, and it never
will. You need a ROM image dumped from a cartridge you own. Extracted or cached
game data is written to your per-user data directory, never into this
repository (`.gitignore` blocks `*.nds` as a safety net).

Recognised releases, by cartridge game code: `BO5E` (USA), `BO5P` (Europe),
`BO5J` (Japan).

## What works today

| Component | State |
| --- | --- |
| ROM validation (header checksum, completeness, game identification) | Done, tested |
| Cartridge file system reader (names, directories, file data) | Done, tested |
| ARM9 program and overlay decompression (BLZ) | Done, tested |
| LZ10 / LZ11 decompression | Done, tested |
| NARC archive reader | Done, tested |
| LZ40 decompression (the scheme this game uses for almost every file, overlays included) | Done, tested, and confirmed on the real ROM: all 15,355 LZ40 items decode, and all 366 overlays decode to the size the game expects |
| File format survey, looking inside archives and compressed files | Done, tested |
| `bdtool` command-line tool | Done, tested |
| Code translator (machine code to C++), from the owner's earlier project | In the repository and run by the build; tested on the guest corpus |
| Guest test corpus: 26 programs built natively and as ARMv5TE code, in ARM and Thumb state | Done; the native reference is pinned by checksums on every platform |
| Fallback interpreter (SkyEmu's ARM core) | In the repository; gives the reference result on the whole corpus |
| Running translated code with the interpreter behind it, translated code first | Done for the test programs: all pass. **Not yet run on the game** |
| Overlays that share an address | Done: a translation is used only while the bytes it was made from are in memory |
| Processor modes, each with its own registers | Done, tested in translated and interpreted code |
| Translating the real game | Done: main program and all 366 overlays, 17,076 functions, compile and link. **No translated game code has been executed** |
| Memory map, system coprocessor, the state a game starts in | Done, tested, with video memory in its nine banks |
| Running the game's start-up code (`bdtool boot`) | Works in the interpreter, on the real ROM: the program sets up the machine, takes interrupts, reads file tables and overlays from the cartridge, passes its check of the console, sets up its save memory and reaches its frame loop |
| Interrupts, in interpreted code | Done, tested: the display's and timers' interrupts are taken, and the processor sleeps until one comes |
| Console time, display timing, hardware timers, hardware divider and square root | Done, tested. Time per instruction is a coarse figure |
| Channel to the second processor, start-up handshake | Done, tested |
| The second processor's services | Begun: words are received as the game's own second-processor program receives them, and the cartridge service (save memory included) and one-off touch screen samples are done. Sound: next row. Clock, power: not yet |
| Sound | Begun, tested: the sound hardware (channels, mixer, capture units) and the part of the second processor's sound program that drives it directly, with its alarms. The real program's echo set-up runs on it. The music player (sequences) is not there, so nothing is heard, and there is no audio device yet |
| Buttons and lid | Done, tested: the main program's register and what the second processor passes on. Driven by the window |
| Save memory (256 KiB serial flash) | Done, tested; the real program reads it. Not yet kept in a file between runs |
| Console settings the game reads (language, owner, network address) | Done, tested. Fixed values for now; not yet a user setting |
| BIOS calls | 12 of the 17 the game has wrappers for, tested: 9 natively, 3 that wait for interrupts as this project's own code on the processor |
| DMA | Transfers that start at once, transfers fed by the cartridge and transfers timed by the display's blanks: done, tested. Transfers started by other hardware events: not yet |
| Cartridge slot (reading data) | Done, tested, and used by the real program to read its file tables and overlays |
| 3D engine | Begun, tested: the command queue, status and interrupt, the matrices and their stacks, the tests a program can ask for, vertices made into polygons (facing, cutting to the view, placing on the screen, the limits of a frame), lighting, and the drawing of a frame's polygons with textures, blending, shadows, fog, edge marking and anti-aliasing at the hardware's resolution. The real program's 3D commands run through it; `bdtool boot --picture` writes the drawn 3D layer of the last frame (the title screen) and `--wireframe` its outlines. Not compared with a console pixel by pixel; no sorting of translucent polygons; nothing puts the frame on a screen yet |
| Display | Begun, tested: both 2D engines (backgrounds of every kind, sprites, windows, effects, master brightness), the 3D picture among them, the display capture, both screens, a line at a time. On the real ROM the title screen comes out on both screens (`bdtool boot --screens`); looked at, not compared with a console pixel by pixel. Not shown in a window yet |
| Window (`bright-dawn`) | Begun, tested where it can be without a screen: both screens, stacked or side by side, scaled by whole numbers or to fill the window, full screen; the keyboard, any number of controllers and the mouse as the stylus; the console's frame rate kept. On the real ROM its picture matches the trace tool's pixel for pixel, and a touch passes the title screen |
| Interrupts in translated code | Not started |
| High-resolution renderer, audio output, rebinding, mods, settings menu | Not started |

The tests use ROM images and test programs generated by the test suite itself;
no game data is involved. The tools have also been run against a real European
copy of the game by its owner; what that showed is recorded in
[docs/ROM_SURVEY.md](docs/ROM_SURVEY.md), how the code is run is described in
[docs/FALLBACK_INTERPRETER.md](docs/FALLBACK_INTERPRETER.md), and how the game
is started in [docs/BOOT.md](docs/BOOT.md).

## Building

Requirements: CMake 3.20+ and a C++20 compiler. The window, `bright-dawn`,
uses SDL 3: the build downloads release 3.2.26 from the SDL project, checks it
against its SHA-256 and builds it in (`-DBD_SYSTEM_SDL=ON` uses an installed
SDL 3 instead; `-DBD_BUILD_APP=OFF` leaves the window out). On Linux, SDL
needs the development headers of the window systems it should support, for
example on Ubuntu `libx11-dev libxext-dev libxrandr-dev libxcursor-dev
libxi-dev libxfixes-dev libxss-dev libwayland-dev libxkbcommon-dev
wayland-protocols libegl-dev libudev-dev`; without them it builds for none
(`bright-dawn --video-drivers` lists what a build has). The tests that execute DS code
also need Python 3.10+ and Clang with its ARM backend, LLD and llvm-objcopy;
without them those tests are listed as disabled and everything else builds. Verified with GCC 13 and Clang 18
on Linux and with MSVC 2022 on Windows (CI). macOS should work but is
unverified: the CI job for it was never given a runner, so it was removed from
the matrix.
Third-party code: one header in the repository (`third_party/skyemu`), and
SDL 3, downloaded by the build.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

Developer options: `-DBD_SANITIZE=ON` (AddressSanitizer + UBSan on GCC/Clang),
`-DBD_WERROR=ON` (warnings as errors), `-DBD_BUILD_TESTS=OFF`,
`-DBD_STATIC_RUNTIME=ON` (self-contained binaries, as used for releases).

## Pre-built packages

Every change that reaches `main` with all checks passing is published
automatically as a pre-release on the repository's Releases page, for Windows
and Linux (64-bit). The packages contain `bright-dawn` and `bdtool`; they
include no ROM and no game data.

## Using bright-dawn

```bash
build/bright-dawn path/to/your-rom.nds
```

| Input | DS |
| --- | --- |
| Arrow keys | The +Control Pad |
| X, Z, S, A | A, B, X, Y |
| Q, W | L, R |
| Enter, Right Shift | Start, Select |
| Left mouse button on the touch screen | The stylus |
| A controller | By position, as on the DS: the right face button is A, the bottom one B, the top one X, the left one Y; the shoulders and triggers L and R; the left stick or the pad the directions |

F11 or Alt+Enter switches full screen, Tab puts the screens side by side or one
above the other, F12 switches between whole-number scaling and filling the
window. The title bar shows the frames shown per second; the console's rate is
59.83. `bright-dawn --help` lists the options, among them `--hold` and
`--touch`, which press a button or touch the screen over given frames, and
`--frames` with `--capture`, which write the window's last frame to a file:
together they reproduce a run without a person at the keyboard. Nothing is
saved between runs yet.

## Using bdtool

```bash
build/bdtool validate path/to/your-rom.nds
```

```bash
build/bdtool survey path/to/your-rom.nds
```

| Command | Purpose |
| --- | --- |
| `validate <rom>` | Exit status 0 only for an intact, recognised copy of the game |
| `info <rom>` | Header, program sizes, SDK version, and how the overlays are stored and whether they decode to the expected size |
| `list <rom>` | Every file in the ROM file system |
| `survey <rom>` | Classifies the contents by format, including archive members and compressed data, and summarises the overlays |
| `extract <rom> [--out DIR] [--code] [PATH...]` | Writes files to the per-user data directory; with `--code`, also the decompressed program code and a manifest of where each part loads |
| `boot <rom or code directory> [--instructions N]` | Runs the game's start-up code in the interpreter and reports what it asked of the machine. A development diagnostic; it does not run the game |

### How you can help

Run `bdtool validate`, `bdtool info`, `bdtool survey` and `bdtool boot` on your
ROM and share the output, including its first line, which names the build. It
contains file names, sizes, addresses and statistics only, no game content. [docs/ROM_SURVEY.md](docs/ROM_SURVEY.md) lists the questions that
are still open.

## Repository layout

```
src/core       Error handling, bounds-checked byte access, checksums
src/platform   Operating-system services: per-user data directory, frame pacing
src/rom        ROM validation, file system, archives, decompression, format detection
src/runtime    Runs translated code, with the interpreter behind it
src/nds        The console around the processor: memory, registers, interrupts, timing, BIOS calls
src/gfx        Where the screens go in a window; their pixels for the window system
src/input      Bindings of keys and controller buttons, scripted input
src/app        bright-dawn: the window (SDL 3)
tools/translator   Machine code to C++
third_party    SkyEmu's ARM core (the interpreter), MIT licence
tools/bdtool   Command-line inspection tool
tests          Unit and end-to-end tests, with a synthetic ROM generator
tests/guest    Guest test corpus: reference results for executors of DS code
docs           Assessment, roadmap, architecture, and what has been learned about the game
```

## Licence

GPL-3.0; see [LICENSE](LICENSE). `third_party/skyemu` is under the MIT licence
(its `LICENSE` is alongside). *Golden Sun* is a trademark of Nintendo. This
project is not affiliated with or endorsed by Nintendo or Camelot.
