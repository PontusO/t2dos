@ addw_b.s - 5000 bytes of data; the last byte is at 4999
        .syntax unified
        .thumb
        .global big_end
        .data
        .space  4999
big_end: .byte  0
