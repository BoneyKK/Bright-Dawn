#!/usr/bin/env python3
"""Build the game library from your own ROM: Golden Sun: Dark Dawn's code
translated to C++ and compiled for this computer, for Bright Dawn to load.

usage: build_game.py <rom> --program PATH [--out FILE] [--jobs N]
                     [--compiler COMMAND] [--temp DIR] [--keep-temp]
                     [--stop-on-stdin-eof]
       build_game.py --code DIR --program PATH ...

--program is the Bright Dawn program the library is for (bright-dawn, or
bright-dawn.exe): it checks the ROM and writes out its program code, and it
names the version the library is built for.

Steps, each announced as "== N/5. <name>":

  1. Getting the compiler: Zig, which contains Clang, downloaded from the
     Python Package Index (pinned below, its SHA-256 checked) into the
     temporary folder; or --compiler, a GCC- or Clang-style C++ compiler
     already on this computer (g++, clang++).
  2. Checking the ROM and extracting its program code (the program's
     --extract-code); or, with --code, taking the code already extracted
     (by bdtool extract --code-only, or --extract-code) from DIR.
  3. Translating it to C++ (tools/translator/translate_game.py).
  4. Compiling it, a file at a time, --jobs at once.
  5. Linking the game library: one file, by default bright-dawn-game.dll (or
     .so) in the "game" folder of Bright Dawn's per-user data directory.

Everything else happens in a temporary folder that is removed afterwards,
however the build ends (--keep-temp keeps it): the compiler, the extracted
and translated code (the game's own, from your ROM) and the compiled
objects. A record of the whole build, everything printed by it and by the
programs it runs, is written to build.log next to the library.

Needs Python 3.10 or later, an internet connection for the compiler (about
95 MB) unless --compiler is given, and about 2 GB of free space for the
temporary folder.

--stop-on-stdin-eof is for a program that runs the builder and reads its
output (Bright Dawn's own builder page): the build stops, with the programs
it started, when the standard input is closed, so that it stops when that
program asks it to or ends.
"""
import argparse
import concurrent.futures
import hashlib
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent                      # tools/ in the source tree, or in the package's builder/
RUNTIME = TOOLS.parent / "src" / "runtime"
TRANSLATOR = TOOLS / "translator" / "translate_game.py"
GLUE = RUNTIME / "game_library_glue.cpp"

# The compiler: Zig, as the Zig project publishes it on the Python Package
# Index (https://pypi.org/project/ziglang/), by system and processor.
ZIG_VERSION = "0.16.0"
_PYPI = "https://files.pythonhosted.org/packages/"
ZIG_WHEELS = {
    ("Linux", "x86_64"): (
        _PYPI + "3e/ed/7b79023aa27ceb5d461ecf761181e7c33c57bbc1a6256a39535d1c7083d2/"
        "ziglang-0.16.0-py3-none-manylinux_2_12_x86_64.manylinux2010_x86_64.musllinux_1_1_x86_64.whl",
        "9fcda73f62b851dd72a54b710ad40a209896db14cfb13649e62191243556342b"),
    ("Linux", "aarch64"): (
        _PYPI + "7e/ed/d6663a5e52c504944d578b9e0bfcb7857f292803bcd09ebe0d10fe2b293d/"
        "ziglang-0.16.0-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64.whl",
        "e27d409812b11e0fb89ed0200cf2e55b6464d43f9461553104e4a4f9a94a1fd5"),
    ("Windows", "AMD64"): (
        _PYPI + "a6/3c/baff40b3fc8ab4e83530246a52e8f3a5186c3606562712dfb58483b04f79/"
        "ziglang-0.16.0-py3-none-win_amd64.whl",
        "089a16a4eb5a2f45151993342f8fabad24ff1a0723dc146642895c29208a3939"),
    ("Windows", "ARM64"): (
        _PYPI + "f0/0b/bff28a4acc437cb7da705bffafd81fafbbd37a23144363cecc72becaef93/"
        "ziglang-0.16.0-py3-none-win_arm64.whl",
        "e44a5271f7b72f7980017bb615823ed52e765f96b6482d93a2fd338cd53101bf"),
    ("Darwin", "x86_64"): (
        _PYPI + "75/60/f924aa24b95a1ad347e845acc7ab6b5d062ae5b0b540d494654cd40d4e0b/"
        "ziglang-0.16.0-py3-none-macosx_12_0_x86_64.whl",
        "18e14f6b25678d7b7c65708c82501ee6090fe39ed5c783477d56267af1fa5629"),
    ("Darwin", "arm64"): (
        _PYPI + "cd/59/012f0c2800f7428b87bb16c5c78db7ef806efed274491998155955c02558/"
        "ziglang-0.16.0-py3-none-macosx_12_0_arm64.whl",
        "b61e5413c49508d9d62e5dcea543e3af491594154d74a00ff52f84ed508260cc"),
}

LEAST_FREE_SPACE = 2 * 1024 ** 3
WINDOWS = os.name == "nt"
LIBRARY_NAME = "bright-dawn-game.dll" if WINDOWS else "bright-dawn-game.so"


class BuildError(RuntimeError):
    pass


def user_data_dir() -> Path:
    """As src/platform/paths.cpp decides it."""
    system = platform.system()
    if system == "Windows":
        base = os.environ.get("LOCALAPPDATA")
        if not base:
            raise BuildError("LOCALAPPDATA is not set; give --out")
        return Path(base) / "BrightDawn"
    if system == "Darwin":
        return Path.home() / "Library" / "Application Support" / "BrightDawn"
    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg and Path(xdg).is_absolute():
        return Path(xdg) / "bright-dawn"
    return Path.home() / ".local" / "share" / "bright-dawn"


class Output:
    """What the build prints: to the standard output and to build.log."""

    def __init__(self) -> None:
        self.log = None
        self.lock = threading.Lock()

    def open_log(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.log = open(path, "w", encoding="utf-8", errors="replace", newline="\n")

    def say(self, text: str = "", error: bool = False) -> None:
        with self.lock:
            stream = sys.stderr if error else sys.stdout
            stream.write(text + "\n")
            stream.flush()
            if self.log is not None:
                self.log.write(text + "\n")
                self.log.flush()


OUT = Output()


class Stopper:
    """With --stop-on-stdin-eof: when the standard input ends, the programs
    running now are stopped with every program they started, and so is the
    build."""

    def __init__(self) -> None:
        self.enabled = False
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.running = set()

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
                for process in list(self.running):
                    stop_tree(process)

        threading.Thread(target=wait_for_end, daemon=True).start()

    def check(self) -> None:
        if self.stopped.is_set():
            raise BuildError("the build was stopped")


STOP = Stopper()


def stop_tree(process: subprocess.Popen) -> None:
    """Ends a program and every program it started."""
    if process.poll() is not None:
        return
    if WINDOWS:
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


def start(command: list, env=None, capture=True) -> subprocess.Popen:
    """Starts a program, in a group of its own when the build can be
    stopped, so that it can be stopped with what it starts."""
    STOP.check()
    options = {}
    if STOP.enabled:
        if WINDOWS:
            options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options["start_new_session"] = True
    process = subprocess.Popen([str(c) for c in command], stdout=subprocess.PIPE if capture else None,
                               stderr=subprocess.STDOUT if capture else None, stdin=subprocess.DEVNULL,
                               env=env, **options)
    with STOP.lock:
        STOP.running.add(process)
    if STOP.stopped.is_set():
        stop_tree(process)
    return process


def finish(process: subprocess.Popen) -> int:
    status = process.wait()
    with STOP.lock:
        STOP.running.discard(process)
    STOP.check()
    return status


def run(command: list, what: str, env=None) -> None:
    """Runs a program, its output passed on line by line."""
    OUT.say("  $ " + " ".join(str(c) for c in command))
    process = start(command, env)
    try:
        for raw in process.stdout:
            OUT.say(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
    finally:
        status = finish(process)
    if status != 0:
        raise BuildError(f"{what} failed (exit status {status})")


def output_of(command: list, what: str) -> str:
    process = start(command)
    text = process.stdout.read().decode("utf-8", errors="replace")
    status = finish(process)
    if status != 0:
        raise BuildError(f"{what} failed (exit status {status}): {text.strip()}")
    return text


def download(url: str, sha256: str, target: Path) -> None:
    """Downloads `url` to `target`, reporting how far it is, and checks it."""
    OUT.say(f"  {url}")
    digest = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=60) as response, open(target, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        shown = -1
        while True:
            STOP.check()
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            digest.update(block)
            done += len(block)
            if total:
                percent = done * 100 // total
                if percent // 5 != shown // 5:
                    shown = percent
                    # As Make reports progress, which Bright Dawn's builder page follows.
                    OUT.say(f"[{percent:3d}%] {done // (1 << 20)} of {total // (1 << 20)} MB")
    if digest.hexdigest() != sha256:
        raise BuildError(f"the download is not what was expected (SHA-256 {digest.hexdigest()}, "
                         f"expected {sha256}): try again")


def get_zig(temp: Path) -> tuple:
    """Downloads Zig into `temp`. Returns the command that compiles C++ and
    the environment to run it in."""
    key = (platform.system(), platform.machine())
    if key not in ZIG_WHEELS:
        raise BuildError(f"no compiler can be downloaded for {key[0]} on {key[1]}: give --compiler")
    url, sha256 = ZIG_WHEELS[key]
    wheel = temp / "zig.whl"
    download(url, sha256, wheel)
    folder = temp / "zig"
    with zipfile.ZipFile(wheel) as archive:
        members = [m for m in archive.namelist() if m.startswith("ziglang/")]
        archive.extractall(folder, members)
    wheel.unlink()
    zig = folder / "ziglang" / ("zig.exe" if WINDOWS else "zig")
    if not zig.is_file():
        raise BuildError("the downloaded compiler is not complete")
    if not WINDOWS:
        zig.chmod(0o755)
    env = dict(os.environ)
    # Zig keeps what it builds for itself (its C++ library, for one) in its
    # caches: here, so that they go with the temporary folder.
    env["ZIG_GLOBAL_CACHE_DIR"] = str(temp / "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(temp / "zig-cache")
    OUT.say(f"  Zig {ZIG_VERSION}: {zig}")
    return [str(zig), "c++"], env


def compile_all(compiler: list, env, sources: list, objects: Path, flags: list, jobs: int) -> list:
    """Compiles every source, `jobs` at once. Reports each as Ninja does
    ("[done/total] ..."), which Bright Dawn's builder page follows."""
    objects.mkdir(parents=True, exist_ok=True)
    outputs = []
    work = []
    for n, source in enumerate(sources):
        target = objects / f"{n:04d}_{source.parent.name}_{source.stem}.o"
        outputs.append(target)
        work.append((source, target))
    done = 0
    failure = []
    lock = threading.Lock()

    def one(item) -> None:
        nonlocal done
        source, target = item
        if failure or STOP.stopped.is_set():
            return
        command = compiler + flags + ["-I", str(source.parent), "-c", str(source), "-o", str(target)]
        process = start(command, env)
        text = process.stdout.read().decode("utf-8", errors="replace")
        status = process.wait()
        with STOP.lock:
            STOP.running.discard(process)
        with lock:
            if status != 0 and not STOP.stopped.is_set():
                failure.append((source, status, text))
                return
            done += 1
            OUT.say(f"[{done}/{len(work)}] {source.parent.name}/{source.name}")
            if text.strip():
                OUT.say(text.rstrip())

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(one, work))
    STOP.check()
    if failure:
        source, status, text = failure[0]
        OUT.say(text.rstrip())
        raise BuildError(f"compiling {source.parent.name}/{source.name} failed (exit status {status})")
    return outputs


def remove_folder(folder: Path) -> None:
    """Removes the temporary folder; on Windows a file can stay in use for a
    moment after its program has ended, so it is tried again."""
    for attempt in range(10):
        shutil.rmtree(folder, ignore_errors=True)
        if not folder.exists():
            OUT.say(f"Temporary files removed: {folder}")
            return
        time.sleep(1)
    OUT.say(f"warning: some temporary files could not be removed: {folder}", error=True)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("rom", type=Path, nargs="?", help="your Golden Sun: Dark Dawn ROM")
    parser.add_argument("--code", type=Path, help="program code already extracted, instead of a ROM")
    parser.add_argument("--program", type=Path, required=True, help="the Bright Dawn program the library is for")
    parser.add_argument("--out", type=Path, help=f"the library to write (default: game/{LIBRARY_NAME} in the "
                                                 "user data directory)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="files compiled at once "
                                                                               "(default: all processors)")
    parser.add_argument("--compiler", help="a GCC- or Clang-style C++ compiler to use instead of downloading Zig")
    parser.add_argument("--temp", type=Path, help="where to make the temporary folder (default: the system's)")
    parser.add_argument("--keep-temp", action="store_true", help="keep the temporary folder")
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
    if (args.rom is None) == (args.code is None):
        OUT.say("error: give a ROM, or --code", error=True)
        return 1
    rom = args.rom.resolve() if args.rom else None
    given_code = args.code.resolve() if args.code else None
    program = args.program.resolve()
    if rom is not None and not rom.is_file():
        OUT.say(f"error: {rom} is not a file", error=True)
        return 1
    if given_code is not None and not (given_code / "manifest.txt").is_file():
        OUT.say(f"error: no manifest.txt in {given_code}", error=True)
        return 1
    if not program.is_file():
        OUT.say(f"error: {program} is not a file", error=True)
        return 1

    temp = None
    steps = []
    total = 5
    try:
        out = (args.out if args.out else user_data_dir() / "game" / LIBRARY_NAME).resolve()
        OUT.open_log(out.parent / "build.log")
        OUT.say(f"Bright Dawn game builder, {time.strftime('%Y-%m-%d %H:%M:%S')}")
        OUT.say(f"Python {platform.python_version()} on {platform.platform()} ({platform.machine()})")
        OUT.say(f"Builder: {HERE}")
        OUT.say(f"Program: {program}")
        OUT.say(f"ROM: {rom}" if rom else f"Program code: {given_code}")
        OUT.say(f"Library: {out}")

        version_line = output_of([program, "--version"], "asking the program its version").strip().splitlines()
        if not version_line or not version_line[0].startswith("bright-dawn "):
            raise BuildError(f"{program} did not name its version")
        version = version_line[0][len("bright-dawn "):]
        if '"' in version or "\\" in version:
            raise BuildError(f"the program's version cannot be used: {version}")
        OUT.say(f"For Bright Dawn {version}")

        if args.temp:
            args.temp.mkdir(parents=True, exist_ok=True)
        temp = Path(tempfile.mkdtemp(prefix="bright-dawn-build-", dir=args.temp))
        OUT.say(f"Temporary folder: {temp}")
        free = shutil.disk_usage(temp).free
        if free < LEAST_FREE_SPACE:
            raise BuildError(f"the temporary folder's drive has {free // 1024 ** 2} MB free; "
                             f"about {LEAST_FREE_SPACE // 1024 ** 2} MB are needed")

        def step(name: str, action):
            STOP.check()
            OUT.say(f"\n== {len(steps) + 1}/{total}. {name}")
            began = time.monotonic()
            result = action()
            STOP.check()
            steps.append((name, time.monotonic() - began))
            return result

        def get_compiler():
            if args.compiler:
                text = output_of([args.compiler, "--version"], f"running the compiler {args.compiler}")
                OUT.say("  " + text.strip().splitlines()[0])
                return [args.compiler], None
            return get_zig(temp)

        compiler, env = step("Getting the compiler", get_compiler)
        if given_code is None:
            code = temp / "code"
            step("Checking the ROM and extracting its program code",
                 lambda: run([program, "--extract-code", rom, code], "extracting the program code"))
        else:
            code = given_code
            step("Taking the program code", lambda: OUT.say(f"  {code}"))
        translated = temp / "translated"
        step("Translating it to C++",
             lambda: run([sys.executable, TRANSLATOR, code, translated, "--jobs", args.jobs], "translating"))
        if given_code is None:
            # The game's code is no longer needed once translated.
            shutil.rmtree(code, ignore_errors=True)

        sources = sorted(translated.glob("*/code_*.cpp")) + [translated / "table.cpp", GLUE]
        flags = ["-std=c++20", "-O2", "-w", "-fno-exceptions", "-fno-rtti", "-fvisibility=hidden",
                 "-I", str(RUNTIME), f'-DBD_GAME_BUILT_FOR="{version}"']
        if not WINDOWS:
            flags.append("-fPIC")
        objects = step(f"Compiling it ({len(sources)} files)",
                       lambda: compile_all(compiler, env, sources, temp / "objects", flags, args.jobs))

        def link() -> None:
            linked = temp / LIBRARY_NAME
            # The objects go in a response file: listed on the command line
            # they would pass Windows' limit of 32767 characters. Its quoting
            # is the GNU one, where a backslash escapes: forward slashes.
            listing = temp / "objects.rsp"
            listing.write_text("\n".join('"' + o.as_posix().replace('"', '\\"') + '"' for o in objects) + "\n",
                               encoding="utf-8")
            command = compiler + ["-shared", "-o", str(linked), "@" + str(listing)]
            if platform.system() == "Linux":
                # Every symbol defined: the library needs nothing of the
                # program but what it is handed when it is loaded.
                command.append("-Wl,--no-undefined")
            OUT.say(f"  linking {len(objects)} objects")
            run(command, "linking the game library", env)
            out.parent.mkdir(parents=True, exist_ok=True)
            fresh = out.with_name(out.name + ".new")
            shutil.copyfile(linked, fresh)
            os.replace(fresh, out)

        step("Linking the game library", link)
    except BuildError as error:
        OUT.say("")
        OUT.say(f"error: {error}", error=True)
        return 1
    except KeyboardInterrupt:
        OUT.say("")
        OUT.say("error: the build was stopped", error=True)
        return 130
    except OSError as error:
        OUT.say("")
        OUT.say(f"error: {error}", error=True)
        return 1
    finally:
        if temp is not None:
            if args.keep_temp:
                OUT.say(f"Temporary folder kept: {temp}")
            else:
                remove_folder(temp)

    OUT.say("\nDone:")
    for name, seconds in steps:
        OUT.say(f"  {name}: {seconds:.0f} s")
    OUT.say(f"\nThe game: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
