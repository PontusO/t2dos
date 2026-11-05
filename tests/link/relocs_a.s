@ relocs_a.s - with relocs_b.s: every relocation type across files and within a file.
@ Each line of output is "name value" or "name offset string": a data offset and the
@ string found at r9 + offset (show and num take the name's address in r1).
@ This file's text is 310 bytes, so relocs_b's text starts at 312 after one nop; this file's
@ data is 127 bytes, so relocs_b's data starts at 128, and its bfar (5143 in relocs_b) is at
@ 5271, beyond addw's reach. relocs.expected lists the values.
        .syntax unified
        .thumb
        .global main, show, num, aadd, anz, aval, amsg, atail
        .text
main:
        push    {r4, r5, r6, lr}
@ data references to this file's data and to relocs_b's
        addw    r0, r9, #amsg           @ R_DATA_ADDW, own
        sub     r0, r0, r9              @ the data offset
        addw    r1, r9, #n_addw_own
        bl      show
        addw    r0, r9, #bmsg           @ R_DATA_ADDW, other
        sub     r0, r0, r9              @ the data offset
        addw    r1, r9, #n_addw_b
        bl      show
        movw    r0, #:lower16:atail     @ R_DATA_MOVW/MOVT, own
        movt    r0, #:upper16:atail
        addw    r1, r9, #n_movw_own
        bl      show
        movw    r0, #:lower16:bfar + 2  @ R_DATA_MOVW/MOVT, other, addend
        movt    r0, #:upper16:bfar + 2
        addw    r1, r9, #n_movw_b
        bl      show
        movw.16 r0, #atail + 2          @ R_DATA_MOVW16, own, addend
        addw    r1, r9, #n_movw16_own
        bl      show
        movw.16 r0, #bfar               @ R_DATA_MOVW16, other (above 4095)
        addw    r1, r9, #n_movw16_b
        bl      show
@ R_WORD in this file's data: a table of data offsets, used with r9
        addw    r4, r9, #atab
        ldr     r0, [r4]                @ .word bmsg
        addw    r1, r9, #n_word0
        bl      show
        ldr     r0, [r4, #4]            @ .word amsg + 2
        addw    r1, r9, #n_word1
        bl      show
@ branches into relocs_b (forward)
        movs    r0, #5
        bl      badd                    @ R_BL: 5 + 100
        addw    r1, r9, #n_bl
        bl      num
        movs    r0, #5
        bl      atail1                  @ R_BW: 5 + 1 + 100
        addw    r1, r9, #n_bw
        bl      num
        movs    r0, #0
        bl      apick                   @ R_BCOND taken: bzero gives 200
        addw    r1, r9, #n_bcond_t
        bl      num
        movs    r0, #7
        bl      apick                   @ R_BCOND not taken: 1
        addw    r1, r9, #n_bcond_n
        bl      num
        fnaddr  r2, bval                @ R_FNADDR: relocs_b's bval gives 66
        blx     r2
        addw    r1, r9, #n_fnaddr
        bl      num
@ relocs_b's checks, then the exit code from relocs_b
        bl      bcheck
        pop     {r4, r5, r6, pc}

atail1: adds    r0, r0, #1
        b.w     badd                    @ R_BW: a tail call into relocs_b
apick:  cmp     r0, #0
        beq     bzero                   @ R_BCOND (an unsized b<cond> to another file is wide)
        movs    r0, #1
        bx      lr

aadd:   adds    r0, r0, #10             @ for relocs_b: r0 + 10
        bx      lr
anz:    movs    r0, #55                 @ for relocs_b's b<cond>
        bx      lr
aval:   push    {r4, lr}                @ for relocs_b's fnaddr
        movs    r0, #11
        pop     {r4, pc}

show:                                   @ printf("%s %d %s\n", r1, r0, r9 + r0)
        push    {r4, lr}
        addw    r2, r9, #fshow
        push    {r2}
        push    {r1}
        push    {r0}
        add     r0, r9
        push    {r0}
        mov     r2, sp
        push    {r4}
        mov     r4, sp
        mov     r3, sp
        bic     r3, r3, #7
        mov     sp, r3
        movs    r1, #4
        movs    r0, #72
        blx     r8
        mov     sp, r4
        pop     {r4}
        add     sp, #16
        pop     {r4, pc}

num:                                    @ printf("%s %d\n", r1, r0)
        push    {r4, lr}
        addw    r2, r9, #fnum
        push    {r2}
        push    {r1}
        push    {r0}
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
amsg:   .asciz  "a-data"
        .align  2
atab:   .word   bmsg, amsg + 2          @ 8: R_WORD in data, to relocs_b's data and to this file's
atail:  .asciz  "a-end"
fshow:  .asciz  "%s %d %s\n"
fnum:   .asciz  "%s %d\n"
n_addw_own:     .asciz "addw"
n_addw_b:       .asciz "addw-b"
n_movw_own:     .asciz "movw"
n_movw_b:       .asciz "movw-b"
n_movw16_own:   .asciz "movw16"
n_movw16_b:     .asciz "movw16-b"
n_word0:        .asciz "word0"
n_word1:        .asciz "word1"
n_bl:           .asciz "bl"
n_bw:           .asciz "b.w"
n_bcond_t:      .asciz "bcond-taken"
n_bcond_n:      .asciz "bcond-not"
n_fnaddr:       .asciz "fnaddr"
