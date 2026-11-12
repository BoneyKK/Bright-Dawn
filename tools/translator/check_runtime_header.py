#!/usr/bin/env python3
"""Fail if src/runtime/ds_cpu.h differs from the header the translator emits.

The translated code and the runtime must agree on the processor model. The
runtime is built from the checked-in header; this keeps it identical to the
one inside the translator (arm_lift.RUNTIME_HEADER).

usage: check_runtime_header.py <path to ds_cpu.h>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dslab import arm_lift  # noqa: E402

path = Path(sys.argv[1])
current = path.read_text(encoding="utf-8")
if current != arm_lift.RUNTIME_HEADER:
    print(f"{path} is out of date: regenerate it from arm_lift.RUNTIME_HEADER")
    sys.exit(1)
print("ds_cpu.h matches the translator")
