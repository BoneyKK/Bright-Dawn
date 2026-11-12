# Survey of the real game

Facts established by running `bdtool` on a real ROM, and what they mean for the
project. This file takes precedence over the guesses in ASSESSMENT.md.

Source: one run of `bdtool info` and `bdtool survey` (version 0.1.0) by the
project owner on their own copy. The figures below are as reported; the
checksum has not been compared with a dump database.

## The image

| | |
| --- | --- |
| Release | Europe, game code `BO5P`, revision 0, internal title `GOLDENSUN_DD` |
| Size | 268,435,456 bytes (256 MiB chip, 214.45 MiB used), CRC32 `C92C3E10` |
| ARM9 program | 309,672 bytes stored, loads at `0x02000000`, entry `0x02000800` |
| ARM7 program | 156,716 bytes, loads at `0x02380000` |
| Secure area | decrypted |
| File system | 13,237 named files in 1,520 directories; 13,603 file IDs |

## Program code

366 overlays, none flagged as compressed, 7.10 MiB when loaded. They load at
only 14 distinct addresses:

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
two marker words in the wrong order. It now accepts either order. Until this is
rerun, the SDK version and whether the ARM9 program is compressed are unknown.

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
  mostly behind the `0x40` compression. That is good news for texture and
  model replacement.
- **Archives** are NARC, including the 317 `.cbarc` files. A quarter of the
  ROM's data is inside them, so the first survey did not see it.
- **Formats specific to this game**, to be reverse-engineered: `.cbmdl` (1,396
  files, compressed; contents unknown), `MDLR` (signature of 1,284 small
  `.mdlr` files), `.col` (454), `.nbb` (443), `.cbin` (622), `.dat2` to `.dat7`
  (253 of each), `.cam`, `.spr`, and `filenames.bin`.

## The `0x40` compression

The only public description found is the decoder in the DSDecmp project's
Golden Sun plugin. `bdtool` now implements it, with two details that the
description leaves uncertain: how flag bytes are encoded, and the byte order of
the longest length form. The decoder is strict: a file counts as decoded only
if the stream yields exactly the declared size and accounts for the whole
file. A wrong reading therefore shows up as files labelled `0x40?`, not as
wrong data. `bdtool lz40-probe` tries every combination and reports which one
works.

## Open questions

Answered by rerunning `info`, `survey` and `lz40-probe` with the current tool:

1. Which SDK version was the game built with, and is the ARM9 program
   compressed? Does it decompress cleanly (`bdtool validate`)?
2. Does the `0x40` decoder work on real files, and with which reading?
3. What do `.cbmdl` files contain once decompressed?
4. What is inside the 1,416 archives?
5. Are overlays stored at the size they load at? (`info` now reports this.)
