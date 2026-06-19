# Nib TODO

## Hardware Investigation
- Check if any V20 extension instructions expose TA/TB/TC/LC internal registers
- Determine if power button NMI is usable as debug break trigger

## Toolchain
- Peephole optimizer: post-binder pass to eliminate self-moves, fold mov+op sequences
- Better error messages with source context
- Binder allocator next pass:
  - Use the reports to target high-pressure Serif functions such as RTC
    read/write paths and framebuffer blits before changing heuristics.
  - Widen stack-cache spill planning from single-block exact LIFO spans to
    full CFG state propagation:
    - compute stack-cache entry/exit state per basic block,
    - require identical state at merge points,
    - require loop back edges to preserve stack state,
    - allow balanced call argument pushes above cached values,
    - fall back to frame spills when states cannot be proven identical.
  - Continue sibling-byte lifetime coalescing:
    - done: opposite byte shifts from one source get allocator affinities
      for sibling low/high halves, so non-special cases naturally prefer
      `DL`/`DH`-style placement;
    - remaining: when two overlapping byte vregs occupy sibling halves of
      one parent word, extend and spill them as one word lifetime so frame
      spill/load traffic can be combined across byte-heavy code.
  - Add byte stack-cache eligibility only after the planner can prove the
    parent word is free at the restore point and the use-site instruction
    does not need the sibling byte at the same time.
  - Add rematerialization locations for constants and labels so cheap
    values do not require frame spill homes.

## Nice to have
- Struct array type syntax (`struct Name[N]` in generic contexts)
- Nested struct initializers
