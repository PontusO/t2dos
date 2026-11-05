@ undef.s - two undefined symbols: ld reports each one, then exits 1
        .syntax unified
        .thumb
        .global main
        .text
main:   push    {r4, lr}
        bl      missing_fn
        addw    r0, r9, #missing_data
        pop     {r4, pc}
