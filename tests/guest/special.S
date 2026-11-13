@ Guest tests written in assembly: control flow that compilers do not produce
@ but that old or hand-written code does, and that an executor of DS code
@ must still get right. Each test has the usual shape (two inputs in r0 and
@ r1, result in r0) and an equivalent in C in special_host.c.
@
@ ARM build only.
        .syntax unified
        .text

@ ---------------------------------------------------------------------------
@ handback: two helpers that can only be reached through a pointer table and
@ do not begin the way a compiled function does, so a translator that finds
@ functions by their prologue misses them. One jumps to an ordinary function,
@ the other calls one: code without a translation must hand back to code that
@ has one.
@ ---------------------------------------------------------------------------
        .arm
        .global a_handback
        .type a_handback, %function
a_handback:
        push    {r4, r5, r6, lr}
        mov     r4, r0
        mov     r5, r1
        ldr     r6, =hidden_table
        ldr     r2, [r6]
        blx     r2                      @ hidden_tail(a, b)
        mov     r2, r4
        mov     r4, r0
        mov     r0, r5
        mov     r1, r2
        ldr     r2, [r6, #4]
        blx     r2                      @ hidden_call(b, a)
        eor     r0, r0, r4
        pop     {r4, r5, r6, pc}
        .ltorg

        .thumb
        .global t_handback
        .thumb_func
        .type t_handback, %function
t_handback:
        push    {r4, r5, r6, lr}
        movs    r4, r0
        movs    r5, r1
        ldr     r6, =hidden_table
        ldr     r2, [r6]
        blx     r2
        movs    r2, r4
        movs    r4, r0
        movs    r0, r5
        movs    r1, r2
        ldr     r2, [r6, #4]
        blx     r2
        eors    r0, r4
        pop     {r4, r5, r6, pc}
        .align  2
        .ltorg

        .arm
        .type hidden_tail, %function
hidden_tail:
        add     r0, r0, #1
        b       a_leaf                  @ a jump to a translated function

        .type hidden_call, %function
hidden_call:
        str     r4, [sp, #-4]!
        mov     r4, lr
        bl      a_leaf                  @ a call to a translated function
        add     r0, r0, #7
        mov     r2, r4
        ldr     r4, [sp], #4
        bx      r2                      @ returns through a register

        .section .rodata
        .align  2
hidden_table:
        .word   hidden_tail
        .word   hidden_call
        .text

@ ---------------------------------------------------------------------------
@ retreg: a function that returns by popping its return address into a
@ register and branching to it (the ARMv4T epilogue), which is not one of the
@ instruction forms normally taken to be a return.
@ ---------------------------------------------------------------------------
        .arm
        .global a_retreg
        .type a_retreg, %function
a_retreg:
        push    {r4, lr}
        ldr     r2, =0x1234
        eor     r0, r0, r2
        bl      a_leaf
        add     r0, r0, #3
        ldr     r4, [sp], #4
        ldr     r3, [sp], #4
        bx      r3
        .ltorg

        .thumb
        .global t_retreg
        .thumb_func
        .type t_retreg, %function
t_retreg:
        push    {r4, lr}
        ldr     r2, =0x1234
        eors    r0, r2
        bl      t_leaf
        adds    r0, #3
        pop     {r4}
        pop     {r3}
        bx      r3
        .align  2
        .ltorg

@ ---------------------------------------------------------------------------
@ v4call: a call made the ARMv4 way, by loading the return address into lr
@ and then branching. The branch looks like a jump, but the callee returns to
@ the instruction after it.
@ ---------------------------------------------------------------------------
        .arm
        .global a_v4call
        .type a_v4call, %function
a_v4call:
        push    {r4, lr}
        ldr     r4, =a_leaf
        mov     lr, pc
        bx      r4
        add     r0, r0, #5
        pop     {r4, lr}
        bx      lr
        .ltorg

        .thumb
        .global t_v4call
        .thumb_func
        .type t_v4call, %function
t_v4call:
        push    {r4, lr}
        blx     a_v4call
        pop     {r4, pc}

@ ---------------------------------------------------------------------------
@ inltab: a helper that never returns to its caller's next instruction.
@ The caller places a table of addresses after the call; the helper picks one
@ and jumps there. Some compilers implement switch statements this way.
@ ---------------------------------------------------------------------------
        .arm
        .global a_inltab
        .type a_inltab, %function
a_inltab:
        push    {r4, lr}
        and     r2, r0, #3
        bl      inltab_helper
        .word   1f, 2f, 3f, 4f
1:      add     r0, r1, #1
        b       9f
2:      add     r0, r1, r1
        b       9f
3:      eor     r0, r0, r1
        b       9f
4:      rsb     r0, r1, #0
9:      pop     {r4, lr}
        bx      lr

        .type inltab_helper, %function
inltab_helper:
        ldr     pc, [lr, r2, lsl #2]

        .thumb
        .global t_inltab
        .thumb_func
        .type t_inltab, %function
t_inltab:
        push    {r4, lr}
        blx     a_inltab
        pop     {r4, pc}

@ ---------------------------------------------------------------------------
@ nonlocal: for odd inputs the innermost of three nested functions restores
@ the outermost one's stack pointer and jumps straight back into it, skipping
@ the function in between (what longjmp does).
@ ---------------------------------------------------------------------------
        .arm
        .global a_nonlocal
        .type a_nonlocal, %function
a_nonlocal:
        push    {r4, r5, r6, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r2, sp
        adr     r3, 2f
        bl      nl_mid
        add     r0, r0, #100
        b       3f
2:      add     r0, r4, r5              @ only ever reached from nl_inner
        add     r0, r0, #7
3:      pop     {r4, r5, r6, lr}
        bx      lr

        .type nl_mid, %function
nl_mid:
        push    {r4, lr}
        bl      nl_inner
        add     r0, r0, #1
        pop     {r4, lr}
        bx      lr

        .type nl_inner, %function
nl_inner:
        tst     r0, #1
        bxeq    lr
        mov     sp, r2
        bx      r3

        .thumb
        .global t_nonlocal
        .thumb_func
        .type t_nonlocal, %function
t_nonlocal:
        push    {r4, lr}
        blx     a_nonlocal
        pop     {r4, pc}

@ ---------------------------------------------------------------------------
@ skipdata: a directly called helper that reads a word placed after the call
@ and returns past it, so the call does not return to its own return address.
@ ---------------------------------------------------------------------------
        .arm
        .global a_skipdata
        .type a_skipdata, %function
a_skipdata:
        push    {r4, lr}
        bl      skip_helper
        .word   0x00001234
        add     r0, r0, r1
        pop     {r4, lr}
        bx      lr

        .type skip_helper, %function
skip_helper:
        ldr     r2, [lr]
        add     r0, r0, r2
        add     lr, lr, #4
        bx      lr

        .thumb
        .global t_skipdata
        .thumb_func
        .type t_skipdata, %function
t_skipdata:
        push    {r4, lr}
        blx     a_skipdata
        pop     {r4, pc}

@ ---------------------------------------------------------------------------
@ svc: software interrupts, from the usual two-instruction wrapper, which
@ relies on lr surviving the interrupt. One wrapper is an ordinary function;
@ the other is reachable only through a pointer. The tests define software
@ interrupt 5 as r0 = r0 * 3 + r1.
@ ---------------------------------------------------------------------------
        .arm
        .global a_svc
        .type a_svc, %function
a_svc:
        push    {r4, r5, lr}
        mov     r4, r1
        bl      svc_wrap_arm
        add     r0, r0, r4
        mov     r1, r4
        ldr     r2, =hidden_svc_table
        ldr     r2, [r2]
        blx     r2
        pop     {r4, r5, pc}
        .ltorg

        .type svc_wrap_arm, %function
svc_wrap_arm:
        svc     0x050000
        bx      lr

        .type hidden_svc_arm, %function
hidden_svc_arm:
        svc     0x050000
        bx      lr

        .thumb
        .global t_svc
        .thumb_func
        .type t_svc, %function
t_svc:
        push    {r4, r5, lr}
        movs    r4, r1
        bl      svc_wrap_thumb
        adds    r0, r0, r4
        movs    r1, r4
        ldr     r2, =hidden_svc_table
        ldr     r2, [r2, #4]
        blx     r2
        pop     {r4, r5, pc}
        .align  2
        .ltorg

        .thumb_func
        .type svc_wrap_thumb, %function
svc_wrap_thumb:
        svc     5
        bx      lr

        .thumb_func
        .type hidden_svc_thumb, %function
hidden_svc_thumb:
        svc     5
        bx      lr

        .section .rodata
        .align  2
hidden_svc_table:
        .word   hidden_svc_arm
        .word   hidden_svc_thumb
        .text

@ ---------------------------------------------------------------------------
@ coro: two flows of control on two stacks, switching back and forth, the
@ way a thread scheduler does. co_switch saves the registers of the current
@ flow and loads those of the other, so it returns into a different function
@ on a different stack every time it is called.
@ ---------------------------------------------------------------------------
        .arm
        .type co_switch, %function
co_switch:                              @ r0: where to save, r1: what to resume
        stmia   r0, {r4-r11, sp, lr}
        ldmia   r1, {r4-r11, sp, lr}
        bx      lr

        .global a_coro
        .type a_coro, %function
a_coro:
        push    {r4, r5, r6, r7, lr}
        ldr     r4, =coro_main_context
        ldr     r5, =coro_other_context
        ldr     r6, =coro_shared
        str     r0, [r6]                @ the value being worked on
        str     r1, [r6, #4]
        ldr     r2, =coro_stack_top     @ the other flow starts in co_body
        str     r2, [r5, #32]           @ on its own stack
        ldr     r2, =co_body
        str     r2, [r5, #36]
        mov     r7, #3
1:      mov     r0, r4
        mov     r1, r5
        bl      co_switch               @ let the other flow run one step
        subs    r7, r7, #1
        bne     1b
        ldr     r0, [r6]
        pop     {r4, r5, r6, r7, lr}
        bx      lr
        .ltorg

        .type co_body, %function
co_body:                                @ never returns; only ever switched away from
        ldr     r4, =coro_main_context
        ldr     r5, =coro_other_context
        ldr     r6, =coro_shared
2:      ldr     r0, [r6]
        ldr     r1, [r6, #4]
        bl      a_leaf
        str     r0, [r6]
        mov     r0, r5
        mov     r1, r4
        bl      co_switch               @ back to the first flow
        b       2b
        .ltorg

        .thumb
        .global t_coro
        .thumb_func
        .type t_coro, %function
t_coro:
        push    {r4, lr}
        blx     a_coro
        pop     {r4, pc}

        .bss
        .align  3
coro_main_context:
        .space  40
coro_other_context:
        .space  40
coro_shared:
        .space  8
        .space  512
coro_stack_top:
        .text

@ ---------------------------------------------------------------------------
@ modes: the processor's modes each have a stack pointer, a link register and
@ a saved status register of their own. Start-up code and interrupt handlers
@ depend on that. This sets them in two modes, checks that System mode's are
@ untouched, reads them back, and moves the System mode registers from
@ another mode with the "^" forms of STM and LDM. FIQ mode has r8 to r12 to
@ itself as well.
@ ---------------------------------------------------------------------------
        .arm
        .global a_modes
        .type a_modes, %function
a_modes:
        push    {r4, r5, r6, r7, r8, r9, r10, r11, lr}
        sub     sp, sp, #8              @ two words of scratch memory
        mov     r7, sp
        mrs     r4, cpsr                @ System mode
        mov     r5, sp
        mov     r6, lr
        bic     r8, r4, #0x1F           @ the status register without its mode

        orr     r2, r8, #0x12           @ --- IRQ mode
        msr     cpsr_c, r2
        add     sp, r0, #0x100
        eor     lr, r0, r1
        and     r2, r1, #0xF0000000
        orr     r2, r2, #0x1F
        msr     spsr_fc, r2

        orr     r2, r8, #0x13           @ --- Supervisor mode
        msr     cpsr_c, r2
        add     sp, r1, #0x200
        sub     lr, r0, r1
        and     r2, r0, #0xF0000000
        orr     r2, r2, #0x92
        msr     spsr_fc, r2

        msr     cpsr_c, r4              @ --- System mode: nothing of it changed
        eor     r9, sp, r5
        eor     r2, lr, r6
        orr     r9, r9, r2              @ r9 = 0 if all is well

        orr     r2, r8, #0x12           @ --- IRQ mode again: read back
        msr     cpsr_c, r2
        mov     r12, sp
        add     r12, r12, lr, ror #7
        mrs     r2, spsr
        eor     r12, r12, r2
        stmia   r7, {sp, lr}^           @ System mode's sp and lr, seen from here
        nop
        ldr     r2, [r7]
        eor     r2, r2, r5
        orr     r9, r9, r2
        ldr     r2, [r7, #4]
        eor     r2, r2, r6
        orr     r9, r9, r2
        ldr     r2, =0x55AA
        eor     r2, r0, r2
        str     r2, [r7]
        add     r2, r1, #3
        str     r2, [r7, #4]
        ldmia   r7, {sp, lr}^           @ and set from here
        nop
        add     r2, r0, #0x100          @ this mode's own are not affected
        eor     r2, r2, sp
        orr     r9, r9, r2

        orr     r2, r8, #0x13           @ --- Supervisor mode again: read back
        msr     cpsr_c, r2
        add     r12, r12, sp
        eor     r12, r12, lr, ror #13
        mrs     r2, spsr
        add     r12, r12, r2

        msr     cpsr_c, r4              @ --- System mode, with the values set from IRQ mode
        eor     r12, r12, sp
        add     r12, r12, lr
        mov     sp, r5
        mov     lr, r6

        mvn     r10, r0                 @ FIQ mode also has r8-r12 to itself
        add     r11, r1, #9
        orr     r2, r8, #0x11           @ --- FIQ mode
        msr     cpsr_c, r2
        mov     r10, r1
        mov     r11, r0
        add     r3, r10, r11
        msr     cpsr_c, r4              @ --- System mode: its r10 and r11 are untouched
        eor     r12, r12, r10
        add     r12, r12, r11
        orr     r2, r8, #0x11           @ --- FIQ mode again: read back
        msr     cpsr_c, r2
        eor     r3, r3, r10, ror #9
        add     r3, r3, r11
        msr     cpsr_c, r4              @ --- System mode
        add     r12, r12, r3

        add     r0, r12, r9
        add     sp, sp, #8
        pop     {r4, r5, r6, r7, r8, r9, r10, r11, pc}
        .ltorg

        .thumb
        .global t_modes
        .thumb_func
        .type t_modes, %function
t_modes:
        push    {r4, lr}
        blx     a_modes
        pop     {r4, pc}

@ ---------------------------------------------------------------------------
@ hidmodes: the same registers, set on one side of the boundary between code
@ that has a translation and code that has none, and read on the other.
@ ---------------------------------------------------------------------------
        .arm
        .global a_hidmodes
        .type a_hidmodes, %function
a_hidmodes:
        push    {r4, r5, r6, lr}
        mrs     r4, cpsr
        bic     r5, r4, #0x1F
        orr     r2, r5, #0x12           @ IRQ mode: set its sp here
        msr     cpsr_c, r2
        add     sp, r0, #1
        msr     cpsr_c, r4
        ldr     r2, =hidden_modes_table
        ldr     r2, [r2]
        blx     r2                      @ r0 = IRQ sp as the hidden code sees it
        orr     r2, r5, #0x12           @ IRQ mode: lr as the hidden code set it
        msr     cpsr_c, r2
        add     r0, r0, lr, ror #3
        orr     r2, r5, #0x13           @ Supervisor mode: sp as the hidden code set it
        msr     cpsr_c, r2
        eor     r0, r0, sp
        msr     cpsr_c, r4
        pop     {r4, r5, r6, pc}
        .ltorg

        .type hidden_modes, %function
hidden_modes:                           @ r0 = a, r1 = b
        mrs     r12, cpsr
        bic     r3, r12, #0x1F
        orr     r2, r3, #0x12
        msr     cpsr_c, r2
        mov     r2, sp                  @ what the caller put there
        eor     lr, r1, #0x77
        orr     r3, r3, #0x13
        msr     cpsr_c, r3
        add     sp, r0, r1
        msr     cpsr_c, r12
        mov     r0, r2
        bx      lr
        .ltorg

        .thumb
        .global t_hidmodes
        .thumb_func
        .type t_hidmodes, %function
t_hidmodes:
        push    {r4, lr}
        blx     a_hidmodes
        pop     {r4, pc}

        .section .rodata
        .align  2
hidden_modes_table:
        .word   hidden_modes
        .text

@ ---------------------------------------------------------------------------
@ excret: returns from exception handlers. They restore the status register
@ from the mode's saved copy and continue wherever the link register (or the
@ stack) says, in the instruction set the saved copy names. Two forms: a
@ data-processing instruction that writes pc with the S bit, and a load of
@ several registers including pc with "^".
@ ---------------------------------------------------------------------------
        .arm
        .global a_excret
        .type a_excret, %function
a_excret:
        push    {r4, r5, r6, r7, lr}
        mov     r4, r0
        mov     r5, r1
        mrs     r6, cpsr
        bic     r7, r6, #0x1F
        orr     r2, r7, #0x12           @ IRQ mode
        msr     cpsr_c, r2
        and     r2, r5, #0xF8000000     @ flags from b; System mode, FIQ masked
        orr     r2, r2, #0x5F
        msr     spsr_fc, r2
        adr     lr, 1f + 4
        subs    pc, lr, #4              @ "return from interrupt"
1:      mrs     r2, cpsr                @ System mode again, with those flags
        bic     r2, r2, #0x07000000
        bic     r2, r2, #0x00FF0000
        bic     r2, r2, #0x0000FF00
        add     r4, r4, r2
        msr     cpsr_c, r6              @ unmask again
        mov     r0, r4
        mov     r1, r5
        bl      excret_second
        mov     r0, r4                  @ never reached: excret_second leaves
        pop     {r4, r5, r6, r7, pc}    @ through excret_tail

        .section .rodata
        .align  2
        .word   excret_second           @ a pointer to it, so that the translator finds it
        .text

        .type excret_second, %function
excret_second:                          @ r0 = running value, r1 = b; r4-r7 are the caller's
        push    {r4, lr}                @ begins the way a function does, for the same reason
        pop     {r4, lr}
        sub     sp, sp, #8              @ a two-word "exception stack" in System mode's
        mov     r3, sp
        eor     r2, r0, r1
        str     r2, [r3]                @ a value for r2
        ldr     r2, =excret_thumb
        bic     r2, r2, #1              @ the address alone; the saved status says Thumb
        str     r2, [r3, #4]
        mrs     r12, cpsr
        bic     r12, r12, #0x1F
        orr     r12, r12, #0x13         @ Supervisor mode
        msr     cpsr_c, r12
        mov     sp, r3
        and     r2, r0, #0xF8000000     @ flags from the running value; System mode, Thumb
        orr     r2, r2, #0x3F
        msr     spsr_fc, r2
        ldmia   sp!, {r2, pc}^
        .ltorg

        .thumb
        .thumb_func
        .type excret_thumb, %function
excret_thumb:                           @ System mode, Thumb state, flags as saved
        add     sp, #8                  @ (does not change the flags)
        ldr     r3, =excret_tail
        bx      r3
        .align  2
        .ltorg

        .arm
        .type excret_tail, %function
excret_tail:
        mrs     r3, cpsr
        bic     r3, r3, #0x07000000
        bic     r3, r3, #0x00FF0000
        bic     r3, r3, #0x0000FF00
        eor     r0, r0, r2, ror #5
        add     r0, r0, r3
        pop     {r4, r5, r6, r7, pc}    @ returns for a_excret

        .thumb
        .global t_excret
        .thumb_func
        .type t_excret, %function
t_excret:
        push    {r4, r5, r6, r7, lr}
        movs    r4, #0x5A               @ the registers a callee must preserve
        movs    r5, #0x33
        mov     r6, sp
        movs    r7, #0x0F
        blx     a_excret
        subs    r4, #0x5A               @ each of these is 0 if they were
        subs    r5, #0x33
        mov     r2, sp
        subs    r6, r6, r2
        subs    r7, #0x0F
        orrs    r4, r5
        orrs    r4, r6
        orrs    r4, r7
        adds    r0, r0, r4
        pop     {r4, r5, r6, r7, pc}
