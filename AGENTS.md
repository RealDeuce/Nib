# Nib

Nib is a compiled language targeting the NEC V20HL CPU in the DreamWriter
portable word processor. It provides C-like syntax over direct hardware
access with automatic register allocation and minimal caller-saves
overhead. The goal is not abstraction — it's a better notation for
writing efficient assembly.

## Architecture

Nib is a cross-compiler toolchain running on FreeBSD, producing flat
binaries for the V20. The target hardware is emulated by `../dreamulator`.

### Toolchain pipeline

```
source.nib → [nib compile] → .nob + .nif
.nob + .nif → [nib bind] → .asm
.asm → [nib asm] → binary
```

- **nib compile** (`nib`): Per-file parser, type checker, local register
  allocator. Outputs `.nir` (pseudo-assembly with virtual registers) and
  `.nif` (interface for cross-module imports via `use`).
- **nib bind** (not yet implemented): Whole-program register resolution
  across function boundaries via call graph analysis. Outputs V20 assembly.
- **nib asm** (`nibasm`): Two-pass V20 assembler. Intel syntax with V20
  extensions (bext/bins for bit field ops). Flat binary output with
  optional map files.
- **nib dis** (`nibdis`): Disassembler wrapping dreamulator's dis86.
  Supports map files for code/data distinction.

### Key design decisions

- **Assignment is `:=`**, declaration initializer is `=`. No ambiguity.
- **Predefined variables are UPPERCASE** (registers: `AX`, `SI`; flags:
  `CF`, `ZF`). User identifiers are lowercase/snake_case only.
- **`$` prefix for signed operators**: `$>`, `$<`, `$*`, `$/`, `$>>`.
- **Checked array access**: `buffer![index]` (BOUND instruction).
- **Arrays don't decay to pointers**. Size is part of the type.
- **The binder (not linker)** does whole-program register allocation.
  Register choices propagate up the call stack.
- **`preserves()` not `clobbers()`** on extern declarations — fail-safe
  default (unlisted registers assumed clobbered). `asm {}` blocks allow
  either keyword (pick whichever is shorter), but not both.
- **Binder outputs .asm** — human-readable, debuggable intermediate form.
- **Strong typing** — no implicit integer promotion. Mixing u8 and u16
  requires explicit `sign_extend()` or `zero_extend()`. Literals auto-promote.
- **Block scoping** with shadowing. No forward calls within a file.
- **Flat namespace** for `use` imports. No module prefixes.
- **Single-pass compilation** — no forward calls, which means the compiler
  doesn't need a second pass to resolve function signatures within a file.

## Building

```sh
make          # builds nib (parser), nibasm (assembler), nibdis (disassembler)
make check    # runs bison with -Wcounterexamples to verify grammar
make test     # runs parser against test .nib files
```

Requires: bison 3.8+, flex 2.6+, clang (FreeBSD base).

## Testing

```sh
# Compile Nib source to IR + interface
./nib tests/basic.nib                    # produces tests/basic.nir + tests/basic.nif
./nib tests/basic.nib --parse-only       # parse and validate only

# Assemble and disassemble roundtrip
./nibasm tests/allops.asm -o tests/allops.bin -m tests/allops.map
./nibdis -m tests/allops.map tests/allops.bin
```

## Debugging workflow

- When fixing codegen or binder bugs, add a focused regression test under
  `tests/` once the failure mode is understood.
- After a bug has a regression test and the fix is confirmed with
  `make test`, commit the fix and regression together.
- Commit messages should match the existing style: concise subject,
  explanatory body wrapped to 72 columns, and the project co-author trailer
  when applicable.

## Files

| File | Purpose |
|------|---------|
| `SPEC.md` | Language specification |
| `TODO.md` | Outstanding work items |
| `asm.c` | V20 cross-assembler |
| `dis.cpp` | Disassembler (wraps `../dreamulator/src/dis86.cpp`) |
| `nib.y` | Bison grammar (%expect 2: far param pin ambiguity) |
| `nib.l` | Flex lexer (two modes: normal + asm block) |
| `ast.h` / `ast.c` | AST node types and constructors |
| `compile.h` / `compile.c` | Compiler backend: symbol table, type checker, .nir/.nif emitters |
| `tests/` | Test files for parser and assembler |

## V20 extension mnemonics

Standard Intel 8086/80186 mnemonics except:
- `bext` / `bins` — V20 bit field extract/insert (avoids conflict with
  Intel's INSB/INSW port string instructions)
- `test1` / `clr1` / `set1` / `not1` — V20 single-bit operations
- `add4s` / `sub4s` / `cmp4s` — V20 BCD string operations
- `rol4` / `ror4` — V20 nibble rotate
- `brkem` — V20 break to 8080 emulation mode

## Dependencies

- The disassembler includes `../dreamulator/src/dis86.cpp` directly.
- The emulator lives at `../dreamulator`.
- The MAME V20 reference is at `../mame`.
