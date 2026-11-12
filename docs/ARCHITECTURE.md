# Architecture

This describes the intended module structure and records the decisions made so
far. Only `core`, `platform` and `rom` exist; the rest is the target that later
milestones build towards. Modules are added when there is working code to put
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
| `core` | Errors, byte access, checksums, logging | nothing |
| `platform` | Files, paths, windowing, devices, configuration storage | `core` |
| `rom` | The only code that reads the user's ROM: validation, file system, decompression | `core` |
| `assets` | Decodes original formats into neutral descriptions (textures, meshes, skeletons, sequences); asset identifiers; cache in the user data directory | `core`, `rom` |
| `mods` | Manifests, load order, override resolution on top of `assets` | `core`, `assets` |
| `recompiler` | Offline: machine code to C | `core`, `rom` |
| `runtime` | What the recompiled game calls: guest memory, replaced SDK services, the fixed-rate simulation loop | `core`, and the *interfaces* of `gfx`, `audio`, `input` |
| `gfx` | Capture of DS-level graphics state, presentation rules, renderer backends | `core`, `assets`, `platform` |
| `audio` | Sequence playback, mixing, replacement tracks | `core`, `assets`, `platform` |
| `input` | Devices, mapping, touch replacements | `core`, `platform` |
| `ui` | The settings book and setup screens | `gfx`, `input`, `assets` |

Rules that keep game logic independent of presentation:

- The game module and `runtime` never include a graphics API header. They emit
  a **frame description**: for each DS screen, the 3D command stream and the 2D
  layer state for that frame. `gfx` consumes it.
- `rom` is the only module that knows the ROM is a file. Everything else asks
  for assets by identifier.
- Dependencies point downwards in the table; `core` depends on nothing.

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
| No third-party dependencies yet | None are needed for ROM handling. Each later dependency is assessed for need and GPL-3.0 compatibility when it is introduced; SDL 3 (zlib licence) is the expected choice for `platform`. |
| Errors as values (`Result<T>`) in parsers | Input is untrusted and failures are expected; callers must handle them. |
| Every read of ROM data is bounds-checked | A malformed or malicious file must produce an error message, not a crash. Enforced by the damage tests under AddressSanitizer. |
| In-house test harness | A few dozen lines; avoids a dependency this early. |
| Extracted data goes to the user data directory | Keeps game data out of the repository and out of the install directory. |
| Simulation stays at the original tick rate | Avoids frame-rate-dependent behaviour; rendering is decoupled later if at all. |
