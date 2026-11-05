@ kindaddw_a.s - with kindaddw_b.s: an addw data reference to a global that is a text symbol
        .syntax unified
        .thumb
        .global main
        .text
main:   addw    r0, r9, #helper
        bx      lr
