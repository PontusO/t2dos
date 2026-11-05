@ badins.s - an instruction as does not know: as fails, cc stops with its exit code
        .syntax unified
        .thumb
        .global main
        .text
main:
        frobnicate r0, r1
        bx      lr
