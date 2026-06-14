# Nib TODO

## Hardware Investigation
- Trace pins between gate array and V20 CPU — does the gate array use LOCK#?
- Check if any V20 extension instructions expose TA/TB/TC/LC internal registers

## Emulator (dreamulator)
- Implement 8080 emulation mode (BRKEM entry, CALLN exit, register mapping, 8080 CPU core)
- Implement EXT instruction (0x0F 0x33, 0x0F 0x3B) in v20.cpp
- Implement INS instruction (0x0F 0x31, 0x0F 0x39) in v20.cpp
- Implement BOUND instruction in v20.cpp (currently stubbed)

## Toolchain — remaining work
- `use` directive: read .nif files and import function signatures for cross-module type checking
- Flag-check blocks: AST nodes and IR emission (grammar parses them, not wired up)
- String literal storage: `u8[5] hello = "Hello"` needs data segment emission
- `nib build` convenience driver: follow `use` chains, timestamp-based recompilation
- Peephole optimizer: post-binder pass to eliminate self-moves, fold mov+op sequences

## Nice to have
- Binder: caller-save push/pop insertion at call sites based on preserves info
- Binder: inter-procedural return register propagation to callers
- Better error messages with source context
