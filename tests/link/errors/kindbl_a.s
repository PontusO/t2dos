@ kindbl_a.s - with kindbl_b.s: bl to a global that is a data symbol
        .syntax unified
        .thumb
        .global main
        .text
main:   bl      table
        bx      lr
