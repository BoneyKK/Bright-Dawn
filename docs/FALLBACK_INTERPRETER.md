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

## What the tests show

All on the guest corpus (`tests/guest`): 23 small programs, each built for
ARM state and for Thumb state, each run on 2,484 input pairs and compared
with the same program built natively. Fifteen are compiled C; eight are
hand-written assembly for the control flow in the table above and for
software interrupts.

| Test | What runs | Result |
| --- | --- | --- |
| `guest_interpreter` | The interpreter alone | All 46 cases give the reference result, except the software-interrupt test, which needs the runtime |
| `guest_translated` | Translated code alone | 13 tests complete; 10 stop at something the translator does not cover; none gives a wrong result |
| `guest_mixed` | Translated code with the interpreter behind it | All 46 cases give the reference result |

In `guest_mixed`, nine tests are allowed to enter the interpreter and each of
them must; the other fourteen must not, and do not. For the assembly tests
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

## On the real game

The owner supplied the game's extracted program files for analysis. They are
kept outside the repository and none of their content appears here; only
counts do.

The ARM9 main program (507,960 bytes) consists of the main block at
`0x02000000` (496,192 bytes) and two blocks that the start-up code copies
elsewhere: 11,584 bytes to `0x01FF8000` and 160 bytes to `0x027E0000`.

| | |
| --- | --- |
| Functions found | 1,822, of which 8 are Thumb |
| Instructions | 79,902 |
| Jump tables recognised | 63, in 58 functions |
| Computed jumps not followed | 69, all in ARM state |
| Calls through a register | 577 |
| Software interrupts | 2 |
| Coprocessor instructions | 47 |
| Bytes covered by the instructions found | 319,486 of 507,936 |

Translated with this repository's translator, that is 266,684 lines of C++
with 6,140 call sites, 629 calls and 131 jumps that go through the runtime's
dispatcher. All of it compiles and links against the runtime. It has not been
executed: there is no memory map, hardware or overlay handling to run it in.

What the numbers say:

- The game's main program is ARM-state code almost throughout, so the
  unrecognised Thumb jump tables seen in the test corpus matter little here.
- About 650 places decide their target at run time (577 calls through a
  register and 69 computed jumps). Whether those targets have translations is
  what determines how much the interpreter does, and that can only be
  measured by running.
- A third of the bytes are not covered by any instruction found. Some is
  data; some is code the translator did not reach. The interpreter's entry
  counts will show which.

The 366 overlays have not been analysed: the files alone do not say where
each one loads. `bdtool extract --code` now writes a manifest with that
information.

## Limits

- Nothing here runs the game yet.
- Overlays: several are translated for the same address, and the runtime
  takes one table keyed by address. Choosing by what is actually loaded is
  not built.
- Processor modes: the processor model has one set of registers. Code that
  switches mode does not see separate stack pointers per mode, translated or
  interpreted.
- Interrupts are not delivered.
- At most 4,096 nested call sites are recorded; beyond that the runtime
  stops with an error.
- The interpreter's search of the waiting call sites runs on every taken
  branch and is linear in the nesting depth.
- The Windows build of the interpreter is checked only by CI.
