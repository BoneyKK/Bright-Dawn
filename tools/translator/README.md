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

## Use

`translate_image.py` translates a flat memory image whose first words are a
table of function addresses (bit 0 set for Thumb), ended by a zero. That is
the form of the guest test image. A driver for a whole game (main program,
autoload blocks and overlays) is not here yet.

Python 3.10 or later; no third-party packages.
