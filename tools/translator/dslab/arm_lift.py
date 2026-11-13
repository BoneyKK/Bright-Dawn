"""
arm_lift.py - translate decoded ARM/Thumb functions into C++.

The result is "lifted" C++: every machine instruction becomes one or a few
C++ statements on a `ds::Cpu` structure (registers r[0..15], flags N Z C V Q),
with the original instruction as a comment above it. It compiles with any
C++17 compiler and does exactly what the machine code does - it is a
faithful translation, not hand-written-looking code. Names are addresses
(f_02004a10); readable names can be given later.

What maps to what:
  * each function           -> void f_<address>(ds::Cpu& c)
  * branches in the function -> goto L_<address>
  * BL / BLX to a function  -> c.r[14] = <return address>; ds::enter(c, ...);
                               f_<address>(c); if (ds::leave(c, ...)) return;
  * calls through registers -> the same, with ds::call(c, address) (the
                               run-time looks the function up)
  * computed jumps          -> ds::jump(c, address); return;
  * returns                 -> return

Every call site registers itself with ds::enter before the call and checks
ds::leave after it. That gives the run-time a list of the call sites that are
waiting for a return, so it can tell when control, by whatever route, has come
back to one of them, and ds::leave tells the function to stop when control
has gone to an outer call site instead.
  * memory                  -> c.rd8/rd16/rd32, c.wr8/wr16/wr32 (the platform
                               layer, ds_platform.cpp, decides what memory is)
  * SWI (BIOS calls), coprocessor 15 (caches, memory protection) -> calls into
                               the platform layer.
Not translated (written as a call to ds::unhandled with a comment): a few
very rare forms (coprocessor instructions other than MRC and MCR, breakpoints,
undefined instructions).
"""

from .arm_analysis import is_return, writes_pc
from .arm_decode import Insn

COND_EXPR = ["c.Z", "!c.Z", "c.C", "!c.C", "c.N", "!c.N", "c.V", "!c.V", "(c.C && !c.Z)", "(!c.C || c.Z)",
             "(c.N == c.V)", "(c.N != c.V)", "(!c.Z && c.N == c.V)", "(c.Z || c.N != c.V)", "true", "false"]


def hexc(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:08x}u"


class Emitter:
    def __init__(self, fn, names: dict, image):
        self.fn = fn
        self.names = names                      # (addr, thumb) -> C++ function name
        self.image = image                      # for constants stored next to the code (literal pools)
        self.lines: list[str] = []
        self.helpers_used: set = set()

    # ---- registers -------------------------------------------------------

    def reg(self, r: int, i: Insn, extra_pc: int = 0, align: bool = False) -> str:
        if r == 15:
            v = i.pc_value + extra_pc
            if align:
                v &= ~3
            return hexc(v)
        return f"c.r[{r}]"

    def shifted(self, i: Insn, op2, want_carry: bool) -> tuple[str, str]:
        """(value expression, carry expression) for a data-processing operand."""
        if op2[0] == "imm":
            carry = ("1u" if op2[1] >> 31 else "0u") if op2[2] else "c.C"
            return hexc(op2[1]), carry
        if op2[0] == "sh":
            _, rm, typ, n = op2
            v = self.reg(rm, i)
            if typ == 0:
                if n == 0:
                    return v, "c.C"
                return f"({v} << {n})", f"(({v} >> {32 - n}) & 1u)"
            if typ == 1:
                if n == 0:
                    return "0u", f"({v} >> 31)"
                return f"({v} >> {n})", f"(({v} >> {n - 1}) & 1u)"
            if typ == 2:
                if n == 0:
                    return f"(u32)((s32){v} >> 31)", f"({v} >> 31)"
                return f"(u32)((s32){v} >> {n})", f"(({v} >> {n - 1}) & 1u)"
            if n == 0:
                return f"(((u32)c.C << 31) | ({v} >> 1))", f"({v} & 1u)"
            return f"ds::ror32({v}, {n})", f"(({v} >> {n - 1}) & 1u)"
        _, rm, typ, rs = op2
        v = self.reg(rm, i, 4)
        amount = f"({self.reg(rs, i, 4)} & 0xFFu)"
        fn = ["lsl", "lsr", "asr", "ror"][typ]
        return f"ds::{fn}_reg({v}, {amount}, sc)", "sc"

    # ---- output helpers --------------------------------------------------

    def out(self, text: str, indent: int = 1) -> None:
        self.lines.append("    " * indent + text)

    def call_expr(self, target: int, thumb: bool, link: bool = True) -> str:
        name = self.names.get((target, thumb))
        if name:
            return f"{name}(c)"
        via = "call" if link else "jump"
        return f"ds::{via}(c, {hexc(target | (1 if thumb else 0))})"

    @staticmethod
    def linked(ret: int, call: str) -> list[str]:
        """Statements for a call that is expected to return to `ret`."""
        r = hexc(ret)
        return [f"c.r[14] = {r}; ds::enter(c, {r});", f"{call};", f"if (ds::leave(c, {r})) return;"]

    def jump_to_value(self, i: Insn, value: str, ret_ok: bool) -> list[str]:
        """Statements for "pc = value" (a computed jump or a return)."""
        fn = self.fn
        if i.addr in fn.tables:
            lines = [f"switch ({value} & ~1u) {{"]
            for t in sorted(set(fn.tables[i.addr])):
                if t in fn.insns:
                    lines.append(f"    case {hexc(t)}: goto L_{t:08x};")
            lines.append(f"    default: ds::jump(c, {value}); return;")
            lines.append("}")
            return lines
        if ret_ok:
            return [f"c.ret_to = {value}; return;"]
        return [f"ds::jump(c, {value}); return; /* computed jump */"]

    # ---- one instruction -------------------------------------------------

    def insn(self, i: Insn) -> list[str]:
        k = i.kind
        f = i.f
        if k == "dp":
            return self.dp(i)
        if k == "mem":
            return self.mem(i)
        if k == "block":
            return self.block(i)
        if k == "b":
            target, tthumb = f["target"], (i.thumb != f["exchange"])
            if f["link"]:
                ret = i.end | (1 if i.thumb else 0)
                return self.linked(ret, self.call_expr(target, tthumb))
            if target in self.fn.insns and tthumb == i.thumb and (target, tthumb) not in self.fn.tails:
                return [f"goto L_{target:08x};"]
            return [f"{self.call_expr(target, tthumb, link=False)}; return; /* tail call */"]
        if k == "bx":
            v = self.reg(f["rm"], i)
            if f["link"]:
                ret = i.end | (1 if i.thumb else 0)
                return [f"{{ u32 t = {v};"] + ["    " + x for x in self.linked(ret, "ds::call(c, t)")] + ["}"]
            if f["rm"] == 14:
                return ["c.ret_to = c.r[14]; return;"]
            return self.jump_to_value(i, v, False)
        if k == "mul":
            rm, rs = self.reg(f["rm"], i), self.reg(f["rs"], i)
            expr = f"{rm} * {rs}" + (f" + {self.reg(f['rn'], i)}" if f["mla"] else "")
            lines = [f"c.r[{f['rd']}] = {expr};"]
            if f["s"]:
                lines.append(f"c.N = c.r[{f['rd']}] >> 31; c.Z = c.r[{f['rd']}] == 0;")
            return lines
        if k == "mull":
            rm, rs = self.reg(f["rm"], i), self.reg(f["rs"], i)
            if f["signed"]:
                prod = f"(u64)((s64)(s32){rm} * (s64)(s32){rs})"
            else:
                prod = f"((u64){rm} * (u64){rs})"
            acc = f" + (((u64)c.r[{f['rdhi']}] << 32) | c.r[{f['rdlo']}])" if f["acc"] else ""
            lines = [f"{{ u64 t = {prod}{acc}; c.r[{f['rdlo']}] = (u32)t; c.r[{f['rdhi']}] = (u32)(t >> 32);"
                     + (" c.N = t >> 63; c.Z = t == 0;" if f["s"] else "") + " }"]
            return lines
        if k == "smla":
            return self.smla(i)
        if k == "qarith":
            rm, rn = self.reg(f["rm"], i), self.reg(f["rn"], i)
            op = f["op"]
            if op == "qadd":
                e = f"ds::qadd(c, {rm}, {rn})"
            elif op == "qsub":
                e = f"ds::qsub(c, {rm}, {rn})"
            elif op == "qdadd":
                e = f"ds::qadd(c, {rm}, ds::qadd(c, {rn}, {rn}))"
            else:
                e = f"ds::qsub(c, {rm}, ds::qadd(c, {rn}, {rn}))"
            return [f"c.r[{f['rd']}] = {e};"]
        if k == "clz":
            return [f"c.r[{f['rd']}] = ds::clz({self.reg(f['rm'], i)});"]
        if k == "mrs":
            return [f"c.r[{f['rd']}] = {'c.spsr' if f['spsr'] else 'ds::get_cpsr(c)'};"]
        if k == "msr":
            op2 = f["op2"]
            v = hexc(op2[1]) if op2[0] == "imm" else self.reg(op2[1], i)
            mask = sum(0xFF << (8 * n) for n in range(4) if f["mask"] & (1 << n))
            if f["spsr"]:
                return [f"c.spsr = (c.spsr & ~{hexc(mask)}) | ({v} & {hexc(mask)});"]
            return [f"ds::set_cpsr(c, {v}, {hexc(mask)});"]
        if k == "swp":
            rn, rm = self.reg(f["rn"], i), self.reg(f["rm"], i)
            if f["byte"]:
                return [f"{{ u32 a = {rn}; u32 v = {rm}; u32 t = c.rd8(a); c.wr8(a, (u8)v); c.r[{f['rd']}] = t; }}"]
            return [f"{{ u32 a = {rn}; u32 v = {rm}; u32 t = ds::ror32(c.rd32(a & ~3u), (a & 3u) * 8); "
                    f"c.wr32(a & ~3u, v); c.r[{f['rd']}] = t; }}"]
        if k == "swi":
            n = f["imm"] if i.thumb else (f["imm"] >> 16) & 0xFF
            # The processor puts the return address in the supervisor mode's own r14; the
            # program's r14 is untouched, and the usual wrapper ("swi n; bx lr") relies on that.
            # The address of the next instruction goes along: what the BIOS call does may
            # depend on it (waiting for an interrupt, which then returns there).
            return [f"ds::swi(c, {n}u, {hexc(i.end | (1 if i.thumb else 0))}); /* BIOS call */"]
        if k == "cop":
            op = f["op"]
            if op == "mcr":
                return [f"ds::cp_write(c, {f['cp']}, {f['opc1']}, {f['crn']}, {f['crm']}, {f['opc2']}, "
                        f"{self.reg(f['rd'], i, 4)});"]
            if op == "mrc":
                read = f"ds::cp_read(c, {f['cp']}, {f['opc1']}, {f['crn']}, {f['crm']}, {f['opc2']})"
                if f["rd"] == 15:
                    return [f"{{ u32 t = {read}; c.N = t >> 31; c.Z = (t >> 30) & 1; c.C = (t >> 29) & 1; "
                            f"c.V = (t >> 28) & 1; }}"]
                return [f"c.r[{f['rd']}] = {read};"]
            return [f"ds::unhandled(c, {hexc(i.addr)}, \"coprocessor {op}\");"]
        if k == "bkpt":
            return [f"ds::unhandled(c, {hexc(i.addr)}, \"breakpoint\");"]
        if k == "pld":
            return ["/* preload hint: no effect */"]
        if k == "blhi":
            return [f"c.r[14] = {hexc(f['value'])};"]
        return [f"ds::unhandled(c, {hexc(i.addr)}, \"undefined instruction\");"]

    def dp(self, i: Insn) -> list[str]:
        f = i.f
        op, s, rd = f["op"], f["s"], f["rd"]
        align = bool(f.get("align_pc"))
        extra = 4 if f["op2"][0] == "rs" else 0
        a = self.reg(f["rn"], i, extra, align)
        b, carry = self.shifted(i, f["op2"], s)
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        pre = []
        if f["op2"][0] == "rs":
            pre.append("bool sc = c.C;")
        lines = []
        if not s:
            expr = {0: f"{a} & {b}", 1: f"{a} ^ {b}", 2: f"{a} - {b}", 3: f"{b} - {a}", 4: f"{a} + {b}",
                    5: f"{a} + {b} + (u32)c.C", 6: f"{a} - {b} - (u32)!c.C", 7: f"{b} - {a} - (u32)!c.C",
                    12: f"{a} | {b}", 13: b, 14: f"{a} & ~{b}", 15: f"~{b}"}[op]
            if rd == 15:
                # A data-processing instruction that writes pc does not change the instruction
                # set (only bx/blx, loads and pops do), so the target carries this function's.
                dest = "(res | 1u)" if i.thumb else "(res & ~3u)"
                return self._wrap(pre + [f"u32 res = {expr};"] + self.jump_to_value(
                    i, dest, f["op2"] == ("sh", 14, 0, 0) and op == 13))
            if pre:
                return ["{ " + " ".join(pre + [f"c.r[{rd}] = {expr};"]) + " }"]
            return [f"c.r[{rd}] = {expr};"]
        if logical:
            expr = {0: f"a & {b}", 1: f"a ^ {b}", 8: f"a & {b}", 9: f"a ^ {b}", 12: f"a | {b}", 13: b,
                    14: f"a & ~{b}", 15: f"~{b}"}[op]
            body = pre + [f"u32 a = {a};"] if op not in (13, 15) else list(pre)
            body += [f"u32 res = {expr};", f"c.C = {carry};", "c.N = res >> 31; c.Z = res == 0;"]
        else:
            fl = {2: f"ds::adds(c, a, ~({b}), 1u)", 3: f"ds::adds(c, {b}, ~a, 1u)", 4: f"ds::adds(c, a, {b}, 0u)",
                  5: f"ds::adds(c, a, {b}, (u32)c.C)", 6: f"ds::adds(c, a, ~({b}), (u32)c.C)",
                  7: f"ds::adds(c, {b}, ~a, (u32)c.C)", 10: f"ds::adds(c, a, ~({b}), 1u)",
                  11: f"ds::adds(c, a, {b}, 0u)"}[op]
            body = pre + [f"u32 a = {a};", f"u32 res = {fl};"]
        if 8 <= op <= 11:
            body.append("(void)res;")
        elif rd == 15:                                  # "movs pc, lr" etc.: return from an exception
            body.append("u32 back = ds::return_from_exception(c, res);")
            body += self.jump_to_value(i, "back", True)
            return self._wrap(body)
        else:
            body.append(f"c.r[{rd}] = res;")
        return self._wrap(body)

    @staticmethod
    def _wrap(body: list[str]) -> list[str]:
        """Statements in their own block (they declare temporaries)."""
        one = "{ " + " ".join(body) + " }"
        if len(one) <= 150 and not any(b.endswith("{") or b == "}" for b in body):
            return [one]
        return ["{"] + ["    " + b for b in body] + ["}"]

    def smla(self, i: Insn) -> list[str]:
        f = i.f
        v = f["variant"]
        rm, rs = self.reg(f["rm"], i), self.reg(f["rs"], i)
        hx = f"(s32)(s16)({rm} >> 16)" if f["x"] else f"(s32)(s16){rm}"
        hy = f"(s32)(s16)({rs} >> 16)" if f["y"] else f"(s32)(s16){rs}"
        if v == "smul":
            return [f"c.r[{f['rd']}] = (u32)({hx} * {hy});"]
        if v == "smla":
            return [f"c.r[{f['rd']}] = ds::add_q(c, (u32)({hx} * {hy}), {self.reg(f['rn'], i)});"]
        if v == "smulw":
            return [f"c.r[{f['rd']}] = (u32)(((s64)(s32){rm} * {hy}) >> 16);"]
        if v == "smlaw":
            return [f"c.r[{f['rd']}] = ds::add_q(c, (u32)(((s64)(s32){rm} * {hy}) >> 16), {self.reg(f['rn'], i)});"]
        return [f"{{ u64 t = (((u64)c.r[{f['rdhi']}] << 32) | c.r[{f['rdlo']}]) + (u64)(s64)({hx} * {hy}); "
                f"c.r[{f['rdlo']}] = (u32)t; c.r[{f['rdhi']}] = (u32)(t >> 32); }}"]

    def literal(self, i: Insn) -> int | None:
        """Value of "ldr rX, [pc, #n]": the compiler's constant stored next to the code."""
        f = i.f
        if not (f["load"] and f["rn"] == 15 and f["size"] == "w" and f["off"][0] == "imm" and f["pre"]
                and not f["wb"] and f["rd"] != 15):
            return None
        base = i.pc_value & ~3 if i.thumb else i.pc_value
        a = base + f["off"][1] if f["up"] else base - f["off"][1]
        if a & 3 or self.image is None:
            return None
        return self.image.word(a)

    def mem(self, i: Insn) -> list[str]:
        f = i.f
        rn, rd, size = f["rn"], f["rd"], f["size"]
        lit = self.literal(i)
        if lit is not None:
            return [f"c.r[{rd}] = {hexc(lit)}; /* constant stored next to the code */"]
        base = self.reg(rn, i, align=i.thumb and rn == 15)
        off = f["off"]
        if off[0] == "imm":
            offset = hexc(off[1]) if off[1] else ""
        else:
            _, rm, typ, n = off
            offset, _carry = self.shifted(i, ("sh", rm, typ, n), False)
        sign = "+" if f["up"] else "-"
        moved = f"base {sign} {offset}" if offset else "base"
        addr = moved if f["pre"] else "base"
        body = [f"u32 base = {base};", f"u32 a = {addr};"]
        wb = f["wb"] and rn != 15
        if f["load"]:
            read = {"w": "ds::ror32(c.rd32(a & ~3u), (a & 3u) * 8)", "b": "c.rd8(a)", "h": "c.rd16(a & ~1u)",
                    "sb": "(u32)(s32)(s8)c.rd8(a)", "sh": "(u32)(s32)(s16)c.rd16(a & ~1u)"}
            if size == "d":
                body.append("u32 v0 = c.rd32(a & ~3u); u32 v1 = c.rd32((a & ~3u) + 4);")
                if wb:
                    body.append(f"c.r[{rn}] = {moved};")
                body.append(f"c.r[{rd}] = v0; c.r[{rd + 1}] = v1;")
                return self._wrap(body)
            body.append(f"u32 v = {read[size]};")
            if wb:
                body.append(f"c.r[{rn}] = {moved};")
            if rd == 15:
                body += self.jump_to_value(i, "v", is_return(i))
            else:
                body.append(f"c.r[{rd}] = v;")
            return self._wrap(body)
        val = self.reg(rd, i, 4) if rd == 15 else f"c.r[{rd}]"
        write = {"w": f"c.wr32(a & ~3u, {val});", "b": f"c.wr8(a, (u8){val});",
                 "h": f"c.wr16(a & ~1u, (u16){val});"}
        if size == "d":
            body.append(f"c.wr32(a & ~3u, c.r[{rd}]); c.wr32((a & ~3u) + 4, c.r[{rd + 1}]);")
        else:
            body.append(write[size])
        if wb:
            body.append(f"c.r[{rn}] = {moved};")
        return self._wrap(body)

    def block(self, i: Insn) -> list[str]:
        f = i.f
        rn, regs, mode = f["rn"], f["regs"], f["mode"]
        n = len(regs)
        start = {"ia": "base", "ib": "base + 4", "da": f"base - {4 * n - 4}", "db": f"base - {4 * n}"}[mode]
        end = f"base + {4 * n}" if mode in ("ia", "ib") else f"base - {4 * n}"
        body = [f"u32 base = c.r[{rn}];", f"u32 a = ({start}) & ~3u;"]
        # "^" without pc in the list transfers the User mode registers, whatever the
        # current mode is. (With pc in a load it means "return from exception" instead.)
        user_bank = f["user"] and not (f["load"] and 15 in regs)

        def reg(r: int) -> str:
            return f"ds::user_reg(c, {r})" if user_bank else f"c.r[{r}]"

        if f["load"]:
            for k, r in enumerate(regs):
                body.append(f"u32 v{r} = c.rd32(a + {4 * k});")
            if f["wb"] and rn not in regs:
                body.append(f"c.r[{rn}] = {end};")
            for r in regs:
                if r != 15:
                    body.append(f"{reg(r)} = v{r};")
            if 15 in regs:
                if f["user"]:
                    body.append("u32 back = ds::return_from_exception(c, v15);")
                    body += self.jump_to_value(i, "back", True)
                else:
                    body += self.jump_to_value(i, "v15", is_return(i))
        else:
            for k, r in enumerate(regs):
                v = hexc(i.pc_value + 4) if r == 15 else ("base" if r == rn and not user_bank else reg(r))
                body.append(f"c.wr32(a + {4 * k}, {v});")
            if f["wb"]:
                body.append(f"c.r[{rn}] = {end};")
        return ["{"] + ["    " + b for b in body] + ["}"]


def ends_flow(i: Insn) -> bool:
    """True if execution never continues with the next instruction."""
    if i.cond != 14:
        return False
    if i.kind == "b":
        return not i.f["link"]
    if i.kind == "bx":
        return not i.f["link"]
    return writes_pc(i)


def lift_function(fn, names: dict, image=None) -> str:
    """C++ source of one function. `image` (arm_analysis.Image) lets constants be read."""
    e = Emitter(fn, names, image)
    kind = "Thumb" if fn.thumb else "ARM"
    lines = [f"// {kind} function at 0x{fn.addr:08x}, {len(fn.insns)} instructions"
             + (f" - note: {fn.bad}" if fn.bad else ""),
             f"void {names[(fn.addr, fn.thumb)]}(Cpu& c) {{"]
    prev = None
    for a in sorted(fn.insns):
        i = fn.insns[a]
        if prev is not None and prev.end != a and not ends_flow(prev):
            lines.append(f"    ds::unhandled(c, {hexc(prev.end)}, \"code continues into data\"); return;")
        prev = i
        if a in fn.labels:
            lines.append(f"L_{a:08x}:")
        raw = f"{i.raw:08x}" if i.size == 4 else f"{i.raw:04x}"
        lines.append(f"    /* {a:08x} {raw:>8}  {i.text()} */")
        stmts = e.insn(i)
        if i.cond not in (14, 15):
            lines.append(f"    if ({COND_EXPR[i.cond]}) {{")
            lines += ["        " + s for s in stmts]
            lines.append("    }")
        else:
            lines += ["    " + s for s in stmts]
    if prev is not None and not ends_flow(prev):
        lines.append(f"    ds::unhandled(c, {hexc(prev.end)}, \"code continues into data\");")
    lines.append("}")
    return "\n".join(lines) + "\n"


RUNTIME_HEADER = r"""// ds_cpu.h - the CPU model used by the translated code (generated by DS Asset Lab).
// The translated functions only use what is declared here. The platform layer
// (ds_platform.cpp) provides memory, BIOS calls and coprocessor access.
#pragma once
#include <cstdint>

namespace ds {
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using s8 = int8_t; using s16 = int16_t; using s32 = int32_t; using s64 = int64_t;

struct Cpu {
    u32 r[16] = {};                  // r13 = sp, r14 = lr, r15 = pc (not used by the translation)
    bool N = false, Z = false, C = false, V = false, Q = false;
    u32 cpsr_ctrl = 0x1F;            // mode, interrupt and Thumb bits of CPSR
    u32 spsr = 0;                    // of the current mode (User and System have none)
    // The registers each processor mode has to itself. r[] and spsr always hold
    // the current mode's; these hold the others' (the current mode's entries
    // are stale). Bank 0 = User and System, 1 = FIQ, 2 = IRQ, 3 = Supervisor,
    // 4 = Abort, 5 = Undefined. See switch_mode.
    u32 bank_r13[6] = {}, bank_r14[6] = {}, bank_spsr[6] = {};
    u32 bank_r8[2][5] = {};          // r8-r12: [1] = FIQ's, [0] = every other mode's
    u32 ret_to = 0;                  // where the last return went (checked by leave)
    int cpu = 9;                     // 9 = ARM9 (main CPU), 7 = ARM7
    void* platform = nullptr;        // the platform layer's own data

    // Call sites that are waiting for their callee to return, innermost last:
    // the return address each one gave, and the stack pointer at the call.
    struct Frame { u32 ret; u32 sp; };
    static constexpr u32 kMaxFrames = 4096;
    Frame frames[kMaxFrames];
    u32 depth = 0;                   // may exceed kMaxFrames; the excess is not recorded
    u32 unwind = 0;                  // call sites that must still give up (see leave)

    u8  rd8(u32 a);
    u16 rd16(u32 a);
    u32 rd32(u32 a);
    void wr8(u32 a, u8 v);
    void wr16(u32 a, u16 v);
    void wr32(u32 a, u32 v);
};

inline u32 ror32(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
inline u32 clz(u32 v) { if (!v) return 32; u32 n = 0; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }

// a + b + carry_in, setting N Z C V (used for ADD/SUB/CMP/... with flags; SUB a,b = a + ~b + 1)
inline u32 adds(Cpu& c, u32 a, u32 b, u32 carry_in) {
    u64 t = (u64)a + (u64)b + carry_in;
    u32 res = (u32)t;
    c.C = (t >> 32) != 0;
    c.V = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
    c.N = res >> 31; c.Z = res == 0;
    return res;
}

// shifts by a register amount (only the bottom byte counts), with the carry rules of the ARM
inline u32 lsl_reg(u32 v, u32 n, bool& carry) {
    if (n == 0) return v;
    if (n < 32) { carry = (v >> (32 - n)) & 1; return v << n; }
    carry = (n == 32) ? (v & 1) : false; return 0;
}
inline u32 lsr_reg(u32 v, u32 n, bool& carry) {
    if (n == 0) return v;
    if (n < 32) { carry = (v >> (n - 1)) & 1; return v >> n; }
    carry = (n == 32) ? (v >> 31) : false; return 0;
}
inline u32 asr_reg(u32 v, u32 n, bool& carry) {
    if (n == 0) return v;
    if (n < 32) { carry = (v >> (n - 1)) & 1; return (u32)((s32)v >> n); }
    carry = v >> 31; return (u32)((s32)v >> 31);
}
inline u32 ror_reg(u32 v, u32 n, bool& carry) {
    if (n == 0) return v;
    n &= 31;
    if (n == 0) { carry = v >> 31; return v; }
    carry = (v >> (n - 1)) & 1; return ror32(v, n);
}

// saturating arithmetic of the ARMv5TE DSP instructions (set Q when they saturate)
inline u32 qadd(Cpu& c, u32 a, u32 b) {
    s64 t = (s64)(s32)a + (s64)(s32)b;
    if (t > 0x7FFFFFFF) { c.Q = true; return 0x7FFFFFFFu; }
    if (t < -0x80000000LL) { c.Q = true; return 0x80000000u; }
    return (u32)t;
}
inline u32 qsub(Cpu& c, u32 a, u32 b) {
    s64 t = (s64)(s32)a - (s64)(s32)b;
    if (t > 0x7FFFFFFF) { c.Q = true; return 0x7FFFFFFFu; }
    if (t < -0x80000000LL) { c.Q = true; return 0x80000000u; }
    return (u32)t;
}
inline u32 add_q(Cpu& c, u32 a, u32 b) {          // add, set Q on signed overflow (SMLA..)
    u32 res = a + b;
    if ((~(a ^ b) & (a ^ res)) >> 31) c.Q = true;
    return res;
}

inline u32 get_cpsr(Cpu& c) {
    return ((u32)c.N << 31) | ((u32)c.Z << 30) | ((u32)c.C << 29) | ((u32)c.V << 28) | ((u32)c.Q << 27) |
           (c.cpsr_ctrl & 0x07FFFFFFu);
}
inline void set_flags(Cpu& c, u32 v) {
    c.N = v >> 31; c.Z = (v >> 30) & 1; c.C = (v >> 29) & 1; c.V = (v >> 28) & 1; c.Q = (v >> 27) & 1;
}
// Which set of banked registers a mode uses (see Cpu::bank_r13).
inline u32 mode_bank(u32 mode) {
    switch (mode & 0x1Fu) {
        case 0x11: return 1; case 0x12: return 2; case 0x13: return 3; case 0x17: return 4; case 0x1B: return 5;
        default: return 0;
    }
}
// Changes the processor mode: r13, r14 and SPSR of the old mode are put away and
// those of the new mode brought in (and r8-r12 when entering or leaving FIQ).
inline void switch_mode(Cpu& c, u32 mode) {
    u32 from = mode_bank(c.cpsr_ctrl), to = mode_bank(mode);
    if (from != to) {
        c.bank_r13[from] = c.r[13]; c.bank_r14[from] = c.r[14]; c.bank_spsr[from] = c.spsr;
        c.r[13] = c.bank_r13[to]; c.r[14] = c.bank_r14[to]; c.spsr = c.bank_spsr[to];
        if ((from == 1) != (to == 1))
            for (int n = 0; n < 5; n++) { c.bank_r8[from == 1][n] = c.r[8 + n]; c.r[8 + n] = c.bank_r8[to == 1][n]; }
    }
    c.cpsr_ctrl = (c.cpsr_ctrl & ~0x1Fu) | (mode & 0x1Fu);
}
// MSR to CPSR. User mode may change the flags only. The Thumb bit cannot be
// changed this way (the processor leaves the result of trying undefined; here
// it is ignored).
inline void set_cpsr(Cpu& c, u32 v, u32 mask) {
    if (mask & 0xFF000000u) set_flags(c, v);
    if ((mask & 0xFFu) && (c.cpsr_ctrl & 0x1Fu) != 0x10u) {
        switch_mode(c, v);
        c.cpsr_ctrl = (c.cpsr_ctrl & ~0xC0u) | (v & 0xC0u);
    }
}
// Register n of User mode, from whatever mode the processor is in ("ldm/stm ...^").
inline u32& user_reg(Cpu& c, u32 n) {
    u32 bank = mode_bank(c.cpsr_ctrl);
    if (bank != 0) {
        if (n == 13) return c.bank_r13[0];
        if (n == 14) return c.bank_r14[0];
        if (bank == 1 && n >= 8 && n <= 12) return c.bank_r8[0][n - 8];
    }
    return c.r[n];
}
// The return from an exception handler ("movs pc, lr", "ldm ..., {..., pc}^"):
// CPSR := SPSR of the current mode. Returns where execution continues, given the
// value written to pc: the instruction set comes from the restored status
// register, not from bit 0 of that value. (The model's own Thumb bit is always
// clear; the instruction set is carried by bit 0 of code addresses.)
inline u32 return_from_exception(Cpu& c, u32 pc) {
    u32 v = c.spsr;
    set_flags(c, v);
    switch_mode(c, v);
    c.cpsr_ctrl = (c.cpsr_ctrl & ~0xC0u) | (v & 0xC0u);
    return (v & 0x20u) ? (pc | 1u) : (pc & ~3u);
}

// Around every call: enter before it, leave after it. leave returns true when the
// calling function must return at once, because control did not come back to this
// call site but went to an outer one. In the ordinary case it costs two compares.
bool leave_slow(Cpu& c, u32 ret);                            // provided by the platform layer
inline void enter(Cpu& c, u32 ret) {
    if (c.depth < Cpu::kMaxFrames) { c.frames[c.depth].ret = ret; c.frames[c.depth].sp = c.r[13]; }
    ++c.depth;
}
inline bool leave(Cpu& c, u32 ret) {
    --c.depth;
    if (c.unwind == 0 && c.ret_to == ret) return false;
    return leave_slow(c, ret);
}

// provided by the platform layer
void call(Cpu& c, u32 address);                              // a call; bit 0 set = Thumb code
void jump(Cpu& c, u32 address);                              // a jump whose target is not known in advance
void swi(Cpu& c, u32 number, u32 return_address);          // return_address: the next instruction, bit 0 set = Thumb
void cp_write(Cpu& c, int cp, int opc1, int crn, int crm, int opc2, u32 value);
u32 cp_read(Cpu& c, int cp, int opc1, int crn, int crm, int opc2);
void unhandled(Cpu& c, u32 address, const char* what);
}  // namespace ds
"""
