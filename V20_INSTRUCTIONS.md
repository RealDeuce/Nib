# V20 Instruction Reference

This is a hardware instruction reference for the NEC V20 instruction
set. It is organized by the Intel-compatible mnemonic used by `nibasm`
where one exists; V20 instructions without an Intel spelling are listed
under their NEC manual names. NEC/V20 names are listed where they
differ.

The source for instruction behavior is Section 12 of the NEC user's
manual. This document covers only the V20/uPD70108. Assembler support
notes are secondary to the hardware forms documented by the manual.

## Naming

The NEC manual uses native V20 register and segment names. The
Intel-compatible spelling used by `nibasm` is shown where it differs.

| Manual | Intel / nibasm | Meaning |
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

Forms in this reference use Intel operand order, `dst, src`, except
where a manual-only instruction form is quoted directly from Section 12.

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
| `src-block`, `dst-block` | implicit block operand addressed through `SI`/`DI` |
| `src-string`, `dst-string` | implicit BCD string operand addressed through `SI`/`DI` |

Memory addressing supports the V20/8086 address forms using `BX`, `BP`,
`SI`, and `DI`, with optional displacement. `BP` defaults to `SS`;
other base/index forms default to `DS`. Segment overrides use Intel
prefix syntax through the parsed memory operand.

## Timing Notation

Timing and transfer counts come from the V20/uPD70108 rows in Section
12 of the NEC manual. `none` is the manual's `Transfers: None`.

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

| Mnemonic | NEC name | Opcode | Clocks | Transfers | Meaning |
|----------|----------|--------|--------|-----------|---------|
| `lock` | `BUSLOCK` | `F0` | 2 | none | Assert bus lock for the following instruction. |
| `rep`, `repe`, `repz` | `REP`, `REPE`, `REPZ` | `F3` | 2 | none | Repeat while `CX != 0`; string compare/scan also require `ZF=1`. |
| `repne`, `repnz` | `REPNE`, `REPNZ` | `F2` | 2 | none | Repeat while `CX != 0`; string compare/scan also require `ZF=0`. |
| `repc` | `REPC` | `65` | 2 | none | V20 repeat while carry. |
| `repnc` | `REPNC` | `64` | 2 | none | V20 repeat while not carry. |

## Instruction Reference

### `aaa`

NEC name: `ADJ4A`.

ASCII adjust after addition. Adjusts unpacked BCD in `AL` after adding
two ASCII/unpacked decimal digits.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `aaa` | `37` | 1 | 3 | none |

Operation: if low nibble of `AL` is greater than 9 or `AF=1`, add
`0x06` to `AL`, increment `AH`, and set `AF`/`CF`; otherwise clear
`AF`/`CF`. `AL` is masked to its low digit.

Flags: `SF=u ZF=u PF=u AF=x CF=x OF=u`.

### `aad`

NEC name: `CVTDB`.

ASCII adjust before division. Converts unpacked BCD digits in `AH:AL`
to binary in `AL`.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `aad` | `D5 0A` | 2 | 7 | none |

Operation: `AL := AH * 10 + AL`; `AH := 0`.

Flags: `SF=x ZF=x PF=x AF=u CF=u OF=u`.

### `aam`

NEC name: `CVTBD`.

ASCII adjust after multiplication. Converts binary `AL` into unpacked
digits in `AH:AL`.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `aam` | `D4 0A` | 2 | 15 | none |

Operation: `AH := AL / 10`; `AL := AL % 10`.

Flags: `SF=x ZF=x PF=x AF=u CF=u OF=u`.

### `aas`

NEC name: `ADJ4S`.

ASCII adjust after subtraction. Adjusts unpacked BCD in `AL` after
subtracting two ASCII/unpacked decimal digits.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `aas` | `3F` | 1 | 7 | none |

Operation: if low nibble of `AL` is greater than 9 or `AF=1`, subtract
`0x06` from `AL`, decrement `AH`, and set `AF`/`CF`; otherwise clear
`AF`/`CF`. `AL` is masked to its low digit.

Flags: `SF=u ZF=u PF=u AF=x CF=x OF=u`.

### `adc`

NEC name: `ADDC`.

Add with carry.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `adc r8, reg8` | `10 /r` | 2 | 2 | none |
| `adc r16, reg16` | `11 /r` | 2 | 2 | none |
| `adc mem8, reg8` | `10 /r` | 2-4 | 16 | 2 |
| `adc mem16, reg16` | `11 /r` | 2-4 | 24 | 2 |
| `adc reg8, mem8` | `12 /r` | 2-4 | 11 | 1 |
| `adc reg16, mem16` | `13 /r` | 2-4 | 15 | 1 |
| `adc reg8, imm8` | `80 /2 ib` | 3 | 4 | none |
| `adc reg16, imm16` | `81 /2 iw` | 4 | 4 | none |
| `adc reg16, imm8` | `83 /2 ib` | 3 | 4 | none |
| `adc mem8, imm8` | `80 /2 ib` | 3-5 | 18 | 2 |
| `adc mem16, imm16` | `81 /2 iw` | 4-6 | 26 | 2 |
| `adc mem16, imm8` | `83 /2 ib` | 3-5 | 26 | 2 |
| `adc AL, imm8` | `14 ib` | 2 | 4 | none |
| `adc AX, imm16` | `15 iw` | 3 | 4 | none |

Operation: `dst := dst + src + CF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `add`

NEC name: `ADD`.

Add.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `add r8, reg8` | `00 /r` | 2 | 2 | none |
| `add r16, reg16` | `01 /r` | 2 | 2 | none |
| `add mem8, reg8` | `00 /r` | 2-4 | 16 | 2 |
| `add mem16, reg16` | `01 /r` | 2-4 | 24 | 2 |
| `add reg8, mem8` | `02 /r` | 2-4 | 11 | 1 |
| `add reg16, mem16` | `03 /r` | 2-4 | 15 | 1 |
| `add reg8, imm8` | `80 /0 ib` | 3 | 4 | none |
| `add reg16, imm16` | `81 /0 iw` | 4 | 4 | none |
| `add reg16, imm8` | `83 /0 ib` | 3 | 4 | none |
| `add mem8, imm8` | `80 /0 ib` | 3-5 | 18 | 2 |
| `add mem16, imm16` | `81 /0 iw` | 4-6 | 26 | 2 |
| `add mem16, imm8` | `83 /0 ib` | 3-5 | 26 | 2 |
| `add AL, imm8` | `04 ib` | 2 | 4 | none |
| `add AX, imm16` | `05 iw` | 3 | 4 | none |

Operation: `dst := dst + src`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `add4s`

NEC name: `ADD4S`.

Packed BCD string addition.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `add4s [ES:]dst-string, [seg:]src-string` | `0F 20` | 2 | `7 + 19*n` | `3*n` |
| `add4s` | `0F 20` | 2 | `7 + 19*n` | `3*n` |

Operation: add the packed BCD string at `DS:SI` to the packed BCD
string at `ES:DI`, storing the result at `ES:DI`. `CL` is the digit
count, from 1 to 254 decimal digits. Each byte contains two packed BCD
digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.
The destination string is always in `ES` (`DS1` in the NEC manual);
segment override is prohibited. Source defaults to `DS` and may use a
segment override.

Timing: `n` is half the digit count.

Flags: for even `CL`, `ZF` and `CF` reflect the result; `PF=u CF=x`.
For odd `CL`, `ZF` and `CF` may not be reliable and the high nibble of
the highest result byte is undefined.

### `and`

Logical AND.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `and r8, reg8` | `20 /r` | 2 | 2 | none |
| `and r16, reg16` | `21 /r` | 2 | 2 | none |
| `and mem8, reg8` | `20 /r` | 2-4 | 16 | 2 |
| `and mem16, reg16` | `21 /r` | 2-4 | 24 | 2 |
| `and reg8, mem8` | `22 /r` | 2-4 | 11 | 1 |
| `and reg16, mem16` | `23 /r` | 2-4 | 15 | 1 |
| `and reg8, imm8` | `80 /4 ib` | 3 | 4 | none |
| `and reg16, imm16` | `81 /4 iw` | 4 | 4 | none |
| `and reg16, imm8` | `83 /4 ib` | 3 | 4 | none |
| `and mem8, imm8` | `80 /4 ib` | 3-5 | 18 | 2 |
| `and mem16, imm16` | `81 /4 iw` | 4-6 | 26 | 2 |
| `and mem16, imm8` | `83 /4 ib` | 3-5 | 26 | 2 |
| `and AL, imm8` | `24 ib` | 2 | 4 | none |
| `and AX, imm16` | `25 iw` | 3 | 4 | none |

Operation: `dst := dst & src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `bext`

NEC name: `EXT`.

Extract bit field. `nibasm` spells the NEC `EXT` instruction as `bext`
to avoid collision with Intel string input mnemonics.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `bext reg8, reg8` | `0F 33 /r` | 3 | 34-59 | 1 or 2 |
| `bext reg8, imm4` | `0F 3B /r ib` | 4 | 25-52 | 1 or 2 |

Operation: load a bit field from memory into `AX`, zero-filling the
upper unused bits. The bit field begins at bit offset `reg1 & 0x0F`
within the byte addressed by `DS:SI`. The length is `(reg2 & 0x0F) + 1`
or `imm4 + 1`. After the transfer, `SI` and the first operand register
advance to the next bit field.

Implicit operands: source `DS:SI`, result `AX`.

Flags: `AF=u PF=u CF=u`; other flags unchanged.

Notes: the manual requires the high nibble of the 8-bit offset and
length registers to be zero for correct operation. A length nibble of 0
means 1 bit; 15 means 16 bits.

### `bins`

NEC name: `INS`.

Insert bit field. `nibasm` spells the NEC `INS` instruction as `bins`
to avoid collision with Intel string input mnemonics.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `bins reg8, reg8` | `0F 31 /r` | 3 | 35-113 | 2 or 4 |
| `bins reg8, imm4` | `0F 39 /r ib` | 4 | 75-103 | 2 or 4 |

Operation: store the low bits of `AX` into a bit field in memory. The
field begins at bit offset `reg1 & 0x0F` within the byte addressed by
`ES:DI`. The length is `(reg2 & 0x0F) + 1` or `imm4 + 1`. After the
transfer, `DI` and the first operand register advance to the next bit
field.

Implicit operands: source `AX`, destination `ES:DI`.

Flags: `PF=u CF=u`; other flags unchanged.

Notes: the manual requires the high nibble of the 8-bit offset and
length registers to be zero for correct operation. A length nibble of 0
means 1 bit; 15 means 16 bits.

### `bound`

NEC name: `CHKIND`.

Check array index against memory bounds.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `bound reg16, mem32`, in bounds | `62 /r` | 2-4 | 26 | 2 |
| `bound reg16, mem32`, traps | `62 /r` | 2-4 | 73-76 | 7 |

Operation: compare signed `reg16` with the lower and upper 16-bit bounds
stored at `mem32`. Trap through interrupt 5 if `reg16 < lower` or
`reg16 > upper`.

Flags: unchanged.

### `brkem`

NEC name: `BRKEM`.

Enter 8080 emulation mode through an interrupt vector.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `brkem imm8` | `0F FF ib` | 3 | 50 | 5 |

Operation: push native `FLAGS`, `CS`, and `IP`; clear the native/emulate
mode bit; load `CS:IP` from vector `imm8`; then execute the target as
8080 code.

Flags: saved on stack; execution continues in emulation mode.

Notes: return from emulation uses NEC emulation-mode instructions, not
`retf`.

### `call`

Call procedure.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `call rel16` | `E8 cw` | 3 | 20 | 1 |
| `call reg16` | `FF /2` | 2 | 18 | 1 |
| `call mem16` | `FF /2` | 2-4 | 31 | 2 |
| `call far seg:off` | `9A cd` | 5 | 29 | 2 |
| `call far mem32` | `FF /3` | 2-4 | 47 | 4 |

Operation: push return address and transfer control. Far calls also push
`CS` and load a new `CS:IP`.

Flags: unchanged.

### `calln`

NEC name: `CALLN`.

Call a native-mode interrupt routine from 8080 emulation mode.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `calln imm8` | `ED ED ib` | 3 | 58 | 5 |

Operation: save emulation-mode `PSW`, `CS`, and `IP`, set native mode,
and load `CS:IP` from interrupt vector `imm8`.

Flags: saved on stack.

### `cbw`

NEC name: `CVTBW`.

Convert byte to word.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `cbw` | `98` | 1 | 2 | none |

Operation: sign-extend `AL` into `AX`.

Flags: unchanged.

### `clc`, `cld`, `cli`, `cmc`

NEC names: `CLR1 CY`, `CLR1 DIR`, `DI`, `NOT1 CY`.

Flag control.

| Form | Opcode | Operation | Clocks | Transfers | Flags |
|------|--------|-----------|--------|-----------|-------|
| `clc` / `clr1 CF` | `F8` | `CF := 0` | 2 | none | `CF=0` |
| `cld` / `clr1 DF` | `FC` | `DF := 0` | 2 | none | `DF=0` |
| `cli` / `di` | `FA` | `IF := 0` | 2 | none | `IF=0` |
| `cmc` / `not1 CF` | `F5` | `CF := !CF` | 2 | none | `CF=x` |

Other flags are unchanged.

### `cmp`

Compare.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `cmp r8, reg8` | `38 /r` | 2 | 2 | none |
| `cmp r16, reg16` | `39 /r` | 2 | 2 | none |
| `cmp mem8, reg8` | `38 /r` | 2-4 | 11 | 1 |
| `cmp mem16, reg16` | `39 /r` | 2-4 | 15 | 1 |
| `cmp reg8, mem8` | `3A /r` | 2-4 | 11 | 1 |
| `cmp reg16, mem16` | `3B /r` | 2-4 | 15 | 1 |
| `cmp reg8, imm8` | `80 /7 ib` | 3 | 4 | none |
| `cmp reg16, imm16` | `81 /7 iw` | 4 | 4 | none |
| `cmp reg16, imm8` | `83 /7 ib` | 3 | 4 | none |
| `cmp mem8, imm8` | `80 /7 ib` | 3-5 | 13 | 1 |
| `cmp mem16, imm16` | `81 /7 iw` | 4-6 | 17 | 1 |
| `cmp mem16, imm8` | `83 /7 ib` | 3-5 | 17 | 1 |
| `cmp AL, imm8` | `3C ib` | 2 | 4 | none |
| `cmp AX, imm16` | `3D iw` | 3 | 4 | none |

Operation: compute `dst - src` for flags only; `dst` is not modified.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `cmp4s`

NEC name: `CMP4S`.

Packed BCD string compare.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `cmp4s [ES:]dst-string, [seg:]src-string` | `0F 26` | 2 | `7 + 19*n` | 2 |
| `cmp4s` | `0F 26` | 2 | `7 + 19*n` | 2 |

Operation: compare the packed BCD string at `ES:DI` with the packed BCD
string at `DS:SI`. The result is not stored. `CL` is the digit count,
from 1 to 254 decimal digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.
Source defaults to `DS` and may use a segment override. The manual also
shows an explicit destination operand in `ES:DI`.

Timing: `n` is half the digit count.

Flags: `OF=u SF=u ZF=x AF=u PF=u CF=x`. For odd `CL`, `ZF` and `CF` may
not be reliable.

### `cmpsb`, `cmpsw`

NEC names: `CMPBK`, `CMPBKB`, `CMPBKW`.

Compare string element.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `cmpbk [seg:]src-block, [ES:]dst-block` byte | `A6` | 1 | single 13; repeat `7 + 14*n` | single 2; repeat `n` |
| `cmpbk [seg:]src-block, [ES:]dst-block` word | `A7` | 1 | single 21; repeat `7 + 22*n` | single 2; repeat `n` |
| `cmpsb` | `A6` | 1 | single 13; repeat `7 + 14*n` | single 2; repeat `n` |
| `cmpsw` | `A7` | 1 | single 21; repeat `7 + 22*n` | single 2; repeat `n` |

Operation: compare `[DS:SI] - [ES:DI]`, then update `SI` and `DI` by
1 or 2 according to operand size and `DF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `cwd`

NEC name: `CVTWL`.

Convert word to doubleword.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `cwd` | `99` | 1 | 4 or 5 | none |

Operation: sign-extend `AX` into `DX:AX`.

Flags: unchanged.

### `daa`, `das`

NEC names: `ADJBA`, `ADJBS`.

Decimal adjust after addition/subtraction.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `daa` | `27` | adjust packed BCD result in `AL` after addition | 3 | none |
| `das` | `2F` | adjust packed BCD result in `AL` after subtraction | 7 | none |

Flags: `SF=x ZF=x AF=x PF=x CF=x OF=u`.

### `dec`

Decrement.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `dec reg8` | `FE /1` | 2 | 2 | none |
| `dec reg16` | `48+rw` | 1 | 2 | none |
| `dec mem8` | `FE /1` | 2-4 | 16 | 2 |
| `dec mem16` | `FF /1` | 2-4 | 24 | 2 |

Operation: `dst := dst - 1`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=-`.

### `div`, `idiv`

NEC names: `DIVU`, `DIV`.

Unsigned and signed divide.

| Form | Opcode | Dividend | Quotient | Remainder | Clocks | Transfers |
|------|--------|----------|----------|-----------|--------|-----------|
| `div reg8` | `F6 /6` | `AX` | `AL` | `AH` | 19 | none |
| `div mem8` | `F6 /6` | `AX` | `AL` | `AH` | 25 | 1 |
| `div reg16` | `F7 /6` | `DX:AX` | `AX` | `DX` | 25 | none |
| `div mem16` | `F7 /6` | `DX:AX` | `AX` | `DX` | 35 | 1 |
| `idiv reg8` | `F6 /7` | `AX` | `AL` | `AH` | 29-34 | none |
| `idiv mem8` | `F6 /7` | `AX` | `AL` | `AH` | 35-40 | 1 |
| `idiv reg16` | `F7 /7` | `DX:AX` | `AX` | `DX` | 38-43 | none |
| `idiv mem16` | `F7 /7` | `DX:AX` | `AX` | `DX` | 48-53 | 1 |

Trap through interrupt 0 on divide error.

Flags: `OF=u SF=u ZF=u AF=u PF=u CF=u`.

### `enter`, `leave`

NEC names: `PREPARE`, `DISPOSE`.

Stack frame setup and teardown.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `enter imm16, 0` | `C8 iw 00` | allocate stack frame | 16 | none |
| `enter imm16, imm8>0` | `C8 iw ib` | allocate stack frame | `23 + 16*(imm8 - 1)` | `1 + 2*(imm8 - 1)` |
| `leave` | `C9` | restore caller frame pointer | 10 | 1 |

`enter` pushes `BP`, copies `SP` to `BP`, optionally builds nested frame
links, then subtracts the frame size from `SP`. `leave` performs
`SP := BP; BP := pop()`.

Flags: unchanged.

### `fp01`, `fp02`

NEC names: `FP01`, `FP02`.

Floating-point escape instructions for an external floating-point
processor.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `fp01 fp-op` | `D8-DF /r` with `mod=11` | 2 | 2 | none |
| `fp01 fp-op, mem` | `D8-DF /r` | 2-4 | 15 | 1 |
| `fp02 fp-op` | `66/67 /r` with `mod=11` | 2 | 2 | none |
| `fp02 fp-op, mem` | `66/67 /r` | 2-4 | 15 | 1 |

Operation: delegate the encoded floating-point operation to an
externally connected floating-point arithmetic chip. Memory forms also
perform the effective-address calculation and memory read cycle needed
by the external processor.

Flags: unchanged.

### `hlt`

NEC name: `HALT`.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `hlt` | `F4` | 1 | 2 | none |

Operation: stop instruction execution until interrupt, reset, or bus
event resumes the processor.

Flags: unchanged.

### `poll`

NEC name: `POLL`.

Poll and wait.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `poll` | `9B` | 1 | `2 + 5*n` | none |

Operation: keep the CPU idle until the `POLL` pin becomes active low.

Flags: unchanged.

### `imul`, `mul`

NEC names: `MUL`, `MULU`.

Signed and unsigned multiply.

| Form | Opcode | Result | Clocks | Transfers |
|------|--------|--------|--------|-----------|
| `mul reg8` | `F6 /4` | `AX := AL * reg8` | 21 or 22 | none |
| `mul mem8` | `F6 /4` | `AX := AL * mem8` | 27 or 28 | 1 |
| `mul reg16` | `F7 /4` | `DX:AX := AX * reg16` | 29 or 30 | none |
| `mul mem16` | `F7 /4` | `DX:AX := AX * mem16` | 39 or 40 | 1 |
| `imul reg8` | `F6 /5` | `AX := AL * reg8` | 33-39 | none |
| `imul mem8` | `F6 /5` | `AX := AL * mem8` | 39-45 | 1 |
| `imul reg16` | `F7 /5` | `DX:AX := AX * reg16` | 41-47 | none |
| `imul mem16` | `F7 /5` | `DX:AX := AX * mem16` | 51-57 | 1 |
| `imul reg16, reg16, imm16` | `69 /r iw` | `reg16 := reg16 * imm16` | 36-42 | none |
| `imul reg16, mem16, imm16` | `69 /r iw` | `reg16 := mem16 * imm16` | 46-52 | 1 |
| `imul reg16, reg16, imm8` | `6B /r ib` | `reg16 := reg16 * sign_extend(imm8)` | 28-34 | none |
| `imul reg16, mem16, imm8` | `6B /r ib` | `reg16 := mem16 * sign_extend(imm8)` | 38-44 | 1 |

One-operand forms set `CF` and `OF` if the upper half of the result is
not the sign/zero extension of the lower half. Other flags are
undefined. Two- and three-operand V20/80186 forms define `CF` and `OF`
similarly for truncation to 16 bits.

### `in`, `out`

Port I/O.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `in AL, imm8` | `E4 ib` | `AL := port[imm8]` | 9 | 1 |
| `in AX, imm8` | `E5 ib` | `AX := port[imm8]` | 13 | 1 |
| `in AL, DX` | `EC` | `AL := port[DX]` | 8 | 1 |
| `in AX, DX` | `ED` | `AX := port[DX]` | 12 | 1 |
| `out imm8, AL` | `E6 ib` | `port[imm8] := AL` | 8 | 1 |
| `out imm8, AX` | `E7 ib` | `port[imm8] := AX` | 12 | 1 |
| `out DX, AL` | `EE` | `port[DX] := AL` | 8 | 1 |
| `out DX, AX` | `EF` | `port[DX] := AX` | 12 | 1 |

Flags: unchanged.

### `inc`

Increment.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `inc reg8` | `FE /0` | 2 | 2 | none |
| `inc reg16` | `40+rw` | 1 | 2 | none |
| `inc mem8` | `FE /0` | 2-4 | 16 | 2 |
| `inc mem16` | `FF /0` | 2-4 | 24 | 2 |

Operation: `dst := dst + 1`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=-`.

### `insb`, `insw`, `outsb`, `outsw`

NEC names: `INM`, `OUTM`.

String port I/O.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `inm dst-block, DX` byte | `6C` | `[ES:DI] := port[DX]`; update `DI` | single 10; repeat `9 + 8*n` | single 2; repeat `2*n` |
| `inm dst-block, DX` word | `6D` | `[ES:DI] := port[DX]`; update `DI` | single 18; repeat `9 + 16*n` | single 2; repeat `2*n` |
| `insb` | `6C` | `[ES:DI] := port[DX]`; update `DI` by 1 | single 10; repeat `9 + 8*n` | single 2; repeat `2*n` |
| `insw` | `6D` | `[ES:DI] := port[DX]`; update `DI` by 2 | single 18; repeat `9 + 16*n` | single 2; repeat `2*n` |
| `outm DX, src-block` byte | `6E` | `port[DX] := [DS:SI]`; update `SI` | single 10; repeat `9 + 8*n` | single 2; repeat `2*n` |
| `outm DX, src-block` word | `6F` | `port[DX] := [DS:SI]`; update `SI` | single 18; repeat `9 + 16*n` | single 2; repeat `2*n` |
| `outsb` | `6E` | `port[DX] := [DS:SI]`; update `SI` by 1 | single 10; repeat `9 + 8*n` | single 2; repeat `2*n` |
| `outsw` | `6F` | `port[DX] := [DS:SI]`; update `SI` by 2 | single 18; repeat `9 + 16*n` | single 2; repeat `2*n` |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: unchanged.

### `int`, `into`, `iret`

NEC names: `BRK`, `BRKV`, `RETI`.

Interrupt control.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `int 3` / `brk 3` | `CC` | one-byte breakpoint interrupt | 50 | 5 |
| `int imm8` / `brk imm8` | `CD ib` | software interrupt | 50 | 5 |
| `into` / `brkv`, taken | `CE` | interrupt 4 if `OF=1` | 52 | 5 |
| `into` / `brkv`, not taken | `CE` | continue if `OF=0` | 4 | none |
| `iret` / `reti` | `CF` | return from interrupt | 39 | 3 |

`int` pushes `FLAGS`, `CS`, and `IP`, clears interrupt/trap state as the
processor enters the handler, then loads `CS:IP` from the vector table.
`iret` pops `IP`, `CS`, and `FLAGS`.

Flags: `iret` restores flags; `int` saves flags and enters the handler.

### `jcc`

Conditional branch.

| Intel / nibasm | NEC name | Condition |
|----------------|----------|-----------|
| `jo` / `jno` | `BV` / `BNV` | overflow / not overflow |
| `jb`, `jc`, `jnae` / `jnb`, `jnc`, `jae` | `BC`, `BL` / `BNC`, `BNL` | below/carry / not below |
| `jz`, `je` / `jnz`, `jne` | `BZ`, `BE` / `BNZ`, `BNE` | zero/equal / not zero |
| `jbe`, `jna` / `ja`, `jnbe` | `BNH` / `BH` | below-or-equal / above |
| `js` / `jns` | `BN` / `BP` | sign / not sign |
| `jp`, `jpe` / `jnp`, `jpo` | `BPE` / `BPO` | parity even / parity odd |
| `jl`, `jnge` / `jge`, `jnl` | `BLT` / `BGE` | signed less / greater-or-equal |
| `jle`, `jng` / `jg`, `jnle` | `BLE` / `BGT` | signed less-or-equal / greater |
| `jcxz` | `BCWZ` | `CX == 0` |

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `jcc rel8`, taken | `70+cc rb` | 2 | 14 | none |
| `jcc rel8`, not taken | `70+cc rb` | 2 | 4 | none |
| `jcxz rel8`, taken | `E3 rb` | 2 | 13 | none |
| `jcxz rel8`, not taken | `E3 rb` | 2 | 5 | none |

Flags: unchanged.

### `jmp`

NEC name: `BR`.

Jump.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `jmp rel8` | `EB rb` | 2 | 12 | none |
| `jmp rel16` | `E9 cw` | 3 | 12 | none |
| `jmp reg16` | `FF /4` | 2 | 11 | none |
| `jmp mem16` | `FF /4` | 2-4 | 24 | 1 |
| `jmp far seg:off` | `EA cd` | 5 | 15 | none |
| `jmp far mem32` | `FF /5` | 2-4 | 35 | 2 |

Flags: unchanged.

### `lahf`, `sahf`

NEC names: `MOV AH,PSW`, `MOV PSW,AH`.

Load/store low flags through `AH`.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `lahf` | `9F` | `AH := SF:ZF:0:AF:0:PF:1:CF` | 2 | none |
| `sahf` | `9E` | low flags loaded from `AH` | 3 | none |

`sahf` affects `SF ZF AF PF CF`; `OF` is unchanged.

### `lds`, `les`, `lea`

NEC names: `MOV DS0,reg16,mem32`, `MOV DS1,reg16,mem32`, `LDEA`.

Address loading.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `lea reg16, mem` | `8D /r` | load effective offset | 4 | none |
| `lds reg16, mem32` | `C5 /r` | load `reg16` and `DS` from far pointer | 26 | 2 |
| `les reg16, mem32` | `C4 /r` | load `reg16` and `ES` from far pointer | 26 | 2 |

Flags: unchanged.

### `lodsb`, `lodsw`

NEC names: `LDM`, `LDMB`, `LDMW`.

Load string element.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `ldm [seg:]src-block` byte | `AC` | `AL := [DS:SI]`; update `SI` | single 7; repeat `7 + 9*n` | single 1; repeat `n` |
| `ldm [seg:]src-block` word | `AD` | `AX := [DS:SI]`; update `SI` | single 11; repeat `7 + 13*n` | single 1; repeat `n` |
| `lodsb` | `AC` | `AL := [DS:SI]`; update `SI` by 1 | single 7; repeat `7 + 9*n` | single 1; repeat `n` |
| `lodsw` | `AD` | `AX := [DS:SI]`; update `SI` by 2 | single 11; repeat `7 + 13*n` | single 1; repeat `n` |

Direction is controlled by `DF`. Flags are unchanged.

### `loop`, `loope`, `loopz`, `loopne`, `loopnz`

NEC names: `DBNZ`, `DBNZE`, `DBNZNE`.

Counted branch.

| Form | Opcode | Condition after `CX := CX - 1` | Clocks | Transfers |
|------|--------|--------------------------------|--------|-----------|
| `loop rel8` / `dbnz rel8`, taken | `E2 rb` | `CX != 0` | 13 | none |
| `loop rel8` / `dbnz rel8`, not taken | `E2 rb` | `CX == 0` | 5 | none |
| `loope rel8`, `loopz rel8` / `dbnze rel8`, taken | `E1 rb` | `CX != 0 && ZF=1` | 14 | none |
| `loope rel8`, `loopz rel8` / `dbnze rel8`, not taken | `E1 rb` | otherwise | 5 | none |
| `loopne rel8`, `loopnz rel8` / `dbnzne rel8`, taken | `E0 rb` | `CX != 0 && ZF=0` | 14 | none |
| `loopne rel8`, `loopnz rel8` / `dbnzne rel8`, not taken | `E0 rb` | otherwise | 5 | none |

Flags: unchanged.

### `mov`

Move data.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `mov reg8, reg8` | `88`/`8A /r` | 2 | 2 | none |
| `mov reg16, reg16` | `89`/`8B /r` | 2 | 2 | none |
| `mov mem8, reg8` | `88 /r` | 2-4 | 9 | 1 |
| `mov mem16, reg16` | `89 /r` | 2-4 | 13 | 1 |
| `mov reg8, mem8` | `8A /r` | 2-4 | 11 | 1 |
| `mov reg16, mem16` | `8B /r` | 2-4 | 15 | 1 |
| `mov reg8, imm8` | `B0+rb ib` | 2 | 4 | none |
| `mov reg16, imm16` | `B8+rw iw` | 3 | 4 | none |
| `mov mem8, imm8` | `C6 /0 ib` | 3-5 | 11 | 1 |
| `mov mem16, imm16` | `C7 /0 iw` | 4-6 | 15 | 1 |
| `mov AL, mem8` | `A0 iw` | 3 | 10 | 1 |
| `mov AX, mem16` | `A1 iw` | 3 | 14 | 1 |
| `mov mem8, AL` | `A2 iw` | 3 | 9 | 1 |
| `mov mem16, AX` | `A3 iw` | 3 | 13 | 1 |
| `mov sreg, reg16` | `8E /r` | 2 | 2 | none |
| `mov sreg, mem16` | `8E /r` | 2-4 | 15 | 1 |
| `mov reg16, sreg` | `8C /r` | 2 | 2 | none |
| `mov mem16, sreg` | `8C /r` | 2-4 | 14 | 1 |

Operation: copy source to destination.

Flags: unchanged.

### `movsb`, `movsw`

NEC names: `MOVBK`, `MOVBKB`, `MOVBKW`.

Move string element.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `movbk [ES:]dst-block, [seg:]src-block` byte | `A4` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` | single 11; repeat `11 + 8*n` | single 2; repeat `2*n` |
| `movbk [ES:]dst-block, [seg:]src-block` word | `A5` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` | single 19; repeat `11 + 16*n` | single 2; repeat `2*n` |
| `movsb` | `A4` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` by 1 | single 11; repeat `11 + 8*n` | single 2; repeat `2*n` |
| `movsw` | `A5` | `[ES:DI] := [DS:SI]`; update `SI`, `DI` by 2 | single 19; repeat `11 + 16*n` | single 2; repeat `2*n` |

Direction is controlled by `DF`. Repeat prefixes may be used.
Destination is always in `ES`; source defaults to `DS` and may use a
segment override.

Flags: unchanged.

### `neg`

Two's-complement negate.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `neg reg8` | `F6 /3` | 2 | 2 | none |
| `neg reg16` | `F7 /3` | 2 | 2 | none |
| `neg mem8` | `F6 /3` | 2-4 | 16 | 2 |
| `neg mem16` | `F7 /3` | 2-4 | 24 | 2 |

Operation: `dst := 0 - dst`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`; `CF=0` only when the original
operand was zero.

### `nop`

No operation.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `nop` | `90` | 1 | 3 | none |

Flags: unchanged.

### `not`

One's-complement invert.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `not reg8` | `F6 /2` | 2 | 2 | none |
| `not reg16` | `F7 /2` | 2 | 2 | none |
| `not mem8` | `F6 /2` | 2-4 | 16 | 2 |
| `not mem16` | `F7 /2` | 2-4 | 24 | 2 |

Operation: `dst := ~dst`.

Flags: unchanged.

### `not1`, `clr1`, `set1`, `test1`

V20 single-bit operations.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `test1 reg8, CL` | `0F 10 /0` | 3 | 3 | 1 |
| `test1 mem8, CL` | `0F 10 /0` | 3-5 | 12 | 1 |
| `test1 reg16, CL` | `0F 11 /0` | 3 | 3 | 1 |
| `test1 mem16, CL` | `0F 11 /0` | 3-5 | 16 | 1 |
| `test1 reg8, imm3` | `0F 18 /0 ib` | 4 | 4 | none |
| `test1 mem8, imm3` | `0F 18 /0 ib` | 4-6 | 13 | 1 |
| `test1 reg16, imm4` | `0F 19 /0 ib` | 4 | 4 | none |
| `test1 mem16, imm4` | `0F 19 /0 ib` | 4-6 | 17 | 1 |
| `clr1 reg8, CL` | `0F 12 /0` | 3 | 5 | none |
| `clr1 mem8, CL` | `0F 12 /0` | 3-5 | 14 | 2 |
| `clr1 reg16, CL` | `0F 13 /0` | 3 | 5 | none |
| `clr1 mem16, CL` | `0F 13 /0` | 3-5 | 22 | 2 |
| `clr1 reg8, imm3` | `0F 1A /0 ib` | 4 | 6 | none |
| `clr1 mem8, imm3` | `0F 1A /0 ib` | 4-6 | 15 | 2 |
| `clr1 reg16, imm4` | `0F 1B /0 ib` | 4 | 6 | none |
| `clr1 mem16, imm4` | `0F 1B /0 ib` | 4-6 | 23 | 2 |
| `set1 reg8, CL` | `0F 14 /0` | 3 | 4 | none |
| `set1 mem8, CL` | `0F 14 /0` | 3-5 | 13 | 2 |
| `set1 reg16, CL` | `0F 15 /0` | 3 | 4 | none |
| `set1 mem16, CL` | `0F 15 /0` | 3-5 | 21 | 2 |
| `set1 reg8, imm3` | `0F 1C /0 ib` | 4 | 5 | none |
| `set1 mem8, imm3` | `0F 1C /0 ib` | 4-6 | 14 | 2 |
| `set1 reg16, imm4` | `0F 1D /0 ib` | 4 | 5 | none |
| `set1 mem16, imm4` | `0F 1D /0 ib` | 4-6 | 22 | 2 |
| `not1 reg8, CL` | `0F 16 /0` | 3 | 4 | none |
| `not1 mem8, CL` | `0F 16 /0` | 3-5 | 18 | 2 |
| `not1 reg16, CL` | `0F 17 /0` | 3 | 4 | none |
| `not1 mem16, CL` | `0F 17 /0` | 3-5 | 26 | 2 |
| `not1 reg8, imm3` | `0F 1E /0 ib` | 4 | 5 | none |
| `not1 mem8, imm3` | `0F 1E /0 ib` | 4-6 | 19 | 2 |
| `not1 reg16, imm4` | `0F 1F /0 ib` | 4 | 5 | none |
| `not1 mem16, imm4` | `0F 1F /0 ib` | 4-6 | 27 | 2 |

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

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `or r8, reg8` | `08 /r` | 2 | 2 | none |
| `or r16, reg16` | `09 /r` | 2 | 2 | none |
| `or mem8, reg8` | `08 /r` | 2-4 | 16 | 2 |
| `or mem16, reg16` | `09 /r` | 2-4 | 24 | 2 |
| `or reg8, mem8` | `0A /r` | 2-4 | 11 | 1 |
| `or reg16, mem16` | `0B /r` | 2-4 | 15 | 1 |
| `or reg8, imm8` | `80 /1 ib` | 3 | 4 | none |
| `or reg16, imm16` | `81 /1 iw` | 4 | 4 | none |
| `or reg16, imm8` | `83 /1 ib` | 3 | 4 | none |
| `or mem8, imm8` | `80 /1 ib` | 3-5 | 18 | 2 |
| `or mem16, imm16` | `81 /1 iw` | 4-6 | 26 | 2 |
| `or mem16, imm8` | `83 /1 ib` | 3-5 | 26 | 2 |
| `or AL, imm8` | `0C ib` | 2 | 4 | none |
| `or AX, imm16` | `0D iw` | 3 | 4 | none |

Operation: `dst := dst | src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `pop`, `popa`, `popf`

NEC names: `POP`, `POPR`, `POP PSW`.

Pop from stack.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `pop reg16` | `58+rw` | pop word into register | 12 | 1 |
| `pop mem16` | `8F /0` | pop word into memory | 25 | 2 |
| `pop sreg` | `07+sr*8` | pop word into segment register | 12 | 1 |
| `popa` / `pop R` | `61` | pop `DI SI BP`, discard saved `SP`, pop `BX DX CX AX` | 75 | 7 |
| `popf` / `pop PSW` | `9D` | pop flags | 12 | 1 |

Flags: only `popf` restores flags.

### `push`, `pusha`, `pushf`

NEC names: `PUSH`, `PUSHR`, `PUSH PSW`.

Push to stack.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `push reg16` | `50+rw` | push register | 12 | 1 |
| `push mem16` | `FF /6` | push memory word | 26 | 2 |
| `push sreg` | `06+sr*8` | push segment register | 12 | 1 |
| `push imm16` | `68 iw` | push word immediate | 12 | 1 |
| `push imm8` | `6A ib` | push sign-extended byte immediate | 11 | 1 |
| `pusha` / `push R` | `60` | push `AX CX DX BX` original `SP`, `BP SI DI` | 67 | 8 |
| `pushf` / `push PSW` | `9C` | push flags | 12 | 1 |

Flags: unchanged.

### `rcl`, `rcr`, `rol`, `ror`

Rotate.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `rol`/`ror`/`rcl`/`rcr reg, 1` | `D0/D1 /0-/3` | 2 | 2 | none |
| `rol`/`ror`/`rcl`/`rcr mem8, 1` | `D0 /0-/3` | 2-4 | 16 | 2 |
| `rol`/`ror`/`rcl`/`rcr mem16, 1` | `D1 /0-/3` | 2-4 | 24 | 2 |
| `rol`/`ror`/`rcl`/`rcr reg, CL` | `D2/D3 /0-/3` | 2 | `7 + n` | none |
| `rol`/`ror`/`rcl`/`rcr mem8, CL` | `D2 /0-/3` | 2-4 | `19 + n` | 2 |
| `rol`/`ror`/`rcl`/`rcr mem16, CL` | `D3 /0-/3` | 2-4 | `27 + n` | 2 |
| `rol`/`ror`/`rcl`/`rcr reg, imm8` | `C0/C1 /0-/3 ib` | 3 | `7 + n` | none |
| `rol`/`ror`/`rcl`/`rcr mem8, imm8` | `C0 /0-/3 ib` | 3-5 | `19 + n` | 2 |
| `rol`/`ror`/`rcl`/`rcr mem16, imm8` | `C1 /0-/3 ib` | 3-5 | `27 + n` | 2 |

`D0/D2/C0` are byte forms; `D1/D3/C1` are word forms.

Flags: `CF=x`. For a rotate count of 1, `OF=x`; for other nonzero
counts, `OF=u`. Other flags are unchanged.

### `ret`, `retf`

Return from procedure.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `ret` | `C3` | pop `IP` | 19 | 1 |
| `ret imm16` | `C2 iw` | pop `IP`, then add `imm16` to `SP` | 24 | 1 |
| `retf` | `CB` | pop `IP`, then `CS` | 29 | 2 |
| `retf imm16` | `CA iw` | pop `IP`, `CS`, then add `imm16` to `SP` | 32 | 2 |

Flags: unchanged.

### `retem`

NEC name: `RETEM`.

Return from 8080 emulation mode.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `retem` | `ED FD` | 2 | 39 | 3 |

Operation: restore `IP`, `CS`, and `PSW` from the stack frame saved by
`brkem`, advance `SP` by 6, and write-disable the mode flag.

Flags: restored from stack.

### `rol4`, `ror4`

V20 nibble rotate through `AL`.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `rol4 reg8` | `0F 28 C0+reg` | 3 | 25 | none |
| `rol4 mem8` | `0F 28 /0` | 3-5 | 28 | 2 |
| `ror4 reg8` | `0F 2A C0+reg` | 3 | 29 | none |
| `ror4 mem8` | `0F 2A /0` | 3-5 | 33 | 2 |

Operation: rotate nibbles between the low nibble of `AL` and the byte
operand. The upper nibble of `AL` is undefined after the operation.

`rol4` rotates the operand left through `AL`; `ror4` rotates the operand
right through `AL`.

Flags: unchanged.

### `sal`, `sar`, `shl`, `shr`

Shift.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `shl`/`sal`/`shr`/`sar reg, 1` | `D0/D1 /4,/5,/7` | 2 | 2 | none |
| `shl`/`sal`/`shr`/`sar mem8, 1` | `D0 /4,/5,/7` | 2-4 | 16 | 2 |
| `shl`/`sal`/`shr`/`sar mem16, 1` | `D1 /4,/5,/7` | 2-4 | 24 | 2 |
| `shl`/`sal`/`shr`/`sar reg, CL` | `D2/D3 /4,/5,/7` | 2 | `7 + n` | none |
| `shl`/`sal`/`shr`/`sar mem8, CL` | `D2 /4,/5,/7` | 2-4 | `19 + n` | 2 |
| `shl`/`sal`/`shr`/`sar mem16, CL` | `D3 /4,/5,/7` | 2-4 | `27 + n` | 2 |
| `shl`/`sal`/`shr`/`sar reg, imm8` | `C0/C1 /4,/5,/7 ib` | 3 | `7 + n` | none |
| `shl`/`sal`/`shr`/`sar mem8, imm8` | `C0 /4,/5,/7 ib` | 3-5 | `19 + n` | 2 |
| `shl`/`sal`/`shr`/`sar mem16, imm8` | `C1 /4,/5,/7 ib` | 3-5 | `27 + n` | 2 |

`D0/D2/C0` are byte forms; `D1/D3/C1` are word forms. `sal` is an alias
for `shl`.

Flags: `SF=x ZF=x PF=x CF=x`. `AF=u`. For a count of 1, `OF=x`; for
other nonzero counts, `OF=u`.

### `sbb`

NEC name: `SUBC`.

Subtract with borrow.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `sbb r8, reg8` | `18 /r` | 2 | 2 | none |
| `sbb r16, reg16` | `19 /r` | 2 | 2 | none |
| `sbb mem8, reg8` | `18 /r` | 2-4 | 16 | 2 |
| `sbb mem16, reg16` | `19 /r` | 2-4 | 24 | 2 |
| `sbb reg8, mem8` | `1A /r` | 2-4 | 11 | 1 |
| `sbb reg16, mem16` | `1B /r` | 2-4 | 15 | 1 |
| `sbb reg8, imm8` | `80 /3 ib` | 3 | 4 | none |
| `sbb reg16, imm16` | `81 /3 iw` | 4 | 4 | none |
| `sbb reg16, imm8` | `83 /3 ib` | 3 | 4 | none |
| `sbb mem8, imm8` | `80 /3 ib` | 3-5 | 18 | 2 |
| `sbb mem16, imm16` | `81 /3 iw` | 4-6 | 26 | 2 |
| `sbb mem16, imm8` | `83 /3 ib` | 3-5 | 26 | 2 |
| `sbb AL, imm8` | `1C ib` | 2 | 4 | none |
| `sbb AX, imm16` | `1D iw` | 3 | 4 | none |

Operation: `dst := dst - src - CF`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `scasb`, `scasw`

NEC names: `CMPM`, `CMPMB`, `CMPMW`.

Scan string element.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `cmpm [ES:]dst-block` byte | `AE` | compare `AL - [ES:DI]`; update `DI` | single 7; repeat `7 + 10*n` | single 1; repeat `n` |
| `cmpm [ES:]dst-block` word | `AF` | compare `AX - [ES:DI]`; update `DI` | single 11; repeat `7 + 14*n` | single 1; repeat `n` |
| `scasb` | `AE` | compare `AL - [ES:DI]`; update `DI` by 1 | single 7; repeat `7 + 10*n` | single 1; repeat `n` |
| `scasw` | `AF` | compare `AX - [ES:DI]`; update `DI` by 2 | single 11; repeat `7 + 14*n` | single 1; repeat `n` |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `stc`, `std`, `sti`

NEC names: `SET1 CY`, `SET1 DIR`, `EI`.

Flag set instructions.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `stc` / `set1 CF` | `F9` | `CF := 1` | 2 | none |
| `std` / `set1 DF` | `FD` | `DF := 1` | 2 | none |
| `sti` / `ei` | `FB` | `IF := 1` | 2 | none |

Other flags are unchanged.

### `stosb`, `stosw`

NEC names: `STM`, `STMB`, `STMW`.

Store string element.

| Form | Opcode | Operation | Clocks | Transfers |
|------|--------|-----------|--------|-----------|
| `stm [ES:]dst-block` byte | `AA` | `[ES:DI] := AL`; update `DI` | single 7; repeat `7 + 4*n` | single 1; repeat `n` |
| `stm [ES:]dst-block` word | `AB` | `[ES:DI] := AX`; update `DI` | single 11; repeat `7 + 8*n` | single 1; repeat `n` |
| `stosb` | `AA` | `[ES:DI] := AL`; update `DI` by 1 | single 7; repeat `7 + 4*n` | single 1; repeat `n` |
| `stosw` | `AB` | `[ES:DI] := AX`; update `DI` by 2 | single 11; repeat `7 + 8*n` | single 1; repeat `n` |

Direction is controlled by `DF`. Repeat prefixes may be used.

Flags: unchanged.

### `sub`

Subtract.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `sub r8, reg8` | `28 /r` | 2 | 2 | none |
| `sub r16, reg16` | `29 /r` | 2 | 2 | none |
| `sub mem8, reg8` | `28 /r` | 2-4 | 16 | 2 |
| `sub mem16, reg16` | `29 /r` | 2-4 | 24 | 2 |
| `sub reg8, mem8` | `2A /r` | 2-4 | 11 | 1 |
| `sub reg16, mem16` | `2B /r` | 2-4 | 15 | 1 |
| `sub reg8, imm8` | `80 /5 ib` | 3 | 4 | none |
| `sub reg16, imm16` | `81 /5 iw` | 4 | 4 | none |
| `sub reg16, imm8` | `83 /5 ib` | 3 | 4 | none |
| `sub mem8, imm8` | `80 /5 ib` | 3-5 | 18 | 2 |
| `sub mem16, imm16` | `81 /5 iw` | 4-6 | 26 | 2 |
| `sub mem16, imm8` | `83 /5 ib` | 3-5 | 26 | 2 |
| `sub AL, imm8` | `2C ib` | 2 | 4 | none |
| `sub AX, imm16` | `2D iw` | 3 | 4 | none |

Operation: `dst := dst - src`.

Flags: `OF=x SF=x ZF=x AF=x PF=x CF=x`.

### `sub4s`

NEC name: `SUB4S`.

Packed BCD string subtraction.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `sub4s [ES:]dst-string, [seg:]src-string` | `0F 22` | 2 | `7 + 19*n` | `3*n` |
| `sub4s` | `0F 22` | 2 | `7 + 19*n` | `3*n` |

Operation: subtract the packed BCD string at `DS:SI` from the packed BCD
string at `ES:DI`, storing the result at `ES:DI`. `CL` is the digit
count, from 1 to 254 decimal digits.

Implicit operands: source `DS:SI`, destination `ES:DI`, length `CL`.
The destination string is always in `ES` (`DS1` in the NEC manual);
segment override is prohibited. Source defaults to `DS` and may use a
segment override.

Timing: `n` is half the digit count.

Flags: for even `CL`, `ZF` and `CF` reflect the result; `PF=u CF=x`.
For odd `CL`, `ZF` and `CF` may not be reliable and the high nibble of
the highest result byte is undefined.

### `test`

Logical test.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `test r8, reg8` | `84 /r` | 2 | 2 | none |
| `test r16, reg16` | `85 /r` | 2 | 2 | none |
| `test mem8, reg8` | `84 /r` | 2-4 | 11 | 1 |
| `test mem16, reg16` | `85 /r` | 2-4 | 15 | 1 |
| `test reg8, imm8` | `F6 /0 ib` | 3 | 4 | none |
| `test reg16, imm16` | `F7 /0 iw` | 4 | 4 | none |
| `test mem8, imm8` | `F6 /0 ib` | 3-5 | 11 | 1 |
| `test mem16, imm16` | `F7 /0 iw` | 4-6 | 15 | 1 |
| `test AL, imm8` | `A8 ib` | 2 | 4 | none |
| `test AX, imm16` | `A9 iw` | 3 | 4 | none |

Operation: compute `dst & src` for flags only; `dst` is not modified.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

### `xchg`

NEC name: `XCH`.

Exchange operands.

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `xchg AX, reg16` | `90+rw` | 1 | 3 | none |
| `xchg reg16, AX` | `90+rw` | 1 | 3 | none |
| `xchg reg8, reg8` | `86 /r` | 2 | 3 | none |
| `xchg reg16, reg16` | `87 /r` | 2 | 3 | none |
| `xchg mem8, reg8` | `86 /r` | 2-4 | 16 | 2 |
| `xchg mem16, reg16` | `87 /r` | 2-4 | 24 | 2 |

Operation: swap source and destination.

Flags: unchanged.

### `xlat`

NEC name: `TRANS`.

Translate byte through table.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `trans src-table` | `D7` | 1 | 9 | 1 |
| `xlat` | `D7` | 1 | 9 | 1 |

Operation: `AL := [DS:BX + zero_extend(AL)]`.

Flags: unchanged.

### `xor`

Logical XOR.

Forms:

| Form | Opcode | Bytes | Clocks | Transfers |
|------|--------|-------|--------|-----------|
| `xor r8, reg8` | `30 /r` | 2 | 2 | none |
| `xor r16, reg16` | `31 /r` | 2 | 2 | none |
| `xor mem8, reg8` | `30 /r` | 2-4 | 16 | 2 |
| `xor mem16, reg16` | `31 /r` | 2-4 | 24 | 2 |
| `xor reg8, mem8` | `32 /r` | 2-4 | 11 | 1 |
| `xor reg16, mem16` | `33 /r` | 2-4 | 15 | 1 |
| `xor reg8, imm8` | `80 /6 ib` | 3 | 4 | none |
| `xor reg16, imm16` | `81 /6 iw` | 4 | 4 | none |
| `xor reg16, imm8` | `83 /6 ib` | 3 | 4 | none |
| `xor mem8, imm8` | `80 /6 ib` | 3-5 | 18 | 2 |
| `xor mem16, imm16` | `81 /6 iw` | 4-6 | 26 | 2 |
| `xor mem16, imm8` | `83 /6 ib` | 3-5 | 26 | 2 |
| `xor AL, imm8` | `34 ib` | 2 | 4 | none |
| `xor AX, imm16` | `35 iw` | 3 | 4 | none |

Operation: `dst := dst ^ src`.

Flags: `OF=0 SF=x ZF=x AF=u PF=x CF=0`.

## NEC Mnemonic Cross-Reference

These Section 12 names map to the Intel-compatible spelling used in the
instruction entries above where a conventional Intel spelling exists.
Manual-only V20 names are listed as themselves.

| NEC manual | Reference spelling |
|------------|--------------------|
| `ADDC` | `adc` |
| `ADJ4A` | `aaa` |
| `ADJ4S` | `aas` |
| `ADJBA` | `daa` |
| `ADJBS` | `das` |
| `BR` | `jmp` |
| `BRK` | `int` |
| `BRKEM` | `brkem` |
| `BRKV` | `into` |
| `BC`, `BL` | `jb`, `jc` |
| `BNC`, `BNL` | `jnb`, `jnc`, `jae` |
| `BE`, `BZ` | `je`, `jz` |
| `BNE`, `BNZ` | `jne`, `jnz` |
| `BH` | `ja`, `jnbe` |
| `BNH` | `jbe`, `jna` |
| `BN` | `js` |
| `BP` | `jns` |
| `BPE` | `jpe`, `jp` |
| `BPO` | `jpo`, `jnp` |
| `BV` | `jo` |
| `BNV` | `jno` |
| `BLT` | `jl`, `jnge` |
| `BGE` | `jge`, `jnl` |
| `BLE` | `jle`, `jng` |
| `BGT` | `jg`, `jnle` |
| `BCWZ` | `jcxz` |
| `BUSLOCK` | `lock` |
| `CALLN` | `calln` |
| `CHKIND` | `bound` |
| `CLR1 CY` | `clc` |
| `CLR1 DIR` | `cld` |
| `CMPBK`, `CMPBKB`, `CMPBKW` | `cmpsb`, `cmpsw` |
| `CMPM`, `CMPMB`, `CMPMW` | `scasb`, `scasw` |
| `CVTBD` | `aam` |
| `CVTDB` | `aad` |
| `CVTBW` | `cbw` |
| `CVTWL` | `cwd` |
| `DI` | `cli` |
| `DBNZ` | `loop` |
| `DBNZE` | `loope`, `loopz` |
| `DBNZNE` | `loopne`, `loopnz` |
| `DIV` | `idiv` |
| `DIVU` | `div` |
| `DISPOSE` | `leave` |
| `DS0:` | `DS:` |
| `DS1:` | `ES:` |
| `EI` | `sti` |
| `EXT` | `bext` |
| `FP01` | `fp01` |
| `FP02` | `fp02` |
| `HALT` | `hlt` |
| `INM` | `insb`, `insw` |
| `INS` | `bins` |
| `LDEA` | `lea` |
| `LDM`, `LDMB`, `LDMW` | `lodsb`, `lodsw` |
| `MOV AH,PSW` | `lahf` |
| `MOV PSW,AH` | `sahf` |
| `MOV DS0,reg16,mem32` | `lds` |
| `MOV DS1,reg16,mem32` | `les` |
| `MOVBK`, `MOVBKB`, `MOVBKW` | `movsb`, `movsw` |
| `MUL` | `imul` |
| `MULU` | `mul` |
| `NOT1 CY` | `cmc` |
| `OUTM` | `outsb`, `outsw` |
| `POLL` | `poll` |
| `PREPARE` | `enter` |
| `PS:` | `CS:` |
| `POP PSW` | `popf` |
| `POPR` | `popa` |
| `PUSH PSW` | `pushf` |
| `PUSHR` | `pusha` |
| `RETEM` | `retem` |
| `RET1`, `RETI` | `iret` |
| `ROLC` | `rcl` |
| `RORC` | `rcr` |
| `SET1 CY` | `stc` |
| `SET1 DIR` | `std` |
| `SHRA` | `sar` |
| `SS:` | `SS:` |
| `STM`, `STMB`, `STMW` | `stosb`, `stosw` |
| `SUBC` | `sbb` |
| `TRANS` | `xlat` |
| `XCH` | `xchg` |
