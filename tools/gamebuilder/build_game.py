#!/usr/bin/env python3
"""Build the game from your own ROM: Golden Sun: Dark Dawn with its code
translated to C++ and compiled for this computer.

usage: build_game.py <rom> [--work DIR] [--jobs N] [--config NAME]
                     [--generator NAME] [--sdl-source DIR] [--cmake-arg ARG ...]

Steps, each reported with the time it took:

  1. Build bdtool from this source tree.
  2. Check the ROM (bdtool validate).
  3. Extract its program code (bdtool extract --code-only).
  4. Translate the code to C++ (tools/translator/translate_game.py). Only
     files whose text changed are replaced, so a second build recompiles
     only what changed.
  5. Build bright-dawn-game, the window with the translated code linked in.

Everything is written to the work directory: by default "game-build" in
Bright Dawn's per-user data directory (the one bdtool uses). The extracted
and translated code is the game's own, derived from your ROM: it stays on
your computer. The work directory may not be inside this source tree, so that
none of it can be committed by mistake.

Needs Python 3.10 or later, CMake 3.20 or later and a C++20 compiler, as for
building the rest of the project, and SDL 3, which the build downloads unless
--sdl-source names an unpacked copy of release 3.2.26.
"""
import argparse
import filecmp
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXE = ".exe" if os.name == "nt" else ""


class BuildError(RuntimeError):
    pass


def user_data_dir() -> Path:
    """As src/platform/paths.cpp decides it."""
    system = platform.system()
    if system == "Windows":
        base = os.environ.get("LOCALAPPDATA")
        if not base:
            raise BuildError("LOCALAPPDATA is not set; give --work")
        return Path(base) / "BrightDawn"
    if system == "Darwin":
        return Path.home() / "Library" / "Application Support" / "BrightDawn"
    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg and Path(xdg).is_absolute():
        return Path(xdg) / "bright-dawn"
    return Path.home() / ".local" / "share" / "bright-dawn"


def inside(path: Path, folder: Path) -> bool:
    try:
        path.resolve().relative_to(folder.resolve())
        return True
    except ValueError:
        return False


def run(command: list, what: str) -> None:
    print("  $ " + " ".join(str(c) for c in command), flush=True)
    result = subprocess.run([str(c) for c in command])
    if result.returncode != 0:
        raise BuildError(f"{what} failed (exit status {result.returncode})")


def find_program(build_dir: Path, name: str, config: str) -> Path:
    """Single-configuration generators put programs in the build directory,
    multi-configuration ones (Visual Studio) in a folder per configuration."""
    for candidate in (build_dir / (name + EXE), build_dir / config / (name + EXE)):
        if candidate.is_file():
            return candidate
    raise BuildError(f"{name}{EXE} was not found in {build_dir}")


def configure(build_dir: Path, args, options: list) -> list:
    command = ["cmake", "-S", REPO, "-B", build_dir, f"-DCMAKE_BUILD_TYPE={args.config}", "-DBD_BUILD_TESTS=OFF"]
    if args.generator:
        command += ["-G", args.generator]
    if args.sdl_source:
        command.append(f"-DFETCHCONTENT_SOURCE_DIR_SDL3={Path(args.sdl_source).resolve()}")
    return command + options + list(args.cmake_arg)


def sync_tree(new: Path, old: Path) -> tuple:
    """Makes `old` hold what `new` holds, replacing only files whose bytes
    differ, so that their modification times say what changed. Returns
    (written, removed, unchanged)."""
    written = removed = unchanged = 0
    wanted = set()
    for source in sorted(new.rglob("*")):
        if not source.is_file():
            continue
        relative = source.relative_to(new)
        wanted.add(relative)
        target = old / relative
        if target.is_file() and filecmp.cmp(source, target, shallow=False):
            unchanged += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        written += 1
    if old.exists():
        for target in sorted(old.rglob("*"), reverse=True):
            relative = target.relative_to(old)
            if target.is_file() and relative not in wanted:
                target.unlink()
                removed += 1
            elif target.is_dir() and not any(target.iterdir()):
                target.rmdir()
    return written, removed, unchanged


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("rom", type=Path, help="your Golden Sun: Dark Dawn ROM")
    parser.add_argument("--work", type=Path, help="work directory (default: game-build in the user data directory)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel jobs (default: all processors)")
    parser.add_argument("--config", default="Release", help="build configuration (default: Release)")
    parser.add_argument("--generator", help="CMake generator, if not the default")
    parser.add_argument("--sdl-source", help="an unpacked SDL 3.2.26 source tree, instead of downloading it")
    parser.add_argument("--cmake-arg", action="append", default=[], help="passed to both CMake configurations")
    args = parser.parse_args(argv)

    if sys.version_info < (3, 10):
        print("error: Python 3.10 or later is needed", file=sys.stderr)
        return 1
    if args.jobs < 1:
        print("error: --jobs must be 1 or more", file=sys.stderr)
        return 1
    if shutil.which("cmake") is None:
        print("error: CMake was not found; install CMake 3.20 or later", file=sys.stderr)
        return 1
    rom = args.rom.resolve()
    if not rom.is_file():
        print(f"error: {rom} is not a file", file=sys.stderr)
        return 1

    try:
        work = (args.work if args.work else user_data_dir() / "game-build").resolve()
        if inside(work, REPO) or inside(REPO, work):
            raise BuildError(f"the work directory {work} overlaps the source tree {REPO}: the game's code "
                             "must not be written where it could be committed")
        work.mkdir(parents=True, exist_ok=True)
        print(f"Work directory: {work}")
        steps = []

        def step(name: str, action) -> None:
            print(f"\n== {len(steps) + 1}. {name}", flush=True)
            began = time.monotonic()
            action()
            steps.append((name, time.monotonic() - began))

        tools = work / "tools"
        bdtool = []

        def build_tools() -> None:
            run(configure(tools, args, ["-DBD_BUILD_APP=OFF"]), "configuring bdtool")
            run(["cmake", "--build", tools, "--config", args.config, "--target", "bdtool", "--parallel", args.jobs],
                "building bdtool")
            bdtool.append(find_program(tools, "bdtool", args.config))

        code = work / "code"

        def extract() -> None:
            if code.exists():
                shutil.rmtree(code)
            run([bdtool[0], "extract", rom, "--code-only", "--out", work], "extracting the program code")
            if not (code / "manifest.txt").is_file():
                raise BuildError(f"no manifest.txt in {code}")

        translated = work / "translated"

        def translate() -> None:
            fresh = work / "translated.new"
            if fresh.exists():
                shutil.rmtree(fresh)
            run([sys.executable, REPO / "tools" / "translator" / "translate_game.py", code, fresh,
                 "--jobs", args.jobs], "translating")
            written, removed, unchanged = sync_tree(fresh, translated)
            shutil.rmtree(fresh)
            print(f"  {written} file(s) written, {removed} removed, {unchanged} unchanged")

        game = work / "game"
        result = []

        def build_game() -> None:
            run(configure(game, args, [f"-DBD_TRANSLATED_DIR={translated}"]), "configuring the game")
            run(["cmake", "--build", game, "--config", args.config, "--target", "bright-dawn-game",
                 "--parallel", args.jobs], "building the game")
            result.append(find_program(game, "bright-dawn-game", args.config))

        step("Building bdtool", build_tools)
        step("Checking the ROM", lambda: run([bdtool[0], "validate", rom], "checking the ROM"))
        step("Extracting the program code", extract)
        step("Translating it to C++", translate)
        step("Building the game", build_game)
    except BuildError as error:
        print(f"\nerror: {error}", file=sys.stderr)
        return 1

    print("\nDone:")
    for name, seconds in steps:
        print(f"  {name}: {seconds:.0f} s")
    print(f"\nThe game: {result[0]}")
    print(f"Run it with your ROM: \"{result[0]}\" \"{rom}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
