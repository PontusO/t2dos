@ lib_bad.s - library member that needs a symbol nobody defines
        .syntax unified
        .thumb
        .global fbad
        .text
fbad:
        push    {r4, lr}
        bl      nowhere
        pop     {r4, pc}
