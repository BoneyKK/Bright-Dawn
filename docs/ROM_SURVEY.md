# Survey of the real game

Facts established about the real ROM, and what they mean for the project. This
file takes precedence over the guesses in ASSESSMENT.md.

Every figure is tagged with where it comes from:

- **[run 1]** `bdtool info` and `survey`, first build, run by the project
  owner on their own copy.
- **[run 2]** release `v0.1.0-pre.8`, same copy. Its LZ40 decoder still had
  three errors, so its asset figures are superseded by run 3.
- **[run 3]** `bdtool info` and `survey`, release `v0.1.0-pre.10`, same copy.
  This is the first build with the corrected LZ40 decoder and overlay
  decoding, and the source of the asset figures below.
- **[code]** The game's extracted program files, supplied by the owner and
  analysed in the development session with this repository's translator. The
  files are kept outside the repository; only counts are recorded.
- **[owner]** Notes the owner supplied from an earlier extraction and code
  translation project of theirs, built against the same ROM. Items given here
  were measured by scripts there; they have not been re-measured with this
  project's tools unless a run is also cited.

## The image

| | | |
| --- | --- | --- |
| Release | Europe, game code `BO5P`, revision 0, title `GOLDENSUN_DD` | run 1 |
| Size | 268,435,456 bytes (256 MiB chip, 214.45 MiB used) | run 1 |
| CRC32 | `C92C3E10` | run 1 |
| SHA-1 | `0b60543279fcd04c437ee22906e10887475a48ea` | owner |
| File system | 13,237 named files in 1,520 directories; 13,603 file IDs | run 1 |
| Secure area | decrypted | run 1 |

`bdtool validate` accepts this image and decompresses its ARM9 program without
error [run 2].

## Program code

| | | |
| --- | --- | --- |
| SDK version | 5.4, release step 30001 (raw `0x05047531`) | run 2, run 3 |
| ARM9 program | 309,672 bytes stored, BLZ-compressed; 507,960 bytes once decompressed | run 2, run 3 |
| ARM9 load address, entry | `0x02000000`, `0x02000800` | run 1 |
| ARM7 program | 156,716 bytes, loads at `0x02380000` | run 1 |
| Overlays | 366, none flagged compressed in the overlay table | run 1 |
| Overlay storage | all 366 are LZ40 files, which the overlay table has no flag for; 3.25 MiB stored, 7.10 MiB when loaded | run 3, owner |
| Overlay check | all 366 decode to exactly the size the overlay table gives | run 3 |

The overlays load at only 14 distinct addresses [run 1]:

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
  swaps code constantly. 297 overlays at one address suggests a small module
  per location or event; that is a guess from the numbers.
- Every overlay decodes to exactly the size the game expects, so this
  project can now extract the game's program code: the decompressed ARM9
  program and all 366 overlays. The ARM9 program's decompression is checked
  for internal consistency only; nothing independent confirms its content.

### The ARM9 main program

Once decompressed (507,960 bytes) it is three blocks [code]: the main block at
`0x02000000` (496,192 bytes), and two that the start-up code copies elsewhere,
11,584 bytes to `0x01FF8000` and 160 bytes to `0x027E0000`.

Across the whole ARM9 side (the main program and all 366 overlays, 7,954,272
bytes) the translator finds 17,076 functions, 2,511 of them Thumb, with
1,125,101 distinct instructions covering 52% of the bytes [code]. In the main
program alone: 2,352 functions, 77% of its bytes. Overlay 0, which has an
address range to itself directly below the shared slots, is the largest
module: 896,736 bytes and 2,601 functions. The translation is 3.5 million
lines of C++ that compile and link against the runtime. None of it has been
executed. More in FALLBACK_INTERPRETER.md.

### The owner's existing code translation

The owner's earlier project already translates this game's machine code to
C++ [owner]. In its own words it is static binary translation, not
decompilation: each ARM or Thumb instruction becomes a few C++ statements
acting on a model of the processor.

| | |
| --- | --- |
| Input | 368 program files: ARM9 (with its autoload blocks), ARM7, 366 overlays |
| Output | 14,354 functions, 877,968 instructions, about 190 MB of C++17 |
| Builds | the whole output compiles and links with g++ |
| Tested | decoder against Capstone on the main program (matched, apart from a few differences whose details were not recorded); about 10,000 random instruction sequences and all 485 leaf functions of the main program against Unicorn, with no differences where Unicorn models this processor correctly (it does not model rotated unaligned loads, so those were not compared) |
| Not tested | functions that call other functions, the dispatcher and jump-table paths, system calls, coprocessor access, interrupts, anything touching hardware registers, ARM7 code |
| Never done | running the translated code as a program; there is no platform layer |

Its documented gaps are listed in ASSESSMENT.md, because they define the work
ahead. The translator itself is now in this repository (`tools/translator`).

**Bug found by run 1.** The tool reported that the ARM9 start-up parameters
were missing. The search looked for the two marker words in the wrong order.
Retail images store the bytes `21 06 C0 DE DE C0 06 21` [owner]; the tool now
accepts either order, and run 2 found them.

## Assets

All figures in this section are from run 3.

Of the 13,237 files, 10,369 are LZ40-compressed (31 of them twice) and 2,868
are not compressed. By content, after removing compression:

| Content | Files |
| --- | --- |
| Unrecognised | 9,731 |
| NARC archive | 1,416 |
| 3D model | 444 |
| 3D texture set | 437 |
| Skeletal animation | 368 |
| Texture coordinate animation | 300 |
| Visibility animation | 269 |
| Material animation | 248 |
| Texture pattern animation | 11 |
| Bitmap font | 10 |
| LZ40 header inside LZ40 that did not decode | 2 |
| SDAT sound archive | 1 |

Inside archives: 1,674 archives were found (1,416 top-level, 258 nested) and
1,672 were opened, holding 15,755 members. Two were not opened and produced no
error; the only path in that build that leaves an archive unopened silently is
its nesting limit, which has since been raised and now reports what it skips.

| Member content | Members |
| --- | --- |
| Skeletal animation (4,625 plain, 1,933 under LZ40) | 6,558 |
| Texture coordinate animation | 3,727 |
| Unrecognised (1,369 plain, 567 under LZ40, 3 under LZ10 or LZ11) | 1,939 |
| 3D model (423 plain, 894 under LZ40) | 1,317 |
| 3D texture set (49 plain, 977 under LZ40) | 1,026 |
| Material animation | 575 |
| Visibility animation | 355 |
| Nested archive (9 plain, 249 under LZ40) | 258 |

Counting files and archive members together, the standard formats account for
1,761 models, 1,463 texture sets and 6,926 skeletal animations. Models inside
the game's `.cbmdl` containers are not in these figures, because the container
is what the survey sees.

What this means:

- **Audio** is one standard sound archive (`data/sound/test_data.sdat`,
  49.97 MiB). Its sequence numbers give music packs natural track identifiers.
- **Models, textures and animations** use the standard, documented container
  formats throughout. That is good news for texture and model replacement.
- **Fonts**: ten files decode to the standard bitmap font format. The
  settings menu can therefore take its fonts from the ROM in a documented
  format.
- **No standard 2D graphics files** were found [owner]. Interface graphics are
  stored some other way, which the settings menu milestone has to work out.
- **Archives** are standard NARC, and that includes `.cbarc`: the number of
  files with a NARC signature (1,416) equals the number of `.narc` files
  (1,099) plus `.cbarc` files (317).
- **Extensions agree with content** for the standard formats, at least by
  count: the files whose content is a model, texture set, skeletal animation
  or texture coordinate animation number 444, 437, 368 and 300, the same as
  the counts of `.nsbmd`, `.nsbtx`, `.nsbca` and `.nsbta` files.
- **Game-specific formats**, to be reverse-engineered. The largest groups of
  unrecognised data begin with a small 32-bit number: 0 (1,806 items),
  2 (1,211), 3 (432), 6 (368), 7 (337), 5 (312), 4 (238), 12 (195), 8 (168),
  10 (21), 9 (20). That looks like a count followed by a table, which would
  fit the owner's description of `.cbmdl` below; it is an inference, not a
  measurement. The one clear signature is `MDLR` (2,437 items). The common
  extensions that are not standard format names are `.cbmdl`, `.bin`,
  `.mdlr`, `.cbin`, `.col`, `.nbb`, `.dat2` to `.dat7`, `.cam` and `.spr`.
- **Loose ends**: two files contain, inside their LZ40 layer, data that has an
  LZ40 header but does not decode; five files and three archive members
  decode further as LZ10 or LZ11 to unrecognised content. Whether these are
  real nested compression or data that merely resembles it is not known.

More from the owner's notes:

- About 5,337 models in total, 1,708 of them inside `.cbmdl` containers. A
  decompressed `.cbmdl` is a small table followed by a standard model, which
  starts somewhere between byte `0x0C` and `0x24`.
- `.cbarc` archives hold battle characters: one model and 15 to 30 animations.
  Their animations are a game-specific revision of the standard skeletal
  animation format (2,485 files) with different key-frame and rotation
  encoding. A stock decoder cannot read them, which matters for the model
  replacement pipeline.

## LZ40 compression

Almost every data file, and every overlay, is compressed with LZ40 (type byte
`0x40`), sometimes two layers deep. The console's built-in routines do not
handle this scheme, so the game carries its own decoder in its program code
[owner].

The format, as measured in the owner's project against 15,355 real files and
an independent implementation [owner]:

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

How this project's decoder stands against real data [run 3]:

- Every item whose outer layer has an LZ40 header decodes: 10,369 files,
  4,620 archive members and 366 overlays, with none left undecoded.
- 6,093 of the files and members decode to a recognised format signature,
  and all 366 overlays decode to exactly the size the overlay table gives.
  Both are checks the decoder could not pass by accident.
- 10,369 + 4,620 + 366 = 15,355, which is the number of real LZ40 files the
  owner's project reports having decoded. The two counts were produced
  independently.
- History: the first decoder here, written from the DSDecmp plugin, read the
  16-bit length in the wrong byte order, rejected output past the declared
  size and allowed only 3 bytes of padding. Run 2 used it and failed on 4,055
  items. The owner's notes identified all three errors.

What this means for the recompilation:

- The game's own LZ40 routine is among the translated code. The runtime can
  run it as translated or substitute the native decoder here; the second
  needs the routine to be located first.
- The runtime must serve files to the game compressed, exactly as stored,
  unless the game's file loading is replaced as well.
- Asset replacement does not need an LZ40 encoder: replacements are loaded
  directly. One exists in the test suite if rebuilding files is ever wanted.

## Open questions

1. Why were two archives not opened? A build that prints its version as the
   first line of its output reports archives it skips for nesting depth.
2. Are the "loose ends" above real nested compression?
3. What are the game-specific formats? This needs format work, not another
   survey.
