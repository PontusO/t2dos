@ a .byte value that does not fit in a byte
        .text
start:
        .space  300
        .byte   255, -256
        .byte   end - start
end:
