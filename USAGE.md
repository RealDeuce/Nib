# Nib Command-Line Reference

## nib — Compiler

Compiles a `.nib` source file to Nib IR (`.nir`) and interface (`.nif`).

```
nib [--parse-only] [-D NAME=VALUE ...] [file.nib]
```

| Flag | Description |
|------|-------------|
| `--parse-only` | Parse and validate only; do not compile |
| `-D NAME=VALUE` | Define a compile-time name/value for `when` blocks |

If no input file is given, reads from stdin (output files default to
`out.nir` and `out.nif`). Otherwise, output filenames are derived from
the input: `source.nib` produces `source.nir` and `source.nif`.

### Examples

```sh
./nib source.nib                          # compile
./nib source.nib --parse-only             # parse only
./nib -D PLATFORM=dw -D DEBUG=1 source.nib  # with compile-time defines
```

---

## nibbind — Binder

Performs whole-program register allocation across all compiled modules.
Reads `.nir` files and outputs fully-resolved V20 assembly.

```
nibbind [-o output.asm] file1.nir [file2.nir ...]
```

| Flag | Description |
|------|-------------|
| `-o file` | Output assembly file (default: `out.asm`) |

Accepts multiple `.nir` files. Builds the call graph, propagates
register preferences bottom-up from leaf functions, allocates physical
registers, inserts spill code and callee-save push/pop, and emits
assembly with real register names.

### Examples

```sh
./nibbind app.nir lib.nir -o program.asm
./nibbind module.nir -o module.asm
```

---

## nibasm — Assembler

V20 cross-assembler with automatic Jcc relaxation. Encodes assembly
into machine code.

```
nibasm [-o output] [-m mapfile] [-d dbgfile] [--ihex] [file.asm]
```

| Flag | Description |
|------|-------------|
| `-o file` | Output binary file (default: `a.out`) |
| `-m file` | Write symbol map file |
| `-d file` | Write source debug info file |
| `--ihex` | Output Intel HEX format instead of flat binary |

If no input file is given, reads from stdin. Supports the full
8086/80186 instruction set plus V20 extensions (see SPEC.md for the
mnemonic table). Flat binary output uses 0xFF fill for gaps between
segments.

### Map file format

```
# nib map file
XXXX type name
```

Where `XXXX` is a 4-digit hex address, `type` is `code`, `data`, or
`equ`, and `name` is the label.

### Debug file format

```
# nib debug info
XXXXX file.nib:NN
```

Where `XXXXX` is a 5-digit hex linear address and `file.nib:NN` is
the source file and line number.

### Examples

```sh
./nibasm program.asm -o program.bin -m program.map -d program.dbg
./nibasm program.asm -o program.hex --ihex
./nibasm hand_written.asm -o test.bin
```

---

## nibdis — Disassembler

Disassembles V20 flat binary files with optional label and source
line annotation.

```
nibdis [-o org] [-b bytes] [-m mapfile] [-d dbgfile] file.bin
```

| Flag | Description |
|------|-------------|
| `-o addr` | Set origin address (default: `0x0000`) |
| `-b N` | Disassemble only first N bytes |
| `-m file` | Load symbol map for labels and code/data distinction |
| `-d file` | Load debug info for source line interleaving |

Without a map file, all bytes are disassembled as code. With a map
file, data regions are emitted as `DB` directives and code regions
are disassembled with labels.

### Examples

```sh
./nibdis program.bin
./nibdis -m program.map -d program.dbg program.bin
./nibdis -o 0xF000 -b 256 rom.bin
```

---

## nibbuild — Build Driver

Follows `use` chains from a root source file, compiles changed modules,
binds all IR, and assembles the result.

```
nibbuild [-f] [-o output.bin] main.nib
         [--nib ARGS...] [--asm ARGS...] [--bind ARGS...]
```

| Flag | Description |
|------|-------------|
| `-f` | Force rebuild all modules (ignore timestamps) |
| `-o file` | Output binary file (default: derived from input, e.g. `main.bin`) |
| `--nib` | Pass subsequent arguments to the nib compiler |
| `--bind` | Pass subsequent arguments to nibbind |
| `--asm` | Pass subsequent arguments to nibasm |

Runs three phases automatically:

1. **Compile** — runs `nib` on each module whose source is newer than
   its `.nir` or whose dependencies have changed
2. **Bind** — runs `nibbind` on all `.nir` files to produce assembly
3. **Assemble** — runs `nibasm` to produce the final binary

Module dependencies are discovered by scanning `use` directives in
source files. Build order is topologically sorted so dependencies
compile before dependents.

### Stage argument passthrough

`--nib`, `--asm`, and `--bind` are section markers. All arguments
after a marker are forwarded to that stage until the next marker or
end of the command line.

```sh
./nibbuild app.nib \
  --nib -D PLATFORM=dw -D DEBUG=1 \
  --asm -d app.dbg -m app.map
```

### Build file (`nib.build`)

If a `nib.build` file exists in the current directory, nibbuild reads
it for default settings. CLI arguments append to (not replace) build
file values. CLI `-o` and the root file override build file values.

```
# nib.build
root    src/reset.nib
output  src/reset.bin

nib     -D PLATFORM=dreamwriter
asm     -d src/reset.dbg -m src/reset.map
bind
```

| Key | Description |
|-----|-------------|
| `root` | Root .nib source file |
| `output` | Output binary path |
| `nib` | Extra arguments for the nib compiler |
| `bind` | Extra arguments for nibbind |
| `asm` | Extra arguments for nibasm |

Lines starting with `#` are comments. Values for `nib`/`asm`/`bind`
are tokenized on whitespace; use double quotes for values containing
spaces.

With a build file in place, a bare `nibbuild` with no arguments is
sufficient.

### Examples

```sh
./nibbuild app.nib                          # produces app.bin
./nibbuild app.nib -o firmware.bin          # custom output name
./nibbuild -f app.nib                       # force clean rebuild
./nibbuild app.nib --asm -m app.map         # pass -m to nibasm
./nibbuild                                  # use nib.build in cwd
```
