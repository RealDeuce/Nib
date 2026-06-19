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
  - Replace use-count spill-cost proxies with V20 clock-cost estimates
    for rematerialization, reload/store, push/pop, and scratch routes.
  - Widen stack-cache spill planning from single-block exact LIFO spans to
    full CFG state propagation:
    - compute stack-cache entry/exit state per basic block,
    - require identical state at merge points,
    - require loop back edges to preserve stack state,
    - allow balanced call argument pushes above cached values,
    - fall back to frame spills when states cannot be proven identical.
  - Add stack-cache eligibility for byte values whose parent word is owned
    or whose sibling byte is dead.
  - Add rematerialization locations for constants and labels so cheap
    values do not require frame spill homes.

## Nice to have
- Struct array type syntax (`struct Name[N]` in generic contexts)
- Nested struct initializers
