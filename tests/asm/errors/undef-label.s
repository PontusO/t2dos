@ a branch to a local label that is never defined
        .text
here:
        b       here
        bne     .Lnowhere
        nop
