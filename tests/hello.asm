; Simple V20 test program
; Writes 'H' to the first byte of a memory region

    org 0x0100

start:
    mov ax, 0xB800      ; video segment
    mov es, ax
    mov di, 0            ; first position
    mov al, 'H'          ; character
    mov ah, 0x07         ; attribute
    stosw                ; write char+attr

    ; Test ALU operations
    mov cx, 10
    xor bx, bx
loop1:
    add bx, cx
    loop loop1

    ; Test shifts
    mov ax, 0xFF00
    shr ax, 4
    shl ax, 1
    rol ax, 1
    sar ax, cl

    ; Test memory addressing
    mov al, [bx+si]
    mov [bx+di+4], ax
    mov word ptr [bp+2], 0x1234

    ; Test conditional jumps
    cmp ax, bx
    je done
    jb done
    jg done

    ; Test stack
    push ax
    push bx
    pop bx
    pop ax

    ; Test V20 bit operations
    test1 al, 3
    set1 byte ptr [bx], 5
    clr1 ah, cl

    ; Test I/O
    in al, 0x60
    out 0x90, al
    mov dx, 0x03F8
    in al, dx
    out dx, al

done:
    hlt

msg db 'Hello', 0
vals dw 0x1234, 0x5678
