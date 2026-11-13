@ Overlay B. Loads at 0x02010000, the slot it shares with overlay A. Its code
@ differs from A's from the first instruction, and its second function is at
@ a different address.
        .syntax unified
        .arm
        .text
        .global slot_entry
        .type slot_entry, %function
slot_entry:                             @ 5a - b
        add     r0, r0, r0, lsl #2
        sub     r0, r0, r1
        bx      lr
        nop
        nop

        .type second_b, %function
second_b:                               @ main_helper(a + b + 1)
        push    {r4, lr}
        add     r0, r0, r1
        add     r0, r0, #1
        bl      main_helper
        pop     {r4, pc}

        .org    0x80
        .global slot_table
slot_table:
        .word   second_b
