@ a symbol past the end of its section
        .text
        .global past
start:  nop
        .equ    past, start + 100
