# Nib Language Specification

## Overview

Nib is a compiled language targeting the NEC V20HL (8086/80186-compatible)
processor in the DreamWriter portable word processor. It provides C-like
syntax over direct hardware access with automatic register allocation,
whole-program register optimization, and minimal caller-saves overhead.

The goal is not abstraction — it's a better notation for writing efficient
assembly.

### Identifiers

Identifiers match `[a-zA-Z_][a-zA-Z0-9_]*`. Registers (`AX`, `BX`, ...)
and flags (`CF`, `ZF`, ...) are matched as keyword tokens before the
identifier rule, so there is no ambiguity between uppercase identifiers
and hardware names.

## Toolchain

```
source.nib + deps.nif  --[nib]-->      .nir + .nif
*.nir files            --[nibbind]-->  .asm
.asm                   --[nibasm]-->   .bin + .map + .dbg
```

For convenience, `nibbuild source.nib` runs all three stages, following
`use` chains to discover and compile dependencies.

- **nib**: Per-file compilation. Parses, type-checks, performs local
  register allocation, emits Nib IR (pseudo-assembly with virtual registers),
  and outputs an interface file consumed by other compilations via `use`.

- **nibbind**: Whole-program pass. Builds the call graph, propagates register
  assignments across function boundaries, resolves conflicts by inserting
  minimal moves and saves. Outputs fully-resolved V20 assembly — real
  register names, explicit saves/restores, no pseudoregisters. Inline
  `asm` blocks are spliced through as-is. The output is human-readable:
  the programmer can inspect exactly what the binder decided.

- **nibasm**: V20 assembler. Encodes the assembly output from the binder
  into machine code. Two-pass to resolve jump offsets and short/long
  encoding selection. Also usable standalone for hand-written assembly.
  Uses Intel syntax with `bext`/`bins` for the V20 bit field instructions
  (see Inline Assembly for the full mnemonic table). Outputs flat binary,
  Intel HEX (`--ihex`), map files (`-m`), and debug info (`-d`).


## Types

All types map directly to hardware capabilities.

### Scalar types

| Type  | Size     | Hardware mapping                         |
|-------|----------|------------------------------------------|
| `u8`  | 1 byte   | Byte register (AL, AH, BL, BH, CL, etc) |
| `u16` | 2 bytes  | Word register (AX, BX, CX, DX, SI, etc)  |
| `u32` | 4 bytes  | Register pair (typically DX:AX)           |
| `seg` | 2 bytes  | Segment register (DS, ES, SS, CS)         |
| `bool`| FLAGS    | Condition in flags register, not stored   |

`bool` is not a storable type. It exists only as the result of comparisons
and is consumed immediately by control flow. There is no `true`/`false`
literal.

### Flag variables

The CPU flags register is exposed as predefined `bool` variables. These
are always available without declaration:

| Name   | Flag | Description                              |
|--------|------|------------------------------------------|
| `CF`   | CF   | Carry flag                               |
| `PF`   | PF   | Parity flag (even parity of low byte)    |
| `AF`   | AF   | Auxiliary carry (BCD half-carry)         |
| `ZF`   | ZF   | Zero flag                                |
| `SF`   | SF   | Sign flag                                |
| `TF`   | TF   | Trap flag (single-step debugging)        |
| `DF`   | DF   | Direction flag (string op direction)     |
| `OF`   | OF   | Overflow flag (signed overflow)          |
| `IF`   | IF   | Interrupt enable flag                    |

Predefined variables are uppercase, so `IF` (flag) does not conflict
with `if` (keyword).

Flag variables can be read in conditions:

```
if (CF) { ... }         // JC ...
if (!ZF) { ... }        // JNZ ...
while (CF) { ... }      // loop while carry set
```

Flag variables can be set directly:

```
CF := 0;                // CLC
CF := 1;                // STC
DF := 0;                // CLD
DF := 1;                // STD
IF := 0;                // CLI
IF := 1;                // STI
```

Flags are implicitly set by arithmetic and logic operations. The compiler
tracks flag liveness — a flag set by an ADD is available for a subsequent
`if (CF)` without an explicit comparison.

### Aggregate types

| Type       | Size          | Description                    |
|------------|---------------|--------------------------------|
| `u8[N]`    | N bytes       | Fixed-size byte array          |
| `u16[N]`   | N * 2 bytes   | Fixed-size word array          |
| `far[N]`   | N * 4 bytes   | Fixed-size far pointer array   |
| `bcd[N]`   | N bytes       | Packed BCD, 2 digits per byte  |

Array size is part of the type. `u8[80]` and `u8[40]` are distinct types.
Arrays do not decay to pointers. Struct arrays use `struct Name[N]` syntax.


## Declarations

### Variables

The type serves as the declaration keyword. No `let`, `var`, or other
prefix.

```
u16 count = 100;
u8  ch = 0x20;
seg ES = 0xB800;
bcd[4] price = 9999;
u8[80] buffer;
```

### Array initializers

Arrays can be initialized with C-style brace syntax. Short initializers
are zero-filled to the declared size.

```
u8[8] buf = {0x41, 0x42, 0x43};    // 3 bytes + 5 zero bytes
u16[4] table = {100, 200, 300, 400};
far[4] ivt = {&handler_a, &handler_b, &handler_c, &handler_d};
```

### Constants

Named compile-time constants are declared with `const`. They inline as
literals wherever used — no storage is allocated.

```
const PORT_LCD = 0x60;
const SCREEN_WIDTH = 480;

port_out(PORT_LCD, data);           // same as port_out(0x60, data)
```

### Visibility

By default, declarations are module-private — not visible to other
modules via `use`. The `pub` keyword exports a declaration to the `.nif`
interface file.

```
pub fn lcd_clear(fill: u8) { ... }      // visible to importers
fn helper() { ... }                      // module-private

pub struct Point { u16 x; u16 y; }       // visible to importers
struct InternalState { u16 flags; }      // module-private

pub const PORT_LCD = 0x60;               // visible to importers
const BUFFER_SIZE = 256;                 // module-private
```

`pub` can be applied to functions, structs, globals, and constants.
Extern declarations are always exported.

### Register pinning

A variable whose name matches a register (uppercase) is pinned to that
register. All other variable names are auto-allocated by the compiler.

Pinnable names:
- Byte: `AL`, `AH`, `BL`, `BH`, `CL`, `CH`, `DL`, `DH`
- Word: `AX`, `BX`, `CX`, `DX`, `SI`, `DI`, `BP`
- Segment: `DS`, `ES`, `SS`, `CS`

`SP` is not directly assignable.

```
u16 CX = 256;           // pinned to CX register
u16 count = 256;        // auto-allocated to any free word register
u8  AL = 0x20;          // pinned to AL
u8  ch = 0x20;          // auto-allocated to any free byte register
```

Pinning a variable to a register that conflicts with the current allocation
state is a compile-time error.


## Functions

### Declaration

```
fn name(param: type, ...) -> return_type {
    body
}
```

Return type is omitted if the function returns nothing. Struct type
parameters and return types require the `struct` keyword prefix.

```
fn add(a: u16, b: u16) -> u16 {
    return a + b;
}

fn clear_screen() {
    // ...
}

fn read_point(p: struct Point) -> u16 {
    return p.x + p.y;
}
```

### Parameter and return register pins

For API boundary functions, parameters and return values can be pinned
to specific registers with `in REG`. This locks the ABI so the binder
treats the assignments as hard constraints rather than preferences.

```
fn api_read(port: u16 in DX) -> u8 in AL {
    u8 AL = port_in(port);
    return AL;
}
```

### clobbers()

`clobbers()` is the inverse of `preserves()` — it lists registers the
function is allowed to trash, and everything else is callee-saved. The
return register is implicitly clobbered. Intended for API boundary
functions that preserve almost everything.

```
fn api_read(port: u16 in DX) -> u8 in AL clobbers(FLAGS) {
    // preserves everything except AL (return reg) and FLAGS
    ...
}

fn api_memcopy(dst: u16 in DI, src: u16 in SI, len: u16 in CX)
    clobbers(SI, DI, CX, FLAGS) {
    // preserves AX, BX, DX, BP
    ...
}
```

`clobbers()` and `preserves()` are mutually exclusive on a declaration.
The compiler inverts `clobbers()` to `preserves` in the IR — the binder
only ever sees `preserves`.

### Parameter passing

There are no explicit parameter passing keywords. The binder determines
how each parameter is delivered:

- **Scalars** (u8, u16, u32, seg): Passed in registers. The binder chooses
  which register based on whole-call-graph analysis.
- **Aggregates** (arrays, BCD): Passed by reference. The address arrives
  in a register chosen by the binder.

Register assignments propagate up the call stack. If function C needs a
parameter in SI (because it uses `movsb`), and B calls C, and A calls B,
then A can place the value in SI from the start. No intermediate moves.

### Explicit copy

When a function needs a local copy of an aggregate parameter, use `value`:

```
fn process(value buf: u8[80]) {
    // buf is a local copy on the stack, 80 bytes
    // modifications don't affect the caller's data
}
```

### Return values

Scalars are returned in a register chosen by the binder. The return
register propagates through the call graph like parameter registers:

```
fn compute() -> u16 {
    return result;
}

fn caller() {
    u16 SI = compute();     // caller wants result in SI
    // binder assigns compute's return reg to SI — no move
    asm { rep movsb }
}
```

If multiple callers disagree on which register they want the return
value in, the binder resolves the conflict with a minimal move at the
more expensive call site.

For u32 return values, the binder assigns a register pair (typically
DX:AX). Aggregates are returned via a caller-provided destination
address in a binder-chosen register.

### Extern functions

Functions not written in Nib (ROM routines, hand-written assembly) are
declared `extern`. Since the binder cannot analyze their bodies, the
declaration must fully specify register assignments and clobbers.

```
extern fn far rom_read(port: u16 in DX) -> u16 in AX
    preserves(BX, CX, SI, DI, BP, DS, ES, SS);
```

The `in REG` clause pins a parameter or return value to a specific
register. The binder treats these as fixed constraints — non-negotiable
assignments that the rest of the call graph must work around.

The `preserves` clause lists which registers the function guarantees
not to modify. Everything not listed is assumed clobbered. This is
fail-safe: forgetting a register in the list means the binder treats
it as clobbered (conservative), rather than silently trusting it.
For Nib functions, preserves/clobbers are computed automatically by
the compiler. For externs, `preserves` must be declared explicitly.
Omitting `preserves` on an extern means nothing is preserved — the
binder assumes the function destroys everything.

```
// Multiple parameters, all pinned
extern fn far bios_scroll(
    lines: u8 in AL,
    attr: u8 in BH,
    top: u16 in CX,
    bottom: u16 in DX
) preserves(SI, DI, BP, DS, ES, SS);

// No return value
extern fn far rom_beep(
    freq: u16 in BX,
    duration: u16 in CX
) preserves(AX, DX, SI, DI, BP, DS, ES, SS);

// Near extern (same segment)
extern fn helper(val: u16 in AX) -> u16 in AX
    preserves(BX, CX, DX, SI, DI, BP, DS, ES, SS);
```

Extern declarations have no body. The binder resolves the symbol
address from the link map or an explicit address:

```
// Extern at a known absolute address
extern fn far [0xF000:0x0100] rom_init()
    preserves(BP, DS, ES, SS);
```

### Extern interrupt declarations

Software interrupts can be declared as extern functions using the
`interrupt(N)` modifier. The compiler emits INT N instead of CALL,
but everything else works like a normal function call — register
setup, clobber tracking, and binder integration.

```
extern fn interrupt(0x21) dos_putchar(
    service: u8 in AH,
    char: u8 in DL
) preserves(BX, CX, SI, DI, BP, DS, ES, SS);

// Called like any function — compiler emits INT 0x21
dos_putchar(0x02, "A");
```

This eliminates manual register setup and push/pop for software
interrupt calls. Multiple services on the same vector can be declared
as separate functions:

```
extern fn interrupt(0x21) dos_putchar(svc: u8 in AH, char: u8 in DL)
    preserves(BX, CX, SI, DI, BP, DS, ES, SS);

extern fn interrupt(0x21) dos_getchar(svc: u8 in AH) -> u8 in AL
    preserves(BX, CX, DX, SI, DI, BP, DS, ES, SS);
```

For a raw INT with manual register setup, use `asm { int N }`.


## Expressions

### Arithmetic

Standard operators, mapping to V20 instructions. All operators are
unsigned by default. The `$` prefix selects the signed variant.

#### Unsigned (default)

| Expression | Instruction | Notes                          |
|------------|-------------|--------------------------------|
| `a + b`    | ADD         |                                |
| `a - b`    | SUB         |                                |
| `a * b`    | MUL         | Result may be u32 (DX:AX)     |
| `a / b`    | DIV         |                                |
| `a % b`    | DIV         | Remainder                      |
| `a & b`    | AND         |                                |
| `a | b`    | OR          |                                |
| `a ^ b`    | XOR         |                                |
| `~a`       | NOT         |                                |
| `a << n`   | SHL         | n pinned to CL if not constant |
| `a >> n`   | SHR         | Logical shift (zero-fill)      |
| `-a`       | NEG         |                                |

#### Signed (`$` prefix)

| Expression  | Instruction | Notes                          |
|-------------|-------------|--------------------------------|
| `a $* b`    | IMUL        | Signed multiply                |
| `a $/ b`    | IDIV        | Signed divide                  |
| `a $% b`    | IDIV        | Signed remainder               |
| `a $>> n`   | SAR         | Arithmetic shift (sign-extend) |

ADD and SUB are the same instruction for signed and unsigned — the
difference is only in which flags you check afterward. Therefore `+`
and `-` have no `$` variant.

#### Rotate

| Expression   | Instruction | Notes                           |
|--------------|-------------|---------------------------------|
| `a <<< n`    | ROL         | Rotate left                     |
| `a >>> n`    | ROR         | Rotate right                    |
| `a <<<. n`   | RCL         | Rotate left through carry (CF)  |
| `a >>>. n`   | RCR         | Rotate right through carry (CF) |

Like shifts, `n` is pinned to CL if not a constant.

The carry-rotate operators (`<<<.` and `>>>.`) treat CF as an extra
bit in the rotation. This is useful for multi-word shifts and
serializing bits through the carry flag.

#### Exchange

```
a <=> b                     // XCHG — atomic swap, no temp needed
```

Works between two registers, or between a register and memory.

#### Address-of

```
u16 addr = &variable        // LEA — load effective address
```

Returns the offset of a variable within its segment. For struct fields:

```
u16 addr = &record.field    // LEA with displacement
```

When applied to a function name, `&` returns a `far` address (segment:offset)
that the binder resolves at link time:

```
far handler_addr = &my_handler  // far pointer to function
```

### Comparisons

Comparisons produce `bool` (a flags condition), consumed by control flow.

#### Unsigned (default)

| Expression   | Jump | Flags checked         |
|--------------|------|-----------------------|
| `a == b`     | JZ   | ZF=1                  |
| `a != b`     | JNZ  | ZF=0                  |
| `a > b`      | JA   | CF=0, ZF=0            |
| `a < b`      | JB   | CF=1                  |
| `a >= b`     | JAE  | CF=0                  |
| `a <= b`     | JBE  | CF=1 or ZF=1          |

#### Signed (`$` prefix)

| Expression   | Jump | Flags checked         |
|--------------|------|-----------------------|
| `a $> b`     | JG   | ZF=0, SF=OF           |
| `a $< b`     | JL   | SF!=OF                |
| `a $>= b`    | JGE  | SF=OF                 |
| `a $<= b`    | JLE  | ZF=1 or SF!=OF        |

Equality (`==`, `!=`) is the same for signed and unsigned.

```
if (a > b) { ... }      // CMP a, b; JA ...  (unsigned)
if (a $< b) { ... }     // CMP a, b; JL ...  (signed)
if (a == 0) { ... }     // TEST a, a; JZ ...
```

### Memory access

Square bracket syntax for memory operations:

```
u8 val = [SI];              // MOV val, DS:[SI]
u8 val = [ES:DI];           // MOV val, ES:[DI]
[DI] := val;                // MOV DS:[DI], val
[ES:DI] := val;             // MOV ES:[DI], val

u16 val = [BX + 4];         // MOV val, DS:[BX+4]
u8 val = [BX + SI];         // MOV val, DS:[BX+SI]
u8 val = [BX + SI + 2];     // MOV val, DS:[BX+SI+2]

// Absolute address
u8 val = [0x1234];           // MOV val, DS:[0x1234]
u8 val = [0xB800:0x0000];   // MOV val, 0xB800:[0x0000]
```

The addressing modes available mirror V20 hardware:
- `[BX+SI]`, `[BX+DI]`, `[BP+SI]`, `[BP+DI]`
- `[SI]`, `[DI]`, `[BX]`, `[BP]`
- Any of the above with 8-bit or 16-bit displacement
- Direct 16-bit offset

Note: register names in memory expressions are always uppercase.

#### Checked array access

Array access can be bounds-checked using `!` after the array expression:

```
u8 val = buffer![index];    // BOUND + access
```

The compiler emits a BOUND instruction before the memory access. If
`index` is outside `0..length-1`, the CPU triggers INT 5 (bound range
exceeded). The bounds pair (0 and length-1) is generated automatically
in the data segment for each array type accessed with `!`.

Unchecked access (the default) performs no check:

```
u8 val = buffer[index];     // direct access, no check
```

The `bound()` builtin provides manual bounds checking for cases not
covered by array syntax (see Builtins section).

### BCD operations

BCD arrays support arithmetic directly:

```
bcd[4] total = 0;
bcd[4] item = 1299;
total := total + item;      // ADD4S, CL=4
total := total - item;      // SUB4S, CL=4
if (total > item) { ... }   // CMP4S, CL=4
```


## Builtins

Builtins are compiler-intrinsic functions that map directly to V20
instructions. They look like function calls but are always inlined —
there is no call overhead.

### String operations

Block memory operations using the V20 string instructions with REP
prefix. The compiler sets up SI, DI, CX, and segment registers
automatically. Array sizes are known at compile time, so CX is
loaded from the type.

```
memcopy(dst, src)           // REP MOVSB or REP MOVSW
```

Copies `src` to `dst`. Both must be the same array type.
Uses MOVSW when the type is `u16[N]`, MOVSB otherwise.

```
memset(dst, val)            // REP STOSB or REP STOSW
```

Fills `dst` with `val`. Uses STOSW for `u16[N]`, STOSB for `u8[N]`.

```
memcmp(a, b) -> bool        // REPE CMPSB or REPE CMPSW
```

Compares two arrays. Result is ZF — use with `if (memcmp(a, b))`.
Both must be the same array type.

```
memscan(haystack, needle) -> u16   // REPNE SCASB or REPNE SCASW
```

Scans `haystack` for `needle`. Returns the offset where found, or
the array length if not found. Result derived from DI after REPNE.

```
load(src) -> u8 / u16      // LODSB or LODSW  (not yet implemented)
```

Loads one element from `src` and advances the source pointer (SI).
Respects DF (direction flag). Useful in loops processing arrays
element by element.

```
store(dst, val)             // STOSB or STOSW  (not yet implemented)
```

Stores one element to `dst` and advances the destination pointer (DI).
Respects DF.

Example:

```
fn clear_vram(seg_addr: u16) {
    seg ES = seg_addr;
    u8[2000] screen;         // reference to screen memory
    memset(screen, 0x20);    // REP STOSB — fill with spaces
}
```

### Port I/O

```
port_in(port) -> u8 / u16  // IN AL, port  or  IN AX, port
port_out(port, val)         // OUT port, AL  or  OUT port, AX
```

The value type determines byte vs word I/O. The port can be an
immediate (0x00-0xFF) or a u16 variable (uses DX).

```
u8 scancode = port_in(0x60);       // IN AL, 0x60
u16 DX = 0x03F8;
u8 data = port_in(DX);             // IN AL, DX
port_out(0x90, 0x01);               // OUT 0x90, AL
```

### Type widening

```
sign_extend(val) -> u16 / u32      // CBW or CWD
zero_extend(val) -> u16 / u32      // MOV AH, 0  or  XOR DX, DX
```

Sign-extends or zero-extends a value to the next wider type:
- `u8` → `u16`: sign_extend uses CBW, zero_extend clears AH
- `u16` → `u32`: sign_extend uses CWD, zero_extend clears DX

These are explicit builtins rather than casts because they clobber
the upper register/half (AH or DX).

```
u8 offset = 0xFE;                  // -2 in signed
u16 wide = sign_extend(offset);    // AX = 0xFFFE
u16 wide = zero_extend(offset);    // AX = 0x00FE
```

The compiler does not auto-promote between integer sizes. Mixing
u8 and u16 in an expression is a type error — the programmer must
explicitly choose sign_extend or zero_extend. Literals are the only
exception: they auto-promote to the type required by context.

### Division with remainder

```
divmod(a, b, q, r)              // DIV — q := a/b, r := a%b
sdivmod(a, b, q, r)             // IDIV — signed
```

Emits a single DIV/IDIV instruction and writes both the quotient and
remainder. The `/` and `%` operators still work individually when you
only need one result, but each emits its own DIV. Use `divmod` when
you need both to avoid dividing twice.

```
u16 q;
u16 r;
divmod(total, 10, q, r);        // one DIV instruction
```

### BCD adjustment

Manual packed/unpacked BCD normalization. These are not called
automatically — the programmer invokes them when they need the
result in valid BCD form or want to check flags on a BCD value.

```
daa(val)                    // DAA — decimal adjust after addition
das(val)                    // DAS — decimal adjust after subtraction
```

Packed BCD: two digits per byte. Use after ADD/ADC on packed BCD bytes.

```
aaa(val)                    // AAA — ASCII adjust after addition
aas(val)                    // AAS — ASCII adjust after subtraction
aam(val)                    // AAM — ASCII adjust after multiplication
aad(val)                    // AAD — ASCII adjust before division
```

Unpacked BCD: one digit per byte (0x00-0x09).

```
// Example: manual packed BCD addition of single bytes
u8 AL = bcd_byte1 + bcd_byte2;
daa(AL);                    // AL is now valid packed BCD
```

For multi-byte BCD, prefer the `bcd[N]` type with `+`/`-` operators
which emit ADD4S/SUB4S and handle everything automatically.

### Table lookup

```
xlat(table, index) -> u8   // XLAT — BX=table, AL=index
```

Looks up `table[index]` using the XLAT instruction. The table must be
`u8[256]` (full byte range). The index is a `u8`.

```
u8[256] to_upper;           // lookup table
u8 ch = xlat(to_upper, input_char);  // XLAT
```

### Bit field extract/insert (V20 extension)

Struct bit fields use EXT/INS with compile-time constant offsets and
lengths (see Structs / Bit fields). These builtins expose the full
dynamic capability — runtime bit offset and length via registers.

```
extract(src, offset, length) -> u16     // EXT  (not yet implemented)
insert(dst, offset, length, val)        // INS  (not yet implemented)
```

- `src`/`dst` — memory reference (loaded into SI for EXT, DI for INS)
- `offset` — bit offset, u8 (low nibble, 0-15)
- `length` — bit count, u8 (1-16)
- `val` — u16 value to insert (from AX)
- Returns: extracted bits in the low `length` bits of the result

Bit 0 is LSB, matching the V20 EXT/INS convention. If offset+length
exceeds 15, the instruction crosses into the next word and auto-advances
the source/destination pointer.

```
// Extract 5 bits at a dynamic position
u8 pos = 7;
u16 val = extract(buffer, pos, 5);

// Insert 5 bits at a dynamic position
insert(buffer, pos, 5, val);

// Bitstream reader
fn read_bits(stream: u8[512], pos: u16, width: u8) -> u16 {
    u8 byte_off = pos >> 3;
    u8 bit_off = pos & 0x07;
    return extract(stream[byte_off], bit_off, width);
}
```

### Nibble rotate (V20 extension)

```
nibble_rol(mem, val) -> u8  // ROL4  (not yet implemented)
nibble_ror(mem, val) -> u8  // ROR4  (not yet implemented)
```

Rotates 4-bit nibbles between a memory byte and AL. Used for
manipulating packed BCD digits and nibble-level data.

### Bounds check

```
bound(val, low, high)       // BOUND — INT 5 if val < low or val > high
```

Manual bounds checking. Triggers INT 5 (bound range exceeded) if `val`
is outside the range `[low, high]`. `low` and `high` must be in
memory (the instruction reads a pair of words).

```
bound(index, 0, 79);        // check index is valid for u8[80]
u8 val = buffer[index];
```

For array access, prefer the `!` checked access syntax (see Memory
access section) which generates the BOUND automatically.

### Exchange helpers

```
swap_flags(val)             // LAHF/SAHF  (not yet implemented)
```

LAHF loads SF, ZF, AF, PF, CF into AH. SAHF stores AH back to those
flags. Useful for saving/restoring flag state in a register.

### Complement carry

```
CF := ~CF                   // CMC — complement carry flag
```

Toggles the carry flag. Follows the same `~` toggle syntax as
single-bit struct fields.

### Miscellaneous

```
halt()                      // HLT — halt until next interrupt
nop()                       // NOP — no operation
salc() -> u8                // SALC — AL = CF ? 0xFF : 0x00
emulate(seg, entry)         // BRKEM — enter 8080 emulation mode
```

`emulate()` switches the V20 into 8080 emulation mode. The CPU
executes 8080 instructions in the given segment starting at `entry`
until the 8080 code executes CALLN to return to native mode.
All registers are assumed clobbered.

## Control Flow

### Flag checks on operations

An assignment can be followed by a flag-check block that runs handlers
based on flags set by the operation. This avoids separate flag-checking
lines after arithmetic.

```
total := total + amount {
    CF: { handle_unsigned_overflow(); }
    OF: { handle_signed_overflow(); }
    AF: { bcd_fixup(); }
}
```

The operation executes first. Then each case is checked in order. If
the flag condition is true, its handler block runs. Unlisted flags are
ignored. There is no fall-through between cases.

Flag conditions support full boolean logic:

| Syntax       | Meaning           | Compiled as              |
|--------------|-------------------|--------------------------|
| `CF`         | carry set         | JC                       |
| `!CF`        | carry not set     | JNC                      |
| `CF \| OF`   | either set        | JC or JO                 |
| `CF & OF`    | both set          | JNC skip; JNO skip       |
| `CF ^ OF`    | exactly one set   | test both, XOR result    |
| `!(CF \| OF)`| neither set       | JC skip; JO skip         |

```
total := total + amount {
    CF | OF: { handle_any_overflow(); }
    ZF: { result_was_zero(); }
}

count := count - 1 {
    !CF & ZF: { exactly_zero_no_borrow(); }
}
```

Only flags set by the preceding operation are meaningful. The compiler
emits conditional branches after the instruction:

```asm
    ADD total, amount
    JNC .no_cf
    ; handle_unsigned_overflow body
.no_cf:
    JNO .no_of
    ; handle_signed_overflow body
.no_of:
```

Single-flag shorthand:

```
count := count - 1 {
    CF: { underflow(); }
}
```

#### trap

Instead of an inline handler, `trap` fires an interrupt:

```
total := total + amount {
    OF: trap;           // emits INTO (INT 4)
}
```

For OF, `trap` emits the INTO instruction (INT 4, one byte). For CF
and AF, `trap` emits an explicit conditional INT — the programmer must
have an interrupt handler installed for the corresponding vector.

### if / else

```
if (condition) {
    // ...
} else if (other) {
    // ...
} else {
    // ...
}
```

Braces are required.

### while

```
while (condition) {
    // ...
}
```

### for (countdown)

A loop form that maps to the LOOP instruction:

```
for (CX in 100..0) {
    // body executes with CX = 100, 99, ..., 1
    // compiles to: MOV CX, 100 / .top: / body / LOOP .top
}
```

The loop variable must be `CX` (pinned to CX). The range must count
down to 0. This is the only for loop — general iteration uses `while`.

### break / continue

`break` exits the innermost loop. `continue` jumps to the next iteration
(the LOOP/condition check).


## Inline Assembly

The `asm` block provides an escape hatch for instructions that don't have
a Nib expression or builtin equivalent.

```
asm { rep movsb }
asm { cli }
asm { sti }
asm { out 0x50, al }
asm { hlt }
```

Inside `asm` blocks, register names refer to the actual hardware registers.
Variables pinned to registers are accessible by their register name.

The programmer must declare which registers the block affects using
either `clobbers` or `preserves` (not both). The compiler does not
attempt to analyze the instructions automatically.

```
asm clobbers(AX, DI, CX, FLAGS) {
    rep stosb
}
```

Or equivalently, listing what's untouched:

```
asm preserves(BX, DX, SI, BP, DS, ES, SS) {
    rep stosb
}
```

Use whichever produces the shorter list. Specifying both on the same
block is a compile error.

An `asm` block with neither clause is treated as clobbering everything
— the compiler saves and restores all live registers around it. This
is safe but pessimistic.

Multi-line:

```
asm {
    cli
    mov ax, 0x1234
    out 0x50, al
    sti
}
```

### Syntax

Inline assembly uses Intel syntax (destination first):

```
asm { mov ax, [bx+si+4] }      // Intel: dst, src
```

### V20 extension mnemonics

Standard V20 extension mnemonics are used as-is, except for INS
which conflicts with the Intel port string input instruction
(INSB/INSW). V20 bit field instructions use `bext`/`bins`:

| Mnemonic | Instruction | Description |
|----------|-------------|-------------|
| `bext`   | EXT (0F 33) | Bit field extract from DS:SI into AX |
| `bins`   | INS (0F 31) | Bit field insert from AX into ES:DI |
| `test1`  | TEST1       | Test single bit |
| `set1`   | SET1        | Set single bit |
| `clr1`   | CLR1        | Clear single bit |
| `not1`   | NOT1        | Toggle single bit |
| `add4s`  | ADD4S       | Add packed BCD strings |
| `sub4s`  | SUB4S       | Subtract packed BCD strings |
| `cmp4s`  | CMP4S       | Compare packed BCD strings |
| `rol4`   | ROL4        | Rotate nibbles left through AL |
| `ror4`   | ROR4        | Rotate nibbles right through AL |
| `brkem`  | BRKEM       | Break to emulation (INT with 0F prefix) |

All other instructions use standard Intel 8086/80186 mnemonics.

### SEG operator

The assembler supports `SEG label` in expressions, returning the segment
value that was active when `label` was defined. The `SEG` directive sets
the current segment value for subsequent labels.

```
SEG 0xF000                  ; set segment context
rom_entry:                  ; rom_entry is in segment 0xF000
    ...

    mov ax, SEG rom_entry   ; AX = 0xF000
```


## Segments

### Implicit mode

By default, each source file produces at least one code segment and shares
a data segment. The compiler and binder set up CS and DS automatically.
Memory references without an explicit segment use DS.

This works without the programmer thinking about segments at all, subject
to the 64K-per-segment limit.

### Explicit segment control

```
seg ES = 0xB800;            // MOV AX, 0xB800; MOV ES, AX
[ES:0x0000] := 0x41;        // MOV ES:[0x0000], 0x41
```

### Far calls

Functions in a different code segment are declared `far`:

```
fn far rom_routine(arg: u16) -> u16;    // external declaration

fn main() {
    u16 result = rom_routine(42);       // emits FAR CALL
}
```


## Debug Info (.dbg)

Source-level debug information flows through the pipeline as `; @file:line`
comments. The compiler emits them in the `.nir`, the binder carries them
through to the `.asm`, and the assembler captures them and writes a `.dbg`
file mapping binary addresses to source locations.

### .dbg format

```
# nib debug info
XXXXX file.nib:NN
XXXXX file.nib:NN
```

Where `XXXXX` is a 5-digit hex linear address. One entry per source
statement boundary. The disassembler reads `.dbg` files via `-d` and
interleaves source locations in its output.

### Generating debug info

```
./nib compile source.nib              # .nir includes ; @ comments
./nibbind source.nir -o source.asm    # carries comments through
./nibasm source.asm -o source.bin -d source.dbg   # writes .dbg
./nibdis source.bin -m source.map -d source.dbg   # shows source lines
```


## Interface Files (.nif)

The interface file contains only `pub` declarations — the public API of
a module. It records, per exported function:

- **Parameters**: name, type, register pin (if specified)
- **Preserves**: which registers the function guarantees not to modify
- **Returns**: type, register pin (if specified)

The `.nif` is consumed by other modules via `use` for cross-module type
checking. The binder does not read `.nif` files — it reads `.nir` files
which contain the full IR including function bodies.

Example `.nif`:

```
; Nib interface — generated by nib compile

.fn scroll_line
.preserves AX, BX, DX, BP
.param %0, u16, "src"
.param %1, u16, "dst"
.param %2, u16, "len"
.endfn

.struct Point
    x: u16
    y: u16
.endstruct

.const SCREEN_WIDTH, 480

.global frame_count, u16
```


## Structs

### Declaration

```
struct Name {
    field: type;
    ...
}
```

### Packing

Structs are packed by default — no padding between fields. This matches
hardware reality: memory is scarce, and structs often map to on-disk
formats, ROM tables, or hardware register blocks where layout must be
exact.

```
struct FileHeader {
    u8 type;
    u16 length;         // offset 1, no padding
    u8[8] name;         // offset 3
    u16 checksum;       // offset 11
}
// sizeof = 13 bytes
```

Opt-in word alignment with `aligned`:

```
struct aligned Cursor {
    u16 x;              // offset 0
    u16 y;              // offset 2
    u8 visible;         // offset 4
    // 1 pad byte inserted
    u16 blink_rate;     // offset 6
}
// sizeof = 8 bytes
```

`aligned` inserts padding before any `u16` field at an odd offset so that
word accesses are single-cycle. On the V20 unaligned word access works
but costs an extra bus cycle.

### Struct type references

When referring to a struct type outside of its definition — in variable
declarations, parameters, return types, casts, or `as` typed pointers —
the `struct` keyword prefix is required.

```
struct Point p;                     // variable declaration
fn read(p: struct Point) -> u16;    // parameter and return type
```

Struct definitions still use `struct Name { ... }` without a prefix on
the name being defined.

### Field access

Dot syntax, compiles to base+displacement addressing:

```
fn read_header(hdr: struct FileHeader) -> u16 {
    // hdr is a reference, address in a register (say BX)
    u16 len = hdr.length;       // MOV AX, [BX+1]
    u8 first = hdr.name[0];    // MOV AL, [BX+3]
    return len;
}
```

Array fields inside structs keep their type. `hdr.name` is `u8[8]`, not
a pointer.

### Nesting

Structs can contain other structs. Offsets flatten:

```
struct Point {
    u16 x;
    u16 y;
}

struct Rect {
    Point origin;
    Point size;
}

// rect.origin.x compiles to [reg + 0]
// rect.size.y   compiles to [reg + 6]
```

### Bit fields

Bit fields pack from LSB toward MSB within each byte, matching the V20
EXT/INS bit ordering convention.

```
struct Attributes {
    readonly: bits(1);   // bit 0
    hidden: bits(1);     // bit 1
    system: bits(1);     // bit 2
    archive: bits(1);    // bit 3
    filetype: bits(4);   // bits 7..4
}
// sizeof = 1 byte
```

`bits(N)` declares a field of N bits. The type of a bit field when read
is the smallest scalar that fits: `bits(1)` through `bits(8)` yield `u8`,
`bits(9)` through `bits(16)` yield `u16`.

Reserved or padding bits use `_`:

```
struct Control {
    enable: bits(1);     // bit 0
    mode: bits(3);       // bits 3..1
    _: bits(4);          // bits 7..4, reserved
}
```

### Bit strings (multi-bit fields crossing byte boundaries)

Bit fields can span byte boundaries. The V20 EXT/INS instructions handle
this natively — they operate on arbitrary bit offsets and lengths up to
16 bits within a memory region.

```
struct PackedCoord {
    x: bits(12);         // bits 11..0 (bytes 0-1)
    y: bits(12);         // bits 23..12 (bytes 1-2, crosses boundary)
    flags: bits(8);      // bits 31..24 (byte 3)
}
// sizeof = 4 bytes, 32 bits total
```

### Instruction selection for bit access

The compiler selects instructions based on the field:

| Field shape                   | Read           | Write          |
|-------------------------------|----------------|----------------|
| `bits(1)`, any position       | TEST1          | SET1 / CLR1    |
| `bits(N)`, byte-aligned, N=8  | MOV            | MOV            |
| `bits(N)`, byte-aligned, N=16 | MOV            | MOV            |
| `bits(N)`, otherwise          | EXT            | INS            |

For EXT: source address in SI (DS segment), bit offset and length loaded
from registers or immediates, result in AX.

For INS: destination address in DI (ES segment), value from AX, bit
offset and length from registers or immediates.

### Mixed byte and bit fields

Byte-sized and bit-sized fields can coexist in a struct. Bit fields begin
packing at the bit position after the last field (byte fields contribute
8 bits each).

```
struct FileEntry {
    u8 type;             // byte 0
    u16 size;            // bytes 1-2
    attrs: bits(4);      // byte 3, bits 3..0
    hidden: bits(1);     // byte 3, bit 4
    readonly: bits(1);   // byte 3, bit 5
    _: bits(2);          // byte 3, bits 7..6
    u8[8] name;          // bytes 4-11
}
// sizeof = 12 bytes
```

### Bit toggle

Single-bit fields support toggle via `~=`:

```
attrs.hidden ~= 1;      // NOT1 [addr], bit_pos
```


## Globals

Global variables live in the data segment:

```
u16 screen_width = 480;
u8[80] input_buffer;
bcd[4] running_total = 0;
```

### Placement with `at()`

Global variables can be placed at specific far addresses using the
`at(seg:off)` modifier. The variable is not allocated in the data
segment — it refers to the given absolute address.

```
far[4] ivt at(0x0000:0x0000) = {&handler_0, &handler_1, &handler_2, &handler_3};
u8 keyboard_reg at(0x0000:0x00B0);
```

### Cross-unit access

Globals are accessible from any function in the same compilation unit.
Cross-unit access is declared with `extern`:

```
extern u16 screen_width;
extern u8[80] input_buffer;
```


## String and Character Literals

Double quotes are used for both single-byte and array literals. The type
context determines interpretation — no single-quote character literal
syntax.

```
u8 ch = "A";              // single byte: 0x41
u8[5] hello = "Hello";    // 5 bytes, exact fit, no terminator
u8[6] hello = "Hello";    // 6 bytes: "Hello" + 0x00 pad
u8[80] line = "Ready";    // 80 bytes: "Ready" + 75 zero bytes
```

### Rules

- **Exact fit**: literal length must be <= declared size.
- **Overflow is an error**: `u8[3] x = "Hello";` does not compile.
- **Short literals pad with zeros**: remaining bytes are 0x00.
- **No implicit null terminator**: size the array to include one if needed.

### Escape sequences

| Escape   | Value | Description          |
|----------|-------|----------------------|
| `\0`     | 0x00  | Null                 |
| `\n`     | 0x0A  | Newline              |
| `\r`     | 0x0D  | Carriage return      |
| `\t`     | 0x09  | Tab                  |
| `\\`     | 0x5C  | Backslash            |
| `\"`     | 0x22  | Double quote         |
| `\xNN`   | 0xNN  | Hex byte             |


## Interrupt Handlers

Interrupt handlers are declared with `fn interrupt(vector)`. They take
no parameters, return nothing, and compile to IRET instead of RET.

Unlike normal functions, interrupt handlers do not participate in the
binder's register propagation. Since interrupts are asynchronous, the
handler must save and restore every register it clobbers. The compiler
determines the minimal set automatically — no need for blanket PUSHA.

### Basic handler

```
fn interrupt(0xF8) keyboard_handler() {
    u8 AL = [0x00B0];           // read keyboard matrix
    // ... process key ...
    AL = 0x01;
    asm { out 0x90, AL }        // ack interrupt (explicit)
}
```

The compiler generates:

```asm
keyboard_handler:
    PUSH AX                     ; only clobbered registers
    ; ... body ...
    POP AX
    IRET
```

The binder writes the handler's seg:off into the IVT at
`vector * 4` (physical address 0x003E0 for vector 0xF8).

Interrupt acknowledgment is never automatic — the programmer writes
the appropriate OUT instruction for their IRQ.

### Re-entrant handlers

The `reentrant` modifier causes the compiler to emit STI in the
prologue, re-enabling maskable interrupts immediately on entry:

```
fn interrupt(0x1C) reentrant timer_tick() {
    // interrupts are enabled here
    tick_count := tick_count + 1;
}
```

Generates:

```asm
timer_tick:
    PUSH AX
    STI                         ; reentrant modifier
    ; ... body ...
    POP AX
    IRET
```

Without `reentrant`, interrupts remain disabled (IF=0, as set by the
CPU on interrupt entry) for the duration of the handler.

### Chaining (hooking existing vectors)

The `chain` clause captures the previous IVT entry as a callable
far function reference:

```
fn interrupt(0x1C, chain old_handler) my_timer() {
    // do our work
    tick_count := tick_count + 1;

    old_handler();              // far call to previous handler
}
```

The binder:
1. Reads the existing seg:off at the vector before installing.
2. Stores it as a far pointer in the data segment.
3. `old_handler()` compiles to a far call through that pointer.

The chain call can appear anywhere in the body — before the handler's
own logic, after it, or conditionally:

```
fn interrupt(0x09, chain prev) keyboard_hook() {
    u8 AL = [0x0060];           // read scancode
    if (AL == 0x3B) {           // F1 key
        // handle it ourselves
    } else {
        prev();                 // let original handler deal with it
    }
}
```

If `chain` is omitted, the old vector is overwritten. There is no
implicit way to recover it.

### Combining modifiers

```
fn interrupt(0x08, chain old) reentrant system_timer() {
    old();                      // chain first
    // our work with interrupts enabled
}
```

### Constraints

- `interrupt` functions cannot have parameters or return types.
- `interrupt` functions cannot be called directly from Nib code.
- The vector number must be a constant (0x00-0xFF).
- `chain` names are scoped to the function body.


## Modules

### use

The `use` directive imports a module's interface (.nif file) so its
functions can be called from the current compilation unit.

```
use "lcd.nif";
use "../drivers/keyboard.nif";
```

The path is a string literal, resolved relative to the source file's
directory. No implicit search paths.

After `use`, the module's functions are available by their names:

```
use "lcd.nif";

fn main() {
    lcd_clear();
    lcd_putchar(0x41);
}
```

The compiler reads the .nif to type-check calls and record call graph
edges. It does not read the .nir (implementation).

### Compiler outputs

Each source file compiles to two files:

- **`.nir`** (Nib IR): Pseudo-assembly with virtual registers and
  metadata directives (`.fn`, `.param`, `.prefer`, `.calls`, etc).
  Consumed by the binder.
- **`.nif`** (Nib interface): `pub` function signatures, struct layouts,
  constants, and extern declarations. Consumed by `use` in other source
  files for cross-module type checking.

### Pipeline

```
source.nib + deps.nif  →  [nib]      →  .nir + .nif
all .nir files         →  [nibbind]  →  .asm
.asm                   →  [nibasm]   →  .bin + .map + .dbg
```


## Unresolved / Future

- **Preprocessor / macros**: Not planned unless needed.
