@ dsz_a.s - with dsz_b.s, dsz_c.s: data of 1 byte, then 3 bytes, then none: the data size is 7
        .syntax unified
        .thumb
        .global main
        .text
main:
        movs    r0, #0
        bx      lr
        .data
        .byte   1
