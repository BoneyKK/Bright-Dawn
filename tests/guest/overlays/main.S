@ Overlay test program, main part. Loads at 0x02000000; og_run is its entry.
@
@ og_run(a, b) calls into the overlays three ways: straight into the slot
@ that two overlays share, through a function pointer the loaded overlay
@ exports, and straight into an overlay that has an address range to itself.
        .syntax unified
        .arm
        .text
        .global og_run
        .type og_run, %function
og_run:
        push    {r4, r5, r6, lr}
        mov     r4, r0
        mov     r5, r1
        bl      slot_entry              @ whichever overlay is in the shared slot
        mov     r6, r0
        ldr     r2, =slot_table
        ldr     r2, [r2]                @ a function pointer that overlay exports
        mov     r0, r4
        mov     r1, r5
        blx     r2
        eor     r6, r6, r0
        mov     r0, r4
        mov     r1, r5
        bl      fixed_entry             @ the overlay with its own address range
        add     r0, r0, r6
        pop     {r4, r5, r6, pc}
        .ltorg

@ Called only from the overlays, so it can be found only by following calls
@ from one module into another. At a fixed address (0x02000100), which is
@ what the overlays are linked against.
        .org    0x100
        .global main_helper
        .type main_helper, %function
main_helper:
        push    {r4, lr}
        add     r0, r0, r0, lsl #2
        add     r0, r0, #9
        pop     {r4, pc}
