@ dup_b.s - the second definition of twice
        .syntax unified
        .thumb
        .global twice
        .text
twice:  lsls    r0, r0, #1
        bx      lr
