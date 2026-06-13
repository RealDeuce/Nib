; Comprehensive V20 instruction test
; Every instruction form, for assembler/disassembler roundtrip validation

    org 0x0000

; ============================================================
; ALU: ADD OR ADC SBB AND SUB XOR CMP
; Each with: reg,reg  reg,mem  mem,reg  acc,imm  r/m,imm  r/m,imm8-sign-ext
; ============================================================

; --- ADD ---
    add al, bl              ; 00/02 byte reg,reg
    add ax, bx              ; 01/03 word reg,reg
    add al, [bx]            ; 02 byte reg,mem
    add ax, [bx]            ; 03 word reg,mem
    add [bx], al            ; 00 byte mem,reg
    add [bx], ax            ; 01 word mem,reg
    add al, 0x42            ; 04 acc,imm8
    add ax, 0x1234          ; 05 acc,imm16
    add bl, 0x42            ; 80 /0 r/m8,imm8
    add bx, 0x1234          ; 81 /0 r/m16,imm16
    add bx, 5               ; 83 /0 r/m16,imm8 sign-ext

; --- OR ---
    or al, bl
    or ax, bx
    or al, [si]
    or ax, [di]
    or [bp+2], cl
    or [bp+2], cx
    or al, 0x0F
    or ax, 0xFF00
    or dl, 0x80
    or dx, 0x4000
    or dx, 3

; --- ADC ---
    adc al, bl
    adc ax, bx
    adc al, [bx+si]
    adc ax, [bx+di]
    adc [si], al
    adc [di], ax
    adc al, 0x01
    adc ax, 0x0001
    adc cl, 0x10
    adc cx, 0x8000
    adc cx, 7

; --- SBB ---
    sbb al, bl
    sbb ax, bx
    sbb al, [bp+si]
    sbb [bp+di], al
    sbb al, 0x01
    sbb ax, 0x0100
    sbb bl, 0x05
    sbb bx, 0x0500
    sbb bx, 2

; --- AND ---
    and al, bl
    and ax, bx
    and al, [bx+di+4]
    and [bp+si+8], ax
    and al, 0x0F
    and ax, 0x00FF
    and ch, 0x7F
    and si, 0x000F
    and si, 4

; --- SUB ---
    sub al, bl
    sub ax, bx
    sub al, [di]
    sub [si], ax
    sub al, 1
    sub ax, 100
    sub dh, 10
    sub bp, 1000
    sub bp, 3

; --- XOR ---
    xor al, bl
    xor ax, bx
    xor al, [bx]
    xor [bx+4], ax
    xor al, 0xFF
    xor ax, 0xFFFF
    xor dl, 0xAA
    xor di, 0x5555
    xor di, 1

; --- CMP ---
    cmp al, bl
    cmp ax, bx
    cmp al, [bp+6]
    cmp [bx+si], ax
    cmp al, 0x00
    cmp ax, 0x0000
    cmp cl, 0x20
    cmp sp, 0x1000
    cmp sp, 0

; ============================================================
; INC / DEC — short form (word reg) and r/m form
; ============================================================

    inc ax
    inc cx
    inc dx
    inc bx
    inc sp
    inc bp
    inc si
    inc di

    dec ax
    dec cx
    dec dx
    dec bx
    dec sp
    dec bp
    dec si
    dec di

    inc byte ptr [bx]
    inc word ptr [bx]
    dec byte ptr [si]
    dec word ptr [di]

; ============================================================
; NOT / NEG / MUL / IMUL / DIV / IDIV
; ============================================================

    not al
    not ax
    not byte ptr [bx]
    not word ptr [si]

    neg al
    neg ax
    neg byte ptr [di]
    neg word ptr [bp+4]

    mul bl
    mul bx
    mul byte ptr [si]
    mul word ptr [di]

    imul cl
    imul cx
    imul byte ptr [bx]
    imul word ptr [bp+2]

    div dl
    div dx
    div byte ptr [bx+si]
    div word ptr [bx+di]

    idiv ah
    idiv sp
    idiv byte ptr [bp+si]
    idiv word ptr [bp+di]

; ============================================================
; Shifts and rotates — by 1, by CL, by immediate
; ============================================================

    rol al, 1
    rol ax, 1
    rol al, cl
    rol ax, cl
    rol al, 4
    rol ax, 4

    ror bl, 1
    ror bx, 1
    ror bl, cl
    ror bx, cl
    ror bl, 3
    ror bx, 3

    rcl cl, 1
    rcl cx, 1
    rcl cl, cl
    rcl cx, cl
    rcl dl, 2
    rcl dx, 2

    rcr dh, 1
    rcr si, 1
    rcr dh, cl
    rcr si, cl
    rcr ah, 5
    rcr di, 5

    shl al, 1
    shl ax, 1
    shl al, cl
    shl ax, cl
    shl bl, 4
    shl bx, 4

    shr al, 1
    shr ax, 1
    shr al, cl
    shr ax, cl
    shr cl, 2
    shr cx, 2

    sar dl, 1
    sar dx, 1
    sar dl, cl
    sar dx, cl
    sar dh, 3
    sar bp, 3

; ============================================================
; MOV — all forms
; ============================================================

    ; reg, reg
    mov al, bl
    mov ax, bx
    mov cl, dh
    mov si, di

    ; reg, imm
    mov al, 0x41
    mov ah, 0x07
    mov bl, 0xFF
    mov ax, 0x1234
    mov cx, 0x0000
    mov dx, 0xFFFF
    mov sp, 0x0100
    mov bp, 0x2000
    mov si, 0x3000
    mov di, 0x4000

    ; reg, mem
    mov al, [bx]
    mov ax, [bx]
    mov cl, [bx+si]
    mov cx, [bx+di]
    mov dl, [bp+4]
    mov dx, [bp+si+8]

    ; mem, reg
    mov [bx], al
    mov [bx], ax
    mov [si], cl
    mov [di], cx
    mov [bp+2], dl
    mov [bx+di+6], dx

    ; mem, imm
    mov byte ptr [bx], 0x41
    mov word ptr [si], 0x1234

    ; sreg, r/m
    mov es, ax
    mov ds, bx
    mov ss, cx
    mov es, [bx]

    ; r/m, sreg
    mov ax, es
    mov bx, ds
    mov cx, ss
    mov [bx], es

; ============================================================
; XCHG
; ============================================================

    xchg ax, bx
    xchg ax, cx
    xchg ax, dx
    xchg ax, sp
    xchg ax, bp
    xchg ax, si
    xchg ax, di
    xchg al, bl
    xchg cx, dx

; ============================================================
; LEA / LDS / LES
; ============================================================

    lea ax, [bx+si]
    lea dx, [bp+4]
    lea si, [bx+di+10]
    lds bx, [si]
    les di, [bx+4]

; ============================================================
; TEST
; ============================================================

    test al, 0x01
    test ax, 0x8000
    test al, bl
    test ax, bx
    test byte ptr [bx], 0xFF
    test word ptr [si], 0x00FF

; ============================================================
; PUSH / POP — all forms
; ============================================================

    push ax
    push cx
    push dx
    push bx
    push sp
    push bp
    push si
    push di

    pop ax
    pop cx
    pop dx
    pop bx
    pop sp
    pop bp
    pop si
    pop di

    push es
    push cs
    push ss
    push ds

    pop es
    pop ss
    pop ds

    push 0x1234            ; 80186+ push immediate
    push 5                 ; 80186+ push sign-ext imm8

; ============================================================
; Stack frame
; ============================================================

    pusha
    popa
    pushf
    popf
    enter 0x0010, 0
    enter 0x0020, 1
    leave

; ============================================================
; Control flow — JMP, CALL, RET
; ============================================================

jmp_test:
    jmp near_target         ; near jump
    call near_target        ; near call
    ret
    ret 4
    retf
    retf 8

near_target:
    nop

; ============================================================
; Conditional jumps — all conditions
; ============================================================

cond_base:
    jo cond_target
    jno cond_target
    jb cond_target
    jnb cond_target
    jz cond_target
    jnz cond_target
    jbe cond_target
    ja cond_target
    js cond_target
    jns cond_target
    jp cond_target
    jnp cond_target
    jl cond_target
    jge cond_target
    jle cond_target
    jg cond_target

cond_target:
    nop

; ============================================================
; LOOP variants
; ============================================================

loop_top:
    loop loop_top
    loope loop_top
    loopne loop_top
    jcxz loop_top

; ============================================================
; INT
; ============================================================

    int 3                   ; breakpoint (0xCC)
    int 0x21
    int 0x10
    into
    iret

; ============================================================
; String operations — with and without REP prefix
; ============================================================

    movsb
    movsw
    cmpsb
    cmpsw
    stosb
    stosw
    lodsb
    lodsw
    scasb
    scasw

    rep movsb
    rep movsw
    rep stosb
    rep stosw
    rep lodsb
    rep lodsw
    repe cmpsb
    repe cmpsw
    repe scasb
    repe scasw
    repne cmpsb
    repne cmpsw
    repne scasb
    repne scasw

; ============================================================
; I/O
; ============================================================

    in al, 0x60
    in ax, 0x60
    in al, dx
    in ax, dx
    out 0x90, al
    out 0x90, ax
    out dx, al
    out dx, ax

    insb
    insw
    outsb
    outsw

; ============================================================
; Flag manipulation
; ============================================================

    clc
    stc
    cmc
    cld
    std
    cli
    sti
    lahf
    sahf

; ============================================================
; BCD / ASCII adjust
; ============================================================

    daa
    das
    aaa
    aas
    aam
    aam 16
    aad
    aad 16

; ============================================================
; Misc
; ============================================================

    cbw
    cwd
    xlat
    hlt
    nop
    salc

; ============================================================
; BOUND (80186+)
; ============================================================

    bound ax, [bx]
    bound si, [bp+4]

; ============================================================
; V20 extensions — bit operations
; ============================================================

    ; TEST1: r/m, CL and r/m, imm
    test1 al, cl
    test1 ax, cl
    test1 byte ptr [bx], cl
    test1 word ptr [si], cl
    test1 al, 0
    test1 ax, 0
    test1 al, 7
    test1 byte ptr [bx], 3
    test1 word ptr [di], 15

    ; CLR1
    clr1 al, cl
    clr1 ax, cl
    clr1 byte ptr [bx], cl
    clr1 al, 0
    clr1 ax, 0
    clr1 byte ptr [bx+si], 5

    ; SET1
    set1 al, cl
    set1 ax, cl
    set1 byte ptr [di], cl
    set1 al, 7
    set1 ax, 15
    set1 byte ptr [bx], 0

    ; NOT1
    not1 al, cl
    not1 ax, cl
    not1 byte ptr [si], cl
    not1 al, 3
    not1 ax, 8
    not1 byte ptr [bp+2], 6

; ============================================================
; V20 extensions — BCD string operations
; ============================================================

    add4s
    sub4s
    cmp4s

; ============================================================
; V20 extensions — nibble rotate
; ============================================================

    rol4
    ror4

; ============================================================
; V20 extensions — BRKEM
; ============================================================

    brkem 0x00
    brkem 0xFF

; ============================================================
; LOCK prefix
; ============================================================

    lock xchg ax, bx
    lock add [bx], ax

; ============================================================
; Segment override prefixes
; ============================================================

    mov al, [es:bx]
    mov al, [cs:si]
    mov al, [ss:bp+4]
    mov al, [ds:di]
    mov ax, [es:bx+si+2]

; ============================================================
; Data directives
; ============================================================

data_byte db 0x00, 0xFF, 0x41, 'A'
data_word dw 0x0000, 0xFFFF, 0x1234
data_str  db 'Hello, World!', 0
label_equ equ 0x1234
