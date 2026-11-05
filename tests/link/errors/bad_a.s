@ bad_a.s - with bad_b.o, which is not an object
        .syntax unified
        .thumb
        .global main
        .text
main:   movs    r0, #0
        bx      lr
