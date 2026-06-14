# Nib TODO

## Hardware Investigation
- Trace pins between gate array and V20 CPU — does the gate array use LOCK#?
- Check if any V20 extension instructions expose TA/TB/TC/LC internal registers
- Determine if power button NMI is usable as debug break trigger

## Emulator (dreamulator)
- Implement 8080 emulation mode (BRKEM entry, CALLN exit, register mapping, 8080 CPU core)
- Implement EXT instruction (0x0F 0x33, 0x0F 0x3B) in v20.cpp
- Implement INS instruction (0x0F 0x31, 0x0F 0x39) in v20.cpp
- Implement BOUND instruction in v20.cpp (currently stubbed)

## Toolchain
- Peephole optimizer: post-binder pass to eliminate self-moves, fold mov+op sequences
- Better error messages with source context
- Binder: emit at() functions last to avoid org/position issues with subsequent code

## Nice to have
- Struct array type syntax (`struct Name[N]` in generic contexts)
- Nested struct initializers
