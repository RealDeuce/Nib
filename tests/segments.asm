; Test Intel HEX output with multiple segments and gaps

    org 0x0000

boot:
    mov ax, 0x1234
    mov bx, 0x5678
    jmp start

    org 0x0100

start:
    mov cx, 10
    nop
    hlt

    org 0xFFF0

reset:
    jmp 0x0000
