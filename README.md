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
| Window (`bright-dawn`) | Begun, tested where it can be without a screen: both screens, stacked or side by side, scaled by whole numbers or to fill the window, with sharp pixels and smooth edges (or plain bilinear, or nearest), full screen; the keyboard, any number of controllers and the mouse as the stylus; the console's frame rate kept. On the real ROM its picture matches the trace tool's pixel for pixel, and a touch passes the title screen |
| Launcher, settings book, pause menu | Begun: without a ROM on the command line the program opens its launcher (choose the ROM, play, build the game, settings, log), dressed in the game's settings menu and font from the ROM; the settings are described once and the book and the settings file are made from the descriptions; Escape pauses the game with a menu. Tried on the real ROM under a virtual display; the settings store and the font reader are unit tested |
| Textures of 3D scenes, sharp or smooth | Done, tested: nearest (as on the DS) by default; bilinear as a setting, for polygons drawn with perspective only, so that a HUD drawn in 3D keeps its texels |
| Log | Done, tested: every run writes one, with the system, the settings, what the player did and how the run ended; a crash is noted in it with where it happened, and a game that stops finishing frames |
| Interrupts in translated code | Done, tested: taken where the code polls for them (loops, coprocessor and status register writes, calls through the runtime, BIOS calls), with time passing as the code counts its instructions. The built game reaches the name entry like the interpreted one, about twice as fast |
| High-resolution renderer, audio output, rebinding, mods | Not started |

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
Third-party code in the repository: SkyEmu's ARM core (`third_party/skyemu`)
and Dear ImGui (`third_party/imgui`), which draws the program's own screens;
SDL 3 is downloaded by the build.

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
and Linux (64-bit). A package holds what a player needs and nothing else:
`bright-dawn`, this project's licence, the licences of the third-party code
built into it (`licenses/`), and `builder/`, with which the launcher builds
the game library (see below): the translator and the few headers the
translated code is compiled with. No ROM, no game data, and none of the
tests, developer documents or tools beyond the builder's.

A pre-release's description starts with the state of the project
(`.github/release-state.md`: whether the game is playable, and what does not
work yet), then lists the big changes since the last pre-release, then how to
start (`.github/prerelease-notes.md`). `tools/release/release_notes.py` writes
it from the commits since the last pre-release's tag: each `Release-Note:`
trailer of a commit is one entry, a commit without one is listed by its
subject, and one with `Release-Note: none` is left out. Every run of the
checks shows the description that `main` would get in its summary.

## Using bright-dawn

Start `bright-dawn` without anything else. The first time, and again after an
update, it opens the setup: choose your ROM (it is remembered), and build the
game from it (next section). Then it opens its launcher: Play, the settings,
building the game again, and where the log is. Once your ROM is chosen, both
look like the game's own settings menu: its wooden board, title ribbon,
parchment strips and message bar, the travel journal's parchment, and the
game's font, all read from your ROM; nothing of the game comes with Bright
Dawn. Started with a ROM, it plays it at once, built or not:

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

Escape, or a controller's guide button, pauses the game and opens its menu:
continue, the settings, back to the launcher, quit. F11 or Alt+Enter switches
full screen, Tab puts the screens side by side or one above the other, F12
switches between whole-number scaling and filling the window. The title bar
shows the frames shown per second; the console's rate is 59.83.

*Screen filtering* (Settings, Display) says how the screens are scaled to the
window. By default each DS pixel stays a crisp block, every one the same size,
with the edges between them softened over one window pixel: the look of
melonDS's display. The screen is enlarged without smoothing to the next whole
multiple of its size, then smoothed (bilinear) down to the window, both by the
graphics hardware. *Smooth* is plain bilinear, *Sharp* plain nearest. The
textures of 3D scenes are separate: sharp, as on the DS, unless *Textures in 3D
scenes* (Settings, Graphics) is set to smooth (bilinear); polygons drawn
without perspective, as a game draws its HUD in 3D, keep their texels either
way.

The settings are kept in `settings.txt` in the per-user data directory
(`%LOCALAPPDATA%\BrightDawn` on Windows, `~/.local/share/bright-dawn` on
Linux), a text file with each setting described. Every run writes a log to
`logs/` there, keeping the last ten: the system, the settings, what was done
and how the run ended, including a crash (where it happened, in the program
and in the game's code) or the game not finishing a frame for 10 seconds.
Please attach it when reporting a problem. `--log-level debug` (or Settings,
Log) records more.

Bright Dawn draws through Vulkan where the computer has it, and through the
system's own renderer (Direct3D on Windows, OpenGL elsewhere) where it does
not: *Drawing* (Settings, Display) or `--renderer system` chooses the latter.
The log says which one is used.

`bright-dawn --help` lists the options. Some change a setting for one run
(`--scale`, `--side-by-side`, `--textures smooth`, ...); `--no-settings` uses
the defaults. `--hold` and `--touch` press a button or touch the screen over
given frames, and `--frames` with `--capture` write the window's last frame to
a file: together they reproduce a run without a person at the keyboard. The
game's save is not kept between runs yet.

## Building the game from your ROM

`bright-dawn` interprets every instruction of the game. The game builder makes
the game library instead (`bright-dawn-game.dll` on Windows,
`bright-dawn-game.so` on Linux): the game's code translated to C++ and
compiled for your computer, which `bright-dawn` loads when it plays, with the
interpreter behind it for whatever was not translated. The translation is
made from your ROM, on your computer; it is the game's own code, so it is
never in this repository or in a package.

In the launcher, *Build the game* does it, and shows its steps, its progress
and its output, with a button to stop it. Nothing needs installing first:

- **The compiler** is Zig 0.16.0 (its Clang), downloaded from the Python
  Package Index for the build, its checksum checked, into a temporary folder
  that is deleted afterwards with everything else the build made. *Compiler*
  (Settings, Builder) names one of your own instead, such as `clang++` or
  `g++`.
- **Python**, which runs the builder and the translator: on Windows, if none
  is installed, Python 3.13 is downloaded from NuGet for the build (checked
  and unpacked with Windows's own `curl` and `tar`) and deleted after it.
  Elsewhere the system's Python 3.10+ is used.

The build needs about 2 GB of free space while it runs, and a few minutes on
four cores. Only the library stays, in `game` in the user data folder, with
`build.log`, everything the build printed, beside it. A library is used only
by the version of `bright-dawn` it was built for; after an update, build it
again. Settings, Game, *Play the built game* turns it off; `--interpret-only`
does the same for one run.

From a source tree or a package, the same builder runs from the command line:

```bash
python3 tools/gamebuilder/build_game.py path/to/your-rom.nds --program build/bright-dawn
```

`--program` is the `bright-dawn` the library is for: it reads the game's code
out of the ROM (`bright-dawn --extract-code ROM FOLDER`) and names the version.
`--out` chooses where the library goes, `--compiler` a compiler of your own,
`--keep-temp` keeps the temporary folder for a look inside.

The built game gets as far as the interpreted one, to the name entry, and
runs about twice as fast (a release build on four cores: 600 frames in 11
seconds instead of 24, unthrottled). See [docs/BOOT.md](docs/BOOT.md).

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

Build the repository, run `bdtool validate`, `bdtool info`, `bdtool survey` and
`bdtool boot` on your ROM and share the output, including its first line, which names the build. It
contains file names, sizes, addresses and statistics only, no game content. [docs/ROM_SURVEY.md](docs/ROM_SURVEY.md) lists the questions that
are still open.

## Repository layout

```
src/core       Error handling, bounds-checked byte access, checksums, the log
src/platform   Operating-system services: per-user data directory, frame pacing,
               the settings file, crashes noted in the log
src/rom        ROM validation, file system, archives, decompression, format detection
src/runtime    Runs translated code, with the interpreter behind it
src/nds        The console around the processor: memory, registers, interrupts, timing, BIOS calls
src/gfx        Where the screens go in a window; their pixels for the window system
src/input      Bindings of keys and controller buttons, scripted input
src/ui         Bright Dawn's settings, described; following the game builder
src/app        bright-dawn: the window (SDL 3), launcher, settings book, game screen
tools/gamebuilder  Builds bright-dawn-game from the user's ROM
tools/translator   Machine code to C++
third_party    SkyEmu's ARM core (the interpreter) and Dear ImGui, MIT licence
tools/bdtool   Command-line inspection tool
tests          Unit and end-to-end tests, with a synthetic ROM generator
tests/guest    Guest test corpus: reference results for executors of DS code
docs           Assessment, roadmap, architecture, and what has been learned about the game
```

## Licence

GPL-3.0; see [LICENSE](LICENSE). `third_party/skyemu` and `third_party/imgui`
are under the MIT licence (each with its licence alongside). *Golden Sun* is a trademark of Nintendo. This
project is not affiliated with or endorsed by Nintendo or Camelot.
