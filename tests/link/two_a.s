@ two_a.s - with two_b.s: calls across the files both ways and globals of this file used there
        .syntax unified
        .thumb
        .global main, addone, base, tag
        .text
main:
        push    {r4, r5, r6, lr}
        movs    r0, #1
        bl      twice                   @ two_b: r0 = 2 * r0 + base, then addone
        mov     r5, r0
        bl      say                     @ two_b: printf("%s: %d\n", tag, r0)
        mov     r0, r5
        pop     {r4, r5, r6, pc}

addone:                                 @ called from two_b
        adds    r0, r0, #1
        bx      lr

        .data
base:   .word   40
tag:    .asciz  "two"
pad:    .byte   7                       @ 9 bytes of data: two_b's data starts at 12
