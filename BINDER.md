# Binder Design

The binder reads `.nir` files, resolves virtual registers to physical
registers across the whole program, inserts spills/saves, and outputs
`.asm` with real register names.

## Register Model

### Allocatable registers

| Register | Class | Aliases | Notes |
|----------|-------|---------|-------|
| AX | word | AL, AH | MUL/DIV implicit |
| BX | word, base | BL, BH | memory base |
| CX | word | CL, CH | LOOP, shift count |
| DX | word | DL, DH | MUL/DIV implicit, I/O port |
| SI | word, index | — | string source |
| DI | word, index | — | string dest |
| BP | word, base | — | frame pointer (when used) |

SP is never allocatable. Segment registers (DS, ES, SS, CS) are
managed separately — not part of the general allocation.

### Register classes

A vreg's class is determined by how it's used in the IR:

| Usage | Allowed registers |
|-------|-------------------|
| General word | AX, BX, CX, DX, SI, DI, BP |
| General byte | AL, AH, BL, BH, CL, CH, DL, DH |
| Memory base | BX, BP |
| Memory index | SI, DI |
| Addressable (LOAD/STORE) | BX, BP, SI, DI |
| Shift count | CL |
| Loop counter | CX |
| MUL/DIV input | AX |
| MUL/DIV high | DX |
| String source | SI |
| String dest | DI |

### Alias interference

When a word register is live, its byte halves are unavailable:
- AX live → AL, AH unavailable
- BX live → BL, BH unavailable
- CX live → CL, CH unavailable
- DX live → DL, DH unavailable

Conversely, AL or AH live → AX unavailable.

## Algorithm

### Phase 1: Parse IR

Read all `.nir` files. Build per-function:
- List of vregs with types, preferences, and pin constraints
- IR instruction list with source line debug info
- Extern function declarations with parameter pins

Also parse:
- `.data` blocks (initialized globals with placement)
- `.const` entries (string literals, far references)

### Phase 2: Call graph and topological sort

Build a call graph from `call` instructions. Topologically sort
functions so leaves are resolved first — their register choices
propagate upward to callers.

### Phase 3: Inter-procedural preference propagation

Bottom-up from leaf functions:
1. Each function's parameter vregs get preferences from how they're
   used (string ops → SI/DI, etc.)
2. These preferences propagate to the caller's argument vregs at
   each call site
3. Extern function parameter pins are pre-propagated before the
   main topological pass

This is the key innovation: register assignments chosen by leaf
functions ripple up through the entire call graph, so callers place
values in the right registers from the start.

### Phase 4: Addressing constraint scan

Vregs used in LOAD/STORE instructions (memory operands) are marked
`needs_addressable`, restricting them to {BX, BP, SI, DI}. Pre-colored
preferences that violate addressing constraints are overridden.

### Phase 5: Liveness analysis

Simple linear liveness — no CFG, no SSA. Forward scan finds first def,
backward scan finds last use. Live range is the interval [def, last_use].
Loop-aware extension: backward jumps (loops) extend the live range of
vregs defined before the loop and used inside it to the loop back-edge.
This is conservative but correct for reducible control flow.

### Phase 6: Register allocation

Linear scan with pre-coloring (not graph coloring — no interference
graph is built; conflicts are checked pairwise during assignment):
1. Pre-colored vregs (from `.prefer` or `in REG` pins) get their
   assigned register first
2. Remaining vregs are allocated in order, checking for conflicts
   with already-assigned vregs (overlap + alias interference)
3. Vregs that can't be colored are spilled to stack slots

Two-pass BP reservation: first attempt uses 7 registers (BP available).
If any spills result, BP is reserved as frame pointer and allocation
re-runs with 6 registers.

### Phase 7: Emit .asm

Walk the IR, replacing virtual registers with physical register names:
- Function label and prologue (PUSHA for interrupts, frame setup)
- Callee-save PUSH for registers in the `preserves` list that are used
- IR instructions lowered to V20 assembly:
  - ALU: `op dst, src` with MOV for three-address → two-address
  - CMP+Jcc: finds preceding CMP and emits correct conditional jump
  - Calls: `call label` (near) or `call far seg:off` (extern)
  - Chain calls: `pushf` + `call far [chain_addr]`
  - Inline asm: spliced through verbatim
- Callee-save POP in reverse order
- Function epilogue (IRET for interrupts, RET for normal)
- `; @file:line` debug comments carried through for `.dbg` generation

After all functions:
- Constant pool (string literals, far references with SEG operator)
- Data blocks (initialized globals with `seg`/`org` placement)

## Stack frame layout

```
[BP+N]   parameters (if passed on stack)
[BP+4]   return address (far) or [BP+2] (near)
[BP+0]   saved BP (if frame pointer used)
[BP-2]   first spill slot
[BP-4]   second spill slot
...
[SP]     current stack top
```

BP is used as frame pointer only when the function has spills.
If no spills, BP is available for allocation.

**Important**: SP cannot appear in any V20 memory addressing mode.
There is no `[SP+N]` — only `[BP+N]`. So any function that needs
stack-relative access (spills) MUST reserve BP as frame pointer,
reducing allocatable word registers from 7 to 6.

## Return register assignment

Default return register is AX (word) or AL (byte). Functions with
`-> type in REG` override this with a pinned return register. The
binder propagates the return register preference to callers so the
call site can place the result directly.
