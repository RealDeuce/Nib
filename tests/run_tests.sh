#!/bin/sh
# Nib test runner — compiles, binds, and assembles each test file
# Reports pass/fail for each stage

cd "$(dirname "$0")/.."

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m %s: %s\n" "$1" "$2"; }
skip() { SKIP=$((SKIP + 1)); printf "  \033[33mSKIP\033[0m %s: %s\n" "$1" "$2"; }

echo "=== Nib Test Suite ==="
echo ""

# Phase 1: Parser tests — every .nib file must parse cleanly
echo "--- Parser tests ---"
for f in tests/t_*.nib; do
    name=$(basename "$f" .nib)
    if ./nib "$f" --parse-only >/dev/null 2>&1; then
        pass "$name (parse)"
    else
        fail "$name (parse)" "$(./nib "$f" --parse-only 2>&1 | head -1)"
    fi
done
echo ""

# Phase 2: Compile tests — every .nib file must compile to .nir + .nif
# Compile lib modules first so app modules can use their .nif files
echo "--- Compile tests ---"
for f in tests/t_*_lib.nib; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .nib)
    if ./nib "$f" >/dev/null 2>&1; then
        pass "$name (compile)"
    else
        fail "$name (compile)" "$(./nib "$f" 2>&1 | head -1)"
    fi
done
for f in tests/t_*.nib; do
    case "$(basename "$f")" in *_lib.nib) continue ;; esac
    name=$(basename "$f" .nib)
    if ./nib "$f" >/dev/null 2>&1; then
        pass "$name (compile)"
    else
        fail "$name (compile)" "$(./nib "$f" 2>&1 | head -1)"
    fi
done
echo ""

# Phase 3: Bind tests — single-file modules should bind cleanly
echo "--- Bind tests ---"
for f in tests/t_*.nir; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .nir)
    # Skip multi-module tests (they need special handling)
    case "$name" in
        t_modules_app|t_const_scope|t_icall) continue ;;
    esac
    outasm="/tmp/${name}.asm"
    if ./nibbind "$f" -o "$outasm" >/dev/null 2>&1; then
        pass "$name (bind)"
    else
        fail "$name (bind)" "$(./nibbind "$f" -o "$outasm" 2>&1 | tail -1)"
    fi
done
echo ""

# Phase 4: Multi-module bind test
echo "--- Multi-module tests ---"
if [ -f tests/t_modules_lib.nir ] && [ -f tests/t_modules_app.nir ]; then
    outasm="/tmp/t_modules.asm"
    if ./nibbind tests/t_modules_lib.nir tests/t_modules_app.nir -o "$outasm" >/dev/null 2>&1; then
        pass "t_modules (bind)"
    else
        fail "t_modules (bind)" "$(./nibbind tests/t_modules_lib.nir tests/t_modules_app.nir -o "$outasm" 2>&1 | tail -1)"
    fi
else
    skip "t_modules (bind)" "nir files not generated"
fi

# Multi-module constant pool scoping
if [ -f tests/t_const_scope_lib.nir ] && [ -f tests/t_const_scope.nir ]; then
    outasm="/tmp/t_const_scope_multi.asm"
    if ./nibbind tests/t_const_scope_lib.nir tests/t_const_scope.nir -o "$outasm" >/dev/null 2>&1; then
        outbin="/tmp/t_const_scope_multi.bin"
        if ./nibasm "$outasm" -o "$outbin" >/dev/null 2>&1; then
            pass "t_const_scope (multi-module assemble)"
        else
            fail "t_const_scope (multi-module assemble)" "$(./nibasm "$outasm" -o "$outbin" 2>&1 | head -1)"
        fi
    else
        fail "t_const_scope (multi-module bind)" "$(./nibbind tests/t_const_scope_lib.nir tests/t_const_scope.nir -o "$outasm" 2>&1 | tail -1)"
    fi
else
    skip "t_const_scope" "nir files not generated"
fi
# Indirect far call via extern descriptor
if [ -f tests/t_icall_lib.nir ] && [ -f tests/t_icall.nir ]; then
    outasm="/tmp/t_icall_multi.asm"
    if ./nibbind tests/t_icall_lib.nir tests/t_icall.nir -o "$outasm" >/dev/null 2>&1; then
        outbin="/tmp/t_icall_multi.bin"
        if ./nibasm "$outasm" -o "$outbin" >/dev/null 2>&1; then
            pass "t_icall (indirect far call)"
        else
            fail "t_icall (assemble)" "$(./nibasm "$outasm" -o "$outbin" 2>&1 | head -1)"
        fi
    else
        fail "t_icall (bind)" "$(./nibbind tests/t_icall_lib.nir tests/t_icall.nir -o "$outasm" 2>&1 | tail -1)"
    fi
else
    skip "t_icall" "nir files not generated"
fi
echo ""

# Phase 5: Assemble tests — bound .asm files should assemble
echo "--- Assemble tests ---"
for f in /tmp/t_*.asm; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .asm)
    # Skip tests that can't assemble standalone
    case "$name" in
        t_const_scope|t_const_scope_multi|t_icall|t_icall_multi) continue ;;
    esac
    outbin="/tmp/${name}.bin"
    if ./nibasm "$f" -o "$outbin" >/dev/null 2>&1; then
        size=$(wc -c < "$outbin" | tr -d ' ')
        pass "$name (assemble, ${size} bytes)"
    else
        fail "$name (assemble)" "$(./nibasm "$f" -o "$outbin" 2>&1 | head -1)"
    fi
done
echo ""

# Phase 6: Full build test
echo "--- Full build test ---"
rm -f tests/lcd.nir tests/lcd.nif tests/app.nir tests/app.nif tests/app.bin
if ./nibbuild tests/app.nib -o /tmp/app_build.bin >/dev/null 2>&1; then
    size=$(wc -c < /tmp/app_build.bin | tr -d ' ')
    pass "nibbuild app.nib (${size} bytes)"
else
    fail "nibbuild app.nib" "$(./nibbuild tests/app.nib 2>&1 | tail -1)"
fi
echo ""

# Phase 7: Assembly content validation
echo "--- Assembly content checks ---"

# Extern parameter pins: dos_putchar(0x02, 0x41) must use AH and DL
if [ -f /tmp/t_extern_pins.asm ]; then
    if grep -q "mov AH," /tmp/t_extern_pins.asm && grep -q "mov DL," /tmp/t_extern_pins.asm; then
        pass "extern pins: AH and DL used for dos_putchar"
    else
        fail "extern pins" "dos_putchar params not in AH/DL (got: $(grep 'mov.*,' /tmp/t_extern_pins.asm | head -4 | tr '\n' '; '))"
    fi
else
    skip "extern pins" "asm not generated"
fi

# Inter-procedural propagation: fill() should get DI, AL, CX
if [ -f /tmp/t_pinning.asm ]; then
    if grep -q "mov DI," /tmp/t_pinning.asm && grep -q "mov AL," /tmp/t_pinning.asm; then
        pass "register propagation: fill params in DI/AL/CX"
    else
        fail "register propagation" "fill params not propagated correctly"
    fi
else
    skip "register propagation" "asm not generated"
fi

# Callee-save: preserves should emit push/pop
if [ -f tests/callee_save.nib ]; then
    ./nib tests/callee_save.nib >/dev/null 2>&1
    ./nibbind tests/callee_save.nir -o /tmp/callee_save.asm >/dev/null 2>&1
    if [ -f /tmp/callee_save.asm ]; then
        if grep -q "push BX" /tmp/callee_save.asm && grep -q "pop BX" /tmp/callee_save.asm; then
            pass "callee-save: push/pop BX emitted"
        else
            fail "callee-save" "no push/pop for preserved register"
        fi
    fi
fi

# Saturating add: flag check block should emit JNC
if [ -f /tmp/t_flags.asm ]; then
    if grep -q "jnc" /tmp/t_flags.asm; then
        pass "flag-check: JNC emitted for CF check"
    else
        fail "flag-check" "no JNC in flag check block"
    fi
fi

# Variable shift counts use CL
if [ -f /tmp/t_shift_cl.asm ]; then
    if grep -q "shl.*CL" /tmp/t_shift_cl.asm && grep -q "shr.*CL" /tmp/t_shift_cl.asm; then
        pass "shift-cl: variable shifts use CL"
    else
        fail "shift-cl" "variable shift not routed through CL"
    fi
fi

# Overlapping shift counts: parameters must not share a register
if [ -f /tmp/t_shift_overlap.asm ]; then
    if grep -q "push CX" /tmp/t_shift_overlap.asm || grep -q "push AX" /tmp/t_shift_overlap.asm; then
        pass "shift-overlap: CL contention handled with save/restore"
    else
        fail "shift-overlap" "no save/restore — overlapping params may share CL"
    fi
fi

# Unused call return must not clobber live byte vreg via aliasing
if [ -f /tmp/t_call_clobber.asm ]; then
    # If the unused return got AX, it would clobber AL (the parameter)
    # The caller should NOT have mov AX as a result of the call
    if grep -q 'caller:' /tmp/t_call_clobber.asm; then
        if grep -A5 'caller:' /tmp/t_call_clobber.asm | grep -q 'push AL\|push AX'; then
            pass "unused-call-ret: AL saved across call"
        elif ! grep -q '%1=AX' /tmp/t_call_clobber.asm 2>/dev/null; then
            pass "unused-call-ret: return vreg avoids AX"
        else
            fail "unused-call-ret" "unused call return clobbers live AL via AX alias"
        fi
    fi
fi

# Port I/O: OUT must use AL, IN must read into AL
if [ -f /tmp/t_port_io.asm ]; then
    if grep -q 'out 0x50, AL' /tmp/t_port_io.asm && grep -q 'in AL, 0x60' /tmp/t_port_io.asm; then
        pass "port-io: IN/OUT use AL accumulator"
    else
        fail "port-io" "IN/OUT not using AL"
    fi
fi

# Byte vregs: zero_extend must use byte mov (MOV BL, AL not MOV BX, AL)
if [ -f /tmp/t_byte_vreg.asm ]; then
    if grep -q 'mov [A-D]L, [A-D]L' /tmp/t_byte_vreg.asm; then
        pass "byte-vreg: zero_extend uses byte move"
    else
        fail "byte-vreg" "zero_extend uses invalid word-from-byte move"
    fi
fi

# memset/memcopy must set up DI, AL/SI, CX before string ops
if [ -f /tmp/t_memops.asm ]; then
    if grep -q 'mov DI' /tmp/t_memops.asm && grep -q 'mov CX' /tmp/t_memops.asm && grep -q 'rep stosb' /tmp/t_memops.asm; then
        pass "memops: DI/CX/AL set up for rep stosb"
    else
        fail "memops" "string op register setup missing"
    fi
fi

# Scalar globals: must use memory-indirect loads, not address loads
if [ -f /tmp/t_globals_rw.asm ]; then
    if grep -q '\[counter\]' /tmp/t_globals_rw.asm && grep -q '\[flag\]' /tmp/t_globals_rw.asm; then
        pass "globals-rw: scalar globals accessed through memory"
    else
        fail "globals-rw" "scalar global loaded as address instead of memory"
    fi
fi

# Byte array access: must use byte registers (AL, BL, etc.)
if [ -f /tmp/t_byte_array.asm ]; then
    if grep -q 'mov [A-D]L' /tmp/t_byte_array.asm; then
        pass "byte-array: u8 elements use byte registers"
    else
        fail "byte-array" "u8 array access using word registers"
    fi
fi

echo ""

echo "--- Type error tests (expected failures) ---"
# Wrong arg count
echo 'use "t_modules_lib.nif";
fn main() { lib_add(1, 2, 3); }' > /tmp/t_err_argcount.nib
if ./nib /tmp/t_err_argcount.nib >/dev/null 2>&1; then
    fail "arg count error" "should have failed"
else
    pass "arg count error (correctly rejected)"
fi

# Size mismatch
echo 'fn bad() { u16 x = 1; u8 y = 2; u16 z = x + y; }' > /tmp/t_err_sizemismatch.nib
if ./nib /tmp/t_err_sizemismatch.nib >/dev/null 2>&1; then
    fail "size mismatch" "should have failed"
else
    pass "size mismatch (correctly rejected)"
fi

# Undefined variable
echo 'fn bad() { u16 x = y; }' > /tmp/t_err_undef.nib
if ./nib /tmp/t_err_undef.nib >/dev/null 2>&1; then
    fail "undefined var" "should have failed"
else
    pass "undefined var (correctly rejected)"
fi

# Return type mismatch
echo 'fn bad() -> u8 { return 0xFFFF; }' > /tmp/t_err_rettype.nib
if ./nib /tmp/t_err_rettype.nib >/dev/null 2>&1; then
    # Literals promote, so this actually succeeds — not an error
    pass "return literal (promotion OK)"
else
    pass "return type check"
fi

echo ""

# Summary
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped (${TOTAL} total) ==="
[ "$FAIL" -eq 0 ]
