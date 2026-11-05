@ movw16_a.s - with movw16_b.s: a movw.16 to a data offset above 65535
        .syntax unified
        .thumb
        .global main
        .text
main:   movw.16 r0, #far
        bx      lr
