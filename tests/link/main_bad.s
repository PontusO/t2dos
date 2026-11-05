@ main_bad.s - calls fbad (lib_bad.s)
        .syntax unified
        .thumb
        .global main
        .text
main:
        push    {r4, lr}
        bl      fbad
        pop     {r4, pc}
