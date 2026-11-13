@ Overlay C. Loads at 0x02020000, which no other overlay uses.
        .syntax unified
        .arm
        .text
        .global fixed_entry
        .type fixed_entry, %function
fixed_entry:                            @ (a - b) + main_helper(a)
        push    {r4, lr}
        sub     r4, r0, r1
        bl      main_helper
        add     r0, r0, r4
        pop     {r4, pc}
