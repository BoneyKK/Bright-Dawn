#!/usr/bin/env python3
"""Translate a whole game's ARM9 program code to C++.

usage: translate_game.py <code directory> <output directory> [--jobs N]

The code directory is what `bdtool extract --code` writes: arm9.bin, the
overlay files and manifest.txt, which says where each of them loads.

Each program file becomes a module: the main program (with the blocks its
start-up code copies elsewhere) and one module per overlay. Output:

  <module>/functions.h, <module>/code_NNN.cpp   the translated functions
  table.cpp      every function by address, with the overlay it belongs to
                 and the code bytes it was translated from
  report.txt     what was found, module by module

Two rules matter for correctness and differ from translating one image:

* A function in one module may only be reached from another. Call targets
  that fall outside the calling module are therefore added as entry points to
  every module that could be loaded at that address, until nothing new turns
  up.

* Several overlays can share an address range, though only one is loaded at a
  time. A call is bound directly to a function only when a single module can
  ever occupy the target address. Otherwise it goes through the run-time,
  which picks the translation whose code is actually in memory.
"""
import argparse
import struct
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dslab import arm_analysis, arm_lift  # noqa: E402

FUNCTIONS_PER_FILE = 200
MODULE_MAGIC = b"\x21\x06\xC0\xDE\xDE\xC0\x06\x21"


class ManifestError(ValueError):
    pass


def read_manifest(path: Path) -> dict:
    """{'arm9': {...}, 'overlays': [{...}]} with integer fields."""
    out = {"arm9": None, "overlays": []}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        kind, *pairs = line.split()
        fields = {}
        for pair in pairs:
            key, sep, value = pair.partition("=")
            if not sep:
                raise ManifestError(f"{path}:{number}: expected key=value, got {pair!r}")
            fields[key] = value if key == "file" else int(value, 0)
        try:
            if kind == "arm9":
                out["arm9"] = {k: fields[k] for k in ("load", "entry", "file")}
            elif kind == "overlay9":
                out["overlays"].append({k: fields[k] for k in
                                        ("id", "load", "size", "bss", "init_start", "init_end", "file")})
            elif kind != "arm7":
                raise ManifestError(f"{path}:{number}: unknown entry {kind!r}")
        except KeyError as exc:
            raise ManifestError(f"{path}:{number}: missing field {exc}") from exc
    if out["arm9"] is None:
        raise ManifestError(f"{path}: no arm9 entry")
    return out


def split_arm9(data: bytes, load: int) -> list:
    """[(name, address, bytes)]: the main block and the blocks the start-up code copies elsewhere.

    The SDK start-up parameters (found by their marker words) give a list of
    such blocks: for each, its destination address, size and zero-filled size.
    Their contents follow the main block in the file. Code in them runs at the
    destination address, so that is the address it must be translated for.
    Layout as in DS Asset Lab's arm_code.split_sections.
    """
    whole = [("main", load, data)]
    pos = data.find(MODULE_MAGIC)
    while pos != -1 and (pos % 4 != 0 or pos < 0x1C):
        pos = data.find(MODULE_MAGIC, pos + 4)
    if pos == -1:
        return whole
    params = pos - 0x1C
    list_start, list_end, data_start = struct.unpack_from("<3I", data, params)
    main_len = data_start - load
    if not 0 < main_len <= len(data) or list_start > list_end or not load <= list_start \
            or list_end - load > len(data):
        return whole
    sections = [("main", load, data[:main_len])]
    offset, entry = main_len, list_start - load
    while entry + 12 <= list_end - load:
        address, size, _bss = struct.unpack_from("<3I", data, entry)
        entry += 12
        if offset + size > len(data):
            break
        sections.append((f"autoload_{len(sections)}", address, data[offset:offset + size]))
        offset += size
    return sections


def plan_modules(code_dir: Path) -> list:
    manifest = read_manifest(code_dir / "manifest.txt")
    arm9 = manifest["arm9"]
    modules = [{
        "name": "arm9", "prefix": "f", "overlay": -1,
        "regions": split_arm9((code_dir / arm9["file"]).read_bytes(), arm9["load"]),
        "entries": [(arm9["entry"] & ~1, bool(arm9["entry"] & 1))],
    }]
    for ov in manifest["overlays"]:
        data = (code_dir / ov["file"]).read_bytes()
        if ov["size"]:
            data = data[:ov["size"]]
        # An overlay's known entry points are its static initialisers.
        entries = []
        for address in range(ov["init_start"], ov["init_end"], 4):
            offset = address - ov["load"]
            if 0 <= offset <= len(data) - 4:
                (pointer,) = struct.unpack_from("<I", data, offset)
                if pointer:
                    entries.append((pointer & ~1, bool(pointer & 1)))
        modules.append({"name": f"overlay9_{ov['id']:04d}", "prefix": f"o9_{ov['id']:03d}",
                        "overlay": ov["id"], "regions": [("main", ov["load"], data)], "entries": entries})
    for m in modules:
        m["ranges"] = [(start, start + len(data)) for _, start, data in m["regions"]]
    return modules


def contains(module: dict, address: int) -> bool:
    return any(start <= address < end for start, end in module["ranges"])


def overlap(a: dict, b: dict) -> bool:
    return any(s1 < e2 and s2 < e1 for s1, e1 in a["ranges"] for s2, e2 in b["ranges"])


def image_of(module: dict):
    return arm_analysis.Image([arm_analysis.Region(n, s, d) for n, s, d in module["regions"]])


def decode_cleanly(image, address: int, thumb: bool):
    """The code at `address` if the bytes there read as a complete piece of code, else None.

    Used for call targets that arrive from another module at an address that
    several overlays share. The caller means one of those overlays; in the
    others the same address may hold data or the middle of something else.
    Reading it as code there would add a meaningless translation. Complete
    means: every instruction is defined, and every path ends by transferring
    control and does not run off into bytes that were never decoded.

    Rejecting a real function here costs speed, not correctness: the
    interpreter runs whatever has no translation.
    """
    fn = arm_analysis.explore(image, address, thumb, set())
    if fn.bad or not fn.insns:
        return None
    for insn in fn.insns.values():
        if insn.kind == "undef":
            return None
        if not arm_lift.ends_flow(insn) and insn.end not in fn.insns:
            return None
    return fn


def analyse(module: dict) -> dict:
    """Function starts of one module, and the call targets that leave it."""
    image = image_of(module)
    entries = list(module["entries"])
    functions = arm_analysis.find_functions(image, entries, scan_pointers=True)
    # Entry points offered by other modules for a shared address are taken only
    # where they make sense here: not inside code this module already has (the
    # caller meant a different overlay, and translating from the middle would
    # only duplicate that code), and only if the bytes read as complete code.
    # They are considered lowest address first, each one adding to what is
    # covered, so several that fall in one block do not each translate it.
    covered = set()
    for fn in functions.values():
        covered.update(fn.insns)
    accepted = []
    for key in sorted(module.get("offered", ())):
        if key in functions or key[0] in covered:
            continue
        candidate = decode_cleanly(image, key[0], key[1])
        if candidate is not None:
            accepted.append(key)
            covered.update(candidate.insns)
    if accepted:
        entries += accepted
        functions = arm_analysis.find_functions(image, entries, scan_pointers=True)
    outside = set()
    for fn in functions.values():
        for target in fn.calls | fn.tails:
            if not image.contains(target[0]):
                outside.add(target)
    return {"name": module["name"], "keys": sorted(functions), "outside": sorted(outside),
            "entries": entries}


def function_name(module: dict, key) -> str:
    return f"{module['prefix']}_{key[0]:08x}" + ("t" if key[1] else "")


class Resolver:
    """C++ name of a call target as seen from one module, or None for 'ask the run-time'."""

    def __init__(self, own: dict, direct: dict):
        self.own, self.direct = own, direct

    def __getitem__(self, key):
        return self.own[key]

    def get(self, key, default=None):
        return self.own.get(key) or self.direct.get(key, default)


def code_runs(fn, image) -> list:
    """[(address, length)]: the bytes a translation depends on, merged into runs.

    That is every instruction, and every constant the translation read from
    the image next to the code. The run-time compares these with memory to
    decide whether a translation applies to what is loaded.
    """
    spans = [(a, i.size) for a, i in fn.insns.items()]
    for i in fn.insns.values():
        f = i.f
        if i.kind == "mem" and f["load"] and f["rn"] == 15 and f["size"] == "w" and f["off"][0] == "imm" \
                and f["pre"] and not f["wb"] and f["rd"] != 15:
            base = i.pc_value & ~3 if i.thumb else i.pc_value
            address = base + f["off"][1] if f["up"] else base - f["off"][1]
            if not address & 3 and image.word(address) is not None:
                spans.append((address, 4))
    spans.sort()
    runs = []
    for address, length in spans:
        if runs and address <= runs[-1][0] + runs[-1][1]:
            runs[-1][1] = max(runs[-1][1], address + length - runs[-1][0])
        else:
            runs.append([address, length])
    return [(a, n) for a, n in runs]


def lift(task: dict) -> dict:
    module, out = task["module"], Path(task["out"])
    image = image_of(module)
    functions = arm_analysis.find_functions(image, module["entries"], scan_pointers=True)
    keys = sorted(functions)
    own = {k: function_name(module, k) for k in keys}
    resolver = Resolver(own, {tuple(k): v for k, v in task["direct"]})
    folder = out / module["name"]
    folder.mkdir(parents=True, exist_ok=True)

    called = set()
    for fn in functions.values():
        for target in fn.calls | fn.tails:
            if target not in own and resolver.get(target):
                called.add(resolver.get(target))
    declarations = [f"void {own[k]}(ds::Cpu& c);" for k in keys] + [f"void {n}(ds::Cpu& c);" for n in sorted(called)]
    (folder / "functions.h").write_text(
        f"// {module['name']}: generated by translate_game.py.\n#pragma once\n#include \"ds_cpu.h\"\n"
        + "\n".join(declarations) + "\n", encoding="utf-8", newline="\n")

    files = []
    for n in range(0, len(keys), FUNCTIONS_PER_FILE):
        parts = [f"// {module['name']}: generated by translate_game.py.", '#include "ds_cpu.h"',
                 '#include "functions.h"', "", "using ds::Cpu;", "using namespace ds;", ""]
        parts += [arm_lift.lift_function(functions[k], resolver, image) for k in keys[n:n + FUNCTIONS_PER_FILE]]
        name = f"code_{n // FUNCTIONS_PER_FILE:03d}.cpp"
        (folder / name).write_text("\n".join(parts), encoding="utf-8", newline="\n")
        files.append(f"{module['name']}/{name}")

    return {
        "name": module["name"], "overlay": module["overlay"], "files": files,
        "table": [(k[0] | (1 if k[1] else 0), own[k], code_runs(functions[k], image)) for k in keys],
        "functions": len(keys), "thumb": sum(1 for k in keys if k[1]),
        "instructions": sum(len(f.insns) for f in functions.values()),
        # Code reached from two functions is translated in both, so the count
        # above exceeds the number of distinct instructions.
        "distinct": len({a for f in functions.values() for a in f.insns}),
        "bytes": sum(len(d) for _, _, d in module["regions"]),
        "covered": sum(i.size for i in {a: i for f in functions.values() for a, i in f.insns.items()}.values()),
        "unresolved": sum(len(f.unresolved) for f in functions.values()),
        "tables": sum(len(f.tables) for f in functions.values()),
        "cut_short": sum(1 for f in functions.values() if f.bad),
    }


def run_all(function, tasks: list, jobs: int) -> list:
    if jobs <= 1 or len(tasks) < 4:
        return [function(t) for t in tasks]
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(function, tasks, chunksize=4))


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="Translate a whole game's ARM9 program code to C++.")
    parser.add_argument("code_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args(argv[1:])
    started = time.time()
    try:
        modules = plan_modules(args.code_dir)
    except (OSError, ManifestError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    by_name = {m["name"]: m for m in modules}

    # --- Find the functions, feeding call targets across modules, to a fixed point ---
    found = {}
    pending = list(modules)
    rounds = 0
    while pending:
        rounds += 1
        for result in run_all(analyse, pending, args.jobs):
            found[result["name"]] = result
        additions = defaultdict(set)
        offers = defaultdict(set)
        for m in modules:
            for target in map(tuple, found[m["name"]]["outside"]):
                # A module cannot call into one that occupies its own memory.
                candidates = [o for o in modules
                              if o is not m and contains(o, target[0]) and not overlap(m, o)]
                for other in candidates:
                    if len(candidates) == 1:
                        # Only one module can be meant: the target is a function there.
                        if target not in other["entries"]:
                            additions[other["name"]].add(target)
                    elif target not in other.get("offered", ()):
                        offers[other["name"]].add(target)
        pending = []
        for name in set(additions) | set(offers):
            module = by_name[name]
            module["entries"] = module["entries"] + sorted(additions.get(name, ()))
            module["offered"] = sorted(set(module.get("offered", ())) | offers.get(name, set()))
            pending.append(module)
        if rounds > 20:
            print("error: the set of functions did not settle", file=sys.stderr)
            return 1

    # --- Which targets may be called directly from another module ---
    # Only where exactly one module can ever occupy the address.
    owners = defaultdict(list)
    for m in modules:
        for key in map(tuple, found[m["name"]]["keys"]):
            occupants = [o for o in modules if contains(o, key[0])]
            if len(occupants) == 1:
                owners[key].append(function_name(m, key))
    direct = [(k, v[0]) for k, v in owners.items() if len(v) == 1]

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for m in modules:
        m["entries"] = [tuple(e) for e in found[m["name"]]["entries"]]
    results = run_all(lift, [{"module": m, "out": str(args.out_dir), "direct": direct} for m in modules], args.jobs)

    # --- The table the run-time looks functions up in ---
    lines = ["// Generated by translate_game.py.", '#include "translated_table.h"', ""]
    for r in results:
        lines += [f"void {name}(ds::Cpu& c);" for _, name, _ in r["table"]]
    lines += ["", "namespace {", "const TranslatedRun runs[] = {"]
    entries, run_index = [], 0
    for r in results:
        for address, name, runs in r["table"]:
            lines.append("    " + " ".join(f"{{0x{a:08x}u, {n}u}}," for a, n in runs))
            entries.append((address, r["overlay"], name, run_index, len(runs)))
            run_index += len(runs)
    lines += ["};", "}  // namespace", "", "extern const TranslatedEntry translated_table[] = {"]
    for address, overlay, name, first, count in sorted(entries, key=lambda e: (e[0], e[1])):
        lines.append(f"    {{0x{address:08x}u, {name}, {overlay}, runs + {first}, {count}u}},")
    lines += ["    {0xFFFFFFFFu, nullptr, -1, nullptr, 0u},", "};", ""]
    (args.out_dir / "table.cpp").write_text("\n".join(lines), encoding="utf-8", newline="\n")

    total = {k: sum(r[k] for r in results) for k in
             ("functions", "thumb", "instructions", "distinct", "bytes", "covered", "unresolved", "tables",
              "cut_short")}
    report = [f"modules: {len(results)}", f"rounds of cross-module discovery: {rounds}",
              f"functions: {total['functions']} ({total['thumb']} Thumb)",
              f"instructions translated: {total['instructions']} ({total['distinct']} distinct)",
              f"program bytes covered by them: {total['covered']} of {total['bytes']}",
              f"jump tables recognised: {total['tables']}", f"computed jumps not followed: {total['unresolved']}",
              f"functions cut short: {total['cut_short']}",
              f"functions callable directly from other modules: {len(direct)}",
              f"seconds: {time.time() - started:.1f}", "",
              "module  functions  thumb  instructions  distinct  bytes-covered  bytes  jump-tables  "
              "computed-jumps  cut-short"]
    report += [f"{r['name']}  {r['functions']}  {r['thumb']}  {r['instructions']}  {r['distinct']}  "
               f"{r['covered']}  {r['bytes']}  {r['tables']}  {r['unresolved']}  {r['cut_short']}"
               for r in results]
    (args.out_dir / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(report[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
