# Translated code and the fallback interpreter

The game's program code is run as C++ produced by the translator
(`tools/translator`). Where the translator has no output for an address, an
interpreter runs the code there. The owner's rule is that translated code is
always preferred. This document describes how that is built, what has been
tested, and what has not.

Implemented in `src/runtime` (`machine.h`). It has been tested on this
project's own test programs only. **It has not run any of the game.**

## The rule

1. A call or jump to an address is served by its translated function whenever
   one exists. The interpreter is entered only where there is none.
2. The interpreter hands back at the first opportunity. Every call or jump it
   makes to a translated function runs that function natively.
3. Every entry into the interpreter is counted by address. The list is the
   work queue for the translator: each address in it is a gap to close.

The tests enforce the rule directly: a test whose code is completely
translated fails if it enters the interpreter even once.

## How the two are joined

Translated functions keep all of their state in the processor model and in
guest memory. Nothing lives in native variables beyond a single instruction.
The native call stack is therefore only a way of running fast: any native
frame can be abandoned and the program continued from the model alone.

That property is used as follows.

- Every translated call site records the return address and the stack pointer
  it is waiting for, before the call, and removes the record after it.
- When control arrives at a recorded return address with the recorded stack
  pointer, by any route, execution resumes natively at that call site. Any
  call sites recorded inside it are told to give up, and their functions
  return at once.
- Until that happens the interpreter runs.

Requiring both the address and the stack pointer makes the match exact. The
calling convention returns with the stack pointer the call was made with, and
the stack pointer distinguishes one activation of a recursive call from
another.

The result is that unusual control flow is handled exactly, not by guessing
at instruction patterns:

| Case | What happens |
| --- | --- |
| A call through a pointer to a function with no translation | Interpreted until it returns to the call site |
| A jump table the translator did not recognise | The rest of that function is interpreted; its caller continues natively |
| Untranslated code calls or jumps to a translated function | The function runs natively |
| A return made through a register (`pop {r3}; bx r3`) | Recognised as a return to the waiting call site; nothing is interpreted |
| A call made with `mov lr, pc` and a branch | The callee runs natively; the instructions after the branch are interpreted |
| A callee that returns past data placed after the call | The caller's remaining instructions are interpreted |
| A helper that jumps into its caller through a table after the call | The caller's remaining instructions are interpreted |
| A jump out of several nested functions | The functions in between are abandoned; the outer one continues |
| A switch to another stack, as a thread scheduler does | The flow switched to runs; when control returns to a waiting call site, native execution resumes there |

About the last row. When one flow of control switches to another and back,
the native frames of the flow that was left are abandoned. When that flow is
resumed later, the interpreter picks it up and runs it until its next call
into translated code. That is correct, and the coroutine test checks it, but
its cost on a real game with several threads has not been measured. Giving
each game thread its own native stack remains possible as an optimisation; it
is no longer needed for correctness.

## Overlays

The game loads code on demand, and several overlays are built for the same
address range: only one of them is in memory at a time, and the game's own
code does the loading. Each overlay is translated separately, so one address
can have several translations.

The runtime is never told which overlay is loaded. For every translated
function the table records the bytes it was translated from: its instructions
and the constants the translator read from next to them. A translation that
belongs to an overlay is used only while exactly those bytes are in guest
memory. If none matches, the code there is interpreted.

This is the same idea as the rule itself, applied to presence: a translation
stands for particular bytes, so it is valid exactly when those bytes are
there. It needs no knowledge of the game's loader, and it also covers memory
that holds something no translation was made from.

Comparing is not repeated needlessly. Writes to the overlay address range are
counted per 4 KiB page, and a translation is compared with memory again only
after a page holding its code has been written.

Two things are deliberately not checked:

- Calls that the translator binds directly, which it does only when a single
  module can ever occupy the target address, and calls inside one module.
- The main program's translations.

So code that rewrites itself in place is not detected. Nothing suggests this
game does that, but it has not been ruled out.

## The interpreter

SkyEmu's ARM core, `arm7.h`, unmodified, under its MIT licence
(`third_party/skyemu`). It is used only through `src/runtime/interp/arm_core.h`,
a small C interface (host-supplied memory, state exchange, "execute exactly
one instruction"), so it can be replaced.

Why this one:

| Candidate | Licence | Assessment |
| --- | --- | --- |
| SkyEmu `arm7.h` | MIT | One C header, memory through callbacks, separate ARM9 tables with the ARMv5TE instructions. **Chosen.** |
| NooDS | GPL-3.0-or-later | Usable, but built around the emulator's own core object |
| melonDS | GPL-3.0-or-later | The most widely used DS core; its processor class holds a reference to the whole console |
| mGBA (`medusa` branch) | MPL-2.0 | Saturating add and subtract not found in its ARM decoder; last non-merge commit June 2024 |
| dynarmic | 0BSD | A just-in-time compiler, not an interpreter |
| Unicorn | GPL-2.0 | Licence cannot be combined into a GPL-3.0 program; still usable as a separate test tool |

What the wrapper has to deal with:

- The core models the instruction pipeline: after a branch it refills over
  several calls, and multi-register transfers move one register per call. The
  wrapper steps through those phases.
- Software interrupts would jump to a BIOS that is not in memory. The runtime
  intercepts them before they execute and sends them where translated code
  sends them.
- The breakpoint instruction is not implemented, and unimplemented cases
  print to standard output.
- One defect, found by this project's tests and corrected in the wrapper
  because the core is kept unmodified: for `ldm rn!, {..., pc}^`, an exception
  return that also updates its base register, the core writes the base
  register once more after the mode has changed. The value then lands in the
  register of the mode being returned to. With `ldmfd sp!, {..., pc}^` that
  destroys the stack pointer of the interrupted program.

## Processor modes

The processor has seven modes, and five of them have a stack pointer, a link
register and a saved status register of their own; FIQ mode also has its own
r8 to r12. The game's start-up code gives the interrupt, supervisor and system
modes a stack each, and interrupt handlers run on theirs.

The processor model keeps the current mode's registers where translated code
uses them and the other modes' beside them, and exchanges them when the mode
changes. The interpreter receives all of them when it takes over and hands
all of them back. Tested in both directions: registers set by translated code
are read by interpreted code and the reverse.

## What the tests show

All on the guest corpus (`tests/guest`): 26 small programs, each built for
ARM state and for Thumb state, each run on 2,484 input pairs and compared
with the same program built natively. Fifteen are compiled C; eleven are
hand-written assembly for the control flow in the table above, software
interrupts, processor modes and exception returns.

| Test | What runs | Result |
| --- | --- | --- |
| `guest_interpreter` | The interpreter alone | All 52 cases give the reference result, except the software-interrupt test, which needs the runtime |
| `guest_translated` | Translated code alone | 14 tests complete; 12 stop at something the translator does not cover; none gives a wrong result |
| `guest_mixed` | Translated code with the interpreter behind it | All 52 cases give the reference result |
| `guest_overlays` | A program in four modules, two of them at one address, translated as a whole game is | Correct with either overlay loaded and when swapping between them; a function with one byte changed is interpreted, alone; an overlay with no registered image is interpreted |

In `guest_mixed`, eleven tests are allowed to enter the interpreter and each
of them must; the other fifteen must not, and do not. For the assembly tests
the counters the runtime keeps (hand-backs to translated code, returns
through jumps, calls that returned elsewhere, abandoned call sites) are
checked against the exact number each test should produce.

Each list of expected gaps is checked both ways: a listed test fails if it
gives a wrong result, and also if it stops needing the exception.

## Bugs this found in the translator

Fixed in this repository, each with a test:

- A Thumb-state jump through `mov pc` or `add pc` lost its Thumb marker, so
  the target was treated as ARM code.
- A software interrupt overwrote r14. The standard two-instruction wrapper
  (`swi n; bx lr`) returns through r14, so every such wrapper returned to the
  wrong place. This was invisible while return addresses were not checked.
- A callee that did not return to its call site let the caller continue as if
  it had. With call sites recorded, this is now detected and handled.
- An exception return continued in the instruction set that bit 0 of the
  return address suggested. The saved status register decides it.

Missing from the translator and added, also with tests: registers per
processor mode, and the forms of `ldm` and `stm` that move the User mode
registers.

## On the real game

The owner supplied the game's extracted program files for analysis. They are
kept outside the repository and none of their content appears here; only
counts do. Everything below is translation and compilation. **None of it has
been executed.**

The whole ARM9 side, 7,954,272 bytes in 367 modules (the main program and
366 overlays), translates in about 25 seconds:

| | |
| --- | --- |
| Functions | 17,076, of which 2,511 are Thumb and 14,724 are in overlays |
| Instructions translated | 1,165,200 (1,125,101 distinct; the rest is code reached from two functions) |
| Program bytes covered by those instructions | 4,159,654 of 7,954,272 (52%) |
| Jump tables recognised | 379 |
| Computed jumps left to the runtime | 794 |
| Functions the translator could not finish | 1 |
| Functions that other modules call directly | 3,287 |
| Output | 3.5 million lines of C++ in 395 files |

All 395 files compile, and the result links against the runtime with a table
of 17,076 functions and 18,702 code runs.

What the numbers say:

- **Following calls between modules matters.** Translating each module from
  its own entry points alone finds 14,242 functions. Feeding call targets
  from one module into the others finds 2,834 more, including 530 in the main
  program that only overlays call. Coverage of the main program rises from
  63% to 77% of its bytes.
- **Entry points offered for shared addresses need care.** Where several
  overlays share an address, a call target seen in one module was first added
  to all of them. That decoded data as code and translated the same code many
  times over: 1.9 million instructions for the same 1.1 million distinct
  ones. Taking an offered entry point only where it is not already inside
  that overlay's code, and only if it reads as complete code, removed the
  waste and lost almost no coverage.
- **Half of the bytes are not covered.** Some of that is data. The rest is
  code that nothing found leads to: 162 of the 366 overlays have less than
  30% of their bytes covered. Their code is presumably reached through
  pointers held elsewhere. That code will be interpreted, and the
  interpreter's entry counts will show where the translator should look.
- The game is mostly ARM-state code, with Thumb in parts of some overlays.

## Limits

- Nothing here runs the game yet.
- Code that rewrites itself in place is not detected (see Overlays).
- Comparing an overlay translation with memory reads the bytes one at a time
  through the memory interface. Correct, and not repeated while memory is
  unchanged, but slow for large functions on first use.
- Interrupts are not delivered.
- An exception return in translated code leaves translated code: what follows
  it is interpreted until the next call into translated code. Correct, and
  the test checks it; what it costs depends on how interrupts are delivered,
  which is not built.
- At most 4,096 nested call sites are recorded; beyond that the runtime
  stops with an error.
- The interpreter's search of the waiting call sites runs on every taken
  branch and is linear in the nesting depth.
- The Windows build of the interpreter is checked only by CI.
