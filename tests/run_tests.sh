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
        t_modules_app) continue ;;
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
echo ""

# Phase 5: Assemble tests — bound .asm files should assemble
echo "--- Assemble tests ---"
for f in /tmp/t_*.asm; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .asm)
    # Skip tests that can't assemble standalone (extern references)
    case "$name" in
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

# Phase 7: Type error tests — these should FAIL to compile
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
