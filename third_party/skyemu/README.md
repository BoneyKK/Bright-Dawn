# SkyEmu ARM core

`arm7.h` is the ARM7TDMI / ARM946E-S interpreter of SkyEmu
(https://github.com/skylersaleh/SkyEmu), copied unmodified from commit
`f4d3238bf42da483ed1b7cc35649398b2110e4b4` (24 October 2026). It is licensed
under the MIT licence; see `LICENSE` in this directory.

It is used as the fallback interpreter for guest code that has no translation.
All use goes through `src/runtime/interp/arm_core_skyemu.c`, which supplies the
four macros the header expects from elsewhere in SkyEmu and adapts its
pipeline-stepped execution to "run exactly one instruction".

To update: replace `arm7.h` with the upstream file, record the new commit
here, and run the tests; `guest_interpreter` runs the whole guest corpus
through this core.
