# Architecture

This describes the intended module structure and records the decisions made so
far. `core`, `platform`, `rom`, the code translator and the first part of
`runtime` exist; the rest is the target that later milestones build towards. Modules are added when there is working code to put
in them, not before.

## Data flow

```
 user's ROM
     |
     v
 +-------+     +-----------------+     +------------------------------+
 |  rom  |---->|  recompiler     |---->|  game module (generated C,   |
 |       |     |  (offline tool) |     |  built on the user's machine)|
 +-------+     +-----------------+     +------------------------------+
     |                                          | calls
     v                                          v
 +--------+   +------+                 +-----------------+
 | assets |<--| mods |                 |     runtime     |  guest memory,
 +--------+   +------+                 |  (SDK replaced) |  threads, files
     |                                 +-----------------+
     |                 +----------+------------+-----------+
     |                 v          v            v
     |             +-------+  +-------+   +---------+
     +------------>|  gfx  |  | audio |   |  input  |
                   +-------+  +-------+   +---------+
                       |          |            |
                       v          v            v
                   +--------------------------------+
                   |            platform            |
                   +--------------------------------+
```

## Modules and their boundaries

| Module | Responsibility | May depend on |
| --- | --- | --- |
| `core` | Errors, byte access, checksums, the log | nothing |
| `platform` | Files, paths, windowing, devices, the settings file, crashes noted in the log | `core` |
| `rom` | The only code that reads the user's ROM: validation, file system, decompression | `core` |
| `assets` | Decodes original formats into neutral descriptions: screen-sized pictures, as the menus' backgrounds are kept (built); textures, meshes, skeletons, sequences; asset identifiers; cache in the user data directory (to come) | `core`, `rom` |
| `mods` | Manifests, load order, override resolution on top of `assets` | `core`, `assets` |
| `tools/translator` | Offline: machine code to C++ (Python). The owner's translator, with the changes listed in its README | nothing in `src` |
| `runtime` | What the translated game calls: the dispatcher and the fallback interpreter. Knows nothing of the DS beyond its processor | `third_party/skyemu` |
| `nds` | The console around the processor: memory map, system coprocessor, hardware registers, the state a game starts in (built); BIOS calls, interrupts, the second processor's services, the fixed-rate simulation loop (to come) | `core`, `rom`, `runtime`, and the *interfaces* of `gfx`, `audio`, `input` |
| `gfx` | Capture of DS-level graphics state, presentation rules, renderer backends | `core`, `assets`, `platform` |
| `audio` | Sequence playback, mixing, replacement tracks | `core`, `assets`, `platform` |
| `input` | Devices, mapping, touch replacements | `core`, `platform` |
| `ui` | What the settings book and setup screens show and keep, without drawing it: Bright Dawn's settings, each described once; following the game builder (built) | `platform`, `gfx`, `input`, `assets` |
| `app` | The window program: the setup, the launcher, the settings book and the pause menu drawn with Dear ImGui (the launcher in the look of the game's menus, from the ROM), the game's screens, the game builder run and followed (built) | everything above, SDL 3 |

Rules that keep game logic independent of presentation:

- The game module and `runtime` never include a graphics API header. They emit
  a **frame description**: for each DS screen, the 3D command stream and the 2D
  layer state for that frame. `gfx` consumes it.
- `rom` is the only module that knows the ROM is a file. Everything else asks
  for assets by identifier.
- Dependencies point downwards in the table; `core` depends on nothing.

## The boundary between translated code and the runtime

The translated game code sees the machine only through a narrow interface: a
processor state it passes to every function, memory reads and writes, a
dispatcher for calls to addresses known only at run time, and hooks for system
calls and coprocessor access. `runtime` implements that interface. This is the
seam that keeps game logic independent of everything else: nothing above it
knows about ARM, and nothing below it knows about windows or graphics APIs.

How the runtime executes translated code, including what happens where there
is no translation, is described in FALLBACK_INTERPRETER.md. In short:

- **Translated code first.** An address with a translation always runs
  natively. An interpreter runs everything else and hands back at every call
  or jump to a translated function.
- **Native frames are disposable.** Translated code keeps all state in the
  processor model and guest memory, and every call site records what it is
  waiting for. Control can therefore leave and re-enter native code by any
  route, including a switch to another stack, and the runtime picks up
  correctly.

- **Calls that never return.** The game uses `bl` as a long jump in places:
  a call whose callee goes on elsewhere and never comes back. Each such call
  leaves a call site waiting, and a native frame under it, without end; in
  a few minutes of play they reach thousands, more than the 1 MB a Windows
  program's main thread has by default. When more than
  `Machine::kDeepestNesting` (1024) call sites wait above the outermost run,
  the runtime lets go of all of them as for a jump out, and execution goes on
  at the address it was going to, in the run the letting go ends in (the
  outermost, or one at its depth that a computed jump began). A call site let go of that is
  returned to after all is met as a return elsewhere, and execution goes on
  there, so nothing the game does changes. The program's main thread also
  has 16 MB of stack on Windows, as on Linux.

- **A translation stands for particular bytes.** Several overlays are
  translated for the same address. The runtime uses an overlay's translation
  only while the bytes it was made from are in guest memory, so it follows
  whatever the game's own loader does without knowing anything about it.

- **Interrupts.** Translated code cannot be interrupted at an arbitrary
  instruction: it does not keep the program counter. It polls instead, where
  the translator knows the address of the next instruction and the whole
  state is in the processor model: before each backward branch (every loop),
  after writes to the system coprocessor (which can wait for an interrupt)
  and to the status register (which can unmask one). The runtime also takes
  one before a call or jump it resolves, and after a BIOS call. The poll is
  one load and compare (`ds::poll`) while nothing is requested. Taking it,
  the runtime enters IRQ mode and runs the handler, interpreted or
  translated, as if a call site waited for its return to the interrupted
  instruction; a handler that switches threads goes elsewhere, and the
  waiting native frames are given up as for any other jump out.
- **Time.** Translated code counts the instructions it runs (`c.executed`,
  added at each place code is entered: the start of a function and each
  branch target), and the runtime turns them into console time at its next
  request: a memory access, a call it resolves, a BIOS call. A BIOS call that
  runs as code (the ones that wait for an interrupt) is run until it returns,
  from translated code as from interpreted.

## How the three headline features attach

**Single screen.** The frame description keeps the two DS screens separate and
labelled. A presentation stage inside `gfx` applies a *layout profile* chosen by
the current game situation (field, battle, menu, dialogue, two-screen
cutscene). A profile says which screen fills the viewport and what becomes of
the other one: an overlay, a panel opened on demand, or a natively drawn
replacement. Profiles are data plus a small amount of code; the game logic is
not modified to know about them. Identifying the current situation requires
per-revision knowledge of the game's state, supplied as symbol data.

**Resolution and widescreen.** 3D geometry reaches `gfx` as commands, not
pixels, so it can be rasterised at any resolution. Widescreen replaces the
projection at the point where the game loads it and widens the viewport; places
where the game culls against the original frustum are patched individually. 2D
elements are positioned through anchors (left, centre, right, stretch-safe
area) rather than raw 256x192 coordinates. Fixed-layout 2D scenes keep their
composition and are bordered or extended, never stretched.

**Graphics compatibility layer.** Each DS feature with fixed-resolution or
hardware-specific behaviour (display capture, windows, blending, brightness,
mosaic, edge marking, fog) is handled by a named effect in `gfx` with its own
implementation and a status entry in a catalogue, so one effect can be
reworked without touching the others or the game.

**Mods.** `assets` gives every original asset a stable identifier (ROM path,
index within its container, and a content hash). `mods` is an ordered stack of
packs that may supply a replacement for an identifier; lookup falls through to
the original. Packs are directories with a versioned manifest; they contain
data only, never executable code. Textures are images, models are glTF 2.0
with a mapping file for skeleton differences, music is audio files with loop
metadata.

## Decisions

| Decision | Reason |
| --- | --- |
| C++20, CMake | The recompiler's output is C; C++ is the common language for the surrounding runtime and for the graphics, audio and windowing libraries likely to be used. |
| Third-party code kept to a minimum | SkyEmu's `arm7.h` (MIT) is the fallback interpreter, chosen after comparing six candidates on licence and fit and then proving it on the test corpus. Each later dependency is assessed the same way. SDL 3 (zlib licence) is the window, input and, later, sound layer: a pinned release, downloaded and checked against its hash by the build. Only the window program (`src/app`) uses it; `gfx` and `input` hold the rules that need no window, and are tested without one. |
| The interpreter sits behind a C interface of its own | Nothing but one wrapper file includes the third-party header, so the interpreter can be replaced without touching the runtime. |
| The translator stays in Python | It is the owner's tested code and runs once, offline. Rewriting it would discard its test history for no gain. |
| Errors as values (`Result<T>`) in parsers | Input is untrusted and failures are expected; callers must handle them. |
| Every read of ROM data is bounds-checked | A malformed or malicious file must produce an error message, not a crash. Enforced by the damage tests under AddressSanitizer. |
| In-house test harness | A few dozen lines; avoids a dependency this early. |
| Extracted data goes to the user data directory | Keeps game data out of the repository and out of the install directory. |
| Screens: sharp pixels with smooth edges; textures of 3D scenes smoothed only if asked | Nearest scaling makes some DS pixels a window pixel wider than others at sizes that are not whole numbers; bilinear scaling blurs the HUD and text. By default each screen is enlarged nearest to the next whole multiple of its size and that is scaled bilinear to the window (gfx::ScreenFilter::SharpSmoothEdges): crisp blocks of one size, with their edges mixed over one window pixel, the look of melonDS's display, which enlarges its screens to a whole-number internal resolution and scales them to the window with linear filtering. Two draws per screen, both on the graphics hardware. Plain bilinear and nearest remain choices. Smoothing textures (bilinear) is a separate choice for the 3D scenes and is done in the 3D renderer, before the screens are put together, for polygons drawn with perspective only: a polygon whose w does not depend on where its corners are was drawn with an orthographic projection, which is how a game draws a HUD in 3D. Textures whose texels are there or not keep hard outlines (a pixel is drawn where at least half of what it mixes is there). |
| The settings are described once | Each setting (key, chapter, label, help, kind, values, default) is described in `ui/game_settings.cpp`. The settings book, the settings file (a commented text file a person can edit) and the checks on values are all made from the descriptions, so a new setting needs no screen work. The command line changes settings for one run only, without saving them. |
| Dear ImGui for the program's own screens | MIT licence, kept in the repository like SkyEmu's core, with its SDL 3 backends: widgets, keyboard and controller navigation and text input that work everywhere SDL does, drawn with SDL's renderer. The look of the book is a matter of style and drawing on top of it; the settings and the builder are kept apart from it (`ui`), so the screens can be redrawn without touching them. |
| Every run writes a log | A player's problem has to be traceable afterwards: a log in the user data directory, a line per event with wall-clock time and time since start, written out at once so that it ends with what happened last even when the program crashes, which is noted in it. The last ten runs are kept. The builder writes everything it and its programs print to `build.log`. |
| The game builder runs on the player's computer, from the package | The translated code is the game's own, so it is made where the ROM is. The package carries the translator and the headers the translated code is compiled with (`builder/`, no game data); `bright-dawn --extract-code` reads the code out of the ROM. Nothing has to be installed: the compiler (Zig 0.16.0's Clang, from the Python Package Index) and, on Windows without Python, Python 3.13 (from NuGet) are downloaded for the build at pinned versions, their SHA-256 checked, into a temporary folder deleted afterwards with all the build made. The launcher follows its steps and output; closing its standard input stops it and every program it started. |
| The game is built once before the launcher opens | The setup asks for the ROM and builds the game library for this version, and only then opens the launcher; after an update it asks again. It is in the program's own look: nothing of the game is shown before the game is built. A ROM given on the command line still plays at once, interpreted if not built, for development. |
| The launcher looks like the game's own menus, from the player's ROM | Bright Dawn carries nothing of the game: the page of the list of saves (the first page, with Play and what the save holds, `ui/save_summary.h`), the settings menu's background, parchment strips and title ribbon, the travel journal's parchment and the menus' font (NFTR, `assets/font.h`) are decoded from the ROM when it is chosen and held in memory. The pictures are enlarged by whole numbers and drawn with linear filtering, as the screens are; the launcher's window has their shape (4 by 3, a whole multiple of 256 by 192 that fits the display) and is not resized. The font is drawn at whole multiples of its pixels, outlined in black on wood and plain on parchment; its full-width punctuation is set close for Latin text. Buttons and the message bar take the menus' colours. Without a ROM the launcher keeps ImGui's own look. |
| Vulkan first, the system's renderer after | SDL's GPU renderer on a Vulkan device draws the window where the computer has Vulkan (else SDL's Vulkan renderer); where neither can be made (no driver, no device), the window is made again for the system's own renderer (Direct3D 11 on Windows), and the log says so. |
| 3D scenes on the graphics hardware, for speed rather than exactness | Where the window is drawn through SDL's GPU API, the game's 3D is drawn by the graphics hardware on the same Vulkan device, at a whole number of times the DS's resolution (`app/hardware_3d.h`). The program still turns each frame's polygons into triangles where it would draw them, and decodes each texture once by its contents (`nds/hardware_scene.h`): a tenth of the processor time of drawing them. The main engine's screen is put together without the 3D layer, each pixel where the 3D layer would be on top marked with how it would be mixed there; the graphics hardware draws the scene with a depth buffer (and a stencil buffer for the rule that a translucent polygon is not drawn twice over itself, and for shadow polygons: a run of masks marks where it is hidden, a shadow unmarks the pixels of its own polygon number and is drawn where marked, as the processor draws them) and puts it in where marked. Edge marking is drawn by the same rule as the processor's (`nds::PolygonRenderer::is_edge`): the scene pass also writes each opaque pixel's polygon number and depth, and a pixel with a pixel of another number further away, within the width chosen, takes its number's edge colour. Fog, anti-aliasing and polygons drawn as outlines only are left out. A display capture is made by the graphics hardware too, at its resolution, whatever it takes (the main engine's picture or the 3D alone, a bank, or two blended, as 3D on both screens and the trail behind what moves in battles use) and wherever it goes in its bank: the capture in memory holds only the 2D layers (where the 3D goes, the lowest bit of each colour part tells it from the captures before), the graphics hardware keeps its own copy at the higher resolution, laid out as its bank, a capture that reads a bank holding one of these reads that copy, and each screen is marked where it shows a capture completed in an earlier frame (`nds/display.h`). The program's own drawing stays where the graphics hardware cannot be used, and when chosen; the frames it draws then are counted, by reason, at the end of the not-native log. |
| A crash or a hang is told in the log with where it happened | A crash's line names the signal or exception, the address read or written, the module and offset in the program, and the address in the game's code last entered; a stack overflow is caught on a stack kept for it. A watch thread notes when the game has not finished a frame for 10 seconds, and when it goes on. |
| The translated game is a library the program loads | One program for players, whether built or not, and no compiler of the program's own needed: the library (`bright-dawn-game.dll`/`.so`) may be compiled by another compiler than the program, so what passes between them is plain C (`src/runtime/game_library.h`): the program hands over the functions translated code needs, the library answers with its table and what it was built for. Sizes, an offset and the version are checked before any of it is used; a library built for another version is not. It exports one symbol. |
| The game's logic steps at the original rate; what is shown follows real time | The game's code, translated or interpreted, advances one fixed step per console frame (about 59.83 a second) and counts frames for its timers, physics and animations: feeding it variable time would mean rewriting each of its systems and would change how it plays. Presentation is decoupled instead (`platform/frame_clock.h`): real elapsed time (the delta) goes into an account, a console frame is run for each whole step in it, and pictures are shown at the screen's own refresh, with the fraction of the next step that has passed. That fraction is what a renderer needs to interpolate between a scene's last two states: the way to show the 3D of both screens, which the DS draws for one screen a frame (30 a second each), 60 or more times a second. |
