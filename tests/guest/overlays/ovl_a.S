@ Overlay A. Loads at 0x02010000, the slot it shares with overlay B.
        .syntax unified
        .arm
        .text
        .global slot_entry
        .type slot_entry, %function
slot_entry:                             @ a + 3b + main_helper(b)
        push    {r4, lr}
        add     r4, r0, r1
        add     r4, r4, r1, lsl #1
        mov     r0, r1
        bl      main_helper
        add     r0, r0, r4
        pop     {r4, pc}

        .type second_a, %function
second_a:                               @ (a ^ b) + 0x11
        push    {r4, lr}
        eor     r0, r0, r1
        add     r0, r0, #0x11
        pop     {r4, pc}

        .org    0x80
        .global slot_table
slot_table:
        .word   second_a
