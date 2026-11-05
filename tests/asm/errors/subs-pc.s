@ sub to pc: only subs pc, lr, #imm (the exception return)
        .text
        subs    pc, lr, #4
        sub     pc, lr, #4
