# Survey of the real game

Facts established by running `bdtool` on a real ROM, and what they mean for the
project. This file takes precedence over the guesses in ASSESSMENT.md.

Sources, and how far to trust each:

- **bdtool run**: one run of `bdtool info` and `bdtool survey` (version 0.1.0)
  by the project owner on their own copy. Figures are as reported.
- **Owner's notes**: a summary the owner supplied from an earlier asset
  extraction project of theirs, built against the same ROM. Items it marks as
  measured were counted by scripts on the real ROM there; they have not been
  re-measured with this project's tools.

## The image

| | |
| --- | --- |
| Release | Europe, game code `BO5P`, revision 0, internal title `GOLDENSUN_DD` |
| Size | 268,435,456 bytes (256 MiB chip, 214.45 MiB used), CRC32 `C92C3E10` |
| SHA-1 | `0b60543279fcd04c437ee22906e10887475a48ea` (owner's notes) |
| ARM9 program | 309,672 bytes stored, loads at `0x02000000`, entry `0x02000800` |
| ARM7 program | 156,716 bytes, loads at `0x02380000` |
| Secure area | decrypted |
| File system | 13,237 named files in 1,520 directories; 13,603 file IDs |

## Program code

366 overlays, 7.10 MiB when loaded. None is flagged as compressed in the
overlay table, yet they are compressed: this game stores its overlays as LZ40
files, which the table has no flag for (owner's notes). `bdtool` now decodes
them and checks each result against the size the table says is loaded. They
load at only 14 distinct addresses:

| Load address | Overlays | | Load address | Overlays |
| --- | --- | --- | --- | --- |
| `0x0210A280` | 3 | | `0x021F34A0` | 2 |
| `0x02161C60` | 2 | | `0x021F3820` | 2 |
| `0x02170BA0` | 24 | | `0x021F6CC0` | 1 |
| `0x021EE1A0` | 297 | | `0x021F78C0` | 14 |
| `0x021EF540` | 2 | | `0x021F9880` | 1 |
| `0x021F1D00` | 5 | | `0x021FA340` | 8 |
| `0x021F2740` | 1 | | `0x021FF160` | 4 |

What this means:

- Overlays that share an address can never be loaded together, so the game
  swaps code constantly. 297 overlays at one address is the signature of
  per-location or per-event code: a small module loaded for the current scene.
  This is a guess from the numbers, to be confirmed by code analysis.
- The first overlay starts about 1.04 MiB above the ARM9 load address, while
  the stored ARM9 program is 0.30 MiB. The stored program is therefore very
  likely compressed (the rest being uninitialised data would also fit).
- The recompiler has to treat overlays as its normal case: translate each
  separately and resolve calls according to what is loaded.

**Bug found.** The tool reported that the ARM9 start-up parameters were
missing. They cannot be missing from a retail game; the search looked for the
two marker words in the wrong order. Retail images store the bytes
`21 06 C0 DE DE C0 06 21` (confirmed by the owner's notes); the tool now
accepts either order. Until it is rerun, the SDK version and whether the ARM9
program is compressed are unknown.

## Assets

By content signature, top-level files only:

| Detected | Files | Size |
| --- | --- | --- |
| Unrecognised | 11,773 | 93.52 MiB |
| NARC archive | 1,416 | 61.37 MiB |
| SDAT sound archive | 1 | 49.97 MiB |
| Standard 3D formats (model, texture, animation) | 47 | 1.75 MiB |

By extension, the standard 3D formats are far more common than their
signatures suggest: 444 `.nsbmd`, 437 `.nsbtx`, 368 `.nsbca`, 300 `.nsbta`.
Most of those files do not begin with the format's signature. They begin with
the byte `0x40` followed by what reads as a 24-bit size, as do thousands of
other files. That is the shape of a compression header.

What this means:

- **Audio** is one standard sound archive (`data/sound/test_data.sdat`). The
  format is documented, and its sequence numbers give music packs natural
  track identifiers.
- **Models, textures and animations** use the standard, documented formats,
  mostly behind LZ40 compression (see below). That is good news for texture
  and model replacement.
- **Archives** are NARC, including the 317 `.cbarc` files. A quarter of the
  ROM's data is inside them, so the first survey did not see it.
- **Formats specific to this game**, to be reverse-engineered: `.cbmdl` (1,396
  files, compressed; contents unknown), `MDLR` (signature of 1,284 small
  `.mdlr` files), `.col` (454), `.nbb` (443), `.cbin` (622), `.dat2` to `.dat7`
  (253 of each), `.cam`, `.spr`, and `filenames.bin`.

More from the owner's notes:

- About 5,337 models in total, 1,708 of them inside `.cbmdl` containers. A
  decompressed `.cbmdl` is a small table followed by a standard model, which
  starts somewhere between byte `0x0C` and `0x24`.
- `.cbarc` archives hold battle characters: one model and 15 to 30 animations.
  Their animations are a game-specific revision of the standard skeletal
  animation format (2,485 files) with different key-frame and rotation
  encoding. This matters for the model replacement pipeline: original
  animations cannot be read by a stock decoder.
- No files in the standard 2D graphics formats were found. Fonts and interface
  graphics must therefore be stored some other way, which the settings menu
  milestone will have to work out.

## LZ40 compression

Almost every data file, and every overlay, is compressed with LZ40 (type byte
`0x40`), sometimes two layers deep. The console's built-in routines do not
handle this scheme, so the game carries its own decoder in its program code.

The format, as measured in the owner's project against 15,355 real files and
an independent implementation:

```
u8   0x40
u24  decompressed size, little-endian (if 0: a u32 size follows)
repeat until the output is full:
  u8 flags, stored negated: logical flags = (-flags) & 0xFF
  for each bit, most significant first:
    0: copy one literal byte
    1: u16 little-endian word; distance = word >> 4, code = word & 15
         code 0:     length = next u8 + 16
         code 1:     length = next u16 (little-endian) + 272
         code 2..15: length = code
       copy `length` bytes from `distance` bytes back (distance 0 is invalid)
output past the declared size is discarded
```

Up to 32 bytes of padding may follow the stream in a file.

This project's first implementation, written from the DSDecmp plugin before
the owner's notes arrived, had the 16-bit length in the wrong byte order,
rejected output past the declared size, and allowed only 3 bytes of padding.
All three are corrected and covered by tests.

What this means for the recompilation:

- The game's own LZ40 routine will be among the recompiled code. The runtime
  can either run it as translated or substitute the native decoder here; the
  second is simpler and faster, and needs the routine to be located first.
- The runtime must serve files to the game compressed, exactly as stored,
  unless the game's file loading is replaced as well.
- Asset replacement does not need an LZ40 encoder: replacements are loaded
  directly. One exists in the test suite if rebuilding files is ever wanted.

## Open questions

Answered by rerunning `validate`, `info` and `survey` with the current tool:

1. Which SDK version was the game built with, and is the ARM9 program
   compressed? Does it decompress cleanly?
2. Do all 366 overlays decode to exactly the size the overlay table gives?
3. Does this project's LZ40 decoder agree with the owner's figures on the
   real ROM (no files left labelled `0x40?`)?
4. What is inside the 1,416 archives, and how common is double compression?
5. What do the game-specific formats look like once decompressed (the survey
   lists the leading bytes of everything it does not recognise)?
