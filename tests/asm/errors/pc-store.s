@ stores cannot be PC-relative (GNU as rejects them too)
        .text
        ldr     r0, [pc, #4]
        str     r0, [pc, #4]
