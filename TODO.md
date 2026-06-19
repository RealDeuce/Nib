# Nib TODO

## Hardware Investigation
- Trace pins between gate array and V20 CPU — does the gate array use LOCK#?
- Check if any V20 extension instructions expose TA/TB/TC/LC internal registers
- Determine if power button NMI is usable as debug break trigger

## Toolchain
- Peephole optimizer: post-binder pass to eliminate self-moves, fold mov+op sequences
- Better error messages with source context
- Binder: emit at() functions last to avoid org/position issues with subsequent code
- Replace fixed-size compiler/binder tables with a generic growable
  table/vector helper so capacity failures are rare, explicit, and
  handled consistently across functions, globals, data blocks, externs,
  use directives, labels, and fixup lists.
- Binder allocator next pass:
  - Teach pressure reports to compare before/after allocator decisions
    across revisions: selected colors, actual spills, fixed-register
    pressure, inserted fixups, and spill actions.
  - Use the reports to target high-pressure Serif functions such as RTC
    read/write paths and framebuffer blits before changing heuristics.

## Nice to have
- Struct array type syntax (`struct Name[N]` in generic contexts)
- Nested struct initializers
