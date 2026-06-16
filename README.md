# Nib

[![CI](https://github.com/RealDeuce/Nib/actions/workflows/ci.yml/badge.svg)](https://github.com/RealDeuce/Nib/actions/workflows/ci.yml)

A compiled language for the NEC V20HL processor in the
[DreamWriter](https://en.wikipedia.org/wiki/NTS_DreamWriter) portable
word processor.

Nib sits between C and assembly — C-like syntax with automatic
whole-program register allocation, but compilation is transparent and
predictable. You can reason about what assembly gets emitted.

## Quick start

```sh
make                                # build the toolchain
./nibbuild app.nib                  # compile + bind + assemble
./nibdis -m app.map -d app.dbg app.bin   # disassemble with source lines
```

## What it looks like

```nib
const PORT_LCD = 0x60;

pub fn api_read(port: u16 in DX) -> u8 in AL clobbers(FLAGS) {
    u8 AL = port_in(port);
    return AL;
}

fn interrupt(0x1C, chain old_handler) reentrant timer() {
    tick_count := tick_count + 1;
    old_handler();
}

far[256] ivt at(0x0000:0x0000) = {&handler0, &handler1};
```

## Documentation

- [SPEC.md](SPEC.md) — Language specification
- [USAGE.md](USAGE.md) — Command-line reference
- [BINDER.md](BINDER.md) — Register allocator design
- [V20_INSTRUCTIONS.md](V20_INSTRUCTIONS.md) — nibasm instruction reference

## Toolchain

```
source.nib  --[nib]-->      .nir + .nif
*.nir       --[nibbind]-->  .asm
.asm        --[nibasm]-->   .bin + .map + .dbg
```

`nibbuild` runs all three stages, following `use` chains automatically.

## Building

Requires bison 3.8+, flex, and a C/C++ compiler. Tested on FreeBSD,
Linux, and macOS.

```sh
make
make CC=gcc14   # or any gcc/clang
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
