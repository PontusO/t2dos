@ reloc.s - the relocation rules of as (task-4 brief, rules 1 to 8). `tools/check_asm.py --relocs`
@ prints the relocations and the nm listing of the object; tests/asm/reloc.expected holds them.
@ Symbols: lfun/ldat named locals, gfun/gdat named globals, .Ltext/.Ldat .L labels (they use
@ the section symbols .text/.data), ext/ext2/extd undefined, CONST/FWD/FWD12 absolute (no relocation).
        .syntax unified
        .thumb
        .global gfun, gdat, notdefined
        .extern ext, notused            @ notused and notdefined are never referenced: no symbol
        .equ    CONST, 100

        .text
lfun:
@ rule 1: PC-relative text references; only the undefined ones are relocated
        bl      lfun                    @ local: resolved
        bl      gfun                    @ global, defined here: resolved
        bl      .Ltext                  @ .L: resolved
        bl      ext                     @ R_BL ext+0
        bl      ext - 4                 @ R_BL ext-4
        bl      puts                    @ R_BL puts+0 (undefined without .extern)
        b       lfun                    @ resolved (narrow)
        b       ext                     @ R_BW ext+0 (unsized b to undefined is wide)
        b.w     ext2 + 8                @ R_BW ext2+8
        beq     gfun                    @ resolved
        beq     ext                     @ R_BCOND ext+0 (wide)
        bne.w   ext2                    @ R_BCOND ext2+0
        it      eq
        beq     ext                     @ in an IT block: R_BW ext+0
gfun:
        fnaddr  r0, lfun                @ resolved: S + 1 - (off + 12)
        fnaddr  r1, gfun                @ resolved
        fnaddr  r2, .Ltext              @ resolved
        fnaddr  r3, ext                 @ R_FNADDR ext+0
        fnaddr  r12, ext2 + 4           @ R_FNADDR ext2+4
.Ltext:
@ rule 2: data references are always relocated (also to text symbols); absolute ones are not
        addw    r0, r9, #ldat           @ R_DATA_ADDW ldat+0
        addw    r1, r9, #gdat + 4       @ R_DATA_ADDW gdat+4
        addw    r2, r9, #.Ldat          @ R_DATA_ADDW .data+8
        addw    r3, r9, #.Ldat + 2      @ R_DATA_ADDW .data+10
        addw    r4, r9, #extd           @ R_DATA_ADDW extd+0
        addw    r5, r9, #lfun           @ R_DATA_ADDW lfun+0 (text symbol)
        addw    r6, r9, #.Ltext - 2     @ R_DATA_ADDW .text+(.Ltext-2)
        addw    r7, r9, #CONST          @ absolute: resolved
        addw    r8, r9, #FWD12          @ absolute, defined later: resolved
        movw    r0, #:lower16:ldat      @ R_DATA_MOVW ldat+0
        movt    r0, #:upper16:ldat      @ R_DATA_MOVT ldat+0
        movw    r1, #:lower16:gdat + 6  @ R_DATA_MOVW gdat+6
        movt    r1, #:upper16:gdat + 6  @ R_DATA_MOVT gdat+6
        movw    r2, #:lower16:.Ldat + 4 @ R_DATA_MOVW .data+12
        movt    r2, #:upper16:.Ldat + 4 @ R_DATA_MOVT .data+12
        movw    r3, #:lower16:extd      @ R_DATA_MOVW extd+0
        movt    r3, #:upper16:extd - 1  @ R_DATA_MOVT extd-1
        movw    r4, #:lower16:gfun      @ R_DATA_MOVW gfun+0 (text symbol)
        movt    r4, #:upper16:.Ltext    @ R_DATA_MOVT .text+(.Ltext)
        movw    r5, #:lower16:CONST     @ resolved
        movt    r5, #:upper16:FWD       @ resolved
        movw.16 r0, #ldat               @ R_DATA_MOVW16 ldat+0
        movw.16 r1, #gdat + 2           @ R_DATA_MOVW16 gdat+2
        movw.16 r2, #.Ldat              @ R_DATA_MOVW16 .data+8
        movw.16 r3, #extd + 100         @ R_DATA_MOVW16 extd+100
        movw.16 r4, #lfun               @ R_DATA_MOVW16 lfun+0 (text symbol)
        movw.16 r5, #CONST + 1          @ resolved
        movw.16 r6, #FWD12 + 1          @ resolved
@ rule 3: .word of a text or data symbol is R_WORD; of an absolute symbol or a difference it is not
        .align  2
        .word   lfun                    @ R_WORD lfun+0
        .word   gfun + 4                @ R_WORD gfun+4
        .word   .Ltext                  @ R_WORD .text+(.Ltext)
        .word   ldat                    @ R_WORD ldat+0
        .word   gdat                    @ R_WORD gdat+0
        .word   .Ldat - 8               @ R_WORD .data+0
        .word   ext                     @ R_WORD ext+0
        .word   .                       @ R_WORD .text+(here)
        .word   CONST, FWD              @ resolved
        .word   gfun - lfun             @ resolved: a difference in one section
@ rule 7: run-before-main routines
        .t2init lfun
        .t2init .Linit
.Linit:
        bx      lr

        .data
        .word   0
ldat:   .word   1
.Ldat:  .word   2
gdat:   .word   lfun                    @ R_WORD lfun+0 (from .data)
        .word   .Ltext + 1              @ R_WORD .text+(.Ltext+1)
        .word   ext + 12                @ R_WORD ext+12
        .word   .Ldat                   @ R_WORD .data+8
        .word   gdat - ldat             @ resolved
        .equ    FWD, 0x12344567
        .equ    FWD12, 300
