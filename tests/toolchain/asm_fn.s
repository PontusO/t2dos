@ asm_fn.s - with c_calls_asm.c: functions called from C (arguments on the stack, the
@ first deepest, result in r0) and a data word C reads and writes
        .syntax unified
        .thumb
        .global asm_sub3, asm_get, asm_apply, asm_val
        .text
asm_sub3:                               @ (a, b, c) -> a - b - c
        ldr     r0, [sp, #8]
        ldr     r1, [sp, #4]
        subs    r0, r0, r1
        ldr     r1, [sp]
        subs    r0, r0, r1
        bx      lr

asm_get:                                @ () -> asm_val
        movw    r1, #:lower16:asm_val
        movt    r1, #:upper16:asm_val
        add.w   r1, r9, r1
        ldr     r0, [r1]
        bx      lr

asm_apply:                              @ (f, x) -> f(x), a call back into C
        push    {r4, lr}
        ldr     r1, [sp, #12]           @ f
        ldr     r0, [sp, #8]            @ x
        push    {r0}
        blx     r1
        add     sp, #4
        pop     {r4, pc}

        .data
pad:    .byte   1, 2, 3                 @ asm_val is not at offset 0
        .align  2
asm_val: .word  1234
