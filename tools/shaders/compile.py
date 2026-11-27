#!/usr/bin/env python3
"""Compiles the graphics hardware's shaders (src/app/shaders/*.vert, *.frag,
*.comp)
to SPIR-V with glslangValidator, into src/app/hardware_shaders.h.

The header is kept in the repository, so a build needs no shader compiler.
Run this after a change to a shader; `--check` says whether the header is
as the shaders make it.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS = ROOT / "src" / "app" / "shaders"
HEADER = ROOT / "src" / "app" / "hardware_shaders.h"
NAMES = ["scene.vert", "scene.frag", "compose.vert", "compose.frag", "raster.comp", "finish.comp", "engine2d.frag", "edges.frag", "nearby.frag", "outline.frag", "resolve.frag"]


def compiled(name: str) -> bytes:
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch) / "shader.spv"
        subprocess.run(["glslangValidator", "-V", "--quiet", str(SHADERS / name), "-o", str(out)], check=True)
        return out.read_bytes()


def header() -> str:
    lines = [
        "// Made by tools/shaders/compile.py from src/app/shaders: do not edit.",
        "// SPIR-V for the graphics hardware (app/hardware_3d.h).",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace bd::app::shaders {",
        "",
    ]
    for name in NAMES:
        data = compiled(name)
        symbol = "k" + "".join(part.capitalize() for part in name.replace(".", "_").split("_"))
        lines.append(f"inline constexpr std::uint8_t {symbol}[] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace bd::app::shaders")
    return "\n".join(lines) + "\n"


def main() -> int:
    text = header()
    if "--check" in sys.argv[1:]:
        if HEADER.read_text() != text:
            print(f"{HEADER} is not as the shaders make it: run tools/shaders/compile.py")
            return 1
        return 0
    HEADER.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
