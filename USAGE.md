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
nibbind [-o output.asm] [--pressure-report file]
        [--pressure-fn name] [--cost-report file]
        [--cost-fn name] [--cost-annotate] file1.nir [file2.nir ...]
nibbind --pressure-compare old.txt new.txt [--pressure-fn name]
nibbind --cost-compare old.txt new.txt [--cost-fn name]
```

| Flag | Description |
|------|-------------|
| `-o file` | Output assembly file (default: `out.asm`) |
| `--pressure-report file` | Write pressure, allocation, and fixup diagnostics |
| `--pressure-fn name` | Limit the pressure report to one function |
| `--pressure-compare old new` | Compare two existing pressure reports |
| `--cost-report file` | Write static V20 timing, byte, transfer, and mix diagnostics |
| `--cost-fn name` | Limit the cost report or comparison to one function |
| `--cost-annotate` | Add per-instruction cost comments to the emitted assembly |
| `--cost-compare old new` | Compare two existing cost reports |

Accepts multiple `.nir` files. Builds the call graph, propagates
register preferences bottom-up from leaf functions, allocates physical
registers, inserts spill code and callee-save push/pop, and emits
assembly with real register names.

The pressure report includes a per-function pressure timeline, the peak
live span, allocation summaries, fixup counts by reason, spill-action
counts, live ranges with their final allocation, dead/early-load warnings,
and call-split advice. Fixup reasons include call argument routing,
caller-save save/restore, CL routing for variable shifts, address-register
routing, and return-value capture/reload around calls. Spill actions
include spill loads/stores, scratch save/restore, stack call-argument
routes, call-temp return saves/reloads, and memory-to-memory routes.

`--pressure-compare` is a compare-only mode. It reads two existing
pressure reports and prints per-function deltas for live pressure, spill
counts, allocation pressure, fixups, spill actions, and selected vreg
allocation changes.

The cost report is a static estimate from the emitted assembly using the
compiled V20 timing model. It includes per-function totals for bytes,
clocks, transfers, unknown instructions, variable-cost instructions,
instruction mix, per-source-line costs, and backward-branch loop bodies.
Conditional branches are reported as clock ranges because static analysis
does not know whether the branch is taken. Variable string/shift forms
are counted separately instead of being folded into a fake fixed total.

`--cost-annotate` rewrites the generated assembly with cost comments
before each instruction. These comments are assembler-safe and are off by
default to keep normal `.asm` output compact.

`--cost-compare` is a compare-only mode. It reads two existing cost
reports and prints per-function deltas for clock ranges, byte ranges,
unknown instruction counts, and variable-cost instruction counts.

### Examples

```sh
./nibbind app.nir lib.nir -o program.asm
./nibbind module.nir -o module.asm
./nibbind app.nir lib.nir -o program.asm \
  --pressure-report pressure.txt --pressure-fn boot
./nibbind --pressure-compare pressure-before.txt pressure-after.txt \
  --pressure-fn boot
./nibbind app.nir lib.nir -o program.asm \
  --cost-report cost.txt --cost-annotate
./nibbind --cost-compare cost-before.txt cost-after.txt \
  --cost-fn fb_copy_s1_full
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
line annotation. Supports 20-bit linear addresses for the full 1MB
address space.

```
nibdis [options] file.bin
```

| Flag | Description |
|------|-------------|
| `-o addr` | Set origin address (default: `0x0000`) |
| `-b N` | Disassemble N bytes |
| `-s offset` | Start at file offset |
| `-a addr` | Start at linear address |
| `-l label` | Disassemble a function/label (requires `-m`) |
| `-m file` | Load symbol map for labels and code/data distinction |
| `-d file` | Load debug info for source line interleaving |

Without a map file, all bytes are disassembled as code. With a map
file, data regions are emitted as `DB` directives and code regions
are disassembled with labels.

`-l label` looks up the label in the map, disassembles from that
address to the next label. Combine with `-b` to override the range.

### Examples

```sh
./nibdis program.bin
./nibdis -m program.map -d program.dbg program.bin
./nibdis -a 0xE0000 -b 256 rom.bin
./nibdis -m serif.map -l lcd_init serif.bin
./nibdis -a 0xFFFF0 -b 16 serif.bin
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
