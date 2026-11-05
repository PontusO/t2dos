@ fnaddr needs a text symbol
        .text
        fnaddr  r0, f
        fnaddr  r1, d
f:      bx      lr
        .data
d:      .word   0
