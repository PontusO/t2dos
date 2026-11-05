@ lib_c.s - library member nobody calls: it must not be linked
        .syntax unified
        .thumb
        .global fc
        .text
fc:
        movs    r0, #7
        bx      lr
