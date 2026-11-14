@ The code Bright Dawn puts where the DS main processor's BIOS would be.
@
@ This is not Nintendo's BIOS and contains none of it. It is the small part of
@ a BIOS that has to run as code on the processor, because the program's own
@ interrupt handler runs in the middle of it: the interrupt entry, and the
@ three calls that wait for an interrupt. Every other BIOS call is done
@ natively (src/nds/bios.cpp) and never comes here.
@
@ Assembled for address 0xFFFF0000.
        .syntax unified
        .arm
        .text
vectors:
        b       unexpected              @ 00  reset
        b       unexpected              @ 04  undefined instruction
        b       call_entry              @ 08  software interrupt
        b       unexpected              @ 0C  prefetch abort
        b       unexpected              @ 10  data abort
        b       unexpected              @ 14  (unused)
        b       interrupt_entry         @ 18  interrupt
        b       unexpected              @ 1C  fast interrupt

@ Nothing should arrive here. An undefined instruction stops the run with
@ this address in the report.
unexpected:
        .word   0xE7F000F0

@ --- An interrupt ---------------------------------------------------------
@ IRQ mode, interrupts masked, lr = where to return + 4. The program keeps
@ the address of its handler in the last word of the data memory; it is
@ called as a function, and may use r0-r3 and r12.
interrupt_entry:
        stmdb   sp!, {r0-r3, r12, lr}
        mrc     p15, 0, r0, c9, c1, 0   @ where the data memory is...
        mov     r0, r0, lsr #12
        mov     r0, r0, lsl #12
        add     r0, r0, #0x4000         @ ...and where it ends
        add     lr, pc, #0              @ the handler returns to the ldmia
        ldr     pc, [r0, #-4]
        ldmia   sp!, {r0-r3, r12, lr}
        subs    pc, lr, #4

@ --- A BIOS call that waits -------------------------------------------------
@ Supervisor mode, interrupts masked, lr = the instruction after the call.
@ The call runs in System mode, on the caller's stack and with interrupts as
@ the caller had them.
call_entry:
        stmdb   sp!, {r11, r12, lr}
        ldrb    r12, [lr, #-2]          @ the call's number, in both instruction sets
        mrs     r11, spsr
        stmdb   sp!, {r11}
        and     r11, r11, #0x80
        orr     r11, r11, #0x1F
        msr     cpsr_c, r11
        stmdb   sp!, {lr}
        cmp     r12, #6
        bleq    halt
        cmp     r12, #5
        moveq   r0, #1                  @ VBlankIntrWait is IntrWait(1, 1)
        moveq   r1, #1
        moveq   r12, #4
        cmp     r12, #4
        bleq    interrupt_wait
        ldmia   sp!, {lr}
        mov     r12, #0xD3
        msr     cpsr_c, r12
        ldmia   sp!, {r11}
        msr     spsr_fc, r11
        ldmia   sp!, {r11, r12, lr}
        movs    pc, lr

@ Halt (6): sleep until an enabled interrupt is requested.
halt:
        mov     r0, #0
        mcr     p15, 0, r0, c7, c0, 4
        mov     r12, #0                 @ not 4, 5 or 6: nothing more to do
        bx      lr

@ IntrWait (4): r0 = 1 to discard what was already flagged, r1 = the
@ interrupts to wait for. The program's handler records each interrupt it
@ has dealt with in the word before its own address; this waits until one of
@ the wanted ones is recorded there, and clears it.
@
@ As on the console's main processor, r0 = 0 does not return at once when a
@ wanted interrupt is already recorded: at least one interrupt is waited for.
@
@ (*) That the call turns the interrupt master switch on is from recollection
@ of the documentation alone and has not been checked against anything.
interrupt_wait:
        stmdb   sp!, {r4, r5, lr}
        mov     r5, r1
        mrc     p15, 0, r4, c9, c1, 0
        mov     r4, r4, lsr #12
        mov     r4, r4, lsl #12
        add     r4, r4, #0x4000         @ the record is at [r4, #-8]
        mov     r2, #0x04000000
        mov     r3, #1
        str     r3, [r2, #0x208]        @ the master switch is turned on (*)
        cmp     r0, #0
        beq     1f
        mrs     r12, cpsr               @ discard: clear the wanted ones,
        orr     r3, r12, #0x80          @ with interrupts masked meanwhile
        msr     cpsr_c, r3
        ldr     r2, [r4, #-8]
        bic     r2, r2, r5
        str     r2, [r4, #-8]
        msr     cpsr_c, r12
1:      mov     r0, #0
        mcr     p15, 0, r0, c7, c0, 4   @ sleep; the interrupt is taken on waking
        mrs     r12, cpsr
        orr     r3, r12, #0x80
        msr     cpsr_c, r3
        ldr     r2, [r4, #-8]
        ands    r3, r2, r5
        bicne   r2, r2, r5
        strne   r2, [r4, #-8]
        msr     cpsr_c, r12             @ (the control field only: the flags stay)
        beq     1b
        mov     r12, #0
        ldmia   sp!, {r4, r5, pc}
