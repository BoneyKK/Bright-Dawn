"""
arm_analysis.py - find the functions in decoded DS code.

How (recursive descent, the usual method of disassemblers):
  1. Start at the known entry points (the program's entry address, and the
     static initialisers of overlays) and follow the code: every BL/BLX
     target is a new function; conditional branches continue on both paths;
     unconditional branches, returns and computed jumps end a path.
  2. Words in the data that point into the code and look like the start of
     a function (it saves registers with PUSH/STMDB and LR) are added as
     extra entry points - that finds functions called through pointers.
  3. Repeat until nothing new is found. A branch to the start of another
     function is a "tail call".

Jump tables the ARM compiler emits ("ldrls pc, [pc, rX, lsl #2]" followed by
the table, or "addls pc, pc, rX, lsl #2" followed by branches) are read, so
their targets become part of the function. Other computed jumps are left to
the run-time dispatcher of the generated C++ (see arm_lift.py).

Everything not reached is treated as data in the listings.
"""

from dataclasses import dataclass, field

from .arm_decode import Insn, decode_at

MAX_FUNC_INSNS = 60000


@dataclass
class Region:
    """A piece of memory with code/data: one section of a program file."""
    name: str
    start: int
    data: bytes

    @property
    def end(self) -> int:
        return self.start + len(self.data)


class Image:
    """All regions of one module, for decoding by address."""

    def __init__(self, regions: list):
        self.regions = sorted(regions, key=lambda r: r.start)
        self.cache: dict = {}

    def region(self, addr: int) -> Region | None:
        for r in self.regions:
            if r.start <= addr < r.end:
                return r
        return None

    def contains(self, addr: int) -> bool:
        return self.region(addr) is not None

    def word(self, addr: int) -> int | None:
        r = self.region(addr)
        if r is None or addr + 4 > r.end:
            return None
        o = addr - r.start
        d = r.data
        return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24

    def half(self, addr: int) -> int | None:
        r = self.region(addr)
        if r is None or addr + 2 > r.end:
            return None
        o = addr - r.start
        return r.data[o] | r.data[o + 1] << 8

    def decode(self, addr: int, thumb: bool) -> Insn | None:
        key = (addr, thumb)
        if key not in self.cache:
            r = self.region(addr)
            self.cache[key] = None if r is None or (addr & (1 if thumb else 3)) else \
                decode_at(r.data, addr - r.start, addr, thumb)
        return self.cache[key]


@dataclass
class Function:
    addr: int
    thumb: bool
    insns: dict = field(default_factory=dict)        # address -> Insn
    labels: set = field(default_factory=set)         # addresses that are branch targets inside
    calls: set = field(default_factory=set)          # (target, thumb) called with BL/BLX
    tails: set = field(default_factory=set)          # (target, thumb) jumped to as tail call
    tables: dict = field(default_factory=dict)       # address of a computed jump -> [targets]
    unresolved: set = field(default_factory=set)     # addresses of computed jumps we could not follow
    bad: str = ""                                    # why the function was cut short

    @property
    def name(self) -> str:
        return f"f_{self.addr:08x}"


def is_return(i: Insn) -> bool:
    """bx lr / mov pc, lr / pop {..., pc} / ldm sp!, {..., pc} / ldr pc, [sp], #4."""
    f = i.f
    if i.kind == "bx" and not f["link"] and f["rm"] == 14:
        return True
    if i.kind == "dp" and f["op"] == 13 and f["rd"] == 15 and f["op2"] == ("sh", 14, 0, 0) and not f["s"]:
        return True
    if i.kind == "block" and f["load"] and 15 in f["regs"] and f["rn"] == 13:
        return True
    if i.kind == "mem" and f["load"] and f["rd"] == 15 and f["rn"] == 13 and f["size"] == "w" and not f["pre"]:
        return True
    return False


def writes_pc(i: Insn) -> bool:
    f = i.f
    if i.kind == "dp":
        return f["rd"] == 15 and not 8 <= f["op"] <= 11
    if i.kind == "mem":
        return f["load"] and f["rd"] == 15
    if i.kind == "block":
        return f["load"] and 15 in f["regs"]
    if i.kind in ("mul", "clz", "qarith", "mrs"):
        return f.get("rd") == 15
    return False


def _jump_table(img: Image, i: Insn) -> list | None:
    """Targets of "ldr pc, [pc, rX, lsl #2]" / "add pc, pc, rX, lsl #2" tables (ARM only)."""
    if i.thumb:
        return None
    f = i.f
    if i.cond == 14:
        # unconditional "add pc, pc, rX, lsl #2": a jump into the following straight-line code
        # (the unrolled loops of the standard division routines). Every following instruction
        # up to the first return / jump can be the target.
        if i.kind == "dp" and f["op"] == 4 and f["rn"] == 15 and f["op2"][:1] == ("sh",) \
                and f["op2"][2:] == (0, 2) and not f["s"]:
            targets, a = [], i.addr + 8
            while len(targets) < 512:
                n = img.decode(a, False)
                if n is None or n.kind == "undef":
                    break
                targets.append(a)
                if n.cond == 14 and (n.kind in ("b", "bx") and not n.f.get("link") or writes_pc(n)):
                    break
                a += 4
            return targets or None
        return None
    if i.kind == "mem" and f["load"] and f["rn"] == 15 and f["off"][0] == "reg" and f["off"][2:] == (0, 2) \
            and f["pre"] and f["up"] and f["size"] == "w":
        targets, a = [], i.addr + 8
        while len(targets) < 1024:
            w = img.word(a)
            if w is None or not img.contains(w) or w & 3:
                break
            targets.append(w)
            a += 4
        return targets or None
    if i.kind == "dp" and f["op"] == 4 and f["rn"] == 15 and f["op2"][:1] == ("sh",) and f["op2"][2:] == (0, 2):
        targets, a = [], i.addr + 8
        while len(targets) < 1024:
            n = img.decode(a, False)
            if n is None or n.kind != "b" or n.cond != 14 or n.f["link"]:
                break
            targets.append(a)
            a += 4
        return targets or None
    return None


def explore(img: Image, addr: int, thumb: bool, known: set) -> Function:
    """All instructions of the function at addr. `known` = (addr, thumb) of known function starts."""
    fn = Function(addr, thumb)
    todo = [addr]
    seen: set = set()
    while todo:
        a = todo.pop()
        while True:
            if a in seen:
                break
            if len(fn.insns) > MAX_FUNC_INSNS:
                fn.bad = "function too large - cut short"
                return fn
            i = img.decode(a, thumb)
            if i is None or i.kind == "undef":
                if i is None:
                    fn.bad = fn.bad or f"runs out of the code at 0x{a:08x}"
                else:
                    fn.insns[a] = i              # kept: becomes an "undefined instruction" call
                seen.add(a)
                break
            seen.add(a)
            fn.insns[a] = i
            if i.kind == "b":
                t, tthumb = i.f["target"], (thumb != i.f["exchange"])
                if i.f["link"]:
                    fn.calls.add((t, tthumb))
                    a = i.end
                    continue
                if (t, tthumb) in known and t != addr:
                    fn.tails.add((t, tthumb))
                elif img.contains(t) and tthumb == thumb:
                    fn.labels.add(t)
                    todo.append(t)
                else:
                    fn.tails.add((t, tthumb))
                if i.cond != 14:
                    a = i.end
                    continue
                break
            if i.kind == "bx" and i.f["link"]:
                a = i.end
                continue
            if i.kind == "bx" or writes_pc(i):
                table = _jump_table(img, i)
                if table:
                    fn.tables[a] = table
                    for t in table:
                        fn.labels.add(t)
                        todo.append(t)
                elif not is_return(i):
                    fn.unresolved.add(a)
                if i.cond != 14:                  # not taken: the next instruction runs
                    a = i.end
                    continue
                break
            a = i.end
    return fn


def _looks_like_start(img: Image, addr: int, thumb: bool) -> bool:
    """A function that saves LR (or the frame) on the stack as its first or second instruction."""
    i = img.decode(addr, thumb)
    if i is None:
        return False
    if i.kind == "block" and not i.f["load"] and i.f["rn"] == 13 and 14 in i.f["regs"] and i.cond == 14:
        return True
    if not thumb and i.kind == "dp" and i.f["op"] == 13 and i.f["rd"] == 12 and i.f["op2"] == ("sh", 13, 0, 0):
        n = img.decode(addr + 4, False)
        return bool(n and n.kind == "block" and not n.f["load"] and n.f["rn"] == 13)
    return False


def find_functions(img: Image, entries: list, extra_known: set | None = None,
                   scan_pointers: bool = True, progress=None) -> dict:
    """{(addr, thumb): Function}. entries = [(addr, thumb)]."""
    known = set(entries) | set(extra_known or ())
    funcs: dict = {}
    todo = list(entries)

    def run() -> None:
        while todo:
            key = todo.pop()
            if key in funcs or not img.contains(key[0]):
                continue
            fn = explore(img, key[0], key[1], known)
            funcs[key] = fn
            for c in fn.calls | fn.tails:
                if c not in known and img.contains(c[0]):
                    known.add(c)
                    todo.append(c)
            if progress is not None and len(funcs) % 500 == 0:
                progress(len(funcs))

    run()
    if scan_pointers:
        covered = set()
        for fn in funcs.values():
            covered.update(fn.insns)
        for r in img.regions:
            for o in range(0, len(r.data) - 3, 4):
                w = int.from_bytes(r.data[o:o + 4], "little")
                if not img.contains(w & ~1):
                    continue
                key = (w & ~1, bool(w & 1))
                if key in known or (w & ~1) in covered:
                    continue
                if (w & 3) in (0, 1) and _looks_like_start(img, key[0], key[1]):
                    known.add(key)
                    todo.append(key)
        run()
    # a later found function start inside an earlier function becomes a tail call target there
    return funcs
