@ addw_a.s - with addw_b.s: an addw to the last byte of addw_b's 5000 bytes of data
        .syntax unified
        .thumb
        .global main
        .text
main:   addw    r0, r9, #big_end
        bx      lr
