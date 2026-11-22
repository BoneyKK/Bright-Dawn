# What works today

The state of each part of Bright Dawn, in detail. The
[project map](https://claude.ai/artifact/Gt9rSTaUus5uaPnmhWNwN8) shows the same
at a glance, with what comes next.

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
| Running translated code with the interpreter behind it, translated code first | Done: the test programs all pass, and the game runs this way from the title screen to battles |
| Overlays that share an address | Done: a translation is used only while the bytes it was made from are in memory; the same for the main program's code, which the game changes in places as it runs |
| Processor modes, each with its own registers | Done, tested in translated and interpreted code |
| Translating the real game | Done: main program and all 366 overlays, 18,271 functions, compile, link and run |
| Memory map, system coprocessor, the state a game starts in | Done, tested, with video memory in its nine banks |
| Running the game's start-up code (`bdtool boot`) | Works in the interpreter, on the real ROM: the program sets up the machine, takes interrupts, reads file tables and overlays from the cartridge, passes its check of the console, sets up its save memory and reaches its frame loop |
| Interrupts, in interpreted code | Done, tested: the display's and timers' interrupts are taken, and the processor sleeps until one comes |
| Console time, display timing, hardware timers, hardware divider and square root | Done, tested. Time per instruction is a coarse figure |
| Channel to the second processor, start-up handshake | Done, tested |
| The second processor's services | Begun: words are received as the game's own second-processor program receives them, and the cartridge service (save memory included) and one-off touch screen samples are done. Sound: next row. Clock, power: not yet |
| Sound | Begun, tested: the sound hardware (channels, mixer, capture units) and the part of the second processor's sound program that drives it directly, with its alarms. The real program's echo set-up runs on it. The sequence player (music and most sound effects) is written natively, and the sound goes to the computer's audio device through SDL, with a volume and an on/off setting. Pitch and volume use computed BIOS tables, which can be one step off the console's |
| Buttons and lid | Done, tested: the main program's register and what the second processor passes on. Driven by the window |
| Save memory (256 KiB serial flash) | Done, tested; the real program reads it, and it is kept in a file between runs (`saves` in the user data directory), with a copy of the save before the last run that changed it. A save made in the game (Travel Log), in Saha Town and in the first cave, is kept through a restart and loads |
| Console settings the game reads (language, owner, network address) | Done, tested. The language is a setting (Settings, Game, or `--language`): on the real European ROM, German gives the German title and name entry. The others are fixed values |
| BIOS calls | 12 of the 17 the game has wrappers for, tested: 9 natively, 3 that wait for interrupts as this project's own code on the processor |
| DMA | Transfers that start at once, transfers fed by the cartridge and transfers timed by the display's blanks: done, tested. Transfers started by other hardware events: not yet |
| Cartridge slot (reading data) | Done, tested, and used by the real program to read its file tables and overlays |
| 3D engine | Begun, tested: the command queue, status and interrupt, the matrices and their stacks, the tests a program can ask for, vertices made into polygons (facing, cutting to the view, placing on the screen, the limits of a frame), lighting, and the drawing of a frame's polygons with textures, blending, shadows, fog, edge marking and anti-aliasing at the hardware's resolution. The real program's 3D commands run through it; `bdtool boot --picture` writes the drawn 3D layer of the last frame (the title screen) and `--wireframe` its outlines. Not compared with a console pixel by pixel; no sorting of translucent polygons; nothing puts the frame on a screen yet |
| Display | Begun, tested: both 2D engines (backgrounds of every kind, sprites, windows, effects, master brightness), the 3D picture among them, the display capture, both screens, a line at a time. On the real ROM the title screen comes out on both screens (`bdtool boot --screens`); looked at, not compared with a console pixel by pixel. Not shown in a window yet |
| Window (`bright-dawn`) | Begun, tested where it can be without a screen: both screens, stacked or side by side, or the touch screen alone, scaled by whole numbers or to fill the window, with sharp pixels and smooth edges (or plain bilinear, or nearest), full screen; the keyboard, any number of controllers and the mouse as the stylus; the console's frame rate kept. On the real ROM its picture matches the trace tool's pixel for pixel, and a touch passes the title screen |
| Launcher, settings book, pause menu | Begun: without a ROM on the command line the program opens its launcher (a 4 by 3 window: Play and what the save holds on the game's page of saves; building the game, settings, log on its settings menu; all from the ROM), after the setup (choose the ROM, build the game) in its own look; the settings are described once and the book and the settings file are made from the descriptions; Escape pauses the game with a menu. Tried on the real ROM under a virtual display; the settings store and the font reader are unit tested |
| Textures of 3D scenes, sharp or smooth | Done, tested: nearest (as on the DS) by default; bilinear as a setting, for polygons drawn with perspective only, so that a HUD drawn in 3D keeps its texels |
| Log | Done, tested: every run writes one, with the system, the settings, what the player did and how the run ended; a crash is noted in it with where it happened, and a game that stops finishing frames |
| Interrupts in translated code | Done, tested: taken where the code polls for them (loops, coprocessor and status register writes, calls through the runtime, BIOS calls), with time passing as the code counts its instructions. The built game reaches the name entry like the interpreted one, about twice as fast |
| 3D on the graphics hardware, at higher resolutions | Begun, tested where it can be without a graphics device: the frame's polygons as triangles and its textures decoded once, drawn through SDL's GPU API on Vulkan at 1 to 8 times the DS's resolution, and put into the main engine's screen where it is marked. On the real ROM the name entry's 3D keys match the processor's drawing but for under 1% of the pixels (a highlight). Edge marking (outlines around models), shadow polygons and fog are drawn as the processor draws them, with the same rules. Anti-aliasing and polygons drawn as outlines only are left out |
| Mods | Not started |

The tests use ROM images and test programs generated by the test suite itself;
no game data is involved. The tools have also been run against a real European
copy of the game by its owner; what that showed is recorded in
[docs/ROM_SURVEY.md](ROM_SURVEY.md), how the code is run is described in
[docs/FALLBACK_INTERPRETER.md](FALLBACK_INTERPRETER.md), and how the game
is started in [docs/BOOT.md](BOOT.md).
