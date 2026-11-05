@ two_b.s - with two_a.s: twice calls back into two_a, say uses two_a's tag and its own format
        .syntax unified
        .thumb
        .global twice, say
        .text
twice:                                  @ r0 = 2 * r0 + base + 1
        push    {r4, lr}
        adds    r0, r0, r0
        addw    r1, r9, #base           @ two_a's global
        ldr     r1, [r1]
        adds    r0, r0, r1
        bl      addone                  @ back into two_a
        pop     {r4, pc}

say:                                    @ printf(fmt, tag, r0)
        push    {r4, lr}
        mov     r1, r0
        addw    r0, r9, #fmt            @ this file's data
        push    {r0}
        addw    r0, r9, #tag            @ two_a's data
        push    {r0}
        push    {r1}
        mov     r2, sp
        push    {r4}
        mov     r4, sp
        mov     r3, sp
        bic     r3, r3, #7
        mov     sp, r3
        movs    r1, #3
        movs    r0, #72
        blx     r8
        mov     sp, r4
        pop     {r4}
        add     sp, #12
        pop     {r4, pc}

        .data
fmt:    .asciz  "%s: %d\n"
