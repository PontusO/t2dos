@ the condition inside an IT block must match
        .text
        ite     eq
        moveq   r0, #1
        movgt   r0, #0
