@ dup_a.s - with dup_b.s: both define the global twice
        .syntax unified
        .thumb
        .global main, twice
        .text
main:   push    {r4, lr}
        bl      twice
        pop     {r4, pc}
twice:  adds    r0, r0, r0
        bx      lr
