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

### Phase 4: Machine constraint collection

Scan every instruction into a compact machine-constraint record. This
centralizes hardware requirements before allocation:

- instruction clobbers such as accumulator and port I/O effects
- hard register requirements such as shift counts in CL
- addressability requirements for memory base and index operands
- register affinities for operands that are faster or simpler in a
  particular physical register

Vregs used in memory operands are marked `needs_addressable`,
restricting them to {BX, BP, SI, DI}. Base-only operands are restricted
to {BX, BP}; index-only operands are restricted to {SI, DI}. Hard
constraints override ordinary preferences.

### Phase 5: CFG and liveness

Build a control flow graph: split IR into basic blocks at labels and
jumps, connect successor/predecessor edges. Run standard backward
dataflow liveness to fixpoint — loops are handled naturally without
special-case extensions. Parameters are live-in at the entry block.

From the live sets, build an interference graph: two vregs that are
simultaneously live at any instruction point get an edge. MOV
instructions get a coalescing exception (src and dst don't interfere,
enabling future move coalescing).

### Phase 6: Register allocation (Chaitin-Briggs)

1. **Pressure estimate**: if any vreg's interference degree exceeds
   its pool size, reserve BP for the frame pointer upfront
2. **Pre-color**: vregs with `.prefer` or `in REG` pins get their
   assigned register, respecting addressing constraints
3. **Simplify**: repeatedly remove vregs with degree < pool_size
   from the graph, pushing onto a stack
4. **Potential spill**: when stuck, push the vreg with lowest spill
   cost. Uses are weighted heavily, loop values are expensive to spill,
   consts are cheaper to rematerialize, fixed registers are very
   expensive, preferred registers are somewhat expensive, and very long
   low-use ranges are cheaper.
5. **Select**: pop stack and assign colors with a shared color chooser.
   Explicit preferences dominate, machine affinities rank the remaining
   valid colors, and pool order is the final speed-first tie-breaker. If
   no color fits, actually spill to a stack slot.

### Phase 7: Move insertion

Post-allocation pass that builds a **resolved instruction stream**.
Uses `free_regs_at()` to query which physical registers are free
at each instruction point (from CFG liveness + allocation results).

Handles three fixup categories via explicit moves instead of
push/pop sequences:

- **CL routing**: variable shift counts not in CL get an explicit
  `mov CL, src` inserted. Uses a free register when CX is occupied.
- **Call arguments**: mismatched caller/callee register assignments
  get explicit movs. Includes caller-save push/pop and BP save.
- **Address registers**: `[ES:SI]` operands where `.prefer SI`
  failed get an explicit `mov SI, actual_reg` inserted.

### Phase 8: Emit .asm

Walk the resolved stream, outputting pre-formatted moves (RINS_ASM)
and lowering IR instructions (RINS_IR) to V20 assembly:

- Function label and prologue (PUSHA for interrupts, frame setup)
- Callee-save PUSH for registers in the `preserves` list that are used
- Hardware workarounds in named helpers:
  - `emit_alu()`: IN/OUT accumulator routing, far.off/far.seg spill
    handling, byte IMUL, spill-to-spill ALU, three-address lowering
  - `emit_load()`: spilled base/index via BX/SI scratch, CS: prefix
  - `emit_store()`: spilled value/base/index handling
  - `emit_mov()`: byte/word conversion, seg-to-seg, spill-to-spill
- CMP+Jcc: finds preceding CMP and emits correct conditional jump
- Calls: `call label` (near) or `call far seg:off` (extern)
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
