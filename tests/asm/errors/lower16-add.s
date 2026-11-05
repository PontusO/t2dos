@ the relocation operators only go with movw and movt
        .text
        adds    r0, #:lower16:d
        .data
d:      .word   0
