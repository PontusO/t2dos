@ movt takes :upper16:, movw takes :lower16:
        .text
        movw    r0, #:lower16:d
        movt    r0, #:lower16:d
        .data
d:      .word   0
