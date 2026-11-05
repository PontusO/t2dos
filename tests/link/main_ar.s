@ main_ar.s - calls fa(1) = 103, prints it and returns it
        .syntax unified
        .thumb
        .global main
        .text
main:
        push    {r4, r5, r6, lr}
        movs    r0, #1
        bl      fa
        mov     r5, r0
        mov     r1, r0
        addw    r0, r9, #fmt
        push    {r0}
        push    {r1}
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
        mov     r0, r5
        pop     {r4, r5, r6, pc}

        .data
fmt:    .asciz  "ar: %d\n"
