# Nib TODO

## Hardware Investigation
- Trace pins between gate array and V20 CPU — does the gate array use LOCK#?
- Check if any V20 extension instructions expose TA/TB/TC/LC internal registers

## Emulator (dreamulator)
- Implement 8080 emulation mode (BRKEM entry, CALLN exit, register mapping, 8080 CPU core)
- Implement EXT instruction (0x0F 0x33, 0x0F 0x3B) in v20.cpp
- Implement INS instruction (0x0F 0x31, 0x0F 0x39) in v20.cpp
- Implement BOUND instruction in v20.cpp (currently stubbed)

## Language Design
- Determine .nif binary format
- Determine .nob binary format

## Toolchain
- V20 assembler (nib asm) — two-pass, Intel syntax, V20 extensions
- Compiler implementation (nib compile)
- Binder implementation (nib bind)
- `nib build` convenience driver
