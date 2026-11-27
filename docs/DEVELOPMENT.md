# Development

How Bright Dawn is built, packaged and released, and how its tools are used.
Players do not need any of this: they download a pre-release (see the
[README](../README.md)).

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
update, it opens the setup, in the program's own look: choose your ROM (it is
remembered), and build the game from it (next section), following the build's
log. Then it opens its launcher, in a 4 by 3 window that is not resized: its
first page has only Play, and below it what your save holds (name, level,
time played, coins), on the page of the game's own list of saves. The gear
in its lower right corner, or Escape, opens the other pages: the settings, building the
game again, and where the log is, on the game's settings menu (its wooden
board, title ribbon, parchment strips and message bar, the travel journal's
parchment). The pictures and the game's font are read from your ROM; nothing
of the game comes with Bright Dawn. Started with a ROM, it plays it at once,
built or not:

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
| A controller | By position, as on the DS: the right face button is A, the bottom one B, the top one X, the left one Y; the shoulders and triggers L and R; the pad the directions, and the left stick too (on the field it walks, see below) |

These are the defaults. Settings, Controls binds another key or controller
button to each DS button: click the binding, then press the key or button.

*Left stick: analog walking* (Settings, Controls; on by default): on the
field, the left stick walks in any direction, from the game's walk to its
run by how far it is pushed (a dead zone of 20%, the run at 90%). It uses the
game's own walk to a point, the walk a touch leaves behind
(`src/game/stick_walk.h`). Where the game's field control does not run (menus,
text boxes, events, battles) the stick is the d-pad. The d-pad, keys and the
stylus win over it; B with it runs. Its limits: the slowest pace is the
game's walk; another button held (A, for example) stops the walk while it is held,
as in the game; the European release (BO5P, version 0) only, with
others the stick is the d-pad. `--stick X,Y@F[-L]` pushes it in a scripted
run (percent, -100 to 100).

Escape, or a controller's guide button, pauses the game and opens its menu:
continue, the settings, report a problem, back to the launcher, quit. F11 or Alt+Enter switches
full screen, Tab puts the screens side by side or one above the other, F9
shows the touch screen alone or goes back (a binding of Settings, Controls), F12
switches between whole-number scaling and filling the window. The title bar
shows the game's frames a second (the console's rate is 59.83) and the
pictures shown a second.

*Screen filtering* (Settings, Display) says how the screens are scaled to the
window. By default each DS pixel stays a crisp block, every one the same size,
with the edges between them softened over one window pixel: the look of
melonDS's display. The screen is enlarged without smoothing to the next whole
multiple of its size, then smoothed (bilinear) down to the window, both by the
graphics hardware. A picture of the graphics hardware made at more pixels than
the window has for it (8x in a 1080p window) is made smaller by averaging the
pixels each window pixel covers, for smooth edges without stairs.
*Smooth* is plain bilinear, *Sharp* plain nearest. The
textures of 3D scenes are separate: sharp, as on the DS, unless *Textures in 3D
scenes* (Settings, Graphics) is set to smooth (bilinear), with the processor
or the graphics hardware (four texels mixed, each texture wrapped as it asks
and never mixed with its neighbours in the atlas); polygons drawn without
perspective, as a game draws its HUD in 3D, keep their texels either way.

The settings are kept in `settings.txt` in the per-user data directory
(`%LOCALAPPDATA%\BrightDawn` on Windows, `~/.local/share/bright-dawn` on
Linux), a text file with each setting described. Every run writes a log to
`logs/` there, keeping the last ten: the system, the settings, what was done
and how the run ended, including a crash (where it happened, in the program
and in the game's code) or the game not finishing a frame for 10 seconds.
With the built game, `logs/not-native-` and the same time lists where the
game's code ran in the interpreter instead of natively, the most
instructions first, with the instruction and why (no translated code starts
there, or its overlay was not in memory): the places still to work on. At
its end it counts the frames whose 3D the graphics hardware drew and the
frames the processor drew instead, with the reason. It is
written about once a minute and when the game ends.
Please attach it when reporting a problem: *Report a problem* (the
launcher's Log page, or the pause menu) opens the bug form on GitHub with the
version, system, renderer, game code and built game or interpreter filled in
(`src/ui/problem_report.h`), and the logs folder. `--log-level debug` (or Settings,
Log) records more.

To find out what makes a frame slow, switch on the *Frame log* on the
launcher's Debug page (or start with `--frame-log`). From the next start of
the game, `bright-dawn-frames.csv` next to the program has a line for each
frame: the time of each part in microseconds (the game's code, the geometry
engine, making the scene, the 2D engines, each upload and pass for the
graphics hardware, the screen filter, present and the waits), which add up to
the frame's time; the polygons, textures and bytes sent; and what the frame
had (3D on which screen, a display capture, outlines, edge smoothing, fog,
the 3D resolution). Its first line names the graphics hardware and its
driver, the window, the 3D resolution, the screen filter and the mode; its
end, the average and the 95th percentile of each column, and the costliest
one. In the normal mode the times are the processor's own: the graphics
hardware's work is waited for where the program waits for it (mostly at
present). *Sync mode* (or `--frame-log-sync`) waits for the graphics hardware
after each of its passes, so that each has its real time, and the game runs
slower. The Debug page opens the folder the file is in
(`src/debug/frame_log.h`).

The Debug page has more tools, all off by default; each starts with the
next game. Its hotkeys are bound on the Controls page (keyboard defaults in
brackets) and work only while their setting is on.

- *Pause and step*: Pause (F5) holds the game after the frame being run;
  Step (F6) runs exactly one DS frame and holds again; Resume (F7) runs on.
  While held, nothing of the emulated machine runs (game code, timing,
  sound, input); only the window is redrawn, so the menu, the overlay and
  the capture hotkey still work (`src/debug/frame_stepper.h`).
  `--pause-at N` holds after frame N; with `--step-count M` it then takes M
  single steps by itself, each shown (and written with `--capture-every 1`),
  and runs on. Without `--step-count` the hotkeys go on from there.
- *Frame overlay*: a few lines at the window's top-left corner, drawn after
  the screens and the `--capture` pictures, never into the game's screens:
  the frame number, the host's time for the last frame, its three costliest
  frame-log parts in ms and the GPU's name (F3 shows or hides it;
  `src/app/frame_overlay.h`).
- *Capture this frame* (F8): writes the picture shown and that frame's
  frame-log row to `captures/frame-N.bmp` and `frame-N.csv` in the user data
  folder (never next to the program or in the repository).
- *Performance report* (or `--perf-report FILE`): at the end, one JSON file,
  `bright-dawn-performance.json` next to the program, that Claude can read
  (`src/debug/perf_report.h`). It runs the frame log's timing without
  writing the CSV; with `--frame-log-sync` (sync mode) the GPU's passes are
  timed too.
- *GPU workarounds* (on by default): see the known GPU issues below. Off,
  none is applied, to check whether one is still needed.

The overlay, the capture hotkey and the report time the frame as the frame
log does: with one of them on, the frame waits for the swapchain before it
is presented, as with the frame log. With all of them off, the game runs as
before, the same pictures and the same pacing.

The report starts with `"summary"`: a few lines of text, then the verdict.
The sections that follow are `system` (the OS and its version, the processor,
its cores and threads, the memory, the build type and commit), `gpu` (the SDL
GPU backend, the GPU's name, vendor and device IDs, integrated or discrete,
the driver's name and information, its version decoded by vendor (NVIDIA's
10.8.8.6 bits, Intel's 18.14 on Windows, else major.minor.patch, as Mesa and
AMD) and the Vulkan API version, the device-local memory, what SDL reports,
and the fallbacks taken at start with why; on Direct3D 12 and Metal, what SDL
reports only), `known_gpu_issues` (the entries matched and the workarounds
applied), `settings` (scale, 3D resolution, widescreen and its HUD, which
renderer draws the 3D and each screen's 2D, filter, textures, window size,
full screen, vsync and the present mode), then `timing`: for `total` and each
frame-log column the mean, median, 95th and 99th percentile and maximum in ms
and its share of the host's time; the 20 slowest frames with their 3D
screens, renderer path and polygon and vertex counts; the spikes (runs of
frames above 1.5 times the median) with what changed as each began (a
renderer path switch, a new scene, new textures in the atlas, new render
targets, a capture) and the column that grew most; and `gpu_problems`:
device lost, GPU failures, fallbacks and known issues matched, with their
frame. The verdict names the
column with the largest share and the one with the worst 99th percentile,
and says whether the time is the processor's or a wait on the graphics
hardware (or the display, with vsync). The report holds no ROM data, no
pictures, no game text, no user or host name and no path.

To send a report to Claude: switch on *Performance report* (and, for the
GPU's own times, *Sync mode* with the frame log), play the part that is
slow for a minute, close the game, and paste the file (or attach it to an
issue). Say what was on screen.

Known GPU issues (`src/debug/gpu_info.cpp`, `known_gpu_issues()`) is a table
of data: a vendor ID, a device ID range, a part of the driver's name, the OS,
a driver version range (compared as decoded), the problem, and the
workarounds (the processor draws the 2D, or the 3D; vsync on). At start the
running GPU and driver are matched against it; each match is written to the
log and the report, and its workarounds are applied unless *GPU workarounds*
is off. An entry is added only when a report proves the issue; the table is
empty for now.

Bright Dawn draws through Vulkan where the computer has it, and through the
system's own renderer (Direct3D on Windows, OpenGL elsewhere) where it does
not: *Drawing* (Settings, Display) or `--renderer system` chooses the latter.
The log says which one is used. With Vulkan, the graphics hardware also draws
the 3D scenes, at *3D resolution* (Settings, Graphics; `--3d-resolution`)
times the DS's, by default the least that fills the screen as shown; *3D
drawing* or `--3d processor` has the processor draw them as the DS does,
256 by 192, and it does where the graphics hardware cannot. The graphics
hardware then draws the two 2D engines' pictures too, as the processor would,
except in frames where a display capture needs the processor's; *2D by the
processor* (the launcher's Debug page) or `--processor-2d` leaves them to the
processor, and `--check-2d` has the processor draw them as well and logs the
frames whose pictures differ. Above 1x,
*High-resolution coordinates* (Settings, Graphics; on by default) places
vertices between the DS's pixels, as the geometry engine computes them, so
that slowly moving models do not jump from pixel to pixel; polygons drawn
without perspective (message boxes, menus) stay at whole DS pixels, so that
their parts still meet. *Outlines*
(Settings, Graphics; `--edge-marking`) sets the outlines the game asks for
around 3D models (edge marking), in the game's colours: *Original
thickness* (`original`, the default) keeps them one DS pixel wide, so N
pixels wide at N times the resolution; *1 pixel* (`pixel`) makes them one
pixel wide at any resolution; *Off* (`off`) leaves them out. Above 1x they
are drawn from the triangles' own pixels, as wide on every side and soft on
their inner side, and the game's smoothing of polygon edges (anti-aliasing)
is left out. *Widescreen (3D)* (Settings, Graphics; off by default;
`--widescreen`) shows the screen with the 3D scene 16:9 with the graphics
hardware: the scene goes on a third further on each side, the 2D picture
stays 256 by 192 in the middle, and the other screen stays 4:3; the stylus
touches the middle (and the HUD's moved icons, below). Where the processor draws the 2D (battles), the
2D goes on at the sides too: a battle's backdrop shows more of its picture
(the game keeps all 512 columns of it in main memory and puts 256 of them in
video memory as it scrolls; `DisplayEngine::streamed_source` finds it there),
another background that scrolls is mirrored there, a fixed one (a text box)
takes its edge pixel, and a flat 3D band across the whole screen (the battle
log) is made longer, its texture not stretched.
Where the graphics hardware draws the 2D (the field, the menus), a second
pass of `engine2d.frag` draws the main engine's sides: on each line and
side, the lowest background under the 3D (without 3D, under the top
priority in use) that has a pixel at the screen's edge goes on past the 256
columns. A tiled one goes on at the same scroll, a repeating affine one
through its matrix; a 512-wide map at scroll 0, or an affine one that does
not repeat, is mirrored. With *Widescreen HUD: Wide* (Settings, Graphics,
shown with widescreen; the default) the HUD goes to the wide screen's edges
(see the list below); with *Centred* (`centred`) it stays in the middle, as
the DS has it. Under a
menu the game shows a display capture of the field as a bitmap (background
2), changed in its bank (blurred and lightened); the sides show the kept
capture's own sides, changed to match (see the list below). The DS's box
test (which the field uses to leave out models out of view) finds boxes by
a view a little wider than the wide picture, so those models are drawn at
the sides, and are ready before they come in. Its limits: models a game
leaves out by a test of its own can still appear at the sides late; 3D on
both screens at once (the title) stays 4:3; at 1x the polygon edges are not
smoothed.

Rendering functions of our own: where the DS's way cannot give a higher
resolution, the wide view or smoothing, the program has its own version.
Each is used only while the setting that needs it is on; the DS's way stays
as the reference, and at 1x with widescreen off and sharp textures the
picture is the DS's, pixel for pixel (`bd_gpu_tests` compares them).

- *Wide 2D sides* (`engine2d.frag`, side mode; `DisplayEngine::side_backgrounds`):
  for the DS a 2D layer ends at 256 columns. For widescreen. It differs from
  the DS in that only one background (and the 3D) is shown at the sides.
- *Capture sides under a menu* (`Display::capture_at_sides`,
  `Display::blur_between`, `compose.frag`): a DS capture holds 256 by 192
  pixels only. For widescreen: the kept capture is wide, and the game's
  change to it is measured from the bank (a box blur of radius 0 to 3 and
  a change of each colour part), not taken from its code. On the cave save
  the fit is within 1/64 of each part.
- *Box test by a wider view* (`GeometryEngine::kWideBoxTimes`): the DS
  tests boxes against its 4:3 view. For widescreen: x within 3/2 of w (the
  wide picture shows 4/3). The game draws more models than it would.
- *Widescreen HUD* (`nds/wide_hud`; `engine2d.frag` and
  `DisplayEngine::draw_tiles_moved` for the 2D): the game places its HUD on
  the DS's 256 columns. For *Widescreen HUD: Wide*: each frame the HUD's
  elements are found by what they are on the screen (their place, size and
  layer: the flat 3D polygons of the menus' frames, sprites, the text
  background's tiles), and drawn at the wide edges by the side's width (43
  DS pixels at 1x and 2x, 42 above; `Hardware3D::side_shift`). Boxes and
  icons move (the coins, a shop's price and item boxes, the field's L, R, Y
  and X icons, the party's numbers, the battle log's text); bars are made
  longer, their middle going on with its texture at the same rate and their
  ends moved (the help box and its B button, the ring of commands, whose
  icons past the DS's view come from the flat polygons the geometry engine
  keeps for it, `Polygon::beyond`). The text, numbers and cursor in a moved
  box move with it, and a touch on a moved element reaches the game at its
  DS place (`gfx::touch_point` with `TouchMove`; `--touch-wide X,Y@F` for
  runs). The game, its memory and its sprite attributes are not changed.
  An element not found whole in a frame (in a fade, while a box opens) is
  drawn centred that frame. It differs from the DS in that the HUD is
  further apart; elements not in the list stay centred (a dialogue's
  speech bubble, the characters and names on the status screen).
- *Smooth 3D textures* (`scene.frag`, `raster.comp`): the DS takes one texel.
  For *Textures in 3D scenes: Smooth*: four texels mixed in each texture's
  own place in the atlas, with its wrap, mirror and clamp. Polygons that use
  different parts of a texture can show a seam between them, as with the
  processor's smoothing.
- *Outlines above 1x* (`scene.frag`, `edges.frag`, `nearby.frag`,
  `outline.frag`; `Hardware3D::find_edges`): the DS marks outlines by its
  spans at 256 by 192 (at 1x the compute rasterizer does the same). For a
  higher resolution and widescreen: the rule is the DS's (a pixel on its
  polygon's edge, next to one of another polygon number that is further
  away), on the triangles' pixels. A pixel is on its polygon's edge where a
  side of the polygon (not a cut between the triangles of one polygon) is
  less than half a DS pixel away, across or down: the corners carry which
  sides are the polygon's (`HardwareScene::kSidesShift`). So, as on the DS,
  where two models go through each other (a foot in the floor, a house in
  the ground) there is no outline along the cut, only near their sides.
  What is further away counts only where it is at least a DS pixel wide
  across: a sliver of it between two polygons, which the higher resolution
  shows and the DS's pixels do not (the wall between the reaction icon and
  the head under it), gives no outline. The outline is one DS pixel wide on every side, soft on its inner side,
  worked out once for each new scene (`outline.frag`, into a target of its
  own) and only read when the scene is put into a screen or a capture. It
  differs from the DS in that depths nearly equal at a cut can compare the
  other way at a higher resolution (a few pixels at the feet of a model
  standing in a translucent floor patch), and that the game's
  anti-aliasing is left out.

The shaders are kept compiled in `src/app/hardware_shaders.h`; after a
change to `src/app/shaders`, run `tools/shaders/compile.py` (needs `glslangValidator`).
`bd_gpu_tests` (ctest's `gpu`) draws test scenes with the graphics hardware
and the processor and compares them; it needs a Vulkan device, and is
skipped without one. Mesa's software device will do: on Ubuntu,
`mesa-vulkan-drivers`.

The program's icon is `src/app/icon/bright-dawn.svg`. The Windows program
carries it (`bright-dawn.ico`, put in by `bright-dawn.rc`) and the window
shows it (`src/app/window_icon.h`); after a change to the SVG, run
`tools/icon/make_icon.py` (needs `cairosvg` and Pillow) to make those again.

Where the graphics hardware draws a game's 3D differently from the
processor, `--dump-3d FILE` (with `--frames`) writes the last frame's 3D
scene as it was made: its polygons, the 3D registers, and the texture
memory. `build/tests/bd_scene_check FILE [--out DIR] [--scale N]` draws it
with both and compares them, at 1x: how many pixels of the graphics
hardware's own rasterizer (outlines, anti-aliased edges) differ from the
processor's, and the first polygon after which they do, with its corners;
and for each screen, how far the pixels put in from the triangles and from
the rasterizer are from the processor's. With `--time N` it draws the scene
N times at `--scale` and gives the frame log's mean time (sync mode) of the
scene pass and of putting it in (outlines included). `--outlines off`,
`one` or `original` draws the outlines so, not as the dump says (with the
pictures of each, the outlined pixels at 1x and at a scale can be
compared). Its text holds only numbers, and
can be shared; the dump and the pictures `--out` writes hold the game's
textures and are never to be committed (`*.scene` is ignored by git).
`bd_gpu_tests` with `BD_FAILED_SCENES=DIR` writes a dump of each test
scene that differs.

`bright-dawn --help` lists the options. Some change a setting for one run
(`--scale`, `--side-by-side`, `--textures smooth`, ...); `--no-settings` uses
the defaults. `--hold` and `--touch` press a button or touch the screen over
given frames (`--touch-wide` also at the sides of a wide touch screen, X from
-64 to 319), and `--frames` with `--capture` write the window's last frame to
a file: together they reproduce a run without a person at the keyboard.

The game's buttons, for such runs: A accepts or interacts, B goes back, X
opens the menu outside battle, Y switches the top screen between the map and
the party's stats, and L and R are shortcuts to Psynergy that the player
chooses. Outside battle the bottom screen always shows the 3D scene. To save
in the game: X, then Left once to the Travel Log, A, a slot, A, then Left to
*Yes* and A.

The game's save is kept in `saves` in the user data directory, one file for
each release (`BO5E.sav`, `BO5P.sav` or `BO5J.sav`): the chip's 256 KiB as
they are, as many emulators keep a DS save. Beside it, `.sav.backup` is the
save as it was before the last run that changed it. A save file that cannot
be used is said so at the start and never written over.

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
seconds instead of 24, unthrottled). See [docs/BOOT.md](BOOT.md).

## Building

Requirements: CMake 3.20+ and a C++20 compiler. The window, `bright-dawn`,
uses SDL 3: the build downloads release 3.4.12 from the SDL project, checks it
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
contains file names, sizes, addresses and statistics only, no game content. [docs/ROM_SURVEY.md](ROM_SURVEY.md) lists the questions that
are still open.

## Code map (graphify)

[graphify](https://github.com/Graphify-Labs/graphify) maps the repository's
code and docs into a graph that can be asked questions, which is quicker than
searching the files. Claude Code uses it on this project (see
[CLAUDE.md](../CLAUDE.md)); anyone can.

```bash
uv tool install graphifyy==0.9.112
```

```bash
graphify update .
```

The map is made on your computer, without a model, in about 20 seconds, in
`graphify-out/` (not committed). It holds only files git would keep:
graphify follows `.gitignore`, so your ROM and anything made from it are never
read. `.graphifyignore` leaves out Dear ImGui. Then, for example:

```bash
graphify query "how does translated code fall back to the interpreter"
```

```bash
graphify explain "TranslatedEntry"
```

`graphify-out/graph.html` shows the map in a browser.

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
src/debug      The frame log, the frame stepper, the performance report and the known GPU issues
src/app        bright-dawn: the window (SDL 3), launcher, settings book, game screen
tools/gamebuilder  Builds bright-dawn-game from the user's ROM
tools/translator   Machine code to C++
tools/release      Writes the description of each pre-release
tools/status       The project map: its data, and what makes the README's part and the page
third_party    SkyEmu's ARM core (the interpreter) and Dear ImGui, MIT licence
tools/bdtool   Command-line inspection tool
tests          Unit and end-to-end tests, with a synthetic ROM generator
tests/guest    Guest test corpus: reference results for executors of DS code
docs           Assessment, roadmap, architecture, and what has been learned about the game
```
