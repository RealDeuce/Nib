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
  - Add rematerialization locations for constants and labels so cheap
    values do not require frame spill homes.

## Nice to have
- Struct array type syntax (`struct Name[N]` in generic contexts)
- Nested struct initializers
