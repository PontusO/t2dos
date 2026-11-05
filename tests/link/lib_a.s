@ lib_a.s - library member: fa(x) = fb(x) + 100 (fb is in lib_b)
        .syntax unified
        .thumb
        .global fa
        .text
fa:
        push    {r4, lr}
        bl      fb
        adds    r0, r0, #100
        pop     {r4, pc}
