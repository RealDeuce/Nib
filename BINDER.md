# Binder Design

The binder reads `.nir` files, resolves virtual registers to physical
registers, inserts spills/saves, and outputs `.asm`.

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
- List of vregs with types and preferences
- IR instruction list
- Control flow graph (basic blocks from labels/jumps)

### Phase 2: Liveness analysis

For each function, compute live-in and live-out sets for each
basic block using standard backward dataflow:
- A vreg is live at a point if there exists a path from that
  point to a use of the vreg that doesn't pass through a def.
- Iterate until fixed point.

From the block-level liveness, compute per-instruction live
ranges for each vreg.

### Phase 3: Build interference graph

Two vregs interfere if their live ranges overlap. Add edges:
- vreg-vreg: live ranges overlap
- alias: word vreg interferes with all byte vregs from same
  register, and vice versa (added after coloring attempt)

### Phase 4: Constrain

For each vreg, compute the set of allowed physical registers:
- Start with the full class (word or byte)
- Restrict based on instruction constraints (MUL → AX, etc.)
- Fix pre-colored vregs (from .prefer directives)

### Phase 5: Color

Graph coloring with pre-colored nodes:
1. Build worklist of unconstrained vregs, sorted by degree
   (most constrained first — most interference neighbors)
2. Try to assign a color (physical register) to each vreg
   that doesn't conflict with neighbors or aliases
3. If a vreg can't be colored, mark it for spilling

### Phase 6: Spill

For each spilled vreg:
- Allocate a stack slot
- Insert a store after each def
- Insert a load before each use
- Re-run liveness/coloring (the spill loads/stores create
  short-lived vregs that are easy to color)

Repeat until everything is colored.

### Phase 7: Inter-procedural resolution

After local allocation, resolve cross-function register
assignments:
- For each call site, compare the caller's register assignments
  for the arguments with the callee's parameter assignments
- Insert MOV instructions at call sites where they disagree
- Insert PUSH/POP for caller-saved registers that are live
  across the call and clobbered by the callee

### Phase 8: Emit .asm

Walk the IR, replacing virtual registers with physical register
names. Emit:
- Function prologue (PUSH BP if frame pointer used)
- Spill slot allocation (SUB SP, N)
- IR instructions with real registers
- Spill loads/stores
- Call-site saves/restores
- Function epilogue
- Inline asm blocks (verbatim)

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

BP is used as frame pointer only when the function has spills
or local arrays. If no spills, BP is available for allocation.

**Important**: SP cannot appear in any V20 memory addressing mode.
There is no `[SP+N]` — only `[BP+N]`. So any function that needs
stack-relative access (spills, local arrays) MUST reserve BP as
frame pointer, reducing allocatable word registers from 7 to 6.

The allocator handles this as a two-pass decision:
1. First attempt: 7 registers (BP available)
2. If spills result, reserve BP and re-allocate with 6 registers
3. Second pass is final — BP is committed as frame pointer

## Spill cost model

When choosing which vreg to spill, prefer:
1. Vregs with long live ranges (reduces pressure the most)
2. Vregs with few uses (fewer load/store insertions)
3. Vregs that aren't pre-colored (avoid spilling pinned regs)

Cost = uses / live_range_length (lower = better spill candidate)
