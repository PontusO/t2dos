@ core.s - every instruction form and directive of the assembler core.
@ tools/check_asm.py compares our bytes with arm-none-eabi-as, byte for byte.
        .syntax unified
        .thumb
        .text
        .global start
        .globl  helper
        .extern unused_ext
        .equ    TEN, 10
        .set    BIG, 0x1234
        .equ    NEGFOUR, -4
        .equ    MASK, 0b1010
        .equ    CHR, 'A'
        .thumb_func
start:                          // a C++ comment
        nop                     /* a block comment */
        nop.w
/* a block comment
   over several lines */
@ ---------------------------------------------------------------- mov
        movs    r0, #0
        movs    r7, #255
        movs    r3, #TEN
        movs    r1, #CHR
        movs    r2, #MASK
        movs.w  r0, #256
        movs.w  r8, #1
        mov     r0, #1
        mov     r12, #255
        mov     r0, #256
        mov     r0, #4095
        mov     r0, #65535
        mov     r0, #BIG
        mov     r0, #0xff00ff00
        mov     r0, #0x01010101
        mov     r0, #-1
        mov     r0, #-256
        mov     r0, #0xFFFFFF00
        mov.w   r1, #0x3fc
        mov     r0, r1
        mov     r8, r9
        mov     r7, sp
        mov     sp, r7
        mov     r15, lr
        mov     ip, fp
        movs    r0, r1
        movs    r7, r0
        mov.w   r0, r1
        movs.w  r8, r1
        movw    r0, #0
        movw    r1, #4095
        movw    r12, #65535
        movw    r2, #BIG
        movt    r0, #0
        movt    r3, #65535
        movt    r9, #0x8000
        mvn     r0, #1
        mvn     r0, #0xff00ff00
        mvns    r0, r1
        mvns    r7, r7
        mvn     r0, r1
        mvn.w   r0, r8
        mvns.w  r1, r2
@ ---------------------------------------------------------------- add / sub
        adds    r0, r1, r2
        adds    r7, r6, r5
        adds    r0, r1, #0
        adds    r0, r1, #7
        adds    r0, r0, #1
        adds    r0, #255
        adds    r7, r7, #8
        adds    r0, r1, #8
        adds    r0, r1, #256
        adds.w  r0, r1, #1
        adds    r8, r9, #1
        add     r0, r1, #1
        add     r0, r1, #4095
        add     r0, r1, #256
        add     r0, r1, #-4
        add     r0, r1, #NEGFOUR
        add     r0, r0, r1
        add     r0, r1, r0
        add     r0, r1
        add     r8, r9
        add     r0, r1, r2
        add.w   r0, r9, r1
        add     r12, r11, r10
        adds    r12, r11, r10
        add     r0, sp, #0
        add     r7, sp, #1020
        add     r0, sp, #1024
        add     r0, sp, #4095
        add     sp, #4
        add     sp, #508
        add     sp, sp, #12
        add     sp, sp, #512
        add     sp, r0
        add     r0, pc
        addw    r0, r9, #308
        addw    r0, r7, #0
        addw    r12, r1, #4095
        addw    r0, r1, #1
        subs    r0, r1, r2
        subs    r0, r1, #3
        subs    r0, #255
        subs    r2, r2, #100
        sub     r0, r1, #1
        sub     r0, r1, #4095
        sub     r0, r1, #-8
        sub     r0, r1, r2
        sub.w   sp, sp, r1
        sub     sp, #4
        sub     sp, #508
        sub     sp, sp, #1024
        sub     sp, sp, #4095
        subw    r0, r7, #12
        subw    r1, r0, #4095
        subw    r0, r1, #1
        subs.w  r0, r1, #1
@ ---------------------------------------------------------------- compare
        cmp     r0, #0
        cmp     r7, #255
        cmp     r0, #256
        cmp     r8, #1
        cmp     r0, #-1
        cmp     r0, #-5
        cmp     r0, r1
        cmp     r7, r0
        cmp     r0, r8
        cmp     r8, r9
        cmp     r14, r0
        cmp.w   r0, r1
        cmp.w   r0, #1
        cmn     r0, r1
        cmn     r0, #1
        cmn     r8, r1
        cmn.w   r0, r1
        tst     r0, r1
        tst     r0, #1
        tst     r8, #0xff
        tst.w   r0, r1
        tst     r9, r10
@ ---------------------------------------------------------------- logic
        ands    r0, r1
        ands    r0, r0, r1
        ands    r0, r1, r0
        ands    r0, r1, r2
        and     r0, r1
        and     r0, r1, r2
        and     r0, r0, #255
        and     r0, r0, #0xFFFFFF00
        and.w   r0, r0, r1
        ands.w  r0, r1
        orrs    r0, r1
        orrs    r7, r7, r6
        orr     r0, r0, r1
        orr     r0, r1, #0x100
        orr.w   r8, r8, r9
        eors    r0, r1
        eors    r0, r1, r0
        eor     r0, r1, r2
        eor     r0, r1, #0xff
        eors.w  r0, r0, r1
        bics    r0, r1
        bics    r0, r0, r1
        bics    r0, r1, r0
        bic     r0, r0, r1
        bic.w   r3, r3, #7
        bic     r0, r0, #0xFFFFFF00
@ ---------------------------------------------------------------- shifts
        lsls    r0, r1, #2
        lsls    r0, r1, #0
        lsls    r7, r7, #31
        lsrs    r0, r1, #1
        lsrs    r0, r1, #32
        asrs    r0, r1, #32
        asrs    r7, r2, #5
        lsl     r0, r1, #2
        lsl.w   r0, r1, #2
        lsls.w  r0, r1, #2
        lsr     r8, r9, #31
        asr     r0, r1, #1
        ror     r0, r1, #3
        rors.w  r0, r1, #31
        lsls    r0, r1
        lsls    r0, r0, r1
        lsrs    r7, r6
        asrs    r0, r0, r2
        rors    r0, r1
        lsl     r0, r1, r0
        lsl.w   r0, r1, r0
        lsr.w   r0, r1, r0
        asr.w   r0, r1, r0
        ror.w   r0, r1, r0
        lsls.w  r0, r0, r1
        asr     r12, r11, r10
@ ---------------------------------------------------------------- multiply / divide
        muls    r0, r1
        muls    r0, r1, r0
        muls    r0, r0, r1
        muls    r7, r6, r7
        mul     r0, r1, r0
        mul     r0, r1, r2
        mul     r12, r11, r10
        mul.w   r0, r1, r0
        mls     r0, r2, r0, r1
        mls     r12, r11, r10, r9
        sdiv    r0, r1, r0
        sdiv    r2, r1, r0
        sdiv    r0, r0, r2
        sdiv    r12, r11, r10
        udiv    r0, r1, r0
        udiv    r2, r1, r0
@ ---------------------------------------------------------------- negate / extend
        negs    r0, r0
        negs    r0, r1
        negs    r7, r6
        neg     r0, r1
        neg     r8, r9
        negs.w  r0, r1
        sxtb    r0, r0
        sxtb    r7, r6
        sxtb    r8, r0
        sxtb.w  r0, r1
        uxtb    r0, r0
        uxtb    r1, r9
        sxth    r0, r0
        sxth.w  r2, r3
        uxth    r0, r0
        uxth    r12, r14
@ ---------------------------------------------------------------- load / store
        ldr     r0, [r0]
        ldr     r0, [r1, #0]
        ldr     r0, [r1, #4]
        ldr     r7, [r7, #124]
        ldr     r0, [r1, #128]
        ldr     r0, [r1, #4092]
        ldr     r0, [r1, #-4]
        ldr     r0, [r1, #-252]
        ldr     r8, [r1]
        ldr     r0, [r8, #4]
        ldr.w   r0, [r1]
        ldr.w   r0, [r1, #4]
        ldr     r1, [sp]
        ldr     r1, [sp, #4]
        ldr     r7, [sp, #1020]
        ldr     r0, [sp, #1024]
        ldr     r8, [sp, #4]
        ldr     r0, [r1, r2]
        ldr     r7, [r6, r5]
        ldr     r0, [r1, r8]
        ldr.w   r0, [r1, r2]
        str     r0, [r1]
        str     r1, [r0, #0]
        str     r2, [r1, #124]
        str     r0, [r1, #128]
        str     r0, [r1, #-4]
        str     r0, [sp, #8]
        str     r9, [sp, #8]
        str     r0, [r1, r2]
        str     r10, [r11, r12]
        str.w   r0, [r1, #4]
        ldrb    r0, [r0]
        ldrb    r0, [r1, #31]
        ldrb    r0, [r1, #32]
        ldrb    r0, [r1, #4095]
        ldrb    r0, [r1, #-1]
        ldrb    r0, [r1, #-255]
        ldrb    r0, [r1, r2]
        ldrb.w  r0, [r1]
        strb    r0, [r1]
        strb    r2, [r1, #31]
        strb    r0, [r1, #255]
        strb    r0, [r1, #-3]
        strb    r0, [r1, r2]
        strb    r8, [r1, r2]
        ldrh    r0, [r0]
        ldrh    r0, [r1, #62]
        ldrh    r0, [r1, #64]
        ldrh    r0, [r1, #-2]
        ldrh    r0, [r1, r2]
        ldrh.w  r8, [r1, #2]
        strh    r0, [r1]
        strh    r2, [r1, #62]
        strh    r0, [r1, #4094]
        strh    r0, [r1, r2]
        ldrsb   r0, [r0]
        ldrsb   r0, [r1]
        ldrsb   r0, [r1, #1]
        ldrsb   r0, [r1, #4095]
        ldrsb   r0, [r1, #-1]
        ldrsb   r0, [r1, r2]
        ldrsb   r8, [r1, r2]
        ldrsb.w r0, [r1]
        ldrsh   r0, [r0]
        ldrsh   r0, [r1]
        ldrsh   r0, [r1, #2]
        ldrsh   r0, [r1, #-2]
        ldrsh   r0, [r1, r2]
        ldrsh.w r0, [r1, r2]
@ ---------------------------------------------------------------- push / pop
        push    {r0}
        push    {r4}
        push    {r4, lr}
        push    {r7, lr}
        push    {r0-r7}
        push    {r0, r2-r4, lr}
        push    {r8}
        push    {r4, r8}
        push.w  {r4, lr}
        push.w  {r0}
        pop     {r1}
        pop     {r4}
        pop     {r7, pc}
        pop     {r0-r7, pc}
        pop     {r8}
        pop     {r4, lr}
        pop.w   {r4, lr}
        pop.w   {r4}
        pop     {r4-r11, pc}
@ ---------------------------------------------------------------- IT blocks
        cmp     r0, r1
        ite     eq
        moveq   r0, #1
        movne   r0, #0
        it      ne
        movne   r0, #1
        itt     lt
        movlt   r0, #1
        addlt   r0, r0, r1
        itet    ge
        movge   r0, #1
        movlt   r0, #2
        movge   r1, r2
        itete   gt
        movgt   r0, #1
        movle   r0, #0
        addgt   r0, r1, r2
        suble   r0, r1, #1
        ittee   hi
        movhi   r0, #255
        movhi   r8, #1
        movls   r0, #256
        movls.w r0, #1
        it      cs
        movcs   r0, #0
        it      cc
        movcc   r0, #1
        it      mi
        movmi   r0, #1
        it      pl
        movpl   r0, #0
        it      vs
        movvs   r0, #0
        it      vc
        addvc   r0, r1
        it      hs
        movhs   r0, r0
        it      lo
        ldrlo   r0, [r1]
        it      eq
        muleq   r0, r1, r0
        it      eq
        movseq  r0, #1
        it      ne
        addsne  r0, r1, r2
        it      eq
        lslseq  r0, r1, #2
        it      eq
        lsleq   r0, r1, #2
        it      eq
        andeq   r0, r1
        it      eq
        negeq   r0, r1
        it      ne
        cmpne   r0, #1
        it      eq
        bxeq    lr
        it      ne
        bne     fwd1
fwd1:
@ ---------------------------------------------------------------- branches
back:
        b       back
        b.w     back
        b       fwd2
        bl      back
        bl      fwd2
        beq     back
        bne     fwd2
        beq.w   back
        bne.w   fwd2
        bcs     back
        bhs     back
        bcc     back
        blo     back
        bmi     back
        bpl     back
        bvs     back
        bvc     back
        bhi     back
        bls     back
        bge     back
        blt     back
        bgt     back
        ble     back
        bal     back
        b       sp
        bl      add
fwd2:
        bx      lr
        bx      r0
        blx     r1
        blx     r8
        b       far
        bl      far
        beq     far
        beq     near
        .space  250
near:
        .space  1000
        b       back
        .space  2000
        beq     back
        b       far
        .space  0x2000
        .zero   2,0xab
far:
        nop
sp:
add:
helper:
        bx      lr
@ ---------------------------------------------------------------- data in text
        .byte   1, 2, 0xff, -1
        .align  1
        .short  0x1234, -2
        .hword  65535
        .align  2
        .word   0x12345678, -1, TEN + 2, BIG - 4, -TEN
        .word   back, near + 4, back - 2
        .word   .
        .word   . + 8
        .ascii  "ab\n\t\"\\"
        .asciz  "xyz"
        .string "q"
        .byte   'a', '\n', 0b11, 0x7f
        .align  2
        nop
        .balign 8
        .space  3
        .space  2, 0x55
        .zero   1
        .balign 4
        .byte   1
        .align  3
        mov     r0, r0
@ ---------------------------------------------------------------- data
        .data
dstart:
        .word   1, dstart, dend, back
        .byte   7
        .align  2
        .short  2
        .balign 8
        .asciz  "hello"
        .space  3, 1
dend:
        .word   dend - dstart
        .byte   9
@ ---------------------------------------------------------------- more edge cases
@ text again after .data; mov/mvn, add/sub, cmp/cmn, and/bic and orr/orn swaps, IT forms, fills
        .text
        mvn     r0, #0
        adds    r0, #-1
        add     r0, r1, #-4095
        sub     r0, r1, #-4095
        cmn     r0, #-1
        cmn     r0, #-300
        tst     r0, #0x100
        orr     r0, r1, #0xFFFFFF00
        orrs    r0, r1, #0xFFFFFF00
        bic     r0, r1, #0xFFFFFFFE
        lsrs    r0, #2
        lsls    r1, r1
        a1: a2: nop
        push    {lr}
        pop     {pc}
        push    {r0, lr}
        MOVS    R0, #1
        Add     R0, R1, R2
        mov     r0, #010
        mov     r0, #0x10000
        mov     r0, #0xab00ab00
        movs    r0, #-0
        b.n     a1
        beq.n   a1
        bne     a1
        bl      a1
        str     r0, [sp]
        strh    r0, [r1, #-2]
        add     r0, r0, #0
        sub     r0, r0, #0
        subs    r0, r0, #0
        cmp     r0, #0xff00
        cmp     r9, #0
        cmp     r0, sp
        cmp     sp, r0
        add     r0, sp
        add     r0, r0, sp
        add     sp, sp, r1
        mov     lr, pc
        mul     r8, r0, r8
        it      eq
        addeq   r8, r8, r1
        it      eq
        addeq   r0, r1, #200
        it      eq
        addeq   r0, r0, #200
        it      ne
        subne   r0, r0, r1
        it      ne
        andne   r0, r0, r1
        it      ne
        lsrne   r0, r0, r1
        it      ne
        mvnne   r0, r1
        it      ne
        mulne   r0, r0, r1
        it      gt
        bgt.w   a1
        .balign 4, 0xff
        .byte   1
        .balign 4, 0xee
        .short  a1, a2 + 1
        .byte   a2 - a1 + 200
        .space  1
        .align  1
        .word   a1 + 4 - a2
        .equ    LATER, 7
        .word   LATER2
        .equ    LATER2, LATER + 1
loop:
        b       loop2
        .space  3000
loop2:
        b       loop
        .data
        .byte   1
        .align  2, 0x33
        .word   loop
        .ascii  "\101\x42\a\e"
        .data
        .ascii  "\v\b\f\r\0\7\x4142\q@"
        .byte   '\'', '\t', '@', '"'
@ ---------------------------------------------------------------- PC-relative loads
        .text
        ldr     r0, [pc]
        ldr     r0, [pc, #4]
        ldr     r7, [pc, #1020]
        ldr     r0, [pc, #1024]
        ldr     r8, [pc, #4]
        ldr     r0, [pc, #4092]
        ldr     r0, [pc, #-4]
        ldr     r0, [pc, #-4092]
        ldr.w   r0, [pc, #4]
        ldr.w   r0, [pc]
        ldrb    r0, [pc]
        ldrb    r0, [pc, #4095]
        ldrb    r0, [pc, #-4]
        ldrb    r0, [pc, #-4095]
        ldrb.w  r1, [pc, #8]
        ldrh    r0, [pc, #2]
        ldrh    r0, [pc, #-2]
        ldrh    r9, [pc, #4094]
        ldrsb   r0, [pc, #1]
        ldrsb   r0, [pc, #-1]
        ldrsb   r0, [pc]
        ldrsh   r0, [pc, #-300]
        ldrsh.w r0, [pc, #4]
