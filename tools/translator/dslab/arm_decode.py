"""
arm_decode.py - decode ARM and Thumb machine code (ARMv5TE, the ARM9 of the DS;
the ARM7 uses the ARMv4T subset).

Every instruction becomes an `Insn` with a `kind` and decoded fields. The
same structure is used for both instruction sets (Thumb instructions are
described with the ARM equivalent they behave like), so one translator
(arm_lift.py) turns both into C++. `text()` gives the assembly language
form for listings.

Encodings follow the ARM Architecture Reference Manual (ARMv5TE edition);
the decoder is checked against Capstone in the app's tests.

Kinds and fields (f = Insn.f):
  dp     op (0-15: and eor sub rsb add adc sbc rsc tst teq cmp cmn orr mov bic mvn),
         rd, rn, op2 = ("imm", value, rotated) | ("sh", rm, type, amount) | ("rs", rm, type, rs)
         (type 0-3 = lsl lsr asr ror; ("sh", rm, 3, 0) is rrx), and s
  mul    mla bool, rd, rn (accumulate), rm, rs
  mull   signed, acc, rdlo, rdhi, rm, rs
  smla   x, y (0 = bottom half, 1 = top), variant ("smla" "smlaw" "smulw" "smlal" "smul"), rd, rn, rm, rs, rdlo, rdhi
  qarith op ("qadd" "qsub" "qdadd" "qdsub"), rd, rm, rn
  clz    rd, rm
  mrs    rd, spsr
  msr    spsr, mask (bits 0-3 = c x s f), op2
  mem    load, size ("w" "b" "h" "sb" "sh" "d"), rd, rn, off = ("imm", n) | ("reg", rm, type, amount),
         up, pre, wb, user
  block  load, rn, regs (list), mode ("ia" "ib" "da" "db"), wb, user (^)
  swp    byte, rd, rm, rn
  b      target, link, exchange (BLX to the other instruction set)
  bx     rm, link
  swi    imm
  cop    op ("mcr" "mrc" "cdp" "ldc" "stc" "mcrr" "mrrc"), raw fields
  bkpt   imm
  pld    (no effect for us)
  blhi   Thumb BL/BLX first half without a second half: lr = pc + offset
  undef  (not a valid instruction for this CPU)
"""

from dataclasses import dataclass, field

COND = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv"]
DP_NAMES = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr",
            "mov", "bic", "mvn"]
SHIFT = ["lsl", "lsr", "asr", "ror"]
REG = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"]


@dataclass
class Insn:
    addr: int
    size: int            # 4 (ARM, or Thumb BL pair) or 2 (Thumb)
    thumb: bool
    raw: int
    kind: str
    cond: int = 14       # 14 = always
    f: dict = field(default_factory=dict)

    @property
    def end(self) -> int:
        return self.addr + self.size

    @property
    def pc_value(self) -> int:
        """What reading r15 gives in this instruction."""
        return self.addr + (4 if self.thumb else 8)

    def text(self) -> str:
        return format_insn(self)


def _ror(v: int, n: int) -> int:
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def _sx(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# --------------------------------------------------------------------------
# ARM (32-bit)
# --------------------------------------------------------------------------

def _shift_operand(w: int):
    rm, typ = w & 15, (w >> 5) & 3
    if w & 0x10:
        return ("rs", rm, typ, (w >> 8) & 15)
    return ("sh", rm, typ, (w >> 7) & 31)


def decode_arm(w: int, addr: int) -> Insn:
    cond = w >> 28
    i = Insn(addr, 4, False, w, "undef", cond)
    if cond == 15:                                   # unconditional space
        if (w & 0xFE000000) == 0xFA000000:           # BLX immediate
            off = _sx(w & 0xFFFFFF, 24) * 4 + ((w >> 23) & 2)
            i.kind, i.cond = "b", 14
            i.f = {"target": (addr + 8 + off) & 0xFFFFFFFF, "link": True, "exchange": True}
        elif (w & 0x0D70F000) == 0x0550F000:         # PLD
            i.kind, i.cond = "pld", 14
        return i
    op = (w >> 25) & 7
    if op in (0, 1):
        if op == 0 and (w & 0x90) == 0x90:            # multiplies, swap, extra loads/stores
            if ((w >> 5) & 3) == 0:
                if (w & 0x0FC000F0) == 0x00000090:
                    i.kind = "mul"
                    i.f = {"mla": bool(w & 0x200000), "s": bool(w & 0x100000), "rd": (w >> 16) & 15,
                           "rn": (w >> 12) & 15, "rs": (w >> 8) & 15, "rm": w & 15}
                elif (w & 0x0F8000F0) == 0x00800090:
                    i.kind = "mull"
                    i.f = {"signed": bool(w & 0x400000), "acc": bool(w & 0x200000), "s": bool(w & 0x100000),
                           "rdhi": (w >> 16) & 15, "rdlo": (w >> 12) & 15, "rs": (w >> 8) & 15, "rm": w & 15}
                elif (w & 0x0FB00FF0) == 0x01000090:
                    i.kind = "swp"
                    i.f = {"byte": bool(w & 0x400000), "rn": (w >> 16) & 15, "rd": (w >> 12) & 15, "rm": w & 15}
                return i
            sh = (w >> 5) & 3
            load = bool(w & 0x100000)
            if not load and sh in (2, 3):             # LDRD / STRD (encoded with L=0)
                size, load = "d", sh == 2
            else:
                if not load and sh != 1:
                    return i
                size = {1: "h", 2: "sb", 3: "sh"}[sh]
            pre, up, wb = bool(w & 0x1000000), bool(w & 0x800000), bool(w & 0x200000)
            if w & 0x400000:
                off = ("imm", ((w >> 4) & 0xF0) | (w & 15))
            else:
                if (w >> 8) & 15:
                    return i
                off = ("reg", w & 15, 0, 0)
            if not pre and wb:
                return i                               # unpredictable form
            i.kind = "mem"
            i.f = {"load": load, "size": size, "rd": (w >> 12) & 15, "rn": (w >> 16) & 15, "off": off,
                   "up": up, "pre": pre, "wb": wb or not pre, "user": False}
            if size == "d" and (i.f["rd"] & 1 or i.f["rd"] == 14):
                i.kind = "undef"
            return i
        opc = (w >> 21) & 15
        s = bool(w & 0x100000)
        if 8 <= opc <= 11 and not s:                  # misc instructions in the TST..CMN space
            if op == 1:
                if opc in (9, 11):                    # MSR immediate
                    i.kind = "msr"
                    i.f = {"spsr": opc == 11, "mask": (w >> 16) & 15,
                           "op2": ("imm", _ror(w & 0xFF, ((w >> 8) & 15) * 2), bool((w >> 8) & 15))}
                return i
            low = (w >> 4) & 15
            if low == 0:
                if opc in (8, 10):
                    i.kind = "mrs"
                    i.f = {"rd": (w >> 12) & 15, "spsr": opc == 10}
                else:
                    i.kind = "msr"
                    i.f = {"spsr": opc == 11, "mask": (w >> 16) & 15, "op2": ("sh", w & 15, 0, 0)}
            elif low in (1, 3) and opc == 9:
                i.kind = "bx"
                i.f = {"rm": w & 15, "link": low == 3}
            elif low == 1 and opc == 11:
                i.kind = "clz"
                i.f = {"rd": (w >> 12) & 15, "rm": w & 15}
            elif low == 5:
                i.kind = "qarith"
                i.f = {"op": ["qadd", "qsub", "qdadd", "qdsub"][opc - 8], "rd": (w >> 12) & 15,
                       "rm": w & 15, "rn": (w >> 16) & 15}
            elif low == 7 and opc == 9 and cond == 14:
                i.kind = "bkpt"
                i.f = {"imm": ((w >> 4) & 0xFFF0) | (w & 15)}
            elif (low & 9) == 8:                      # SMLA<x><y> family
                x, y = (w >> 5) & 1, (w >> 6) & 1
                regs = {"rd": (w >> 16) & 15, "rn": (w >> 12) & 15, "rs": (w >> 8) & 15, "rm": w & 15}
                if opc == 8:
                    variant = "smla"
                elif opc == 9:
                    variant = "smulw" if x else "smlaw"
                elif opc == 10:
                    variant = "smlal"
                    regs = {"rdhi": (w >> 16) & 15, "rdlo": (w >> 12) & 15, "rs": (w >> 8) & 15, "rm": w & 15}
                else:
                    variant = "smul"
                i.kind = "smla"
                i.f = {"variant": variant, "x": x, "y": y, **regs}
            return i
        if op == 1:
            rot = ((w >> 8) & 15) * 2
            op2 = ("imm", _ror(w & 0xFF, rot), rot != 0)
        else:
            op2 = _shift_operand(w)
        i.kind = "dp"
        i.f = {"op": opc, "s": s or 8 <= opc <= 11, "rd": (w >> 12) & 15, "rn": (w >> 16) & 15, "op2": op2}
        return i
    if op in (2, 3):
        if op == 3 and w & 0x10:
            return i                                   # media / undefined on ARMv5
        pre, up, byte = bool(w & 0x1000000), bool(w & 0x800000), bool(w & 0x400000)
        wb, load = bool(w & 0x200000), bool(w & 0x100000)
        if op == 2:
            off = ("imm", w & 0xFFF)
        else:
            off = ("reg", w & 15, (w >> 5) & 3, (w >> 7) & 31)
        i.kind = "mem"
        i.f = {"load": load, "size": "b" if byte else "w", "rd": (w >> 12) & 15, "rn": (w >> 16) & 15,
               "off": off, "up": up, "pre": pre, "wb": wb or not pre, "user": not pre and wb}
        return i
    if op == 4:
        regs = [r for r in range(16) if w & (1 << r)]
        mode = ["da", "ia", "db", "ib"][(w >> 23) & 3]
        i.kind = "block"
        i.f = {"load": bool(w & 0x100000), "rn": (w >> 16) & 15, "regs": regs, "mode": mode,
               "wb": bool(w & 0x200000), "user": bool(w & 0x400000)}
        if not regs:
            i.kind = "undef"
        return i
    if op == 5:
        off = _sx(w & 0xFFFFFF, 24) * 4
        i.kind = "b"
        i.f = {"target": (addr + 8 + off) & 0xFFFFFFFF, "link": bool(w & 0x1000000), "exchange": False}
        return i
    if op == 6:
        if (w & 0x0FE00000) == 0x0C400000:
            i.kind = "cop"
            i.f = {"op": "mrrc" if w & 0x100000 else "mcrr", "cp": (w >> 8) & 15, "opc": (w >> 4) & 15,
                   "rd": (w >> 12) & 15, "rn": (w >> 16) & 15, "crm": w & 15}
        else:
            i.kind = "cop"
            i.f = {"op": "ldc" if w & 0x100000 else "stc", "cp": (w >> 8) & 15, "crd": (w >> 12) & 15,
                   "rn": (w >> 16) & 15, "imm": (w & 0xFF) * 4, "up": bool(w & 0x800000),
                   "pre": bool(w & 0x1000000), "wb": bool(w & 0x200000), "long": bool(w & 0x400000)}
        return i
    if w & 0x1000000:
        i.kind = "swi"
        i.f = {"imm": w & 0xFFFFFF}
        return i
    if w & 0x10:
        i.kind = "cop"
        i.f = {"op": "mrc" if w & 0x100000 else "mcr", "cp": (w >> 8) & 15, "opc1": (w >> 21) & 7,
               "rd": (w >> 12) & 15, "crn": (w >> 16) & 15, "crm": w & 15, "opc2": (w >> 5) & 7}
    else:
        i.kind = "cop"
        i.f = {"op": "cdp", "cp": (w >> 8) & 15, "opc1": (w >> 20) & 15, "crd": (w >> 12) & 15,
               "crn": (w >> 16) & 15, "crm": w & 15, "opc2": (w >> 5) & 7}
    return i


# --------------------------------------------------------------------------
# Thumb (16-bit)
# --------------------------------------------------------------------------

def _t(addr: int, h: int, kind: str, cond: int = 14, **f) -> Insn:
    """A 16-bit Thumb instruction (keyword arguments become its fields)."""
    return Insn(addr, 2, True, h, kind, cond, f)


def decode_thumb(h: int, addr: int, next_h: int | None = None) -> Insn:
    """Decode the halfword `h`. `next_h` is the following halfword (for BL/BLX pairs)."""
    top = h >> 11
    if top < 3:                                       # LSL/LSR/ASR immediate
        amount = (h >> 6) & 31
        rm, rd = (h >> 3) & 7, h & 7
        if top == 0 and amount == 0:                 # = MOVS rd, rm
            return _t(addr, h, "dp", op=13, s=True, rd=rd, rn=0, op2=("sh", rm, 0, 0))
        return _t(addr, h, "dp", op=13, s=True, rd=rd, rn=0, op2=("sh", rm, top, amount))
    if top == 3:                                      # ADD/SUB register / 3-bit immediate
        sub, imm = bool(h & 0x200), bool(h & 0x400)
        v = (h >> 6) & 7
        op2 = ("imm", v, False) if imm else ("sh", v, 0, 0)
        return _t(addr, h, "dp", op=2 if sub else 4, s=True, rd=h & 7, rn=(h >> 3) & 7, op2=op2)
    if top < 8:                                       # MOV/CMP/ADD/SUB 8-bit immediate
        opn = (h >> 11) & 3
        rd, imm = (h >> 8) & 7, h & 0xFF
        dp = [13, 10, 4, 2][opn]
        return _t(addr, h, "dp", op=dp, s=True, rd=rd, rn=rd, op2=("imm", imm, False))
    if (h >> 10) == 0x10:                             # ALU operations
        opn, rm, rd = (h >> 6) & 15, (h >> 3) & 7, h & 7
        if opn in (2, 3, 4, 7):                       # LSL LSR ASR ROR by register
            typ = {2: 0, 3: 1, 4: 2, 7: 3}[opn]
            return _t(addr, h, "dp", op=13, s=True, rd=rd, rn=0, op2=("rs", rd, typ, rm))
        if opn == 9:                                  # NEG = RSBS rd, rm, #0
            return _t(addr, h, "dp", op=3, s=True, rd=rd, rn=rm, op2=("imm", 0, False))
        if opn == 13:                                 # MUL
            return _t(addr, h, "mul", mla=False, s=True, rd=rd, rn=0, rs=rd, rm=rm)
        dp = {0: 0, 1: 1, 5: 5, 6: 6, 8: 8, 10: 10, 11: 11, 12: 12, 14: 14, 15: 15}[opn]
        return _t(addr, h, "dp", op=dp, s=True, rd=rd, rn=rd, op2=("sh", rm, 0, 0))
    if (h >> 10) == 0x11:                             # hi register operations / BX
        opn = (h >> 8) & 3
        rm = (h >> 3) & 15
        rd = (h & 7) | ((h >> 4) & 8)
        if opn == 3:
            return _t(addr, h, "bx", rm=rm, link=bool(h & 0x80))
        if opn == 0:
            return _t(addr, h, "dp", op=4, s=False, rd=rd, rn=rd, op2=("sh", rm, 0, 0))
        if opn == 1:
            return _t(addr, h, "dp", op=10, s=True, rd=0, rn=rd, op2=("sh", rm, 0, 0))
        return _t(addr, h, "dp", op=13, s=False, rd=rd, rn=0, op2=("sh", rm, 0, 0))
    if top == 9:                                      # LDR rd, [pc, #imm]
        return _t(addr, h, "mem", load=True, size="w", rd=(h >> 8) & 7, rn=15, off=("imm", (h & 0xFF) * 4),
                  up=True, pre=True, wb=False, user=False)
    if top in (10, 11):                               # load/store register offset
        opn = (h >> 9) & 7
        load, size = [(False, "w"), (False, "h"), (False, "b"), (True, "sb"),
                      (True, "w"), (True, "h"), (True, "b"), (True, "sh")][opn]
        return _t(addr, h, "mem", load=load, size=size, rd=h & 7, rn=(h >> 3) & 7,
                  off=("reg", (h >> 6) & 7, 0, 0), up=True, pre=True, wb=False, user=False)
    if top in (12, 13, 14, 15, 16, 17):              # load/store immediate offset (word, byte, halfword)
        size = {12: "w", 13: "w", 14: "b", 15: "b", 16: "h", 17: "h"}[top]
        scale = {"w": 4, "b": 1, "h": 2}[size]
        return _t(addr, h, "mem", load=bool(h & 0x800), size=size, rd=h & 7, rn=(h >> 3) & 7,
                  off=("imm", ((h >> 6) & 31) * scale), up=True, pre=True, wb=False, user=False)
    if top in (18, 19):                               # SP-relative load/store
        return _t(addr, h, "mem", load=bool(h & 0x800), size="w", rd=(h >> 8) & 7, rn=13,
                  off=("imm", (h & 0xFF) * 4), up=True, pre=True, wb=False, user=False)
    if top in (20, 21):                               # ADD rd, pc/sp, #imm
        if h & 0x800:
            return _t(addr, h, "dp", op=4, s=False, rd=(h >> 8) & 7, rn=13, op2=("imm", (h & 0xFF) * 4, False))
        return _t(addr, h, "dp", op=4, s=False, rd=(h >> 8) & 7, rn=15, op2=("imm", (h & 0xFF) * 4, False),
                  align_pc=True)
    if (h >> 8) == 0xB0:                              # ADD/SUB sp, #imm
        return _t(addr, h, "dp", op=2 if h & 0x80 else 4, s=False, rd=13, rn=13,
                  op2=("imm", (h & 0x7F) * 4, False))
    if (h >> 9) in (0x5A, 0x5E):                      # PUSH / POP
        regs = [r for r in range(8) if h & (1 << r)]
        if h & 0x800:
            if h & 0x100:
                regs.append(15)
            if not regs:
                return _t(addr, h, "undef")
            return _t(addr, h, "block", load=True, rn=13, regs=regs, mode="ia", wb=True, user=False)
        if h & 0x100:
            regs.append(14)
        if not regs:
            return _t(addr, h, "undef")
        return _t(addr, h, "block", load=False, rn=13, regs=regs, mode="db", wb=True, user=False)
    if (h >> 8) == 0xBE:
        return _t(addr, h, "bkpt", imm=h & 0xFF)
    if top in (24, 25):                               # LDMIA / STMIA rb!, {list}
        regs = [r for r in range(8) if h & (1 << r)]
        rn = (h >> 8) & 7
        if not regs:
            return _t(addr, h, "undef")
        load = bool(h & 0x800)
        return _t(addr, h, "block", load=load, rn=rn, regs=regs, mode="ia",
                  wb=not (load and rn in regs), user=False)
    if top in (26, 27):                               # conditional branch / SWI
        cond = (h >> 8) & 15
        if cond == 15:
            return _t(addr, h, "swi", imm=h & 0xFF)
        if cond == 14:
            return _t(addr, h, "undef")
        return _t(addr, h, "b", cond, target=(addr + 4 + _sx(h & 0xFF, 8) * 2) & 0xFFFFFFFF,
                  link=False, exchange=False)
    if top == 28:                                     # unconditional branch
        return _t(addr, h, "b", target=(addr + 4 + _sx(h & 0x7FF, 11) * 2) & 0xFFFFFFFF,
                  link=False, exchange=False)
    if top == 30:                                     # BL/BLX prefix
        hi = _sx(h & 0x7FF, 11) << 12
        if next_h is not None and (next_h >> 11) in (29, 31):
            low = (next_h & 0x7FF) << 1
            target = (addr + 4 + hi + low) & 0xFFFFFFFF
            exchange = (next_h >> 11) == 29
            if exchange:
                if next_h & 1:
                    return _t(addr, h, "undef")
                target &= ~3
            return Insn(addr, 4, True, h | (next_h << 16), "b", 14,
                        {"target": target, "link": True, "exchange": exchange})
        return _t(addr, h, "blhi", value=(addr + 4 + hi) & 0xFFFFFFFF)
    return _t(addr, h, "undef")                       # 11101 / 11111 alone


# --------------------------------------------------------------------------
# Text
# --------------------------------------------------------------------------

def _imm(v: int) -> str:
    return f"#{v}" if v < 10 else f"#0x{v:x}"


def _op2_text(op2) -> str:
    if op2[0] == "imm":
        return _imm(op2[1])
    if op2[0] == "sh":
        _, rm, typ, amt = op2
        if typ == 0 and amt == 0:
            return REG[rm]
        if typ == 3 and amt == 0:
            return f"{REG[rm]}, rrx"
        return f"{REG[rm]}, {SHIFT[typ]} #{amt or 32}"
    _, rm, typ, rs = op2
    return f"{REG[rm]}, {SHIFT[typ]} {REG[rs]}"


def _reglist(regs: list) -> str:
    return "{" + ", ".join(REG[r] for r in regs) + "}"


def format_insn(i: Insn) -> str:
    c = COND[i.cond] if i.cond != 15 else ""
    f = i.f
    k = i.kind
    if k == "dp":
        name = DP_NAMES[f["op"]]
        s = "s" if f["s"] and not 8 <= f["op"] <= 11 else ""
        if f["op"] in (13, 15):
            return f"{name}{s}{c} {REG[f['rd']]}, {_op2_text(f['op2'])}"
        if 8 <= f["op"] <= 11:
            return f"{name}{c} {REG[f['rn']]}, {_op2_text(f['op2'])}"
        return f"{name}{s}{c} {REG[f['rd']]}, {REG[f['rn']]}, {_op2_text(f['op2'])}"
    if k == "mul":
        if f["mla"]:
            return f"mla{'s' if f['s'] else ''}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}, {REG[f['rn']]}"
        return f"mul{'s' if f['s'] else ''}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}"
    if k == "mull":
        name = ("s" if f["signed"] else "u") + ("mlal" if f["acc"] else "mull")
        return f"{name}{'s' if f['s'] else ''}{c} {REG[f['rdlo']]}, {REG[f['rdhi']]}, {REG[f['rm']]}, {REG[f['rs']]}"
    if k == "smla":
        v = f["variant"]
        xy = ("t" if f["x"] else "b") + ("t" if f["y"] else "b")
        if v == "smla":
            return f"smla{xy}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}, {REG[f['rn']]}"
        if v == "smul":
            return f"smul{xy}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}"
        if v == "smlaw":
            return f"smlaw{xy[1]}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}, {REG[f['rn']]}"
        if v == "smulw":
            return f"smulw{xy[1]}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rs']]}"
        return f"smlal{xy}{c} {REG[f['rdlo']]}, {REG[f['rdhi']]}, {REG[f['rm']]}, {REG[f['rs']]}"
    if k == "qarith":
        return f"{f['op']}{c} {REG[f['rd']]}, {REG[f['rm']]}, {REG[f['rn']]}"
    if k == "clz":
        return f"clz{c} {REG[f['rd']]}, {REG[f['rm']]}"
    if k == "mrs":
        return f"mrs{c} {REG[f['rd']]}, {'spsr' if f['spsr'] else 'cpsr'}"
    if k == "msr":
        fields = "".join(ch for b, ch in ((1, "c"), (2, "x"), (4, "s"), (8, "f")) if f["mask"] & b)
        return f"msr{c} {'spsr' if f['spsr'] else 'cpsr'}_{fields}, {_op2_text(f['op2'])}"
    if k == "mem":
        name = ("ldr" if f["load"] else "str") + {"w": "", "b": "b", "h": "h", "sb": "sb", "sh": "sh", "d": "d"}[f["size"]]
        if f["user"]:
            name += "t"
        rd = REG[f["rd"]] + (f", {REG[f['rd'] + 1]}" if f["size"] == "d" else "")
        off = f["off"]
        sign = "" if f["up"] else "-"
        if off[0] == "imm":
            o = "" if off[1] == 0 and f["pre"] else f", #{sign}{'0x%x' % off[1] if off[1] >= 10 else off[1]}"
        else:
            o = f", {sign}{REG[off[1]]}" + (f", {SHIFT[off[2]]} #{off[3] or 32}" if off[2] or off[3] else "")
            if off[2] == 3 and off[3] == 0:
                o = f", {sign}{REG[off[1]]}, rrx"
        if f["pre"]:
            return f"{name}{c} {rd}, [{REG[f['rn']]}{o}]{'!' if f['wb'] else ''}"
        return f"{name}{c} {rd}, [{REG[f['rn']]}]{o}"
    if k == "block":
        if f["rn"] == 13 and f["wb"] and not f["user"] and (
                (f["load"] and f["mode"] == "ia") or (not f["load"] and f["mode"] == "db")):
            return f"{'pop' if f['load'] else 'push'}{c} {_reglist(f['regs'])}"
        name = ("ldm" if f["load"] else "stm") + f["mode"]
        return f"{name}{c} {REG[f['rn']]}{'!' if f['wb'] else ''}, {_reglist(f['regs'])}{'^' if f['user'] else ''}"
    if k == "swp":
        return f"swp{'b' if f['byte'] else ''}{c} {REG[f['rd']]}, {REG[f['rm']]}, [{REG[f['rn']]}]"
    if k == "b":
        name = "blx" if f["exchange"] else ("bl" if f["link"] else "b")
        return f"{name}{c} 0x{f['target']:08x}"
    if k == "bx":
        return f"{'blx' if f['link'] else 'bx'}{c} {REG[f['rm']]}"
    if k == "swi":
        return f"swi{c} {_imm(f['imm'])}"
    if k == "bkpt":
        return f"bkpt {_imm(f['imm'])}"
    if k == "pld":
        w = i.raw
        off = w & 0xFFF if not w & 0x2000000 else None
        sign = "" if w & 0x800000 else "-"
        if off is None:
            return f"pld [{REG[(w >> 16) & 15]}, {sign}{REG[w & 15]}]"
        return f"pld [{REG[(w >> 16) & 15]}, #{sign}0x{off:x}]"
    if k == "blhi":
        return f"bl (first half) lr = 0x{f['value']:08x}"
    if k == "cop":
        op = f["op"]
        if op in ("mcr", "mrc"):
            return (f"{op}{c} p{f['cp']}, {f['opc1']}, {REG[f['rd']]}, c{f['crn']}, c{f['crm']}, {f['opc2']}")
        if op == "cdp":
            return f"cdp{c} p{f['cp']}, {f['opc1']}, c{f['crd']}, c{f['crn']}, c{f['crm']}, {f['opc2']}"
        if op in ("mcrr", "mrrc"):
            return f"{op}{c} p{f['cp']}, {f['opc']}, {REG[f['rd']]}, {REG[f['rn']]}, c{f['crm']}"
        return f"{op}{'l' if f['long'] else ''}{c} p{f['cp']}, c{f['crd']}, [{REG[f['rn']]}, #{f['imm']}]"
    return f".word 0x{i.raw:08x}" if i.size == 4 else f".hword 0x{i.raw:04x}"


def decode_at(data: bytes, offset: int, addr: int, thumb: bool) -> Insn | None:
    """Decode one instruction at data[offset] (address addr). None if past the end."""
    if thumb:
        if offset + 2 > len(data):
            return None
        h = data[offset] | data[offset + 1] << 8
        nxt = data[offset + 2] | data[offset + 3] << 8 if offset + 4 <= len(data) else None
        return decode_thumb(h, addr, nxt)
    if offset + 4 > len(data):
        return None
    w = data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24
    return decode_arm(w, addr)
