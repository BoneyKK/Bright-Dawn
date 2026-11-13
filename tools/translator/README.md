# Code translator

Turns Nintendo DS machine code (ARMv5TE, ARM and Thumb) into C++: each
instruction becomes a few statements acting on a model of the processor, and
each function becomes `void f_<address>(ds::Cpu&)`. This is static binary
translation, not decompilation: the output is exact and compilable, not
readable.

## Origin

`dslab/arm_decode.py`, `dslab/arm_analysis.py` and `dslab/arm_lift.py` come
from DS Asset Lab 0.15.0, an earlier project by the owner of this repository,
who added them here and placed them under this project's licence. They were
imported unmodified in their own commit; every later change is in the history
of this repository.

In DS Asset Lab's own tests the decoder was checked against Capstone, and the
translation of about 10,000 random instruction sequences and of 485 leaf
functions of a real game against the Unicorn emulator. Those tests are not
part of this repository; the tests here compare whole translated programs
with reference results instead (`tests/guest`).

## Changes since the import

Made in this repository, each needed to run the translated code together with
the fallback interpreter (`src/runtime`), and each covered by `tests/guest`:

- **Call sites register themselves.** Every call is wrapped in `ds::enter` and
  `ds::leave`, which record the return address and stack pointer the call
  site is waiting for. The runtime uses that list to recognise when control
  has come back to translated code, and `ds::leave` tells a function to stop
  when control has gone to an outer call site instead. Before this, a callee
  that did not return to its call site made the caller carry on as if it had,
  silently.
- **Computed jumps are distinct from calls.** They go to `ds::jump`, not
  `ds::call`, so the runtime knows whether a return address was set.
- **A jump made by a data-processing instruction keeps its instruction set.**
  `mov pc, rX` and `add pc, ...` in Thumb code produced a target without the
  Thumb marker, so it was looked up as ARM code.
- **A software interrupt no longer overwrites r14.** The processor saves the
  return address in the supervisor mode's own register; the usual wrapper
  (`swi n; bx lr`) depends on the program's r14 surviving.
- **Each processor mode has its own registers.** The processor model had one
  r13, r14 and SPSR for all modes. It now keeps a set per mode (and FIQ's
  r8 to r12) and exchanges them when the mode changes, through `msr` or an
  exception return. Start-up code gives each mode its stack this way.
- **`ldm` and `stm` of the User mode registers (`^` without pc) are
  translated.** They were emitted as "not translated". Interrupt handlers
  that switch threads save and restore a whole context with them.
- **An exception return takes its instruction set from the saved status
  register.** `movs pc, lr` and `ldm ..., {..., pc}^` continued in the state
  bit 0 of the address suggested, which is not what selects it.
- **`msr` cannot change the Thumb bit, and does nothing to the control bits
  in User mode.**

## Use

`translate_game.py <code directory> <output directory> [--jobs N]` translates
a whole game from what `bdtool extract --code` writes: the main program, the
overlay files and `manifest.txt`. Its header comment explains the two rules
that matter when code is spread over modules that share addresses. It writes
`report.txt` with what it found, module by module. The output is derived from
the game: keep it out of any repository.

`translate_image.py` translates a single flat memory image whose first words
are a table of function addresses (bit 0 set for Thumb), ended by a zero. That
is the form of the guest test image.

Python 3.10 or later; no third-party packages.
