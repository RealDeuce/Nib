# V20 Instruction Reference for nibasm

This is a working instruction reference for the mnemonics accepted by
`nibasm`. It is organized by assembler mnemonic, not by NEC's native
manual names. NEC/V20 names are listed where they differ.

The source for instruction behavior is Section 12 of the NEC
uPD70108/uPD70116 User's Manual, cross-checked against `asm.c`. V30-only
instructions and manual mnemonics not accepted by `nibasm` are omitted.

## Naming

The NEC manual uses native V20 register and segment names. `nibasm` uses
Intel-compatible spelling.

| Manual | nibasm | Meaning |
|--------|--------|---------|
| `AW` | `AX` | accumulator word |
| `BW` | `BX` | base word |
| `CW` | `CX` | count word |
| `DW` | `DX` | data word |
| `IX` | `SI` | source index |
| `IY` | `DI` | destination index |
| `PS` | `CS` | program/code segment |
| `DS0` | `DS` | default data segment |
| `DS1` | `ES` | extra data segment |
| `CY` | `CF` | carry flag |

`nibasm` uses Intel operand order: `dst, src`.

## Operand Notation

| Form | Meaning |
|------|---------|
| `reg8` | `AL CL DL BL AH CH DH BH` |
| `reg16` | `AX CX DX BX SP BP SI DI` |
| `sreg` | `ES CS SS DS` |
| `r/m8`, `r/m16` | register or memory operand |
| `mem8`, `mem16` | memory operand |
| `imm8`, `imm16` | immediate value |
| `rel8`, `rel16` | relative branch displacement |
| `ptr16` | near indirect pointer |
| `ptr32` | far pointer, segment:offset |

Memory addressing supports the V20/8086 address forms using `BX`, `BP`,
`SI`, and `DI`, with optional displacement. `BP` defaults to `SS`;
other base/index forms default to `DS`. Segment overrides use Intel
prefix syntax through the parsed memory operand.

## Flag Notation

| Mark | Meaning |
|------|---------|
| `0` | cleared |
| `1` | set |
| `x` | set from result |
| `u` | undefined |
| `-` | unchanged |
| `r` | restored from stack/image |

## Prefixes

| nibasm | Opcode | Meaning |
|--------|--------|---------|
| `lock` | `F0` | Assert bus lock for the following instruction. |
| `rep`, `repe`, `repz` | `F3` | Repeat while `CX != 0`; string compare/scan also require `ZF=1`. |
| `repne`, `repnz` | `F2` | Repeat while `CX != 0`; string compare/scan also require `ZF=0`. |
| `repc` | `65` | V20 repeat while carry. |
| `repnc` | `64` | V20 repeat while not carry. |

## Instruction Reference

### `aaa`

NEC name: `ADJ4A`.

ASCII adjust after addition. Adjusts unpacked BCD in `AL` after adding
two ASCII/unpacked decimal digits.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `aaa` | `37` | 1 |

Operation: if low nibble of `AL` is greater than 9 or `AF=1`, add
`0x06` to `AL`, increment `AH`, and set `AF`/`CF`; otherwise clear
`AF`/`CF`. `AL` is masked to its low digit.

Flags: `SF=u ZF=u PF=u AF=x CF=x OF=u`.

### `aad`

NEC name: `CVTBD`.

ASCII adjust before division. Converts unpacked BCD digits in `AH:AL`
to binary in `AL`.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `aad` | `D5 0A` | 2 |
| `aad imm8` | `D5 ib` | 2 |

Operation: `AL := AH * base + AL`; `AH := 0`. The default base is 10.

Flags: `SF=x ZF=x PF=x AF=u CF=u OF=u`.

### `aam`

NEC name: `CVTBW`.

ASCII adjust after multiplication. Converts binary `AL` into unpacked
digits in `AH:AL`.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `aam` | `D4 0A` | 2 |
| `aam imm8` | `D4 ib` | 2 |

Operation: `AH := AL / base`; `AL := AL % base`. The default base is 10.

Flags: `SF=x ZF=x PF=x AF=u CF=u OF=u`.

### `aas`

NEC name: `ADJ4S`.

ASCII adjust after subtraction. Adjusts unpacked BCD in `AL` after
subtracting two ASCII/unpacked decimal digits.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `aas` | `3F` | 1 |

Operation: if low nibble of `AL` is greater than 9 or `AF=1`, subtract
`0x06` from `AL`, decrement `AH`, and set `AF`/`CF`; otherwise clear
`AF`/`CF`. `AL` is masked to its low digit.

Flags: `SF=u ZF=u PF=u AF=x CF=x OF=u`.

### `adc`

NEC name: `ADDC`.

Add with carry.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `adc r/m8, reg8` | `10 /r` | 2-4 |
| `adc r/m16, reg16` | `11 /r` | 2-4 |
| `adc reg8, r/m8` | `12 /r` | 2-4 |
| `adc reg16, r/m16` | `13 /r` | 2-4 |
| `adc AL, imm8` | `14 ib` | 2 |
| `adc AX, imm16` | `15 iw` | 3 |
| `adc r/m8, imm8` | `80 /2 ib` | 3-5 |
| `adc r/m16, imm16` | `81 /2 iw` | 4-6 |
| `adc r/m16, imm8` | `83 /2 ib` | 3-5 |

Operation: `dst := dst + src + CF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `add`

NEC name: `ADD`.

Add.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `add r/m8, reg8` | `00 /r` | 2-4 |
| `add r/m16, reg16` | `01 /r` | 2-4 |
| `add reg8, r/m8` | `02 /r` | 2-4 |
| `add reg16, r/m16` | `03 /r` | 2-4 |
| `add AL, imm8` | `04 ib` | 2 |
| `add AX, imm16` | `05 iw` | 3 |
| `add r/m8, imm8` | `80 /0 ib` | 3-5 |
| `add r/m16, imm16` | `81 /0 iw` | 4-6 |
| `add r/m16, imm8` | `83 /0 ib` | 3-5 |

Operation: `dst := dst + src`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `add4s`

NEC name: `ADD4S`.

Packed BCD string addition.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `add4s` | `0F 20` | 2 |

Operation: add the packed BCD string at `DS:SI` to the packed BCD
string at `ES:DI`, storing the result at `ES:DI`. `CL` is the digit
count, from 1 to 254 decimal digits. Each byte contains two packed BCD
digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.

V20 timing: `7 + 19*n` clocks, where `n` is half the digit count.
Transfers: `3*n`.

Flags: for even `CL`, `ZF` and `CF` reflect the result; `PF=u CF=x`.
For odd `CL`, `ZF` and `CF` may not be reliable and the high nibble of
the highest result byte is undefined.

Notes: destination is always in `ES` (`DS1` in the NEC manual). Source
defaults to `DS` and may use a segment override.

### `and`

Logical AND.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `and r/m8, reg8` | `20 /r` | 2-4 |
| `and r/m16, reg16` | `21 /r` | 2-4 |
| `and reg8, r/m8` | `22 /r` | 2-4 |
| `and reg16, r/m16` | `23 /r` | 2-4 |
| `and AL, imm8` | `24 ib` | 2 |
| `and AX, imm16` | `25 iw` | 3 |
| `and r/m8, imm8` | `80 /4 ib` | 3-5 |
| `and r/m16, imm16` | `81 /4 iw` | 4-6 |
| `and r/m16, imm8` | `83 /4 ib` | 3-5 |

Operation: `dst := dst & src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `bext`

NEC name: `EXT`.

Extract bit field. `nibasm` uses `bext` because `ins` conflicts with
Intel string input mnemonics.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `bext` | `0F 33 C0` | 3 |
| `bext reg8` | `0F 33 /r` | 3 |
| `bext reg8, reg8` | `0F 33 /r` | 3 |
| `bext reg8, imm4` | `0F 3B /r ib` | 4 |

Operation: load a bit field from memory into `AX`, zero-filling the
upper unused bits. The bit field begins at bit offset `reg1 & 0x0F`
within the byte addressed by `DS:SI`. The length is `(reg2 & 0x0F) + 1`
or `imm4 + 1`. After the transfer, `SI` and the first operand register
advance to the next bit field.

Implicit operands: source `DS:SI`, result `AX`.

V20 timing:

| Form | Clocks | Transfers |
|------|--------|-----------|
| register length | 26-55 even address, 34-59 odd address | 1 or 2 |
| immediate length | 21-44 even address, 25-52 odd address | 1 or 2 |

Flags: `AF=u PF=u CF=u`; other flags unchanged.

Notes: the manual requires the high nibble of the 8-bit offset and
length registers to be zero for correct operation. A length nibble of 0
means 1 bit; 15 means 16 bits.

Current assembler caveat: the register-length form is documented here
using the binder/manual two-operand shape. `asm.c` currently parses only
the first register operand when encoding the register form, so this form
needs an assembler follow-up before relying on arbitrary register pairs.

### `bins`

NEC name: `INS`.

Insert bit field. `nibasm` uses `bins` because `ins` conflicts with
Intel string input mnemonics.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `bins` | `0F 31 C0` | 3 |
| `bins reg8` | `0F 31 /r` | 3 |
| `bins reg8, reg8` | `0F 31 /r` | 3 |
| `bins reg8, imm4` | `0F 39 /r ib` | 4 |

Operation: store the low bits of `AX` into a bit field in memory. The
field begins at bit offset `reg1 & 0x0F` within the byte addressed by
`ES:DI`. The length is `(reg2 & 0x0F) + 1` or `imm4 + 1`. After the
transfer, `DI` and the first operand register advance to the next bit
field.

Implicit operands: source `AX`, destination `ES:DI`.

V20 timing:

| Form | Clocks | Transfers |
|------|--------|-----------|
| register length | 31-117 even address, 35-113 odd address | 2 or 4 |
| immediate length | 67-87 even address, 75-103 odd address | 2 or 4 |

Flags: `PF=u CF=u`; other flags unchanged.

Notes: the manual requires the high nibble of the 8-bit offset and
length registers to be zero for correct operation. A length nibble of 0
means 1 bit; 15 means 16 bits.

Current assembler caveat: the register-length form is documented here
using the binder/manual two-operand shape. `asm.c` currently parses only
the first register operand when encoding the register form, so this form
needs an assembler follow-up before relying on arbitrary register pairs.

### `bound`

NEC name: `CHKIND`.

Check array index against memory bounds.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `bound reg16, mem32` | `62 /r` | 2-4 |

Operation: compare signed `reg16` with the lower and upper 16-bit bounds
stored at `mem32`. Trap through interrupt 5 if `reg16 < lower` or
`reg16 > upper`.

Flags: unchanged.

### `brkem`

NEC name: `BRKEM`.

Enter 8080 emulation mode through an interrupt vector.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `brkem imm8` | `0F FF ib` | 3 |

Operation: push native `FLAGS`, `CS`, and `IP`; clear the native/emulate
mode bit; load `CS:IP` from vector `imm8`; then execute the target as
8080 code.

V20 timing: 38 clocks on even-address uPD70116 fetches, 50 clocks on
odd-address uPD70116 fetches. Transfers: 5.

Flags: saved on stack; execution continues in emulation mode.

Notes: return from emulation uses NEC emulation-mode instructions, not
`retf`.

### `call`

Call procedure.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `call rel16` | `E8 cw` | 3 |
| `call far seg:off` | `9A cd` | 5 |
| `call r/m16` | `FF /2` | 2-4 |
| `call far mem32` | `FF /3` | 2-4 |

Operation: push return address and transfer control. Far calls also push
`CS` and load a new `CS:IP`.

Flags: unchanged.

### `cbw`

NEC name: `CVTBW`.

Convert byte to word.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `cbw` | `98` | 1 |

Operation: sign-extend `AL` into `AX`.

Flags: unchanged.

### `clc`, `cld`, `cli`, `cmc`

Flag control.

| Form | Opcode | Operation | Flags |
|------|--------|-----------|-------|
| `clc` | `F8` | `CF := 0` | `CF=0` |
| `cld` | `FC` | `DF := 0` | `DF=0` |
| `cli` | `FA` | `IF := 0` | `IF=0` |
| `cmc` | `F5` | `CF := !CF` | `CF=x` |

Other flags are unchanged.

### `cmp`

Compare.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `cmp r/m8, reg8` | `38 /r` | 2-4 |
| `cmp r/m16, reg16` | `39 /r` | 2-4 |
| `cmp reg8, r/m8` | `3A /r` | 2-4 |
| `cmp reg16, r/m16` | `3B /r` | 2-4 |
| `cmp AL, imm8` | `3C ib` | 2 |
| `cmp AX, imm16` | `3D iw` | 3 |
| `cmp r/m8, imm8` | `80 /7 ib` | 3-5 |
| `cmp r/m16, imm16` | `81 /7 iw` | 4-6 |
| `cmp r/m16, imm8` | `83 /7 ib` | 3-5 |

Operation: compute `dst - src` for flags only; `dst` is not modified.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `cmp4s`

NEC name: `CMP4S`.

Packed BCD string compare.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `cmp4s` | `0F 26` | 2 |

Operation: compare the packed BCD string at `ES:DI` with the packed BCD
string at `DS:SI`. The result is not stored. `CL` is the digit count,
from 1 to 254 decimal digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.

V20 timing: `7 + 19*n` clocks, where `n` is half the digit count.
Transfers: 2.

Flags: `OF=u SF=u ZF=x AF=u PF=u CF=x`. For odd `CL`, `ZF` and `CF` may
not be reliable.

### `cmpsb`, `cmpsw`

Compare string element.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `cmpsb` | `A6` | 1 |
| `cmpsw` | `A7` | 1 |

Operation: compare `[DS:SI] - [ES:DI]`, then update `SI` and `DI` by
1 or 2 according to operand size and `DF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `cwd`

Convert word to doubleword.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `cwd` | `99` | 1 |

Operation: sign-extend `AX` into `DX:AX`.

Flags: unchanged.

### `daa`, `das`

Decimal adjust after addition/subtraction.

| Form | Opcode | Operation |
|------|--------|-----------|
| `daa` | `27` | adjust packed BCD result in `AL` after addition |
| `das` | `2F` | adjust packed BCD result in `AL` after subtraction |

Flags: `SF=x ZF=x AF=x PF=x CF=x OF=u`.

### `dec`

Decrement.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `dec reg16` | `48+rw` | 1 |
| `dec r/m8` | `FE /1` | 2-4 |
| `dec r/m16` | `FF /1` | 2-4 |

Operation: `dst := dst - 1`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=-`.

### `div`, `idiv`

Unsigned and signed divide.

| Form | Opcode | Dividend | Quotient | Remainder |
|------|--------|----------|----------|-----------|
| `div r/m8` | `F6 /6` | `AX` | `AL` | `AH` |
| `div r/m16` | `F7 /6` | `DX:AX` | `AX` | `DX` |
| `idiv r/m8` | `F6 /7` | `AX` | `AL` | `AH` |
| `idiv r/m16` | `F7 /7` | `DX:AX` | `AX` | `DX` |

Trap through interrupt 0 on divide error.

Flags: `OF=u SF=u ZF=u AF=u PF=u CF=u`.

### `enter`, `leave`

NEC names: `PREPARE`, `DISPOSE`.

Stack frame setup and teardown.

| Form | Opcode | Operation |
|------|--------|-----------|
| `enter imm16, imm8` | `C8 iw ib` | allocate stack frame |
| `leave` | `C9` | restore caller frame pointer |

`enter` pushes `BP`, copies `SP` to `BP`, optionally builds nested frame
links, then subtracts the frame size from `SP`. `leave` performs
`SP := BP; BP := pop()`.

Flags: unchanged.

### `hlt`

NEC name: `HALT`.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `hlt` | `F4` | 1 |

Operation: stop instruction execution until interrupt, reset, or bus
event resumes the processor.

Flags: unchanged.

### `imul`, `mul`

Signed and unsigned multiply.

| Form | Opcode | Result |
|------|--------|--------|
| `mul r/m8` | `F6 /4` | `AX := AL * r/m8` |
| `mul r/m16` | `F7 /4` | `DX:AX := AX * r/m16` |
| `imul r/m8` | `F6 /5` | `AX := AL * r/m8` |
| `imul r/m16` | `F7 /5` | `DX:AX := AX * r/m16` |
| `imul reg16, r/m16, imm16` | `69 /r iw` | `reg16 := r/m16 * imm16` |
| `imul reg16, r/m16, imm8` | `6B /r ib` | `reg16 := r/m16 * sign_extend(imm8)` |
| `imul reg16, imm16` | `69 /r iw` | `reg16 := reg16 * imm16` |
| `imul reg16, imm8` | `6B /r ib` | `reg16 := reg16 * sign_extend(imm8)` |

One-operand forms set `CF` and `OF` if the upper half of the result is
not the sign/zero extension of the lower half. Other flags are
undefined. Two- and three-operand V20/80186 forms define `CF` and `OF`
similarly for truncation to 16 bits.

### `in`, `out`

Port I/O.

| Form | Opcode | Operation |
|------|--------|-----------|
| `in AL, imm8` | `E4 ib` | `AL := port[imm8]` |
| `in AX, imm8` | `E5 ib` | `AX := port[imm8]` |
| `in AL, DX` | `EC` | `AL := port[DX]` |
| `in AX, DX` | `ED` | `AX := port[DX]` |
| `out imm8, AL` | `E6 ib` | `port[imm8] := AL` |
| `out imm8, AX` | `E7 ib` | `port[imm8] := AX` |
| `out DX, AL` | `EE` | `port[DX] := AL` |
| `out DX, AX` | `EF` | `port[DX] := AX` |

Flags: unchanged.

### `inc`

Increment.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `inc reg16` | `40+rw` | 1 |
| `inc r/m8` | `FE /0` | 2-4 |
| `inc r/m16` | `FF /0` | 2-4 |

Operation: `dst := dst + 1`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=-`.

### `insb`, `insw`, `outsb`, `outsw`

String port I/O.

| Form | Opcode | Operation |
|------|--------|-----------|
| `insb` | `6C` | `[ES:DI] := port[DX]`; update `DI` by 1 |
| `insw` | `6D` | `[ES:DI] := port[DX]`; update `DI` by 2 |
| `outsb` | `6E` | `port[DX] := [DS:SI]`; update `SI` by 1 |
| `outsw` | `6F` | `port[DX] := [DS:SI]`; update `SI` by 2 |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: unchanged.

### `int`, `into`, `iret`

Interrupt control.

| Form | Opcode | Operation |
|------|--------|-----------|
| `int 3` | `CC` | one-byte breakpoint interrupt |
| `int imm8` | `CD ib` | software interrupt |
| `into` | `CE` | interrupt 4 if `OF=1` |
| `iret` | `CF` | return from interrupt |

`int` pushes `FLAGS`, `CS`, and `IP`, clears interrupt/trap state as the
processor enters the handler, then loads `CS:IP` from the vector table.
`iret` pops `IP`, `CS`, and `FLAGS`.

Flags: `iret` restores flags; `int` saves flags and enters the handler.

### `jcc`

Conditional branch. `nibasm` automatically relaxes out-of-range
conditional branches by emitting an inverted short branch over a near
`jmp`.

| nibasm | Condition |
|--------|-----------|
| `jo` / `jno` | overflow / not overflow |
| `jb`, `jc`, `jnae` / `jnb`, `jnc`, `jae` | below/carry / not below |
| `jz`, `je` / `jnz`, `jne` | zero/equal / not zero |
| `jbe`, `jna` / `ja`, `jnbe` | below-or-equal / above |
| `js` / `jns` | sign / not sign |
| `jp`, `jpe` / `jnp`, `jpo` | parity / not parity |
| `jl`, `jnge` / `jge`, `jnl` | signed less / greater-or-equal |
| `jle`, `jng` / `jg`, `jnle` | signed less-or-equal / greater |
| `jcxz` | `CX == 0` |

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `jcc rel8` | `70+cc rb` | 2 |
| relaxed `jcc` | inverted `jcc` + `jmp rel16` | 5 |
| `jcxz rel8` | `E3 rb` | 2 |

Flags: unchanged.

### `jmp`

Jump.

| Form | Opcode | Bytes |
|------|--------|-------|
| `jmp rel8` | `EB rb` | 2 |
| `jmp rel16` | `E9 cw` | 3 |
| `jmp far seg:off` | `EA cd` | 5 |
| `jmp r/m16` | `FF /4` | 2-4 |
| `jmp far mem32` | `FF /5` | 2-4 |

Flags: unchanged.

### `lahf`, `sahf`

Load/store low flags through `AH`.

| Form | Opcode | Operation |
|------|--------|-----------|
| `lahf` | `9F` | `AH := SF:ZF:0:AF:0:PF:1:CF` |
| `sahf` | `9E` | low flags loaded from `AH` |

`sahf` affects `SF ZF AF PF CF`; `OF` is unchanged.

### `lds`, `les`, `lea`

Address loading.

| Form | Opcode | Operation |
|------|--------|-----------|
| `lea reg16, mem` | `8D /r` | load effective offset |
| `lds reg16, mem32` | `C5 /r` | load `reg16` and `DS` from far pointer |
| `les reg16, mem32` | `C4 /r` | load `reg16` and `ES` from far pointer |

Flags: unchanged.

### `lodsb`, `lodsw`

Load string element.

| Form | Opcode | Operation |
|------|--------|-----------|
| `lodsb` | `AC` | `AL := [DS:SI]`; update `SI` by 1 |
| `lodsw` | `AD` | `AX := [DS:SI]`; update `SI` by 2 |

Direction is controlled by `DF`. Flags are unchanged.

### `loop`, `loope`, `loopz`, `loopne`, `loopnz`

Counted branch.

| Form | Opcode | Condition after `CX := CX - 1` |
|------|--------|--------------------------------|
| `loop rel8` | `E2 rb` | `CX != 0` |
| `loope rel8`, `loopz rel8` | `E1 rb` | `CX != 0 && ZF=1` |
| `loopne rel8`, `loopnz rel8` | `E0 rb` | `CX != 0 && ZF=0` |

Flags: unchanged.

### `mov`

Move data.

| Form | Opcode |
|------|--------|
| `mov r/m8, reg8` | `88 /r` |
| `mov r/m16, reg16` | `89 /r` |
| `mov reg8, r/m8` | `8A /r` |
| `mov reg16, r/m16` | `8B /r` |
| `mov r/m16, sreg` | `8C /r` |
| `mov sreg, r/m16` | `8E /r` |
| `mov AL, mem8` | `A0 iw` |
| `mov AX, mem16` | `A1 iw` |
| `mov mem8, AL` | `A2 iw` |
| `mov mem16, AX` | `A3 iw` |
| `mov reg8, imm8` | `B0+rb ib` |
| `mov reg16, imm16` | `B8+rw iw` |
| `mov r/m8, imm8` | `C6 /0 ib` |
| `mov r/m16, imm16` | `C7 /0 iw` |

Operation: copy source to destination.

Flags: unchanged.

### `movsb`, `movsw`

Move string element.

| Form | Opcode | Operation |
|------|--------|-----------|
| `movsb` | `A4` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` by 1 |
| `movsw` | `A5` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` by 2 |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: unchanged.

### `neg`

Two's-complement negate.

| Form | Opcode |
|------|--------|
| `neg r/m8` | `F6 /3` |
| `neg r/m16` | `F7 /3` |

Operation: `dst := 0 - dst`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`; `CF=0` only when the original
operand was zero.

### `nop`

No operation.

| Form | Opcode | Bytes |
|------|--------|-------|
| `nop` | `90` | 1 |

Flags: unchanged.

### `not`

One's-complement invert.

| Form | Opcode |
|------|--------|
| `not r/m8` | `F6 /2` |
| `not r/m16` | `F7 /2` |

Operation: `dst := ~dst`.

Flags: unchanged.

### `not1`, `clr1`, `set1`, `test1`

V20 single-bit operations.

Forms:

| Form | Opcode |
|------|--------|
| `test1 r/m8, CL` | `0F 10 /0` |
| `test1 r/m16, CL` | `0F 11 /0` |
| `test1 r/m8, imm3` | `0F 18 /0 ib` |
| `test1 r/m16, imm4` | `0F 19 /0 ib` |
| `clr1 r/m8, CL` | `0F 12 /0` |
| `clr1 r/m16, CL` | `0F 13 /0` |
| `clr1 r/m8, imm3` | `0F 1A /0 ib` |
| `clr1 r/m16, imm4` | `0F 1B /0 ib` |
| `set1 r/m8, CL` | `0F 14 /0` |
| `set1 r/m16, CL` | `0F 15 /0` |
| `set1 r/m8, imm3` | `0F 1C /0 ib` |
| `set1 r/m16, imm4` | `0F 1D /0 ib` |
| `not1 r/m8, CL` | `0F 16 /0` |
| `not1 r/m16, CL` | `0F 17 /0` |
| `not1 r/m8, imm3` | `0F 1E /0 ib` |
| `not1 r/m16, imm4` | `0F 1F /0 ib` |

Operation:

| Mnemonic | Effect |
|----------|--------|
| `test1` | copy selected bit into `ZF` without modifying operand |
| `clr1` | clear selected bit |
| `set1` | set selected bit |
| `not1` | invert selected bit |

For byte operands, immediate bit numbers use the low 3 bits. For word
operands, immediate bit numbers use the low 4 bits. `CL` forms use the
corresponding low bits of `CL`.

Flags: `test1` sets `ZF` from the tested bit and leaves the operand
unchanged; other flags are unchanged. `clr1`, `set1`, and `not1` leave
flags unchanged.

### `or`

Logical OR.

Forms:

| Form | Opcode |
|------|--------|
| `or r/m8, reg8` | `08 /r` |
| `or r/m16, reg16` | `09 /r` |
| `or reg8, r/m8` | `0A /r` |
| `or reg16, r/m16` | `0B /r` |
| `or AL, imm8` | `0C ib` |
| `or AX, imm16` | `0D iw` |
| `or r/m8, imm8` | `80 /1 ib` |
| `or r/m16, imm16` | `81 /1 iw` |
| `or r/m16, imm8` | `83 /1 ib` |

Operation: `dst := dst | src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `pop`, `popa`, `popf`

Pop from stack.

| Form | Opcode | Operation |
|------|--------|-----------|
| `pop reg16` | `58+rw` | pop word into register |
| `pop r/m16` | `8F /0` | pop word into memory/register |
| `pop sreg` | `07+sr*8` | pop word into segment register |
| `popa` | `61` | pop `DI SI BP`, discard saved `SP`, pop `BX DX CX AX` |
| `popf` | `9D` | pop flags |

Flags: only `popf` restores flags.

### `push`, `pusha`, `pushf`

Push to stack.

| Form | Opcode | Operation |
|------|--------|-----------|
| `push reg16` | `50+rw` | push register |
| `push r/m16` | `FF /6` | push memory/register |
| `push sreg` | `06+sr*8` | push segment register |
| `push imm16` | `68 iw` | push word immediate |
| `push imm8` | `6A ib` | push sign-extended byte immediate |
| `pusha` | `60` | push `AX CX DX BX` original `SP`, `BP SI DI` |
| `pushf` | `9C` | push flags |

Flags: unchanged.

### `rcl`, `rcr`, `rol`, `ror`

Rotate.

| Form | Opcode |
|------|--------|
| `rol r/m, 1` | `D0/D1 /0` |
| `rol r/m, CL` | `D2/D3 /0` |
| `rol r/m, imm8` | `C0/C1 /0 ib` |
| `ror r/m, 1` | `D0/D1 /1` |
| `ror r/m, CL` | `D2/D3 /1` |
| `ror r/m, imm8` | `C0/C1 /1 ib` |
| `rcl r/m, 1` | `D0/D1 /2` |
| `rcl r/m, CL` | `D2/D3 /2` |
| `rcl r/m, imm8` | `C0/C1 /2 ib` |
| `rcr r/m, 1` | `D0/D1 /3` |
| `rcr r/m, CL` | `D2/D3 /3` |
| `rcr r/m, imm8` | `C0/C1 /3 ib` |

`D0/D2/C0` are byte forms; `D1/D3/C1` are word forms.

Flags: `CF=x`. For a rotate count of 1, `OF=x`; for other nonzero
counts, `OF=u`. Other flags are unchanged.

### `ret`, `retf`

Return from procedure.

| Form | Opcode | Operation |
|------|--------|-----------|
| `ret` | `C3` | pop `IP` |
| `ret imm16` | `C2 iw` | pop `IP`, then add `imm16` to `SP` |
| `retf` | `CB` | pop `IP`, then `CS` |
| `retf imm16` | `CA iw` | pop `IP`, `CS`, then add `imm16` to `SP` |

Flags: unchanged.

### `rol4`, `ror4`

V20 nibble rotate through `AL`.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `rol4` | `0F 28` | 2 |
| `ror4` | `0F 2A` | 2 |

Operation: rotate nibbles between `AL` and the byte addressed by
`DS:SI`.

`rol4` rotates left through `AL`; `ror4` rotates right through `AL`.

Implicit operands: `AL`, memory byte at `DS:SI`.

Flags: unchanged.

### `sal`, `sar`, `shl`, `shr`

Shift.

| Form | Opcode |
|------|--------|
| `shl r/m, 1`, `sal r/m, 1` | `D0/D1 /4` |
| `shl r/m, CL`, `sal r/m, CL` | `D2/D3 /4` |
| `shl r/m, imm8`, `sal r/m, imm8` | `C0/C1 /4 ib` |
| `shr r/m, 1` | `D0/D1 /5` |
| `shr r/m, CL` | `D2/D3 /5` |
| `shr r/m, imm8` | `C0/C1 /5 ib` |
| `sar r/m, 1` | `D0/D1 /7` |
| `sar r/m, CL` | `D2/D3 /7` |
| `sar r/m, imm8` | `C0/C1 /7 ib` |

`D0/D2/C0` are byte forms; `D1/D3/C1` are word forms. `sal` is an alias
for `shl`.

Flags: `SF=x ZF=x PF=x CF=x`. `AF=u`. For a count of 1, `OF=x`; for
other nonzero counts, `OF=u`.

### `salc`

Set `AL` from carry.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `salc` | `D6` | 1 |

Operation: `AL := 0xFF` if `CF=1`, otherwise `AL := 0x00`.

Flags: unchanged.

Notes: accepted by `nibasm`; this opcode is not listed in the NEC
Section 12 instruction index.

### `sbb`

NEC name: `SUBC`.

Subtract with borrow.

Forms:

| Form | Opcode |
|------|--------|
| `sbb r/m8, reg8` | `18 /r` |
| `sbb r/m16, reg16` | `19 /r` |
| `sbb reg8, r/m8` | `1A /r` |
| `sbb reg16, r/m16` | `1B /r` |
| `sbb AL, imm8` | `1C ib` |
| `sbb AX, imm16` | `1D iw` |
| `sbb r/m8, imm8` | `80 /3 ib` |
| `sbb r/m16, imm16` | `81 /3 iw` |
| `sbb r/m16, imm8` | `83 /3 ib` |

Operation: `dst := dst - src - CF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `scasb`, `scasw`

Scan string element.

| Form | Opcode | Operation |
|------|--------|-----------|
| `scasb` | `AE` | compare `AL - [ES:DI]`; update `DI` by 1 |
| `scasw` | `AF` | compare `AX - [ES:DI]`; update `DI` by 2 |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `stc`, `std`, `sti`

Flag set instructions.

| Form | Opcode | Operation |
|------|--------|-----------|
| `stc` | `F9` | `CF := 1` |
| `std` | `FD` | `DF := 1` |
| `sti` | `FB` | `IF := 1` |

Other flags are unchanged.

### `stosb`, `stosw`

Store string element.

| Form | Opcode | Operation |
|------|--------|-----------|
| `stosb` | `AA` | `[ES:DI] := AL`; update `DI` by 1 |
| `stosw` | `AB` | `[ES:DI] := AX`; update `DI` by 2 |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: unchanged.

### `sub`

Subtract.

Forms:

| Form | Opcode |
|------|--------|
| `sub r/m8, reg8` | `28 /r` |
| `sub r/m16, reg16` | `29 /r` |
| `sub reg8, r/m8` | `2A /r` |
| `sub reg16, r/m16` | `2B /r` |
| `sub AL, imm8` | `2C ib` |
| `sub AX, imm16` | `2D iw` |
| `sub r/m8, imm8` | `80 /5 ib` |
| `sub r/m16, imm16` | `81 /5 iw` |
| `sub r/m16, imm8` | `83 /5 ib` |

Operation: `dst := dst - src`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `sub4s`

NEC name: `SUB4S`.

Packed BCD string subtraction.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `sub4s` | `0F 22` | 2 |

Operation: subtract the packed BCD string at `DS:SI` from the packed BCD
string at `ES:DI`, storing the result at `ES:DI`. `CL` is the digit
count, from 1 to 254 decimal digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.

V20 timing: `7 + 19*n` clocks, where `n` is half the digit count.
Transfers: `3*n`.

Flags: for even `CL`, `ZF` and `CF` reflect the result; `PF=u CF=x`.
For odd `CL`, `ZF` and `CF` may not be reliable and the high nibble of
the highest result byte is undefined.

### `test`

Logical test.

Forms:

| Form | Opcode |
|------|--------|
| `test r/m8, reg8` | `84 /r` |
| `test r/m16, reg16` | `85 /r` |
| `test AL, imm8` | `A8 ib` |
| `test AX, imm16` | `A9 iw` |
| `test r/m8, imm8` | `F6 /0 ib` |
| `test r/m16, imm16` | `F7 /0 iw` |

Operation: compute `dst & src` for flags only; `dst` is not modified.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `xchg`

NEC name: `XCH`.

Exchange operands.

| Form | Opcode |
|------|--------|
| `xchg AX, reg16` | `90+rw` |
| `xchg reg16, AX` | `90+rw` |
| `xchg r/m8, reg8` | `86 /r` |
| `xchg r/m16, reg16` | `87 /r` |

Operation: swap source and destination.

Flags: unchanged.

### `xlat`

NEC name: `TRANS`.

Translate byte through table.

Forms:

| Form | Opcode | Bytes |
|------|--------|-------|
| `xlat` | `D7` | 1 |

Operation: `AL := [DS:BX + zero_extend(AL)]`.

Flags: unchanged.

### `xor`

Logical XOR.

Forms:

| Form | Opcode |
|------|--------|
| `xor r/m8, reg8` | `30 /r` |
| `xor r/m16, reg16` | `31 /r` |
| `xor reg8, r/m8` | `32 /r` |
| `xor reg16, r/m16` | `33 /r` |
| `xor AL, imm8` | `34 ib` |
| `xor AX, imm16` | `35 iw` |
| `xor r/m8, imm8` | `80 /6 ib` |
| `xor r/m16, imm16` | `81 /6 iw` |
| `xor r/m16, imm8` | `83 /6 ib` |

Operation: `dst := dst ^ src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

## Manual Mnemonics Not Used by nibasm

These Section 12 names are intentionally not the primary spelling in
this reference because `nibasm` uses Intel-compatible names:

| Manual | Use in nibasm |
|--------|---------------|
| `ADDC` | `adc` |
| `ADJ4A` | `aaa` |
| `ADJ4S` | `aas` |
| `ADJBA` | `daa` |
| `ADJBS` | `das` |
| `BR` | `jmp` |
| `BRK` | `int` |
| `CHKIND` | `bound` |
| `CVTBD` | `aad` |
| `CVTBW` | `aam` or `cbw`, depending on opcode |
| `CVTWL` | `cwd` |
| `DISPOSE` | `leave` |
| `EXT` | `bext` |
| `HALT` | `hlt` |
| `INS` | `bins` |
| `PREPARE` | `enter` |
| `SUBC` | `sbb` |
| `XCH` | `xchg` |

Other V20/V30 manual entries such as native/emulation bridge helpers,
block move aliases, and floating-point escape instructions are omitted
unless `asm.c` accepts the mnemonic directly.
