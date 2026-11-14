@ a forced 16-bit load scales its offset: a misaligned one has no narrow encoding
        .text
        ldr.n   r0, [r1, #4]
        ldr.n   r0, [r1, #6]
