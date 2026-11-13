#!/usr/bin/env python3
"""Write manifest.txt for the overlay test program, in the form bdtool writes it.

usage: make_manifest.py <code directory>
"""
import sys
from pathlib import Path

MAIN_LOAD = 0x02000000
OVERLAYS = [(0, 0x02010000), (1, 0x02010000), (2, 0x02020000)]  # id, load address

code = Path(sys.argv[1])
lines = ["# Bright Dawn code manifest, version 1",
         f"arm9 load=0x{MAIN_LOAD:08x} entry=0x{MAIN_LOAD:08x} file=arm9.bin"]
for number, load in OVERLAYS:
    name = f"overlay9_{number:04d}.bin"
    size = (code / name).stat().st_size
    # No static initialisers: these overlays are entered only from outside.
    lines.append(f"overlay9 id={number} load=0x{load:08x} size={size} bss=0 "
                 f"init_start=0x{load:08x} init_end=0x{load:08x} file={name}")
(code / "manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
