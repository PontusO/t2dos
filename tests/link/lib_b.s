@ lib_b.s - library member: fb(x) = 2x + 1, and fz (never used)
        .syntax unified
        .thumb
        .global fb, fz
        .text
fb:
        adds    r0, r0, r0
        adds    r0, r0, #1
        bx      lr
fz:
        movs    r0, #9
        bx      lr
