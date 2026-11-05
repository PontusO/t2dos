@ asm_calls_c.s - with c_fn.c: main in assembly calls C functions (arguments pushed
@ first to last, the caller pops them) and reads a C global
        .syntax unified
        .thumb
        .global main
        .text
main:
        push    {r4, lr}
        movs    r0, #6
        push    {r0}
        movs    r0, #7
        push    {r0}
        bl      c_mul                   @ c_mul(6, 7)
        add     sp, #8
        push    {r0}
        bl      report                  @ report(42)
        add     sp, #4
        bl      c_bump
        bl      c_bump
        bl      c_bump
        movw    r1, #:lower16:c_count   @ C's global, after three bumps
        movt    r1, #:upper16:c_count
        add.w   r1, r9, r1
        ldr     r0, [r1]
        push    {r0}
        bl      report
        add     sp, #4
        movw    r1, #:lower16:c_count
        movt    r1, #:upper16:c_count
        add.w   r1, r9, r1
        ldr     r0, [r1]
        adds    r0, r0, #10             @ exit code 13
        pop     {r4, pc}
