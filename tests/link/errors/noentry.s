@ noentry.s - no main: the default entry is undefined
        .syntax unified
        .thumb
        .global start
        .text
start:  bx      lr
