#!/usr/bin/env python3
"""Build the game from your own ROM: Golden Sun: Dark Dawn with its code
translated to C++ and compiled for this computer.

usage: build_game.py <rom> [--work DIR] [--jobs N] [--config NAME]
                     [--generator NAME] [--sdl-source DIR] [--cmake-arg ARG ...]
                     [--stop-on-stdin-eof]

Steps, each announced as "== N/5. <name>" and reported with the time it
took:

  1. Build bdtool from this source tree.
  2. Check the ROM (bdtool validate).
  3. Extract its program code (bdtool extract --code-only).
  4. Translate the code to C++ (tools/translator/translate_game.py). Only
     files whose text changed are replaced, so a second build recompiles
     only what changed.
  5. Build bright-dawn-game, the window with the translated code linked in.

Everything is written to the work directory: by default "game-build" in
Bright Dawn's per-user data directory (the one bdtool uses), with a record
of the whole build, everything printed by it and by the programs it runs, in
build.log there. The extracted
and translated code is the game's own, derived from your ROM: it stays on
your computer. The work directory may not be inside this source tree, so that
none of it can be committed by mistake.

Needs Python 3.10 or later, CMake 3.20 or later and a C++20 compiler, as for
building the rest of the project, and SDL 3, which the build downloads unless
--sdl-source names an unpacked copy of release 3.2.26.

--stop-on-stdin-eof is for a program that runs the builder and reads its
output (Bright Dawn's own builder screen): the build stops, with the
programs it started, when the standard input is closed, so that it stops
when that program asks it to or ends.
"""
import argparse
import filecmp
import os
import platform
import shutil
import signal
import subprocess
import sys
import threading
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


class Output:
    """What the build prints: to the standard output and to build.log."""

    def __init__(self) -> None:
        self.log = None

    def open_log(self, path: Path) -> None:
        self.log = open(path, "w", encoding="utf-8", errors="replace", newline="\n")

    def say(self, text: str = "", error: bool = False) -> None:
        stream = sys.stderr if error else sys.stdout
        stream.write(text + "\n")
        stream.flush()
        if self.log is not None:
            self.log.write(text + "\n")
            self.log.flush()


OUT = Output()


class Stopper:
    """With --stop-on-stdin-eof: when the standard input ends, the program
    running now is stopped with every program it started, and so is the
    build."""

    def __init__(self) -> None:
        self.enabled = False
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.current = None

    def watch(self) -> None:
        self.enabled = True

        def wait_for_end() -> None:
            # The file descriptor itself, not sys.stdin: a thread blocked in
            # sys.stdin's read holds its lock, and Python aborts if it is held
            # when the program ends.
            try:
                while os.read(sys.stdin.fileno(), 4096):
                    pass
            except (OSError, ValueError):
                pass
            self.stopped.set()
            with self.lock:
                if self.current is not None:
                    stop_tree(self.current)

        threading.Thread(target=wait_for_end, daemon=True).start()

    def check(self) -> None:
        if self.stopped.is_set():
            raise BuildError("the build was stopped")


STOP = Stopper()


def stop_tree(process: subprocess.Popen) -> None:
    """Ends a program and every program it started."""
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run(command: list, what: str) -> None:
    STOP.check()
    OUT.say("  $ " + " ".join(str(c) for c in command))
    options = {}
    if STOP.enabled:
        # A group of its own, so that it can be stopped with what it starts.
        if os.name == "nt":
            options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options["start_new_session"] = True
    process = subprocess.Popen([str(c) for c in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, **options)
    with STOP.lock:
        STOP.current = process
    if STOP.stopped.is_set():
        stop_tree(process)
    try:
        for raw in process.stdout:
            OUT.say(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        status = process.wait()
    finally:
        with STOP.lock:
            STOP.current = None
    STOP.check()
    if status != 0:
        raise BuildError(f"{what} failed (exit status {status})")


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
    parser.add_argument("--stop-on-stdin-eof", action="store_true",
                        help="stop the build when the standard input is closed")
    args = parser.parse_args(argv)
    if args.stop_on_stdin_eof:
        STOP.watch()

    if sys.version_info < (3, 10):
        OUT.say("error: Python 3.10 or later is needed", error=True)
        return 1
    if args.jobs < 1:
        OUT.say("error: --jobs must be 1 or more", error=True)
        return 1
    if shutil.which("cmake") is None:
        OUT.say("error: CMake was not found; install CMake 3.20 or later", error=True)
        return 1
    rom = args.rom.resolve()
    if not rom.is_file():
        OUT.say(f"error: {rom} is not a file", error=True)
        return 1

    try:
        work = (args.work if args.work else user_data_dir() / "game-build").resolve()
        if inside(work, REPO) or inside(REPO, work):
            raise BuildError(f"the work directory {work} overlaps the source tree {REPO}: the game's code "
                             "must not be written where it could be committed")
        work.mkdir(parents=True, exist_ok=True)
        OUT.open_log(work / "build.log")
        OUT.say(f"Bright Dawn game builder, {time.strftime('%Y-%m-%d %H:%M:%S')}")
        OUT.say(f"Python {platform.python_version()} on {platform.platform()}")
        OUT.say(f"Source: {REPO}")
        OUT.say(f"ROM: {rom}")
        OUT.say(f"Work directory: {work}")
        steps = []
        total = 5

        def step(name: str, action) -> None:
            STOP.check()
            OUT.say(f"\n== {len(steps) + 1}/{total}. {name}")
            began = time.monotonic()
            action()
            STOP.check()
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
            OUT.say(f"  {written} file(s) written, {removed} removed, {unchanged} unchanged")

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
        assert len(steps) == total
    except BuildError as error:
        OUT.say("")
        OUT.say(f"error: {error}", error=True)
        return 1
    except KeyboardInterrupt:
        OUT.say("")
        OUT.say("error: the build was stopped", error=True)
        return 130

    OUT.say("\nDone:")
    for name, seconds in steps:
        OUT.say(f"  {name}: {seconds:.0f} s")
    OUT.say(f"\nThe game: {result[0]}")
    OUT.say(f"Run it with your ROM: \"{result[0]}\" \"{rom}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
