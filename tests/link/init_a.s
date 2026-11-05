@ init_a.s - with init_b.s: each file's .t2init routines run before main, in link order.
@ Each routine sets total = total * 10 + k, so the result shows the order: a (1), then b (2, 3).
        .syntax unified
        .thumb
        .global main, total, step
        .text
main:
        push    {r4, lr}
        addw    r0, r9, #fmt            @ printf(fmt, total)
        push    {r0}
        addw    r0, r9, #total
        ldr     r0, [r0]
        push    {r0}
        mov     r2, sp
        push    {r4}
        mov     r4, sp
        mov     r3, sp
        bic     r3, r3, #7
        mov     sp, r3
        movs    r1, #2
        movs    r0, #72
        blx     r8
        mov     sp, r4
        pop     {r4}
        add     sp, #8
        addw    r0, r9, #total
        ldr     r0, [r0]
        pop     {r4, pc}

step:                                   @ total = total * 10 + r0
        addw    r1, r9, #total
        ldr     r2, [r1]
        movs    r3, #10
        muls    r2, r3, r2
        adds    r2, r2, r0
        str     r2, [r1]
        bx      lr

        .t2init init_a
init_a:                                 @ a normal function: saves lr and returns
        push    {r4, lr}
        movs    r0, #1
        bl      step
        pop     {r4, pc}

        .data
fmt:    .asciz  "init: %d\n"
        .align  2
total:  .word   0
