@ fpu.s - every FPU form as.c supports (single precision, fpv5-sp-d16); tools/check_asm.py
@ compares the bytes with GNU as. The last block holds the constant forms of the relocation
@ operators (:lower16:/:upper16:), which must also equal GNU.
        .syntax unified
        .thumb
        .text
start:
        vadd.f32 s0, s0, s0
        vadd.f32 s1, s15, s16
        vadd.f32 s31, s16, s1
        vadd.f32 s16, s31, s15
        vadd.f32 s15, s1, s31
        vadd.f32 s31, s31, s31
        vadd.f32 s2, s3, s4
        vsub.f32 s0, s0, s0
        vsub.f32 s1, s15, s16
        vsub.f32 s31, s16, s1
        vsub.f32 s16, s31, s15
        vsub.f32 s15, s1, s31
        vsub.f32 s31, s31, s31
        vsub.f32 s2, s3, s4
        vmul.f32 s0, s0, s0
        vmul.f32 s1, s15, s16
        vmul.f32 s31, s16, s1
        vmul.f32 s16, s31, s15
        vmul.f32 s15, s1, s31
        vmul.f32 s31, s31, s31
        vmul.f32 s2, s3, s4
        vdiv.f32 s0, s0, s0
        vdiv.f32 s1, s15, s16
        vdiv.f32 s31, s16, s1
        vdiv.f32 s16, s31, s15
        vdiv.f32 s15, s1, s31
        vdiv.f32 s31, s31, s31
        vdiv.f32 s2, s3, s4
        vneg.f32 s0, s0
        vneg.f32 s1, s31
        vneg.f32 s31, s1
        vneg.f32 s15, s16
        vneg.f32 s16, s15
        vneg.f32 s31, s31
        vneg.f32 s7, s8
        vsqrt.f32 s0, s0
        vsqrt.f32 s1, s31
        vsqrt.f32 s31, s1
        vsqrt.f32 s15, s16
        vsqrt.f32 s16, s15
        vsqrt.f32 s31, s31
        vsqrt.f32 s7, s8
        vcmp.f32 s0, s0
        vcmp.f32 s1, s31
        vcmp.f32 s31, s1
        vcmp.f32 s15, s16
        vcmp.f32 s16, s15
        vcmp.f32 s31, s31
        vcmp.f32 s7, s8
        vcvt.f32.s32 s0, s0
        vcvt.f32.s32 s1, s31
        vcvt.f32.s32 s31, s1
        vcvt.f32.s32 s15, s16
        vcvt.f32.s32 s16, s15
        vcvt.f32.s32 s31, s31
        vcvt.f32.s32 s7, s8
        vcvt.f32.u32 s0, s0
        vcvt.f32.u32 s1, s31
        vcvt.f32.u32 s31, s1
        vcvt.f32.u32 s15, s16
        vcvt.f32.u32 s16, s15
        vcvt.f32.u32 s31, s31
        vcvt.f32.u32 s7, s8
        vcvt.s32.f32 s0, s0
        vcvt.s32.f32 s1, s31
        vcvt.s32.f32 s31, s1
        vcvt.s32.f32 s15, s16
        vcvt.s32.f32 s16, s15
        vcvt.s32.f32 s31, s31
        vcvt.s32.f32 s7, s8
        vcvt.u32.f32 s0, s0
        vcvt.u32.f32 s1, s31
        vcvt.u32.f32 s31, s1
        vcvt.u32.f32 s15, s16
        vcvt.u32.f32 s16, s15
        vcvt.u32.f32 s31, s31
        vcvt.u32.f32 s7, s8
        vcmp.f32 s0, #0
        vcmp.f32 s1, #0
        vcmp.f32 s15, #0
        vcmp.f32 s16, #0
        vcmp.f32 s31, #0
        vcmp.f32 s5, #0.0
        vmrs APSR_nzcv, fpscr
        vmov s0, r0
        vmov r0, s0
        vmov s1, r7
        vmov r7, s1
        vmov s15, r8
        vmov r8, s15
        vmov s16, r12
        vmov r12, s16
        vmov s31, r14
        vmov r14, s31
        vmov s3, ip
        vmov ip, s3
        vmov s17, lr
        vmov lr, s17
        vmov s30, r1
        vmov r1, s30
        vldr s0, [r0]
        vldr s1, [r1, #0]
        vldr s15, [r7, #4]
        vldr s16, [r8, #1020]
        vldr s31, [r12, #-4]
        vldr s0, [sp, #-1020]
        vldr s31, [sp, #8]
        vldr s2, [lr, #1016]
        vldr s17, [r3, #-1016]
        vstr s0, [r0]
        vstr s1, [r1, #0]
        vstr s15, [r7, #4]
        vstr s16, [r8, #1020]
        vstr s31, [r12, #-4]
        vstr s0, [sp, #-1020]
        vstr s31, [sp, #8]
        vstr s2, [lr, #1016]
        vstr s17, [r3, #-1016]
        vldr s4, [pc, #8]
        vldr s5, [pc, #-8]
        vldr s6, [r2, #-0]
        vpush {s0}
        vpush {s31}
        vpush {s0-s31}
        vpush {s16-s31}
        vpush {s1-s3}
        vpush {s15, s16}
        vpush {s8-s15}
        vpush {s4, s5-s7}
        vpop {s0}
        vpop {s31}
        vpop {s0-s31}
        vpop {s16-s31}
        vpop {s1-s3}
        vpop {s15, s16}
        vpop {s8-s15}
        vpop {s4, s5-s7}
@ case does not matter
        VADD.F32 S0, S1, S2
        Vmov R0, S3
        VMRS apsr_nzcv, FPSCR
@ constant forms of the relocation operators
        .equ    K, 0x12345678
        movw    r0, #:lower16:0x12345678
        movt    r0, #:upper16:0x12345678
        movw    r1, #:lower16:K
        movt    r1, #:upper16:K
        movw    r12, #:lower16:-1
        movt    r12, #:upper16:-1
        movw    r7, #:lower16:K + 0x10000
        movt    r7, #:upper16:K + 0x10000
        movw    lr, #:lower16:LATE
        movt    lr, #:upper16:LATE
        movw    r3, #:lower16:0
        movt    r3, #:upper16:0
        .equ    LATE, 0xfedc1234
@ T2DOS forms that as resolves itself (check_asm.py rewrites fnaddr and movw.16 for GNU)
        fnaddr  r0, start
        fnaddr  r7, tail
        fnaddr  r12, .Lfwd + 2
        b       tail                    @ grows wide across the .space
        fnaddr  lr, tail
        movw.16 r1, #0
        movw.16 r2, #65535
        movw.16 r3, #K - 0x12340000
        movw.16 r4, #LATE16
.Lfwd:  nop
        .space  3000
tail:   fnaddr  r1, start
        bx      lr
        .equ    LATE16, 4660
