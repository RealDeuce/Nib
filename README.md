# Nib

A compiled language for the NEC V20HL processor in the
[DreamWriter](https://en.wikipedia.org/wiki/NTS_DreamWriter) portable
word processor.

Nib sits between C and assembly — you get C-like block syntax, automatic
register allocation, and control flow, but the compilation is transparent
and predictable. You can reason about what assembly gets emitted.

## Status

**Early development.** The language spec is drafted, the grammar validates
clean (zero bison conflicts), and the assembler/disassembler roundtrip
correctly for all V20 instructions. The compiler and binder are not yet
implemented.

### What works

- Language specification (`SPEC.md`)
- Bison/flex parser for the full Nib grammar
- Two-pass V20 cross-assembler (`nibasm`)
  - Full 8086/80186 instruction set
  - V20 extensions (bit ops, BCD string ops, nibble rotate, 8080 emulation)
  - Map file output for symbol/data tracking
- V20 disassembler (`nibdis`) with map file support

### What's next

- V20 assembler refinements
- Nib compiler (`nib compile`)
- Binder with whole-program register allocation (`nib bind`)
- First program running in the emulator

## Building

Requires FreeBSD with bison 3.8+, flex, and clang.

```sh
make
```

## Quick start

Assemble a V20 program:

```sh
./nibasm program.asm -o program.bin -m program.map
```

Disassemble with labels:

```sh
./nibdis -m program.map program.bin
```

Parse a Nib source file:

```sh
./nib source.nib
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
u16 result = a $* b;     // IMUL

// Checked array access
u8 val = buffer![index]; // BOUND + access

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
```

See `SPEC.md` for the full language specification.

## License

TBD
