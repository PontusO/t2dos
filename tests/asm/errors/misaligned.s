@ vldr/vstr have only the imm8 * 4 form, so a misaligned offset cannot be encoded
        .text
        vldr    s0, [r1, #4]
        vldr    s0, [r1, #6]
