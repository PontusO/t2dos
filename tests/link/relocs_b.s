@ relocs_b.s - with relocs_a.s: the callee side, and the same checks from the second object,
@ whose text and data do not start at 0: branches back into relocs_a, its own data through
@ named and .L labels (the .data section symbol), and R_WORD in its text and data.
        .syntax unified
        .thumb
        .global badd, bzero, bval, bcheck, bmsg, bfar
        .text
badd:   adds    r0, r0, #100            @ for relocs_a's bl and b.w
        bx      lr
bzero:  movs    r0, #200                @ for relocs_a's b<cond>
        bx      lr
bval:   push    {r4, lr}                @ for relocs_a's fnaddr
        movs    r0, #66
        pop     {r4, pc}

bcheck:
        push    {r4, r5, r6, lr}
@ own data through a .L label: relocations against .data with the label's offset as addend
        addw    r0, r9, #.Lbloc         @ R_DATA_ADDW .data+7
        sub     r0, r0, r9              @ the data offset
        addw    r1, r9, #n_addw_l
        bl      show
        movw    r0, #:lower16:.Lbloc + 2 @ R_DATA_MOVW/MOVT .data+9
        movt    r0, #:upper16:.Lbloc + 2
        addw    r1, r9, #n_movw_l
        bl      show
        movw.16 r0, #.Lbloc + 1         @ R_DATA_MOVW16 .data+8
        addw    r1, r9, #n_movw16_l
        bl      show
        addw    r0, r9, #amsg           @ relocs_a's data, from the second object
        sub     r0, r0, r9              @ the data offset
        addw    r1, r9, #n_addw_a
        bl      show
@ R_WORD in this file's data (.L label and relocs_a's amsg) and in its text (a literal pool)
        addw    r4, r9, #btab
        ldr     r0, [r4]
        addw    r1, r9, #n_bword0
        bl      show
        ldr     r0, [r4, #4]
        addw    r1, r9, #n_bword1
        bl      show
        .align  2                       @ PC-relative loads of R_WORDs in text: they read the
        ldr.w   r0, [pc, #4]            @ right word only if this object's text is 4-aligned
        b.n     .Lw1
        nop
        .word   bfar
.Lw1:
        addw    r1, r9, #n_tword0
        bl      show
        .align  2
        ldr.w   r0, [pc, #4]
        b.n     .Lw2
        nop
        .word   atail + 1
.Lw2:
        addw    r1, r9, #n_tword1
        bl      show
@ branches back into relocs_a
        movs    r0, #5
        bl      aadd                    @ R_BL backward: 5 + 10
        addw    r1, r9, #n_bl_back
        bl      num
        movs    r0, #5
        bl      btail1                  @ R_BW backward: 5 + 2 + 10
        addw    r1, r9, #n_bw_back
        bl      num
        movs    r0, #3
        bl      bpick                   @ R_BCOND backward, taken: anz gives 55
        addw    r1, r9, #n_bcond_t_back
        bl      num
        movs    r0, #0
        bl      bpick                   @ not taken: 2
        addw    r1, r9, #n_bcond_n_back
        bl      num
        fnaddr  r2, aval                @ R_FNADDR backward: 11
        blx     r2
        mov     r5, r0
        addw    r1, r9, #n_fnaddr_back
        bl      num
        adds    r0, r5, #31             @ exit code 42
        pop     {r4, r5, r6, pc}

btail1: adds    r0, r0, #2
        b.w     aadd                    @ R_BW
bpick:  cmp     r0, #0
        bne.w   anz                     @ R_BCOND
        movs    r0, #2
        bx      lr

        .data
bmsg:   .asciz  "b-data"                @ .data+0
.Lbloc: .asciz  "b-local"               @ .data+7
        .align  2
btab:   .word   .Lbloc + 3, amsg        @ R_WORD .data+10, R_WORD amsg+0
n_addw_l:       .asciz "addw-l"
n_movw_l:       .asciz "movw-l"
n_movw16_l:     .asciz "movw16-l"
n_addw_a:       .asciz "addw-a"
n_bword0:       .asciz "bword0"
n_bword1:       .asciz "bword1"
n_tword0:       .asciz "tword0"
n_tword1:       .asciz "tword1"
n_bl_back:      .asciz "bl-back"
n_bw_back:      .asciz "b.w-back"
n_bcond_t_back: .asciz "bcond-taken-back"
n_bcond_n_back: .asciz "bcond-not-back"
n_fnaddr_back:  .asciz "fnaddr-back"
        .space  5000
bfar:   .asciz  "b-far"
