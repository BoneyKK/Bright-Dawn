# Bright Dawn

Bright Dawn is a native PC version of *Golden Sun: Dark Dawn* (Nintendo DS,
2010), made by static recompilation: the game's own code, read from a ROM you
supply, is translated to C++ and compiled for your computer, with an
interpreter behind it for whatever is not translated. The aim is one unified
screen, high-resolution and widescreen rendering, controller-first input, and
packs for textures, models and music. It runs on Windows and Linux.

To try it, download the latest pre-release for Windows or Linux from the
[Releases page](https://github.com/BoneyKK/Bright-Dawn/releases): each one
says how to start it, the controls, and what changed since the last one.

## ROM and asset disclaimer

**Bright Dawn contains no part of the game.** This repository and its
packages hold no ROM, no game code, no pictures, no sound and no other game
data, and they never will. To play, you need a ROM image dumped from a
cartridge of the game that you own. Please do not ask for ROMs, or post
links to them or any game data, in issues.

Found a problem? *Report a problem*, in the launcher (Log) or the pause menu
(Escape), opens the [report form](https://github.com/BoneyKK/Bright-Dawn/issues/new/choose)
filled in with your version and system, and the folder of the logs: attach
the log, never the ROM, saves or other game files.

Everything Bright Dawn takes from the game is read from your own ROM on your
own computer, while it runs: the launcher's pictures and font, and the game's
code that the game builder translates and compiles. What it makes from your
ROM stays in your per-user data directory. 

Recognised releases, by cartridge game code: `BO5E` (USA), `BO5P` (Europe),
`BO5J` (Japan).

*Golden Sun* is a trademark of Nintendo. Bright Dawn is a fan project, not
affiliated with or endorsed by Nintendo or Camelot Software Planning.

## Roadmap and current state

In short: **not playable yet.** The game runs through its opening to the name
entry, built or interpreted, and has not been tried beyond that. There is no
sound. The game's save is kept between runs. Every part in detail:
[docs/STATUS.md](docs/STATUS.md). 

The project map below shows each part of Bright Dawn, how the parts connect,
the state of each, and what comes next. 

<!-- project-map:start: made by tools/status/render_map.py from tools/status/project_map.json; change that file, not this part -->
**Last change** (22 November 2026): With the graphics hardware, outlines and the smoothing of polygon edges are drawn as the processor draws them: the polygons are drawn once more by the DS's rules, on the graphics hardware. In the first cave at 1x those pixels match the processor's; at higher resolutions outlines keep their thickness and colour.

![Bright Dawn project map: each part and its status](docs/status/project-map.svg)

| Part | Status | Now |
| --- | --- | --- |
| `rom` | 🟢 Works | Checks the ROM, reads its file system, archives and every kind of compression the game uses. |
| `assets` | 🟡 Partly works | Decodes the menus' pictures and the game's font (NFTR) for the launcher. Textures, models and sounds: not yet. |
| `mods` | ⚪ Not started | Planned: replace game files without changing the ROM. |
| `translator` | 🟢 Works | Translates 18,271 functions of the game's code, and the interrupt entry of Bright Dawn's own BIOS, to C++, on your computer, with interrupt polls and instruction counts. Where code puts its own return address in lr before a jump, that address is translated too, and so is the place a saved thread context is resumed at, and code whose address the program loads as a constant (its interrupt handler, for example). |
| `game builder` | 🟢 Works | Downloads its compiler (and Python on Windows) for the build and deletes it after. The downloaded compiler is not yet tried on a real build. |
| `game library` | 🟢 Works | Builds, loads and runs: reaches the name entry like the interpreted game, about twice as fast (600 frames in 11 s instead of 24). |
| `runtime` | 🟢 Works | Runs translated code with the interpreter behind it. Interrupts are taken where translated code polls for them. Calls that never return no longer pile up until the program crashes (the Windows crash after about 7 minutes). Each run writes a log of the places run in the interpreter, with why. |
| `nds` | 🟡 Partly works | The console model runs the game from the title screen to battles, with a save from about two thirds through the game. The game's save is kept between runs, also one made in the game; the console's language is a setting. The second processor's music player is written natively. |
| `gfx` | 🟢 Works | Both screens in one window, any size, stacked, side by side or the touch screen alone, sharp pixels with smooth edges. Drawn through Vulkan, or the system's renderer where there is none. 3D scenes by the graphics hardware at up to 8 times the DS's resolution, battles too, with vertices between the DS's pixels so that models move smoothly, on one screen or both, or by the processor. Outlines (original thickness, 1 pixel or off), the smoothing of polygon edges, shadows and fog as on the DS. Widescreen: not yet. |
| `audio` | 🟢 Works | Music and sound effects play through your audio device (SDL 3), kept in step with the frames. Settings, Sound: on or off, and the volume. |
| `input` | 🟢 Works | Keyboard, any number of controllers, the mouse as the stylus. Keys and buttons can be bound in Settings, Controls. |
| `ui, app` | 🟢 Works | The launcher's first page is Play and your save, on the game's page of saves, and a gear to the settings; its other pages look like the game's settings menu; all from your ROM. The setup before the first build is in the program's own look. Settings book, pause menu. Report a problem opens a filled-in bug form on GitHub. |
| `platform, core` | 🟢 Works | Window, files, settings, frame timing, log, checksums. A crash is noted with where it happened; a game that stops finishing frames too. |
| `CI and releases` | 🟢 Works | Every change is built and tested on Linux and Windows; green changes on main become a pre-release. Its description gives the state first, then the big changes since the last one. Issue forms for bugs, a stuck game or wrong graphics, and wishes. |

**Next:**

1. Higher resolutions, 1: the 2D layers, sprites and text stay at the DS's resolution over the sharper 3D.
2. Higher resolutions, 2: a display capture that the game uses as a texture or as a scrolled background is read at the DS's resolution.
3. Higher resolutions, 3: the scroll of the 3D layer (BG0) is not used, by either 3D path.
4. Higher resolutions, 4: at 1x, the pixels of the 3D scene that the graphics hardware draws as triangles differ from the processor's in places (in the first cave, about 4,000 by more than 24 in a colour part), mostly in how textures are sampled.
5. Widescreen and mods.

The same, as a page of its own: [the project map](https://claude.ai/artifact/Gt9rSTaUus5uaPnmhWNwN8).
<!-- project-map:end -->

## Credits and thanks

Bright Dawn stands on the work of these projects and people. Thank you.

| Who | What Bright Dawn takes from it | Licence |
| --- | --- | --- |
| [SkyEmu](https://github.com/skylersaleh/SkyEmu), by Skyler "Sky" Saleh | Its ARM core (`third_party/skyemu`), the interpreter behind the translated code | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui), by Omar Cornut | Draws the launcher, the settings book and the pause menu (`third_party/imgui`) | MIT |
| [SDL 3](https://libsdl.org), by Sam Lantinga and the SDL contributors | The window, drawing (Vulkan and the system's renderers), input, controllers; downloaded and built in by the build | zlib |
| [Zig](https://ziglang.org), by the Zig Software Foundation | The compiler the game builder downloads to build the game from your ROM (from the [`ziglang`](https://pypi.org/project/ziglang/) package) | MIT |
| [LLVM and Clang](https://llvm.org), by the LLVM project | The C++ compiler inside Zig; also used by the tests to build ARM test programs | Apache-2.0 with LLVM exceptions |
| [Python](https://www.python.org), by the Python Software Foundation | Runs the game builder and the translator; downloaded for the build on Windows if not installed | PSF License |
| [NooDS](https://github.com/Hydr8gon/NooDS), by Hydr8gon | A reference for the console's behaviour, such as the second processor's start-up handshake; no code taken | GPL-3.0-or-later |
| [melonDS](https://melonds.kuribo64.net), by Arisotura and the melonDS team | The model for the default screen filter, sharp pixels with smooth edges; no code taken | GPL-3.0-or-later |
| [GBATEK](https://problemkaputt.de/gbatek.htm), by Martin Korth | The documentation of the DS hardware that much of the console model is written from | No licence stated; used as documentation only |
| [DSDecmp](https://github.com/barubary/dsdecmp), by barubary | Its Golden Sun plugin, the only public description of the LZ40 compression, which the first decoder was written from; no code taken | MIT |
| [graphify](https://github.com/Graphify-Labs/graphify), by Safi Shamsi and the Graphify contributors | Maps the repository's code into a graph for development; not part of the packages | Apache-2.0 |
| DS Asset Lab, by BoneyKK | The parts of the translator from this earlier project (`tools/translator/dslab`), placed by its author under this project's licence | GPL-3.0 |
| *Golden Sun: Dark Dawn*, by Camelot Software Planning, published by Nintendo | The game itself, which you supply | Not included |

The packages carry the licences of the code built into them, in `licenses/`.

## Licence

Bright Dawn is under the GPL-3.0; see [LICENSE](LICENSE). `third_party/skyemu`
and `third_party/imgui` are under the MIT licence, each with its licence
alongside.
