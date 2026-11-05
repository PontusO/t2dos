@ hello.s - one file: main prints through the printf syscall (72) and returns 0
        .syntax unified
        .thumb
        .global main
        .text
main:
        push    {r4, lr}
        addw    r0, r9, #msg            @ the string: data base (r9) + its data offset
        push    {r0}                    @ printf(msg), the call_sys sequence of compiler/cc.c
        mov     r2, sp
        push    {r4}
        mov     r4, sp
        mov     r3, sp
        bic     r3, r3, #7
        mov     sp, r3
        movs    r1, #1
        movs    r0, #72
        blx     r8
        mov     sp, r4
        pop     {r4}
        add     sp, #4
        movs    r0, #0
        pop     {r4, pc}

        .data
msg:    .asciz  "hello, world\n"
