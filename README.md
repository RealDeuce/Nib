# Nib

[![CI](https://github.com/RealDeuce/Nib/actions/workflows/ci.yml/badge.svg)](https://github.com/RealDeuce/Nib/actions/workflows/ci.yml)

A compiled language for the NEC V20HL processor in the
[DreamWriter](https://en.wikipedia.org/wiki/NTS_DreamWriter) portable
word processor.

Nib sits between C and assembly — you get C-like block syntax, automatic
register allocation, and control flow, but the compilation is transparent
and predictable. You can reason about what assembly gets emitted.

## Status

**Active development.** The full toolchain compiles Nib source through to
V20 machine code. The language spec, compiler, binder, assembler,
disassembler, and build driver are all working. 95 tests pass.

### What works

- Language specification (`SPEC.md`)
- Compiler (`nib`) — parses `.nib` source, type-checks, emits `.nir` + `.nif`
  - Zero bison conflicts
  - Strong type checking (no implicit integer promotion, literals auto-promote)
  - Virtual register allocation with register pinning preferences
  - Scope tracking with shadowing
  - `const` declarations, array initializers, `at()` placement, `&fn` addresses
  - `pub` visibility control — only `pub` declarations exported to `.nif`
  - Parameter/return register pins (`in REG`) and `clobbers()` for API boundaries
  - Source-level debug info (`; @file:line` comments in IR)
- Binder (`nibbind`) — whole-program register allocation
  - Call graph construction and topological sorting
  - Bottom-up register preference propagation from leaves
  - Inter-procedural register assignment across function boundaries
  - Callee-save push/pop generation from `preserves` lists
  - Extern parameter pin pre-propagation
  - Spill slot allocation with BP frame pointer
  - Constant pool and placed data block emission
- Two-pass V20 cross-assembler (`nibasm`)
  - Full 8086/80186 instruction set
  - V20 extensions (bit ops, BCD string ops, nibble rotate, 8080 emulation)
  - `SEG` operator for segment-relative label references
  - Map file output (`-m`), debug info output (`-d`)
  - Intel HEX output (`--ihex`) for 20-bit address space
- V20 disassembler (`nibdis`) with map file and debug info support
- Build driver (`nibbuild`) — follows `use` chains, recompiles changed modules

### What's next

- Peephole optimizer (post-binder, on real assembly)
- First program running on DreamWriter hardware

## Toolchain pipeline

```
source.nib + deps.nif  --[nib compile]-->  .nir + .nif
*.nir files            --[nibbind]-->      .asm
.asm                   --[nibasm]-->       .bin + .map + .dbg
```

For convenience, `nibbuild source.nib` runs all three stages,
following `use` chains to discover and compile dependencies.

- **`.nir`** — Nib IR: pseudo-assembly with virtual registers (`%0`, `%1`, ...)
  and metadata (`.fn`, `.param`, `.prefer`, `.returns`, `.preserves`)
- **`.nif`** — Nib interface: `pub` function signatures, struct layouts,
  extern declarations, constants. Imported via `use "path.nif";`.
- **`.dbg`** — Debug info: maps binary addresses to source file:line.

## Building

Requires FreeBSD with bison 3.8+, flex, and clang.

```sh
make
```

## Quick start

Compile and bind a Nib program:

```sh
./nib compile source.nib            # produces source.nir + source.nif
./nibbind source.nir -o source.asm  # whole-program register allocation
./nibasm source.asm -o source.bin -m source.map -d source.dbg
```

Or use the build driver:

```sh
./nibbuild source.nib               # compile + bind + assemble
```

Disassemble with labels and source lines:

```sh
./nibdis -m source.map -d source.dbg source.bin
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

// Globals at fixed addresses with initializers
far[4] ivt at(0x0000:0x0000) = {&h0, &h1, &h2, &h3};

// Visibility — only pub declarations exported to .nif
pub fn lcd_clear(fill: u8) { ... }  // visible to importers
fn helper() { ... }                  // module-private

// API boundaries with pinned registers and clobbers
fn api_read(port: u16 in DX) -> u8 in AL clobbers(FLAGS) {
    u8 AL = port_in(port);
    return AL;
}

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

BSD 3-Clause. See [LICENSE](LICENSE).
