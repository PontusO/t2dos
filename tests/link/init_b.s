@ init_b.s - with init_a.s: two .t2init routines in one file (a named label and a .L label),
@ run in symbol order after init_a's
        .syntax unified
        .thumb
        .text
        .t2init init_b
        .t2init .Linit_b2
init_b:
        push    {r4, lr}
        movs    r0, #2
        bl      step                    @ init_a's
        pop     {r4, pc}
.Linit_b2:
        push    {r4, lr}
        addw    r1, r9, #total          @ init_a's global, used directly
        ldr     r0, [r1]
        movs    r2, #10
        muls    r0, r2, r0
        adds    r0, r0, #3
        str     r0, [r1]
        pop     {r4, pc}
