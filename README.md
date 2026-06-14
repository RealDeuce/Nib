# Nib

A compiled language for the NEC V20HL processor in the
[DreamWriter](https://en.wikipedia.org/wiki/NTS_DreamWriter) portable
word processor.

Nib sits between C and assembly — you get C-like block syntax, automatic
register allocation, and control flow, but the compilation is transparent
and predictable. You can reason about what assembly gets emitted.

## Status

**Early development.** The language spec is drafted, the compiler emits
typed IR with virtual registers, and the assembler/disassembler roundtrip
correctly for all V20 instructions. The binder (whole-program register
allocator) is not yet implemented.

### What works

- Language specification (`SPEC.md`)
- Compiler (`nib`) — parses `.nib` source, type-checks, emits `.nir` + `.nif`
  - Zero bison conflicts
  - Strong type checking (no implicit integer promotion, literals auto-promote)
  - Virtual register allocation with register pinning preferences
  - Scope tracking with shadowing
  - `const` declarations, array initializers, `at()` placement, `&fn` addresses
  - Identifiers allow uppercase (`[a-zA-Z_][a-zA-Z0-9_]*`); struct type prefix required
- Two-pass V20 cross-assembler (`nibasm`)
  - Full 8086/80186 instruction set
  - V20 extensions (bit ops, BCD string ops, nibble rotate, 8080 emulation)
  - `SEG` operator for segment-relative label references
  - Map file output for symbol/data tracking
- V20 disassembler (`nibdis`) with map file support

### What's next

- Binder with whole-program register allocation (`nib bind`)
- Peephole optimizer (post-binder, on real assembly)
- First program running in the emulator

## Toolchain pipeline

```
source.nib + deps.nif  --[nib compile]-->  .nir + .nif
.nir files             --[nib bind]-->     .asm          (not yet implemented)
.asm                   --[nib asm]-->      binary
```

- **`.nir`** — Nib IR: pseudo-assembly with virtual registers (`%0`, `%1`, ...)
  and metadata (`.fn`, `.param`, `.prefer`, `.returns`)
- **`.nif`** — Nib interface: function signatures, struct layouts, extern
  declarations. Imported via `use "path.nif";` for cross-module type checking.

## Building

Requires FreeBSD with bison 3.8+, flex, and clang.

```sh
make
```

## Quick start

Compile a Nib source file:

```sh
./nib source.nib                    # produces source.nir + source.nif
./nib source.nib --parse-only       # parse and validate only
```

Assemble a V20 program:

```sh
./nibasm program.asm -o program.bin -m program.map
```

Disassemble with labels:

```sh
./nibdis -m program.map program.bin
```

## Language overview

```
// Types map to hardware
u16 count = 100;
u8  AL = 0x20;           // uppercase = pinned to register
seg ES = 0xB800;

// Assignment uses :=
count := count + 1;
[ES:DI] := AL;

// Signed operators use $ prefix
if (a $> b) { ... }      // JG (signed greater)
u32 result = a $* b;     // IMUL (u16 * u16 -> u32)

// Explicit widening required — no implicit promotion
u16 wide = zero_extend(byte_val);
u16 wide = sign_extend(byte_val);

// Checked array access
u8 val = buffer![index]; // BOUND + access

// Constants inline as literals
const PORT_LCD = 0x60;
port_out(PORT_LCD, data);

// Function address as far pointer
far addr = &my_handler;

// Globals at fixed addresses
far[4] ivt at(0x0000:0x0000) = {&h0, &h1, &h2, &h3};

// Array initializers (short = zero-filled)
u8[8] buf = {0x41, 0x42, 0x43};

// Struct types require the struct keyword prefix
fn read(p: struct Point) -> u16 { ... }

// Cross-module imports
use "lcd.nif";

// The binder resolves register assignments across the whole call graph
fn fill(dest: u16, char: u8, count: u16) {
    seg ES = 0xB800;
    u16 DI = dest;
    u8  AL = char;
    u16 CX = count;
    asm clobbers(DI, CX, FLAGS) {
        rep stosb
    }
}

// Interrupt handlers with automatic save/restore
fn interrupt(0x1C, chain old_handler) reentrant my_timer() {
    tick_count := tick_count + 1;
    old_handler();
}
```

See `SPEC.md` for the full language specification.

## License

TBD
