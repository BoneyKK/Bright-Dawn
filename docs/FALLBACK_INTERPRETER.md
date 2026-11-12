# Fallback interpreter

The owner has decided that the runtime may fall back to an interpreter for
code the translator did not cover, on one condition: translated code is always
preferred. This document records that rule, what a fallback would have to
catch, which existing interpreters were examined, and what is still undecided.

Nothing described here is implemented yet.

## The rule: translated code first

1. A call or jump to an address is served by its translated function whenever
   one exists. The interpreter is entered only when the dispatcher finds none.
2. The interpreter hands control back as early as possible. At every call or
   jump it makes, it looks the target up; if a translated function exists
   there, that function runs natively and the interpreter resumes, or stops,
   after it.
3. Every entry into the interpreter is counted by address and reported. The
   list is the work queue for the translator: an address that appears there is
   a gap to close, not a normal state.
4. A strict mode turns any entry into the interpreter into an error. Tests use
   it for code that is expected to be fully translated.

Because the translated functions keep all of their state in the processor
model and in guest memory, the interpreter can take over in the middle of a
function and continue from exactly the state the translated code left.

## What a fallback has to catch

The owner's translator (DS Asset Lab 0.15.0) was run on this project's guest
test corpus (`tests/guest`), and the C++ it produced was compiled and compared
with the corpus running natively. This was a one-off experiment in the
development session; the translator is not in this repository, so it is not
part of the automated tests.

| | |
| --- | --- |
| Corpus | 15 functions, each built for ARM state and for Thumb state: arithmetic, shifts, multiplies, the DSP multiplies and saturating adds, bit counting, loops, memory access of every width, stack use, recursion across the two states, calls through pointers, a dense `switch`, division helpers, 64-bit arithmetic |
| Inputs | 2,484 pairs per function |
| Translated | 39 functions, 2,227 instructions |
| Result | 26 of 30 produce the reference result on every input |
| Wrong results | none, in any test |
| Not completed | 4 tests stop at a call the dispatcher has no translation for |

As far as is known this is the first time the translator's output has been
executed with functions calling each other; its own tests cover single
instructions and leaf functions.

The four incomplete tests fall into two kinds, both of which the translator's
documentation predicts:

- **Functions reached only through pointers** (`indirect`, both states). Four
  small functions are stored in a table and never called directly. The
  translator looks for such functions by their opening instruction, and these
  do not save the link register, so it does not find them.
- **Jump tables** (`sw`, both states). The computed jump of the `switch` is
  not recognised, so its targets, which lie inside the function, are sent to
  the dispatcher as if they were functions.

One caution about reading these numbers. The corpus is compiled with Clang;
the game was built with a different compiler, which lays out jump tables
differently. The translator already recognises ARM-state table layouts that
Clang did not emit here. How often the real game hits either kind of gap is
measured by the translator itself, which counts unresolved computed jumps per
module; that figure has not been recorded yet.

## Interpreters examined

Requirements: a licence that can be combined with GPL-3.0; ARMv5TE in ARM and
Thumb state; memory access supplied by the host; execution one instruction at
a time, so that rule 2 can be applied; small enough to maintain.

| Candidate | Licence | Assessment |
| --- | --- | --- |
| SkyEmu `arm7.h` | MIT | One C header of 2,279 lines. Memory and coprocessor access are callbacks. Separate ARM9 instruction tables include the ARMv5TE additions. Stepped by repeated calls. **Best fit.** |
| NooDS | GPL-3.0-or-later | Interpreter of about 4,900 lines that declares the full ARMv5TE instruction set. Built around the emulator's own `Core` object, which would have to be imitated or cut out. Usable, at more adaptation cost. |
| melonDS | GPL-3.0-or-later | The most widely used DS core. Its processor class holds a reference to the whole emulated console; heavier to separate than NooDS. |
| mGBA (`medusa` branch) | MPL-2.0 | Clean callback interface, but the saturating add and subtract instructions were not found in its ARM decoder, and the branch's last non-merge commit is from June 2024. |
| dynarmic | 0BSD | A just-in-time compiler for x86-64 and AArch64 hosts, not an interpreter. Does not fit the requirement. |
| Unicorn | GPL-2.0 | Its README states GPLv2 with no "or later", which cannot be combined into a GPL-3.0 program. Still usable as a separate test tool, as the owner's project already does. |

Sources read: SkyEmu at commit `f4d3238` (24 October 2026) and NooDS at commit
`4cb1439` (9 August 2026), both cloned and read in full for the parts cited;
the others from their licence files, headers and READMEs on GitHub.

### SkyEmu `arm7.h` in more detail

Found by reading the source:

- Word loads from unaligned addresses are rotated, and a load into the program
  counter switches instruction set from bit 0. Both match the translator.
- `BLX` in its immediate and register forms is implemented. `CLZ`, the
  saturating instructions, the 16-bit multiplies and doubleword transfers
  have handlers in the ARM9 tables; their bodies were not checked line by
  line.
- It needs four small macros that SkyEmu defines in another header; they are
  one-liners and can be supplied by a wrapper.

Things a wrapper has to deal with:

- It models the processor's instruction pipeline: after a branch, several
  calls refill the pipeline before the next instruction runs, and multi-word
  transfers are spread over several calls. The wrapper must step through
  those phases.
- Software interrupts and hardware interrupts jump to vectors in guest
  memory, where the console's BIOS would be. This project has no BIOS in
  memory, so the wrapper must intercept software interrupts before they
  execute and route them to the runtime, as translated code does.
- The breakpoint instruction is not implemented, and unimplemented cases
  print to standard output.

**Not established:** that it executes the guest corpus correctly. The plan was
to prove that by running it against the corpus, exactly as was done for the
translator. Compiling and running third-party code was not permitted in the
development session, so the recommendation rests on reading the source. It
should be treated as provisional until the corpus has been run through it.

## How it would fit (design, untested)

- **Entering.** The dispatcher finds no translation and passes the processor
  state to the interpreter, which starts at the requested address.
- **Stopping.** Translated code treats a fixed set of instruction forms as
  "return". The interpreter applies the same definition: it keeps a call
  depth, and when it executes a return at depth zero it copies the state back
  and returns to the native caller. This covers both an untranslated function
  and a jump into the middle of a translated one.
- **Handing back.** On a call whose target is translated, the native function
  runs and interpretation continues at the return address. On a jump whose
  target is translated, the native function runs and counts as the return of
  the code that jumped.
- **System calls and coprocessor access** go to the same runtime entry points
  that translated code uses.

## Decisions for the owner

1. **May the translator's source be published in this repository?** The
   repository is public and under GPL-3.0, and every merge is released
   automatically. The uploaded source carries no licence of its own. It has
   been read and run locally but not committed. Committing it makes the
   translator test above a permanent, automated test.
2. **May a third-party interpreter be added to the repository and run in its
   tests?** SkyEmu's `arm7.h` is the proposal. Adding it lets the automated
   tests run the guest corpus through it, which is the proof still missing.
