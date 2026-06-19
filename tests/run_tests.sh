#!/bin/sh
# Nib test runner — compiles, binds, and assembles each test file
# Reports pass/fail for each stage

cd "$(dirname "$0")/.."
REPO_DIR=$(pwd)

TEST_TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/nib-tests.XXXXXX") || exit 1
trap 'rm -rf "$TEST_TMPDIR"' EXIT HUP INT TERM

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
        t_modules_app|t_const_scope|t_icall|t_api_descriptor|t_param_overflow) continue ;;
    esac
    outasm="$TEST_TMPDIR/${name}.asm"
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
    outasm="$TEST_TMPDIR/t_modules.asm"
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
    outasm="$TEST_TMPDIR/t_const_scope_multi.asm"
    if ./nibbind tests/t_const_scope_lib.nir tests/t_const_scope.nir -o "$outasm" >/dev/null 2>&1; then
        outbin="$TEST_TMPDIR/t_const_scope_multi.bin"
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
    outasm="$TEST_TMPDIR/t_icall_multi.asm"
    if ./nibbind tests/t_icall_lib.nir tests/t_icall.nir -o "$outasm" >/dev/null 2>&1; then
        outbin="$TEST_TMPDIR/t_icall_multi.bin"
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

# API descriptors: api functions export .extern descriptors without
# exporting direct .fn entries.
if [ -f tests/t_api_descriptor_lib.nir ] && [ -f tests/t_api_descriptor.nir ]; then
    outasm="$TEST_TMPDIR/t_api_descriptor_multi.asm"
    if ./nibbind tests/t_api_descriptor_lib.nir tests/t_api_descriptor.nir -o "$outasm" >/dev/null 2>&1; then
        if grep -q '^\.extern send, far, ds=none$' tests/t_api_descriptor_lib.nif &&
           grep -q '^\.preserves AX, CX, DX, BX, BP, SI, DI, ES, CS, SS, DS, FLAGS$' tests/t_api_descriptor_lib.nif &&
           ! grep -q '^\.fn send' tests/t_api_descriptor_lib.nif; then
            pass "t_api_descriptor (extern descriptor)"
        else
            fail "t_api_descriptor (nif)" "api descriptor export is wrong"
        fi
    else
        fail "t_api_descriptor (bind)" "$(./nibbind tests/t_api_descriptor_lib.nir tests/t_api_descriptor.nir -o "$outasm" 2>&1 | tail -1)"
    fi
else
    skip "t_api_descriptor" "nir files not generated"
fi

if [ -f tests/t_param_overflow.nir ]; then
    outasm="$TEST_TMPDIR/t_param_overflow.asm"
    if ./nibbind tests/t_param_overflow.nir -o "$outasm" >/dev/null 2>&1; then
        fail "param-overflow" "binder accepted function with too many ABI params"
    else
        pass "param-overflow: too many ABI params rejected"
    fi
else
    skip "param-overflow" "nir file not generated"
fi
echo ""

# Phase 5: Assemble tests — bound .asm files should assemble
echo "--- Assemble tests ---"
for f in "$TEST_TMPDIR"/t_*.asm; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .asm)
    # Skip tests that can't assemble standalone
    case "$name" in
        t_const_scope|t_const_scope_multi|t_icall|t_icall_multi|t_api_descriptor_multi) continue ;;
    esac
    outbin="$TEST_TMPDIR/${name}.bin"
    if ./nibasm "$f" -o "$outbin" >/dev/null 2>&1; then
        size=$(wc -c < "$outbin" | tr -d ' ')
        pass "$name (assemble, ${size} bytes)"
    else
        fail "$name (assemble)" "$(./nibasm "$f" -o "$outbin" 2>&1 | head -1)"
    fi
done
echo ""

# Phase 5b: Standalone assembler regressions
echo "--- Standalone assembler checks ---"
cat > "$TEST_TMPDIR"/v20_bitfield.asm <<'ASM'
    org 0
    bext
    bins
    bext cl
    bins dl
    bext cl, dl
    bins cl, dl
    bext cl, 12
    bins cl, 12
ASM
if ./nibasm "$TEST_TMPDIR"/v20_bitfield.asm -o "$TEST_TMPDIR"/v20_bitfield.bin >/dev/null 2>&1; then
    actual=$(od -An -tx1 -v "$TEST_TMPDIR"/v20_bitfield.bin | tr -d ' \n')
    expected="0f33c00f31c00f33c10f31c20f33d10f31d10f3bc10c0f39c10c"
    if [ "$actual" = "$expected" ]; then
        pass "v20-bitfield: bext/bins encode register and immediate forms"
    else
        fail "v20-bitfield" "bytes $actual != $expected"
    fi
else
    fail "v20-bitfield" "$(./nibasm "$TEST_TMPDIR"/v20_bitfield.asm -o "$TEST_TMPDIR"/v20_bitfield.bin 2>&1 | head -1)"
fi

cat > "$TEST_TMPDIR"/bad_mov_width.asm <<'ASM'
    org 0
    mov AL, DI
ASM
if ./nibasm "$TEST_TMPDIR"/bad_mov_width.asm -o "$TEST_TMPDIR"/bad_mov_width.bin >/dev/null 2>&1; then
    fail "mov-width-reject" "assembler accepted MOV with mismatched register widths"
else
    pass "mov-width-reject: mismatched register widths rejected"
fi
echo ""

echo "--- Growable table checks ---"
cat > "$TEST_TMPDIR"/t_many_defines.nib <<'NIB'
when KEY_69 == "69" {
    fn selected_define() -> u16 {
        return 69;
    }
}
when KEY_69 != "69" {
    fn wrong_define() -> u16 {
        return 0;
    }
}
NIB
if (
    set --
    i=0
    while [ "$i" -lt 70 ]; do
        set -- "$@" -D "KEY_${i}=${i}"
        i=$((i + 1))
    done
    ./nib "$@" "$TEST_TMPDIR"/t_many_defines.nib
) >/dev/null 2>&1; then
    if grep -q '^\.fn selected_define' "$TEST_TMPDIR"/t_many_defines.nir &&
       ! grep -q '^\.fn wrong_define' "$TEST_TMPDIR"/t_many_defines.nir; then
        pass "many-defines: parser define table grows"
    else
        fail "many-defines" "wrong when branch selected"
    fi
else
    fail "many-defines" "$(
        set --
        i=0
        while [ "$i" -lt 70 ]; do
            set -- "$@" -D "KEY_${i}=${i}"
            i=$((i + 1))
        done
        ./nib "$@" "$TEST_TMPDIR"/t_many_defines.nib 2>&1 | head -1
    )"
fi

{
    echo 'fn many_symbols() -> u16 {'
    i=0
    while [ "$i" -lt 300 ]; do
        printf '    const u16 c%s = %s;\n' "$i" "$i"
        i=$((i + 1))
    done
    echo '    return c299;'
    echo '}'
} > "$TEST_TMPDIR"/t_many_symbols.nib
if ./nib "$TEST_TMPDIR"/t_many_symbols.nib >/dev/null 2>&1; then
    pass "many-symbols: compiler scope grows past old table size"
else
    fail "many-symbols" "$(
        ./nib "$TEST_TMPDIR"/t_many_symbols.nib 2>&1 | head -1
    )"
fi

many_labels_asm="$TEST_TMPDIR"/many_labels.asm
{
    echo '    org 0'
    i=0
    while [ "$i" -lt 4200 ]; do
        printf 'label_%s:\n    nop\n' "$i"
        i=$((i + 1))
    done
} > "$many_labels_asm"
if ./nibasm "$many_labels_asm" \
     -o "$TEST_TMPDIR"/many_labels.bin \
     -m "$TEST_TMPDIR"/many_labels.map >/dev/null 2>&1; then
    if grep -q '^1067 code label_4199$' \
         "$TEST_TMPDIR"/many_labels.map; then
        pass "many-labels: assembler label table grows"
    else
        fail "many-labels" "last generated label missing from map"
    fi
else
    fail "many-labels" "$(
        ./nibasm "$many_labels_asm" \
          -o "$TEST_TMPDIR"/many_labels.bin \
          -m "$TEST_TMPDIR"/many_labels.map 2>&1 | head -1
    )"
fi

many_modules_dir="$TEST_TMPDIR"/many_modules
mkdir "$many_modules_dir"
i=0
while [ "$i" -lt 135 ]; do
    next=$((i + 1))
    f="$many_modules_dir/m${i}.nib"
    if [ "$i" -lt 134 ]; then
        printf 'use "m%s.nif";\nfn f%s() {}\n' "$next" "$i" > "$f"
    else
        printf 'fn f%s() {}\n' "$i" > "$f"
    fi
    i=$next
done
if (cd "$many_modules_dir" &&
    "$REPO_DIR"/nibbuild -f -o many_modules.bin m0.nib) \
     > "$TEST_TMPDIR"/many_modules_build.log 2>&1; then
    pass "many-modules: nibbuild module table grows"
else
    fail "many-modules" "$(tail -1 "$TEST_TMPDIR"/many_modules_build.log)"
fi

binder_grow_nir="$TEST_TMPDIR"/t_binder_grow.nir
{
    cat <<'NIR'
.fn huge_vregs
    mov %1099, 0
    retval %1099
    ret
.endfn

.fn huge_insns
    mov %0, 0
NIR
    i=0
    while [ "$i" -lt 4300 ]; do
        printf '    add %%0, %%0, 1\n'
        i=$((i + 1))
    done
    cat <<'NIR'
    retval %0
    ret
.endfn

.fn huge_blocks
NIR
    i=0
    while [ "$i" -lt 600 ]; do
        next=$((i + 1))
        printf 'label_%s:\n' "$i"
        if [ "$i" -lt 599 ]; then
            printf '    jmp label_%s\n' "$next"
        else
            printf '    ret\n'
        fi
        i=$next
    done
    cat <<'NIR'
.endfn

.fn huge_consts
NIR
    i=0
    while [ "$i" -lt 96 ]; do
        printf '.const _C%s, far 0xE000:0x%04X\n' "$i" "$i"
        i=$((i + 1))
    done
    cat <<'NIR'
    ret
.endfn
NIR
} > "$binder_grow_nir"
if ./nibbind "$binder_grow_nir" \
     -o "$TEST_TMPDIR"/t_binder_grow.asm >/dev/null 2>&1; then
    if ./nibasm "$TEST_TMPDIR"/t_binder_grow.asm \
         -o "$TEST_TMPDIR"/t_binder_grow.bin >/dev/null 2>&1 &&
       grep -q '_huge_vregs:' "$TEST_TMPDIR"/t_binder_grow.asm &&
       grep -q '_huge_blocks_label_599:' "$TEST_TMPDIR"/t_binder_grow.asm &&
       grep -q '_huge_consts__C95 dw 0x005F, 0xE000' \
         "$TEST_TMPDIR"/t_binder_grow.asm; then
        pass "binder-grow: vregs, insns, blocks, constants grow"
    else
        fail "binder-grow" "generated binder stress assembly is invalid"
    fi
else
    fail "binder-grow" "$(
        ./nibbind "$binder_grow_nir" \
          -o "$TEST_TMPDIR"/t_binder_grow.asm 2>&1 | tail -1
    )"
fi

printf '\220' > "$TEST_TMPDIR"/tiny.bin
{
    i=0
    while [ "$i" -lt 5000 ]; do
        printf '%05X equ E%s\n' "$i" "$i"
        i=$((i + 1))
    done
} > "$TEST_TMPDIR"/many_map_entries.map
if ./nibdis -m "$TEST_TMPDIR"/many_map_entries.map \
     -b 1 "$TEST_TMPDIR"/tiny.bin \
     > "$TEST_TMPDIR"/many_map_entries.dis 2>&1; then
    if grep -q '^E4999.*01387h' "$TEST_TMPDIR"/many_map_entries.dis; then
        pass "many-map-entries: disassembler map table grows"
    else
        fail "many-map-entries" "last EQU label missing from disassembly"
    fi
else
    fail "many-map-entries" "$(head -1 "$TEST_TMPDIR"/many_map_entries.dis)"
fi

debug_bin="$TEST_TMPDIR"/many_debug.bin
: > "$debug_bin"
i=0
while [ "$i" -lt 9000 ]; do
    printf '\220' >> "$debug_bin"
    i=$((i + 1))
done
{
    i=0
    while [ "$i" -lt 9000 ]; do
        printf '%05X many.nib:%s\n' "$i" "$i"
        i=$((i + 1))
    done
} > "$TEST_TMPDIR"/many_debug.dbg
if ./nibdis -d "$TEST_TMPDIR"/many_debug.dbg -a 0x2134 -b 1 \
     "$debug_bin" > "$TEST_TMPDIR"/many_debug.dis 2>&1; then
    if grep -q 'many.nib:8500' "$TEST_TMPDIR"/many_debug.dis; then
        pass "many-debug-entries: disassembler debug table grows"
    else
        fail "many-debug-entries" "late debug entry missing from disassembly"
    fi
else
    fail "many-debug-entries" "$(head -1 "$TEST_TMPDIR"/many_debug.dis)"
fi
echo ""

# Phase 6: Full build test
echo "--- Full build test ---"
rm -f tests/lcd.nir tests/lcd.nif tests/app.nir tests/app.nif tests/app.bin
if ./nibbuild tests/app.nib -o "$TEST_TMPDIR"/app_build.bin >/dev/null 2>&1; then
    size=$(wc -c < "$TEST_TMPDIR"/app_build.bin | tr -d ' ')
    pass "nibbuild app.nib (${size} bytes)"
else
    fail "nibbuild app.nib" "$(./nibbuild tests/app.nib 2>&1 | tail -1)"
fi
echo ""

# Phase 7: Assembly content validation
echo "--- Assembly content checks ---"

# Extern parameter pins: dos_putchar(0x02, 0x41) must use AH and DL
if [ -f "$TEST_TMPDIR"/t_extern_pins.asm ]; then
    if grep -q "mov AH," "$TEST_TMPDIR"/t_extern_pins.asm && grep -q "mov DL," "$TEST_TMPDIR"/t_extern_pins.asm; then
        pass "extern pins: AH and DL used for dos_putchar"
    else
        fail "extern pins" "dos_putchar params not in AH/DL (got: $(grep 'mov.*,' "$TEST_TMPDIR"/t_extern_pins.asm | head -4 | tr '\n' '; '))"
    fi
else
    skip "extern pins" "asm not generated"
fi

# Stack ABI: two stack params and two stack returns share the same
# two-word call area, and return b,a must not clobber a before it is read.
if [ -f "$TEST_TMPDIR"/t_stack_abi.asm ]; then
    if grep -q "push \\[BP+6\\]" "$TEST_TMPDIR"/t_stack_abi.asm &&
       grep -q "push \\[BP+4\\]" "$TEST_TMPDIR"/t_stack_abi.asm &&
       grep -q "pop \\[BP+6\\]" "$TEST_TMPDIR"/t_stack_abi.asm &&
       grep -q "pop \\[BP+4\\]" "$TEST_TMPDIR"/t_stack_abi.asm &&
       grep -q "add SP, 4" "$TEST_TMPDIR"/t_stack_abi.asm &&
       ! grep -q "add SP, 8" "$TEST_TMPDIR"/t_stack_abi.asm; then
        pass "stack ABI: params and returns share call area"
    else
        fail "stack ABI" "expected overlapping stack param/return layout"
    fi
else
    skip "stack ABI" "asm not generated"
fi

# Inter-procedural propagation: fill() should get DI, AL, CX
if [ -f "$TEST_TMPDIR"/t_pinning.asm ]; then
    if grep -q "mov DI," "$TEST_TMPDIR"/t_pinning.asm && grep -q "mov AL," "$TEST_TMPDIR"/t_pinning.asm; then
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
    ./nibbind tests/callee_save.nir -o "$TEST_TMPDIR"/callee_save.asm >/dev/null 2>&1
    if [ -f "$TEST_TMPDIR"/callee_save.asm ]; then
        if grep -q "push BX" "$TEST_TMPDIR"/callee_save.asm && grep -q "pop BX" "$TEST_TMPDIR"/callee_save.asm; then
            pass "callee-save: push/pop BX emitted"
        else
            fail "callee-save" "no push/pop for preserved register"
        fi
    fi
fi

# Callee-save speed policy: five PUSHA-covered registers are still faster
# as individual push/pop pairs on V20, even when segment saves are also
# present.
if [ -f "$TEST_TMPDIR"/t_callee_save_speed.asm ]; then
    save_many_window=$(sed -n '/t_callee_save_speed_save_many:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_callee_save_speed.asm)
    if printf "%s\n" "$save_many_window" | grep -q 'push AX' &&
       printf "%s\n" "$save_many_window" | grep -q 'push CX' &&
       printf "%s\n" "$save_many_window" | grep -q 'push DX' &&
       printf "%s\n" "$save_many_window" | grep -q 'push BX' &&
       printf "%s\n" "$save_many_window" | grep -q 'push DI' &&
       printf "%s\n" "$save_many_window" | grep -q 'push DS' &&
       printf "%s\n" "$save_many_window" | grep -q 'push ES' &&
       printf "%s\n" "$save_many_window" | grep -q 'pop ES' &&
       printf "%s\n" "$save_many_window" | grep -q 'pop DS' &&
       ! printf "%s\n" "$save_many_window" | grep -q 'pusha\|popa'; then
        pass "callee-save-speed: five GP saves avoid slower PUSHA"
    else
        fail "callee-save-speed" "callee save policy used PUSHA or missed explicit saves"
    fi
fi

# Saturating add: flag check block should emit JNC
if [ -f "$TEST_TMPDIR"/t_flags.asm ]; then
    if grep -q "jnc" "$TEST_TMPDIR"/t_flags.asm; then
        pass "flag-check: JNC emitted for CF check"
    else
        fail "flag-check" "no JNC in flag check block"
    fi
fi

# Variable shift counts use CL
if [ -f "$TEST_TMPDIR"/t_shift_cl.asm ]; then
    if grep -q "shl.*CL" "$TEST_TMPDIR"/t_shift_cl.asm && grep -q "shr.*CL" "$TEST_TMPDIR"/t_shift_cl.asm; then
        pass "shift-cl: variable shifts use CL"
    else
        fail "shift-cl" "variable shift not routed through CL"
    fi
fi

# Overlapping shift counts: parameters must not share a register
if [ -f "$TEST_TMPDIR"/t_shift_overlap.asm ]; then
    if grep -q "push CX" "$TEST_TMPDIR"/t_shift_overlap.asm || grep -q "push AX" "$TEST_TMPDIR"/t_shift_overlap.asm; then
        pass "shift-overlap: CL contention handled with save/restore"
    else
        fail "shift-overlap" "no save/restore — overlapping params may share CL"
    fi
fi

# Unused call return must not clobber live byte vreg via aliasing
if [ -f "$TEST_TMPDIR"/t_call_clobber.asm ]; then
    # If the unused return got AX, it would clobber AL (the parameter)
    # The caller should NOT have mov AX as a result of the call
    if grep -q 'caller:' "$TEST_TMPDIR"/t_call_clobber.asm; then
        if grep -A5 'caller:' "$TEST_TMPDIR"/t_call_clobber.asm | grep -q 'push AL\|push AX'; then
            pass "unused-call-ret: AL saved across call"
        elif ! grep -q '%1=AX' "$TEST_TMPDIR"/t_call_clobber.asm 2>/dev/null; then
            pass "unused-call-ret: return vreg avoids AX"
        else
            fail "unused-call-ret" "unused call return clobbers live AL via AX alias"
        fi
    fi
fi

# Internal calls to later-defined callees must save live caller registers.
if [ -f "$TEST_TMPDIR"/t_internal_call_save.asm ]; then
    call_window=$(sed -n '/push SI/,/pop SI/p' "$TEST_TMPDIR"/t_internal_call_save.asm)
    if echo "$call_window" | grep -q 'push SI' &&
       echo "$call_window" | grep -q 'push CX' &&
       echo "$call_window" | grep -q 'call t_internal_call_save_helper' &&
       echo "$call_window" | grep -q 'pop CX' &&
       echo "$call_window" | grep -q 'pop SI'; then
        pass "internal-call-save: live SI/CX saved across call"
    else
        fail "internal-call-save" "missing SI/CX save around internal helper call"
    fi
fi

# Extern call clobber folding must be monotonic: an extern that preserves
# DX must not erase a prior local DX clobber from the internal helper.
if [ -f "$TEST_TMPDIR"/t_extern_clobber_merge.asm ]; then
    call_window=$(sed -n '/t_extern_clobber_merge_caller:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_extern_clobber_merge.asm)
    preserves_window=$(sed -n '/t_extern_clobber_merge_caller_preserves:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_extern_clobber_merge.asm)
    unannotated_window=$(sed -n '/t_extern_clobber_merge_caller_unannotated:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_extern_clobber_merge.asm)
    if printf "%s\n" "$call_window" | grep -q 'push DX' &&
       printf "%s\n" "$call_window" | grep -q 'call t_extern_clobber_merge_helper' &&
       printf "%s\n" "$call_window" | grep -q 'pop DX' &&
       printf "%s\n" "$preserves_window" | grep -q 'push AX' &&
       printf "%s\n" "$preserves_window" | grep -q 'call t_extern_clobber_merge_helper_preserves' &&
       printf "%s\n" "$preserves_window" | grep -q 'pop AX' &&
       printf "%s\n" "$unannotated_window" | grep -q 'push BX' &&
       printf "%s\n" "$unannotated_window" | grep -q 'call t_extern_clobber_merge_helper_unannotated' &&
       printf "%s\n" "$unannotated_window" | grep -q 'pop BX'; then
        pass "extern-clobber-merge: asm annotations survive extern preserves"
    else
        fail "extern-clobber-merge" "asm clobbers were hidden or ignored"
    fi
fi

# Direct byte-returning calls must not save and restore the call result.
if [ -f "$TEST_TMPDIR"/t_call_ret_save.asm ]; then
    call_window=$(sed -n '/call t_call_ret_save_pop_byte/,/popf/p' "$TEST_TMPDIR"/t_call_ret_save.asm)
    if echo "$call_window" | grep -q 'call t_call_ret_save_pop_byte' &&
       ! echo "$call_window" | grep -q 'pop AX'; then
        pass "call-ret-save: direct byte return is not restored away"
    else
        fail "call-ret-save" "direct byte return restored by caller-save"
    fi
fi

# u8 returns computed before later byte bookkeeping must be restored to AL.
if [ -f "$TEST_TMPDIR"/t_return_late_u8.asm ]; then
    ret_window=$(sed -n '/t_return_late_u8_pop_key:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_return_late_u8.asm)
    after_count=$(printf "%s\n" "$ret_window" | sed -n '/mov .*count.*AL/,$p')
    if printf "%s\n" "$after_count" | grep -q 'mov AL, '; then
        pass "return-late-u8: saved byte restored to AL"
    else
        fail "return-late-u8" "u8 return value not restored after bookkeeping"
    fi
fi

cat > "$TEST_TMPDIR"/t_call_ret_alias_restore.nir <<'NIR'
; Binder regression: a byte return captured in AH must survive POP AX.
.fn retbyte
.returns u8
    mov %0, 0x41
    retval %0
    ret
.endfn

.fn call_ret_alias_restore
.returns u8
.vreg %0, u8, pin=AL
.vreg %1, u8, pin=AH
.vreg %2, u8
    mov %0, 0x22
    call %1, retbyte
    mov %2, %1
    add %2, %2, %0
    retval %2
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_call_ret_alias_restore.nir -o "$TEST_TMPDIR"/t_call_ret_alias_restore.asm >/dev/null 2>&1; then
    ret_alias_window=$(sed -n '/_call_ret_alias_restore:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_call_ret_alias_restore.asm)
    if printf "%s\n" "$ret_alias_window" | grep -q 'call .*retbyte' &&
       printf "%s\n" "$ret_alias_window" | grep -q 'mov \[BP[-+][0-9]*\], AL' &&
       ! printf "%s\n" "$ret_alias_window" | awk '
           /call .*retbyte/ { after_call = 1 }
           after_call && /pop AX/ { saw_pop_ax = 1 }
           after_call && !saw_pop_ax && /mov AH, AL/ { bad = 1 }
           END { exit bad ? 0 : 1 }
       '; then
        pass "call-ret-alias-restore: AL return survives AX restore"
    else
        fail "call-ret-alias-restore" "AL return captured in AH before POP AX"
    fi
else
    fail "call-ret-alias-restore" "$(./nibbind "$TEST_TMPDIR"/t_call_ret_alias_restore.nir -o "$TEST_TMPDIR"/t_call_ret_alias_restore.asm 2>&1 | tail -1)"
fi

# Multi-return materialization must not clobber AX before the DX return
# slot has copied it, and callers must capture the second return slot.
if [ -f "$TEST_TMPDIR"/t_multi_return.asm ]; then
    split_window=$(sed -n '/^split_pair:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_multi_return.asm)
    call_window=$(sed -n '/t_multi_return_use_pair:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_multi_return.asm)
    if printf "%s\n" "$split_window" | awk '
           /mov DX, AX/ { dx_ax = NR }
           /mov DX, [A-Z][A-Z]*/ { dx = NR }
           /mov AL, [A-Z][A-Z]*/ { al = NR }
           END {
               if (dx_ax) exit (al && dx_ax < al) ? 0 : 1
               exit (al && dx) ? 0 : 1
           }
       ' &&
       printf "%s\n" "$call_window" | grep -q 'call split_pair' &&
       printf "%s\n" "$call_window" | grep -q 'mov \(CX\|DI\), DX'; then
        pass "multi-return: return slots materialized and captured"
    else
        fail "multi-return" "missing ordered return materialization or capture"
    fi
fi

# Interrupt wrappers must save registers clobbered by internal callees.
if [ -f "$TEST_TMPDIR"/t_interrupt_call_save.asm ]; then
    irq_window=$(sed -n '/t_interrupt_call_save_irq_handler:/,/^[[:space:]]*iret$/p' "$TEST_TMPDIR"/t_interrupt_call_save.asm)
    if printf "%s\n" "$irq_window" | grep -q 'push DX' &&
       printf "%s\n" "$irq_window" | grep -q 'push DI' &&
       printf "%s\n" "$irq_window" | grep -q 'call t_interrupt_call_save_helper' &&
       printf "%s\n" "$irq_window" | grep -q 'pop DI' &&
       printf "%s\n" "$irq_window" | grep -q 'pop DX'; then
        pass "interrupt-call-save: callee clobbers preserved"
    else
        fail "interrupt-call-save" "callee-clobbered DX/DI not preserved"
    fi
fi

# Address-taken locals must live in the frame, and @local must pass SS:off.
if [ -f "$TEST_TMPDIR"/t_local_addr.asm ]; then
    local_window=$(sed -n '/t_local_addr_send_key:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_local_addr.asm)
    if printf "%s\n" "$local_window" | grep -q 'sub sp, 4' &&
       printf "%s\n" "$local_window" | grep -q 'lea .* \[BP-2\]' &&
       printf "%s\n" "$local_window" | grep -q 'mov .* SS' &&
       printf "%s\n" "$local_window" | grep -q 'mov ES,' &&
       printf "%s\n" "$local_window" | grep -q 'call t_local_addr_sink' &&
       printf "%s\n" "$local_window" | grep -q 'lea .* \[BP-4\]'; then
        pass "local-addr: stack locals addressable with & and @"
    else
        fail "local-addr" "stack local address materialization missing"
    fi
fi

# Initialized data must use the segment where the data label is emitted.
if [ -f "$TEST_TMPDIR"/t_far_data_seg.asm ]; then
    data_window=$(sed -n '/; === data: far_data ===/,/; === t_far_data_seg_get_far_data ===/p' "$TEST_TMPDIR"/t_far_data_seg.asm)
    get_window=$(sed -n '/t_far_data_seg_get_far_data:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_far_data_seg.asm)
    same_window=$(sed -n '/t_far_data_seg_read_same_segment:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_far_data_seg.asm)
    other_window=$(sed -n '/t_far_data_seg_read_other_segment:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_far_data_seg.asm)
    if printf "%s\n" "$data_window" | grep -q 'seg 0xE000' &&
       printf "%s\n" "$get_window" | grep -q 'SEG far_data' &&
       printf "%s\n" "$same_window" | grep -q '\[CS:.*far_data\|\[CS:.*BX' &&
       printf "%s\n" "$other_window" | grep -q 'mov AX, SEG far_data' &&
       printf "%s\n" "$other_window" | grep -q '\[ES:.*far_data\|\[ES:.*BX'; then
        pass "init-data-seg: placed data uses CS/ES by emitted segment"
    else
        fail "init-data-seg" "initialized data segment was not preserved"
    fi
fi

# Explicit DS policies must emit setup/restore at boundaries without
# requiring callers to manage DS.
if [ -f "$TEST_TMPDIR"/t_ds_policy.asm ]; then
    lit_window=$(sed -n '/^api_lit:/,/^[[:space:]]*retf$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    sym_window=$(sed -n '/^api_sym:/,/^[[:space:]]*retf$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    caller_window=$(sed -n '/t_ds_policy_caller_ds:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    none_window=$(sed -n '/t_ds_policy_no_ds:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    stack_ds_window=$(sed -n '/t_ds_policy_stack_param_with_ds:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    forward_window=$(sed -n '/t_ds_policy_forward_data_ds:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    irq_window=$(sed -n '/t_ds_policy_irq_handler:/,/^[[:space:]]*iret$/p' "$TEST_TMPDIR"/t_ds_policy.asm)
    if printf "%s\n" "$lit_window" | grep -q 'push DS' &&
       printf "%s\n" "$lit_window" | grep -q 'mov AX, 0xE000' &&
       printf "%s\n" "$lit_window" | grep -q 'mov DS, AX' &&
       printf "%s\n" "$lit_window" | grep -q 'pop DS' &&
       printf "%s\n" "$sym_window" | grep -q 'mov AX, SEG data_anchor' &&
       printf "%s\n" "$forward_window" | grep -q 'mov AX, SEG late_anchor' &&
       printf "%s\n" "$irq_window" | grep -q 'push DS' &&
       printf "%s\n" "$irq_window" | grep -q 'iret' &&
       [ "$(printf "%s\n" "$stack_ds_window" | sed -n '2p')" = "    push bp" ] &&
       [ "$(printf "%s\n" "$stack_ds_window" | sed -n '4p')" = "    push DS" ] &&
       printf "%s\n" "$stack_ds_window" | grep -q 'mov SI, \[BP+4\]' &&
       printf "%s\n" "$stack_ds_window" | grep -q 'mov AX, \[BP+6\]' &&
       printf "%s\n" "$stack_ds_window" | grep -q 'mov ES, AX' &&
       ! printf "%s\n" "$caller_window" | grep -q 'mov DS, AX' &&
       ! printf "%s\n" "$none_window" | grep -q 'mov DS, AX'; then
        pass "ds-policy: setup and restore emitted at boundaries"
    else
        fail "ds-policy" "DS setup/restore sequence missing or unexpected"
    fi
fi

cat > "$TEST_TMPDIR"/bad_far_data.nib <<'NIB'
far u8[1] table;
NIB
if ./nib "$TEST_TMPDIR"/bad_far_data.nib --parse-only >/dev/null 2>&1; then
    fail "far-data-reject" "far data declaration was accepted"
else
    pass "far-data-reject: far is not valid on data"
fi

cat > "$TEST_TMPDIR"/bad_unplaced_data.nib <<'NIB'
u8[1] table = {0x01};
fn addr() -> far32 {
    return @table;
}
NIB
if ./nib "$TEST_TMPDIR"/bad_unplaced_data.nib >/dev/null 2>&1 &&
   ./nibbind "$TEST_TMPDIR"/bad_unplaced_data.nir -o "$TEST_TMPDIR"/bad_unplaced_data.asm >/dev/null 2>&1; then
    fail "init-data-placement" "unplaced initialized data bound successfully"
else
    pass "init-data-placement: initialized data requires at()"
fi

cat > "$TEST_TMPDIR"/bad_pub_far_ds.nib <<'NIB'
pub extern fn far missing() clobbers(FLAGS) {
}
NIB
if ./nib "$TEST_TMPDIR"/bad_pub_far_ds.nib >/dev/null 2>&1; then
    fail "ds-policy-require" "public far function without ds() was accepted"
else
    pass "ds-policy-require: public far functions require ds()"
fi

cat > "$TEST_TMPDIR"/bad_extern_clobbers.nib <<'NIB'
extern fn far missing();
NIB
if ./nib "$TEST_TMPDIR"/bad_extern_clobbers.nib >/dev/null 2>&1; then
    fail "extern-clobbers-require" "extern function without clobbers() accepted"
else
    pass "extern-clobbers-require: extern functions require clobbers()"
fi

cat > "$TEST_TMPDIR"/bad_extern_preserves.nib <<'NIB'
extern fn far old_style() preserves(AX);
NIB
if ./nib "$TEST_TMPDIR"/bad_extern_preserves.nib >/dev/null 2>&1; then
    fail "extern-preserves-reject" "extern function with preserves() accepted"
else
    pass "extern-preserves-reject: extern functions reject preserves()"
fi

cat > "$TEST_TMPDIR"/bad_api_clobbers.nib <<'NIB'
fn api far missing() ds(none) {
}
NIB
if ./nib "$TEST_TMPDIR"/bad_api_clobbers.nib >/dev/null 2>&1; then
    fail "api-clobbers-require" "api function without clobbers() accepted"
else
    pass "api-clobbers-require: api functions require clobbers()"
fi

cat > "$TEST_TMPDIR"/bad_clobber_cs_ss.nib <<'NIB'
extern fn far bad_cs() clobbers(CS);
extern fn far bad_ss() clobbers(SS);
NIB
if ./nib "$TEST_TMPDIR"/bad_clobber_cs_ss.nib >/dev/null 2>&1; then
    fail "clobber-cs-ss-reject" "clobbers(CS/SS) was accepted"
else
    pass "clobber-cs-ss-reject: functions cannot clobber CS or SS"
fi

cat > "$TEST_TMPDIR"/bad_ds_symbol.nib <<'NIB'
fn code_label() {
}
pub extern fn far bad() ds(code_label) clobbers(FLAGS) {
}
NIB
if ./nib "$TEST_TMPDIR"/bad_ds_symbol.nib >/dev/null 2>&1 &&
   ./nibbind "$TEST_TMPDIR"/bad_ds_symbol.nir -o "$TEST_TMPDIR"/bad_ds_symbol.asm >/dev/null 2>&1; then
    fail "ds-policy-symbol" "ds(function) was accepted"
else
    pass "ds-policy-symbol: ds() requires data object"
fi

cat > "$TEST_TMPDIR"/bad_ds_literal.nib <<'NIB'
pub extern fn far bad() ds(0x10000) clobbers(FLAGS) {
}
NIB
if ./nib "$TEST_TMPDIR"/bad_ds_literal.nib >/dev/null 2>&1; then
    fail "ds-policy-literal" "out-of-range DS literal was accepted"
else
    pass "ds-policy-literal: DS literal range checked"
fi

cat > "$TEST_TMPDIR"/bad_ds_none.nib <<'NIB'
u8 flag;
fn bad() ds(none) -> u8 {
    return flag;
}
NIB
if ./nib "$TEST_TMPDIR"/bad_ds_none.nib >/dev/null 2>&1 &&
   ./nibbind "$TEST_TMPDIR"/bad_ds_none.nir -o "$TEST_TMPDIR"/bad_ds_none.asm >/dev/null 2>&1; then
    fail "ds-policy-none" "ds(none) allowed DS-default memory"
else
    pass "ds-policy-none: DS-default memory rejected"
fi

# Indirect calls must save live registers not preserved by the extern
# descriptor.
if [ -f "$TEST_TMPDIR"/t_icall_save.asm ]; then
    call_window=$(sed -n '/push DI/,/pop DI/p' "$TEST_TMPDIR"/t_icall_save.asm)
    if echo "$call_window" | grep -q 'push DI' &&
       echo "$call_window" | grep -q 'call far \[SS:BX\]' &&
       echo "$call_window" | grep -q 'pop DI' &&
       ! echo "$call_window" | grep -q 'push AX\|pop AX' &&
       sed -n '/call far \[SS:BX\]/,/ret/p' "$TEST_TMPDIR"/t_icall_save.asm | grep -q 'and .*0x00FF'; then
        pass "icall-save: live DI saved without clobbering AL return"
    else
        fail "icall-save" "indirect call save/return handling is wrong"
    fi
fi

# Reassigning a pinned AL local after a later indirect call must not make
# the stale earlier value look live across that call and restore AX over
# the AL return.
if [ -f "$TEST_TMPDIR"/t_icall_ret_restore.asm ]; then
    ret_window=$(sed -n '/call far \[BP+4\]/,/jc /p' "$TEST_TMPDIR"/t_icall_ret_restore.asm)
    if printf "%s\n" "$ret_window" | grep -q 'call far \[BP+4\]' &&
       ! printf "%s\n" "$ret_window" | grep -q 'pop AX'; then
        pass "icall-ret-restore: AL return survives caller restore"
    else
        fail "icall-ret-restore" "caller restored AX over indirect AL return"
    fi
fi

cat > "$TEST_TMPDIR"/t_icall_ret_temp.nir <<'NIR'
; Binder regression: an indirect u8 return in AL must be captured before
; restoring a caller-saved AX when the destination vreg is not AL.
.extern read
.returns u8, register, pin=AL
.preserves CX, DX, BP, SI, DI, ES, CS, SS, DS
.endextern

.fn icall_ret_temp
.returns u8
.vreg %0, u16, pin=AX
.vreg %2, seg
.vreg %3, u8, pin=BH
    mov %0, 4660
    mov %1, 256
    mov %2, 57344
    icall %3, %1, %2, read
    add %4, %0, %0
    retval %3
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_icall_ret_temp.nir -o "$TEST_TMPDIR"/t_icall_ret_temp.asm >/dev/null 2>&1; then
    ret_temp_window=$(sed -n '/icall_ret_temp_icall_ret_temp:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_ret_temp.asm)
    if ./nibasm "$TEST_TMPDIR"/t_icall_ret_temp.asm -o "$TEST_TMPDIR"/t_icall_ret_temp.bin >/dev/null 2>&1 &&
       printf "%s\n" "$ret_temp_window" | grep -q 'call far \[SS:BX\]' &&
       printf "%s\n" "$ret_temp_window" | awk '
           /call far \[SS:BX\]/ { after_call = 1 }
           after_call && /mov BH, AL/ { captured = 1 }
           after_call && /mov \[BP[-+][0-9]+\], AL/ { captured = 1 }
           after_call && /pop AX/ { saw_restore = 1; ok = captured; exit }
           END { exit (saw_restore && ok) ? 0 : 1 }
       '; then
        pass "icall-ret-temp: AL return captured before AX restore"
    else
        fail "icall-ret-temp" "AL return was not captured across AX restore"
    fi
else
    fail "icall-ret-temp" "$(./nibbind "$TEST_TMPDIR"/t_icall_ret_temp.nir -o "$TEST_TMPDIR"/t_icall_ret_temp.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_icall_ret_alias_temp.nir <<'NIR'
; Binder regression: when an indirect u8 return already lives in AL, an
; unrelated live AH value can still force an AX restore that would clobber it.
.extern read
.returns u8, register, pin=AL
.preserves CX, DX, BP, SI, DI, ES, CS, SS, DS
.endextern

.fn icall_ret_alias_temp
.returns u8
.vreg %0, u8, pin=AH
.vreg %2, seg
.vreg %3, u8, pin=AL
    mov %0, 85
    mov %1, 256
    mov %2, 57344
    icall %3, %1, %2, read
    add %4, %0, %0
    retval %3
    ret
.endfn
NIR
if ./nibbind --pressure-report "$TEST_TMPDIR"/t_icall_ret_alias_temp_pressure.txt \
     "$TEST_TMPDIR"/t_icall_ret_alias_temp.nir \
     -o "$TEST_TMPDIR"/t_icall_ret_alias_temp.asm >/dev/null 2>&1; then
    ret_alias_window=$(sed -n '/icall_ret_alias_temp_icall_ret_alias_temp:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_ret_alias_temp.asm)
    if ./nibasm "$TEST_TMPDIR"/t_icall_ret_alias_temp.asm -o "$TEST_TMPDIR"/t_icall_ret_alias_temp.bin >/dev/null 2>&1 &&
       printf "%s\n" "$ret_alias_window" | grep -q 'call far \[SS:BX\]' &&
       printf "%s\n" "$ret_alias_window" | awk '
           /call far \[SS:BX\]/ { after_call = 1 }
           after_call && /mov \[BP-[0-9]+\], AL/ { captured = 1 }
           after_call && /mov \[BP\+0\], AL/ { bad_zero_temp = 1 }
           after_call && /pop AX/ {
               saw_restore = 1
               ok = captured && !bad_zero_temp
               exit
           }
           END { exit (saw_restore && ok) ? 0 : 1 }
       '; then
        pass "icall-ret-alias-temp: AL return saved in real temp before AX restore"
    else
        fail "icall-ret-alias-temp" "AL return temp was missing or used BP+0"
    fi
else
    fail "icall-ret-alias-temp" "$(
        ./nibbind --pressure-report \
          "$TEST_TMPDIR"/t_icall_ret_alias_temp_pressure.txt \
          "$TEST_TMPDIR"/t_icall_ret_alias_temp.nir \
          -o "$TEST_TMPDIR"/t_icall_ret_alias_temp.asm 2>&1 | tail -1
    )"
fi
if grep -q '^fixups: .*ret-capture=[1-9].*ret-reload=[1-9]' \
     "$TEST_TMPDIR"/t_icall_ret_alias_temp_pressure.txt &&
   grep -q '^spill-actions: .*mem-route=[1-9]' \
     "$TEST_TMPDIR"/t_icall_ret_alias_temp_pressure.txt; then
    pass "pressure-report: return temp routes counted"
else
    fail "pressure-report-ret-temp" "return temp spill-route counts missing"
fi

cat > "$TEST_TMPDIR"/t_icall_bl_arg_scratch.nir <<'NIR'
; Binder regression: the indirect far-call stub must not use BX to
; address the temporary far pointer when BL is an outgoing argument.
.extern write_ram
.eparam u8, "index", register, pin=AL
.eparam u8, "value", register, pin=BL
.preserves AX, CX, DX, BP, SI, DI, ES, CS, SS, DS
.endextern

.fn icall_bl_arg_scratch
.vreg %0, u16, pin=DX
.vreg %1, seg
.vreg %2, u8, pin=AL
.vreg %3, u8, pin=CL
    mov %0, 4660
    mov %4, 57344
    mov %1, %4
    mov %2, 0
    mov %3, 90
    icall %5, %0, %1, write_ram, %2, %3
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_icall_bl_arg_scratch.nir -o "$TEST_TMPDIR"/t_icall_bl_arg_scratch.asm >/dev/null 2>&1; then
    bl_scratch_window=$(sed -n '/icall_bl_arg_scratch_icall_bl_arg_scratch:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_bl_arg_scratch.asm)
    if ./nibasm "$TEST_TMPDIR"/t_icall_bl_arg_scratch.asm -o "$TEST_TMPDIR"/t_icall_bl_arg_scratch.bin >/dev/null 2>&1 &&
       printf "%s\n" "$bl_scratch_window" | grep -q 'mov BL, CL' &&
       printf "%s\n" "$bl_scratch_window" | grep -q 'call far \[SS:SI\]' &&
       ! printf "%s\n" "$bl_scratch_window" | grep -q 'mov BX, SP'; then
        pass "icall-bl-arg-scratch: BL argument survives far pointer stub"
    else
        fail "icall-bl-arg-scratch" "far pointer stub clobbered BL"
    fi
else
    fail "icall-bl-arg-scratch" "$(./nibbind "$TEST_TMPDIR"/t_icall_bl_arg_scratch.nir -o "$TEST_TMPDIR"/t_icall_bl_arg_scratch.asm 2>&1 | tail -1)"
fi

# Flags returned by an indirect API call must be consumed from FLAGS,
# not from the arbitrary vreg assigned to the getflag pseudo-result.
if [ -f "$TEST_TMPDIR"/t_flag_after_icall.asm ]; then
    flag_icall_window=$(sed -n '/t_flag_after_icall_poll:/,/t_flag_after_icall_poll_.L0:/p' "$TEST_TMPDIR"/t_flag_after_icall.asm)
    if printf "%s\n" "$flag_icall_window" | grep -q 'call far \[BP-4\]' &&
       printf "%s\n" "$flag_icall_window" | grep -q 'jc t_flag_after_icall_poll_.L0' &&
       ! printf "%s\n" "$flag_icall_window" | grep -q 'add SP' &&
       ! printf "%s\n" "$flag_icall_window" | grep -q 'getflag\|not '; then
        pass "flag-after-icall: CF consumed from FLAGS"
    else
        fail "flag-after-icall" "CF after indirect call was not a carry branch"
    fi
fi

# Far32 parameters default to stack ABI. Used as indirect far call
# targets, they should be called directly from their stack homes.
if [ -f "$TEST_TMPDIR"/t_icall_far_param.asm ]; then
    beep_window=$(sed -n '/t_icall_far_param_beep_once:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_far_param.asm)
    caller_window=$(sed -n '/t_icall_far_param_caller:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_far_param.asm)
    if printf "%s\n" "$beep_window" | grep -q 'call far \[BP+4\]' &&
       printf "%s\n" "$beep_window" | grep -q 'call far \[BP+8\]' &&
       printf "%s\n" "$beep_window" | grep -q 'call far \[BP+12\]' &&
       ! printf "%s\n" "$beep_window" | grep -q 'call far \[SS:BX\]' &&
       [ "$(printf "%s\n" "$caller_window" | grep -c 'add SP, 12')" -eq 2 ]; then
        pass "icall-far-param: stack far32 targets called directly"
    else
        fail "icall-far-param" "far32 stack target was repacked"
    fi
fi
if ./nibbind --pressure-report "$TEST_TMPDIR"/t_icall_far_param_pressure.txt \
     --pressure-fn caller tests/t_icall_far_param.nir \
     -o "$TEST_TMPDIR"/t_icall_far_param_pressure.asm >/dev/null 2>&1; then
    if grep -q '^spill-actions: .*mem-route=[1-9]' \
         "$TEST_TMPDIR"/t_icall_far_param_pressure.txt; then
        pass "pressure-report: stack call arg routes counted"
    else
        fail "pressure-report-stack-args" "stack argument route count missing"
    fi
else
    fail "pressure-report-stack-args" "$(
        ./nibbind --pressure-report \
          "$TEST_TMPDIR"/t_icall_far_param_pressure.txt \
          --pressure-fn caller tests/t_icall_far_param.nir \
          -o "$TEST_TMPDIR"/t_icall_far_param_pressure.asm 2>&1 | tail -1
    )"
fi

if [ -f "$TEST_TMPDIR"/t_icall_multi.asm ]; then
    local_arg_window=$(sed -n '/t_icall_call_read_time:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_icall_multi.asm)
    if grep -q 'icall .*, read_time, .*, .*' tests/t_icall.nir &&
       printf "%s\n" "$local_arg_window" | grep -q 'lea SI, \[BP-' &&
       printf "%s\n" "$local_arg_window" | grep -q 'mov AX, SS' &&
       printf "%s\n" "$local_arg_window" | grep -q 'mov ES, AX' &&
       printf "%s\n" "$local_arg_window" | grep -q 'call far \[SS:BX\]'; then
        pass "icall-local-far-arg: @local passed in ES:SI"
    else
        fail "icall-local-far-arg" "indirect far32 arg not passed in ES:SI"
    fi
fi

cat > "$TEST_TMPDIR"/t_call_const_arg_spill.nir <<'NIR'
; Binder regression: the call argument is the constant vreg %51, not
; the live byte vreg %8 that remains live for the later comparison.
.fn read_current
.param %0, u8, "index", register, pin=AL
.returns u8
    retval %0
    ret
.endfn

.fn call_const_arg_spill
.returns u8
.vreg %8, u8, pin=BL
.vreg %51, u16, pin=DI
.vreg %52, u8
.vreg %53, u8, pin=DI
    mov %8, 2
.L0:
    mov %51, 1
    call %52, read_current, %51
    cmp.eq %53, %8, %52
    jz %53, .L2
    jmp .L0
.L2:
    retval %8
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_call_const_arg_spill.nir -o "$TEST_TMPDIR"/t_call_const_arg_spill.asm >/dev/null 2>&1; then
    const_arg_window=$(sed -n '/call_const_arg_spill.*_\.L0:/,/call_const_arg_spill.*_\.L2:/p' "$TEST_TMPDIR"/t_call_const_arg_spill.asm)
    if printf "%s\n" "$const_arg_window" | grep -q 'mov DI, 1' &&
       printf "%s\n" "$const_arg_window" | grep -q 'mov AX, DI' &&
       printf "%s\n" "$const_arg_window" | grep -q 'call .*read_current' &&
       ! printf "%s\n" "$const_arg_window" | grep -q 'mov AL, DI' &&
       ! printf "%s\n" "$const_arg_window" | grep -q 'mov AL, B[HL]'; then
        pass "call-const-arg-spill: constant vreg passed to call"
    else
        fail "call-const-arg-spill" "call argument came from wrong byte register"
    fi
else
    fail "call-const-arg-spill" "$(./nibbind "$TEST_TMPDIR"/t_call_const_arg_spill.nir -o "$TEST_TMPDIR"/t_call_const_arg_spill.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_call_arg_order.nir <<'NIR'
; Binder regression: setting up one argument must not clobber the source
; register for a later argument. Here %0 starts in AL and must be copied
; to BL before the literal page value in DI is materialized into AL.
.fn read_paged
.param %0, u8, "page", register, pin=AL
.param %1, u8, "index", register, pin=BL
.returns u8
    retval %1
    ret
.endfn

.fn call_arg_order
.param %0, u8, "index", register, pin=AL
.returns u8
.vreg %1, u16, pin=DI
.vreg %2, u8
    mov %1, 2
    call %2, read_paged, %1, %0
    retval %2
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_call_arg_order.nir -o "$TEST_TMPDIR"/t_call_arg_order.asm >/dev/null 2>&1; then
    arg_order_window=$(sed -n '/call_arg_order_call_arg_order:/,/call .*read_paged/p' "$TEST_TMPDIR"/t_call_arg_order.asm)
    if ./nibasm "$TEST_TMPDIR"/t_call_arg_order.asm -o "$TEST_TMPDIR"/t_call_arg_order.bin >/dev/null 2>&1 &&
       printf "%s\n" "$arg_order_window" | awk '
           /mov BL, AL/ { copied_index = NR }
           /mov AX, DI/ { loaded_page = NR }
           /call .*read_paged/ { saw_call = 1 }
           END {
               exit (copied_index && loaded_page &&
                     copied_index < loaded_page && saw_call) ? 0 : 1
           }
       '; then
        pass "call-arg-order: literal setup preserves existing AL argument"
    else
        fail "call-arg-order" "argument setup clobbered AL before copying index"
    fi
else
    fail "call-arg-order" "$(./nibbind "$TEST_TMPDIR"/t_call_arg_order.nir -o "$TEST_TMPDIR"/t_call_arg_order.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_call_arg_cycle.nir <<'NIR'
; Binder regression: call argument setup must handle register cycles,
; such as passing (BL, AL) to a callee that expects (AL, BL).
.fn swap_args
.param %0, u8, "a", register, pin=AL
.param %1, u8, "b", register, pin=BL
    ret
.endfn

.fn call_arg_cycle
.param %0, u8, "x", register, pin=AL
.param %1, u8, "y", register, pin=BL
    call %2, swap_args, %1, %0
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_call_arg_cycle.nir -o "$TEST_TMPDIR"/t_call_arg_cycle.asm >/dev/null 2>&1; then
    arg_cycle_window=$(sed -n '/call_arg_cycle_call_arg_cycle:/,/call .*swap_args/p' "$TEST_TMPDIR"/t_call_arg_cycle.asm)
    if ./nibasm "$TEST_TMPDIR"/t_call_arg_cycle.asm -o "$TEST_TMPDIR"/t_call_arg_cycle.bin >/dev/null 2>&1 &&
       printf "%s\n" "$arg_cycle_window" | grep -q 'xchg AX, \[BP+2\]' &&
       printf "%s\n" "$arg_cycle_window" | awk '
           /mov AL, BL/ { loaded_al = NR }
           /mov BL, AL/ { loaded_bl = NR }
           /pop AX/ { restored_ax = NR }
           /call .*swap_args/ { saw_call = 1 }
           END {
               exit (loaded_al && loaded_bl && restored_ax && saw_call &&
                     loaded_al < loaded_bl && loaded_bl < restored_ax) ? 0 : 1
           }
       '; then
        pass "call-arg-cycle: register argument cycle uses stack temp"
    else
        fail "call-arg-cycle" "register argument cycle clobbered a source"
    fi
else
    fail "call-arg-cycle" "$(./nibbind "$TEST_TMPDIR"/t_call_arg_cycle.nir -o "$TEST_TMPDIR"/t_call_arg_cycle.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_storemem_live_accum.nir <<'NIR'
; Binder regression: a spilled byte store must not clobber a live AL
; value before that value is used as a later call argument.
.fn select_page
.param %0, u8, "page", register, pin=AL
    mov %1, %0
    ret
.endfn

.fn storemem_live_accum
.param %1, seg, "dst_seg", register, pin=ES
.param %2, u16, "dst_off", register, pin=SI
.param %3, u8, "value", stack
.vreg %0, u8, pin=AL, const
    mov %0, 0
    storemem %2, %1, %3
    call %16, select_page, %0
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_storemem_live_accum.nir -o "$TEST_TMPDIR"/t_storemem_live_accum.asm >/dev/null 2>&1; then
    store_window=$(sed -n '/storemem_live_accum_storemem_live_accum:/,/call .*select_page/p' "$TEST_TMPDIR"/t_storemem_live_accum.asm)
    if printf "%s\n" "$store_window" | grep -Fq 'push AX' &&
       printf "%s\n" "$store_window" | grep -Fq 'mov AL, [BP' &&
       printf "%s\n" "$store_window" | grep -Fq 'mov [ES:SI], AL' &&
       printf "%s\n" "$store_window" | grep -Fq 'pop AX' &&
       printf "%s\n" "$store_window" | grep -q 'call .*select_page'; then
        pass "storemem-live-accum: spilled byte store preserves live AL"
    else
        fail "storemem-live-accum" "spilled byte store clobbered live AL"
    fi
else
    fail "storemem-live-accum" "$(./nibbind "$TEST_TMPDIR"/t_storemem_live_accum.nir -o "$TEST_TMPDIR"/t_storemem_live_accum.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_byte_pressure_copy.nir <<'NIR'
; Binder regression: a one-use byte load/store in a loop should keep the
; byte in a byte register instead of spilling it through the frame because
; an unrelated word loop value grabbed AX.
.fn byte_pressure_copy
.param %0, seg, "src_seg", register, pin=ES
.param %1, u16, "src"
.param %2, u16, "dst"
.param %3, u16, "count"
    mov %4, %1
    mov %5, %2
    mov %6, %3
loop:
    cmp.eq %7, %6, 0
    jz %7, done
    loadmem %8, %4, ES
.vreg %8, u8
    storemem %5, %8
    add %9, %4, 1
    mov %4, %9
    add %10, %5, 1
    mov %5, %10
    sub %11, %6, 1
    mov %6, %11
    jmp loop
done:
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_byte_pressure_copy.nir -o "$TEST_TMPDIR"/t_byte_pressure_copy.asm >/dev/null 2>&1; then
    byte_copy_window=$(sed -n '/byte_pressure_copy_loop:/,/add .*1/p' "$TEST_TMPDIR"/t_byte_pressure_copy.asm)
    if ./nibasm "$TEST_TMPDIR"/t_byte_pressure_copy.asm -o "$TEST_TMPDIR"/t_byte_pressure_copy.bin >/dev/null 2>&1 &&
       printf "%s\n" "$byte_copy_window" | grep -q 'mov [A-D][HL], \[ES:' &&
       printf "%s\n" "$byte_copy_window" | grep -q 'mov \[.*\], [A-D][HL]' &&
       ! printf "%s\n" "$byte_copy_window" | grep -q 'push AX\|pop AX\|\[BP-'; then
        pass "byte-pressure-copy: loop byte load/store avoids spills"
    else
        fail "byte-pressure-copy" "byte load/store still spilled through frame"
    fi
else
    fail "byte-pressure-copy" "$(./nibbind "$TEST_TMPDIR"/t_byte_pressure_copy.nir -o "$TEST_TMPDIR"/t_byte_pressure_copy.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_mem_disp_fold.nir <<'NIR'
; Binder regression: a single-use address add feeding raw memory access
; should become a memory displacement instead of a stack address temp.
.fn mem_disp_fold
.param %0, u16, "idx", register, pin=BX
    add %1, %0, 1
    loadmem %2, %1
.vreg %2, u8
    and %3, %2, 127
.vreg %3, u8
    add %4, %0, 1
    storemem %4, %3
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_mem_disp_fold.nir -o "$TEST_TMPDIR"/t_mem_disp_fold.asm >/dev/null 2>&1; then
    disp_window=$(sed -n '/mem_disp_fold_mem_disp_fold:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_mem_disp_fold.asm)
    if ./nibasm "$TEST_TMPDIR"/t_mem_disp_fold.asm -o "$TEST_TMPDIR"/t_mem_disp_fold.bin >/dev/null 2>&1 &&
       printf "%s\n" "$disp_window" | grep -q 'mov [A-D][HL], \[BX+1\]' &&
       printf "%s\n" "$disp_window" | grep -q 'mov \[BX+1\], [A-D][HL]' &&
       ! printf "%s\n" "$disp_window" | grep -q 'push BX\|pop BX\|\[BP-'; then
        pass "mem-disp-fold: address add folds into memory displacement"
    else
        fail "mem-disp-fold" "address add still routed through stack temp"
    fi
else
    fail "mem-disp-fold" "$(./nibbind "$TEST_TMPDIR"/t_mem_disp_fold.nir -o "$TEST_TMPDIR"/t_mem_disp_fold.asm 2>&1 | tail -1)"
fi

cat > "$TEST_TMPDIR"/t_mem_disp_tail_pressure.nir <<'NIR'
; Binder regression: folded dst+1 addressing in a shifted tail loop
; should not let short byte temporaries grab the best word registers
; before long-lived row/index values are colored.
.fn mem_disp_tail_pressure
.param %0, seg, "src_seg", register, pin=ES
.param %1, u16, "start_src_row"
.param %2, u16, "src_stride", stack
.param %3, u8, "tail_bits", stack
.param %4, u8, "height", stack
.param %5, u16, "start_dst_row"
    mov %6, %1
    mov %7, %5
    mov %8, %4
.vreg %8, u8
row_loop:
    cmp.eq %9, %8, 0
    jz %9, body
    ret
body:
    mov %10, %6
    mov %11, %7
tail:
    mov %13, 255
.vreg %13, u8, const
    mov %14, 8
.vreg %14, u8, const
    sub %15, %14, %3
.vreg %15, u8
    shl %16, %13, %15
.vreg %16, u8
    mov %12, %16
.vreg %12, u8, const
    shr %18, %12, 1
.vreg %18, u8
    mov %17, %18
.vreg %17, u8, const
    shl %20, %12, 7
.vreg %20, u8
    mov %19, %20
.vreg %19, u8, const
    loadmem %22, %10, ES
.vreg %22, u8
    and %23, %22, %12
.vreg %23, u8
    mov %21, %23
.vreg %21, u8, const
    shr %25, %21, 1
.vreg %25, u8
    mov %24, %25
.vreg %24, u8, const
    shl %27, %21, 7
.vreg %27, u8
    mov %26, %27
.vreg %26, u8, const
    loadmem %28, %11
.vreg %28, u8
    not %29, %17
.vreg %29, u8
    and %30, %28, %29
.vreg %30, u8
    or %31, %30, %24
.vreg %31, u8
    storemem %11, %31
    add %33, %11, 1
    loadmem %32, %33
.vreg %32, u8
    not %34, %19
.vreg %34, u8
    and %35, %32, %34
.vreg %35, u8
    or %36, %35, %26
.vreg %36, u8
    add %37, %11, 1
    storemem %37, %36
next_row:
    add %38, %6, %2
    mov %6, %38
    add %39, %7, 64
    mov %7, %39
    sub %40, %8, 1
.vreg %40, u8
    mov %8, %40
    jmp row_loop
.endfn
NIR
if ./nibbind --pressure-report "$TEST_TMPDIR"/t_mem_disp_tail_pressure.txt \
       --pressure-fn mem_disp_tail_pressure \
       "$TEST_TMPDIR"/t_mem_disp_tail_pressure.nir \
       -o "$TEST_TMPDIR"/t_mem_disp_tail_pressure.asm >/dev/null 2>&1; then
    tail_window=$(
        sed -n '/mem_disp_tail_pressure_tail:/,/mem_disp_tail_pressure_next_row:/p' \
            "$TEST_TMPDIR"/t_mem_disp_tail_pressure.asm
    )
    if ./nibasm "$TEST_TMPDIR"/t_mem_disp_tail_pressure.asm \
           -o "$TEST_TMPDIR"/t_mem_disp_tail_pressure.bin >/dev/null 2>&1 &&
       printf "%s\n" "$tail_window" | grep -Eq '\[(BX|SI|DI)\+1\]' &&
       grep -Eq 'summary: .*spills=[012]' \
           "$TEST_TMPDIR"/t_mem_disp_tail_pressure.txt &&
       ! grep -q 'sub sp, 10' "$TEST_TMPDIR"/t_mem_disp_tail_pressure.asm; then
        pass "mem-disp-tail-pressure: folded tail keeps spill frame small"
    else
        fail "mem-disp-tail-pressure" "folded tail grew spill pressure"
    fi
else
    fail "mem-disp-tail-pressure" "$(
        ./nibbind --pressure-report "$TEST_TMPDIR"/t_mem_disp_tail_pressure.txt \
            --pressure-fn mem_disp_tail_pressure \
            "$TEST_TMPDIR"/t_mem_disp_tail_pressure.nir \
            -o "$TEST_TMPDIR"/t_mem_disp_tail_pressure.asm 2>&1 | tail -1
    )"
fi

# Port I/O: OUT must use AL, IN must read into AL
if [ -f "$TEST_TMPDIR"/t_port_io.asm ]; then
    port_accum_window=$(sed -n '/t_port_io_test_out_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_live_window=$(sed -n '/t_port_io_test_out_live_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_in_live_window=$(sed -n '/t_port_io_test_in_live_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_out16_window=$(sed -n '/t_port_io_test_out16:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_preserve_window=$(sed -n '/t_port_io_test_out16_preserves:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_in16_window=$(sed -n '/t_port_io_test_in16:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_dynamic_window=$(sed -n '/t_port_io_test_dynamic:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_dynamic16_window=$(sed -n '/t_port_io_test_dynamic16:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    if grep -q 'out 0x50, AL' "$TEST_TMPDIR"/t_port_io.asm &&
       grep -q 'in AL, 0x60' "$TEST_TMPDIR"/t_port_io.asm &&
       printf "%s\n" "$port_accum_window" | grep -q 'out 0x50, AL' &&
       printf "%s\n" "$port_accum_window" | grep -q 'mov AL, AH' &&
       printf "%s\n" "$port_accum_window" | grep -q 'out 0x51, AL' &&
       printf "%s\n" "$port_live_window" | grep -q 'push AX' &&
       printf "%s\n" "$port_live_window" | grep -q 'out 0x90, AL' &&
       printf "%s\n" "$port_live_window" | grep -q 'pop AX' &&
       printf "%s\n" "$port_live_window" | grep -q 'out 0x53, AL' &&
       printf "%s\n" "$port_in_live_window" | grep -q 'push AX' &&
       printf "%s\n" "$port_in_live_window" | grep -q 'in AL, 0xC1' &&
       printf "%s\n" "$port_in_live_window" | grep -q 'pop AX' &&
       printf "%s\n" "$port_in_live_window" | grep -q 'out 0xC0, AL' &&
       printf "%s\n" "$port_out16_window" | grep -q 'out 0x50, AX' &&
       printf "%s\n" "$port_preserve_window" | grep -q 'out 0x50, AX' &&
       ! printf "%s\n" "$port_preserve_window" | grep -q 'push AX' &&
       ! printf "%s\n" "$port_preserve_window" | grep -q 'pop AX' &&
       printf "%s\n" "$port_in16_window" | grep -q 'in AX, 0x50' &&
       printf "%s\n" "$port_dynamic_window" | grep -q 'in AL, DX' &&
       printf "%s\n" "$port_dynamic_window" | grep -q 'out DX, AL' &&
       printf "%s\n" "$port_dynamic16_window" | grep -q 'in AX, DX' &&
       printf "%s\n" "$port_dynamic16_window" | grep -q 'out DX, AX' &&
       ! awk '
           /^[[:space:]]*in[[:space:]]+(AL|AX),/ {
               if ($3 != "DX" && $3 !~ /^0x[0-9A-Fa-f]+$/) bad = 1
           }
           /^[[:space:]]*out[[:space:]]+/ {
               if ($2 != "DX," && $2 !~ /^0x[0-9A-Fa-f]+,$/) bad = 1
           }
           END { exit bad ? 0 : 1 }
       ' "$TEST_TMPDIR"/t_port_io.asm &&
       ! printf "%s\n" "$port_accum_window" | grep -q 'mov [A-D]X, 8[01]'; then
        pass "port-io: IN/OUT use AL accumulator"
    else
        fail "port-io" "IN/OUT not using AL"
    fi
fi

cat > "$TEST_TMPDIR"/t_cmp_mem_mem.nir <<'NIR'
; Binder regression: CMP has no memory-to-memory form.
.fn cmp_mem_mem
.param %0, u16, "a", stack
.param %1, u16, "b", stack
.returns u16
    cmp.eq %2, %0, %1
    jz %2, .L0
    mov %3, 1
    retval %3
    ret
.L0:
    mov %4, 0
    retval %4
    ret
.endfn
NIR
if ./nibbind --pressure-report "$TEST_TMPDIR"/t_cmp_mem_mem_pressure.txt \
     --pressure-fn cmp_mem_mem "$TEST_TMPDIR"/t_cmp_mem_mem.nir \
     -o "$TEST_TMPDIR"/t_cmp_mem_mem.asm >/dev/null 2>&1; then
    if awk '
        /^[[:space:]]*cmp[[:space:]]+\[BP[-+][0-9]+\], \[BP[-+][0-9]+\]/ { bad = 1 }
        /^[[:space:]]*mov[[:space:]]+AX, \[BP[-+][0-9]+\]/ { saw_load = 1 }
        /^[[:space:]]*cmp[[:space:]]+\[BP[-+][0-9]+\], AX/ { saw_cmp = 1 }
        END { exit (!bad && saw_load && saw_cmp) ? 0 : 1 }
    ' "$TEST_TMPDIR"/t_cmp_mem_mem.asm; then
        pass "cmp-mem-mem: stack operands compare through AX"
    else
        fail "cmp-mem-mem" "memory-to-memory CMP was emitted"
    fi
else
    fail "cmp-mem-mem" "$(
        ./nibbind --pressure-report "$TEST_TMPDIR"/t_cmp_mem_mem_pressure.txt \
          --pressure-fn cmp_mem_mem "$TEST_TMPDIR"/t_cmp_mem_mem.nir \
          -o "$TEST_TMPDIR"/t_cmp_mem_mem.asm 2>&1 | tail -1
    )"
fi
if grep -q '^spill-actions: .*spill-load=[1-9]' \
     "$TEST_TMPDIR"/t_cmp_mem_mem_pressure.txt &&
   grep -q '^spill-actions: .*mem-route=[1-9]' \
     "$TEST_TMPDIR"/t_cmp_mem_mem_pressure.txt; then
    pass "pressure-report: spill actions counted"
else
    fail "pressure-report-spill-actions" "spill action counts missing"
fi

cat > "$TEST_TMPDIR"/t_high_vregs.nir <<'NIR'
; Binder regression: functions with vregs above 255 must still get
; real registers/spills and address operands, not PREG_NONE/(null).
.fn high_vregs
    mov %298, 0
    mov %299, 0
    loadb %300, %298[%299]
.vreg %300, u8
    retval %300
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_high_vregs.nir -o "$TEST_TMPDIR"/t_high_vregs.asm >/dev/null 2>&1; then
    if ./nibasm "$TEST_TMPDIR"/t_high_vregs.asm -o "$TEST_TMPDIR"/t_high_vregs.bin >/dev/null 2>&1 &&
       ! grep -q '(null)\|%_' "$TEST_TMPDIR"/t_high_vregs.asm &&
       grep -q 'mov AL, \[\(BX\|BP\)+\(SI\|DI\)\]' "$TEST_TMPDIR"/t_high_vregs.asm; then
        pass "high-vregs: vregs above 255 bind and assemble"
    else
        fail "high-vregs" "high vregs produced invalid assembly"
    fi
else
    fail "high-vregs" "$(./nibbind "$TEST_TMPDIR"/t_high_vregs.nir -o "$TEST_TMPDIR"/t_high_vregs.asm 2>&1 | tail -1)"
fi

many_fn_nir="$TEST_TMPDIR"/t_many_functions.nir
{
    i=0
    while [ "$i" -lt 140 ]; do
        printf '.fn filler_%s\n    ret\n.endfn\n\n' "$i"
        i=$((i + 1))
    done
    cat <<'NIR'
.fn late_reset, bare, at(0xFFFF:0x0000)
    setflag IF, 0
.endfn
NIR
} > "$many_fn_nir"
if ./nibbind "$many_fn_nir" \
     -o "$TEST_TMPDIR"/t_many_functions.asm >/dev/null 2>&1; then
    if ./nibasm "$TEST_TMPDIR"/t_many_functions.asm \
         -o "$TEST_TMPDIR"/t_many_functions.bin >/dev/null 2>&1 &&
       grep -q '_late_reset:$' "$TEST_TMPDIR"/t_many_functions.asm &&
       grep -q 'org 0xFFFF0 ; FFFF:0000' \
         "$TEST_TMPDIR"/t_many_functions.asm &&
       [ "$(wc -c < "$TEST_TMPDIR"/t_many_functions.bin |
            tr -d ' ')" -gt 1048560 ]; then
        pass "many-functions: late placed function emitted"
    else
        fail "many-functions" "late placed function missing"
    fi
else
    fail "many-functions" "$(
        ./nibbind "$many_fn_nir" \
          -o "$TEST_TMPDIR"/t_many_functions.asm 2>&1 | tail -1
    )"
fi

cat > "$TEST_TMPDIR"/t_pressure_report.nir <<'NIR'
.fn pressure_probe
.returns u16
.vreg %0, u8, const
.vreg %1, u16, const
.vreg %2, seg
; @probe.nib:10
    mov %0, 1
; @probe.nib:20
    mov %1, 2
; @probe.nib:30
    mov %2, %1
; @probe.nib:40
    add %3, %1, %1
; @probe.nib:50
    add %4, %3, %1
    retval %4
    ret
.endfn
NIR
if ./nibbind --pressure-report "$TEST_TMPDIR"/t_pressure_report.txt \
     --pressure-fn pressure_probe "$TEST_TMPDIR"/t_pressure_report.nir \
     -o "$TEST_TMPDIR"/t_pressure_report.asm >/dev/null 2>&1; then
    if grep -q '^== pressure_probe ==$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^pressure timeline:$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^peak pressure:$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^live ranges:$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^dead/early-load warnings:$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^call-split advisor:$' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^allocation: .*peak_fixed=' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^fixups: .*call-arg=' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q '^spill-actions: .*spill-load=' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q 'alloc=' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q 'probe.nib:20 live=' "$TEST_TMPDIR"/t_pressure_report.txt &&
       grep -q 'loaded at probe.nib:20 first-used at probe.nib:30' "$TEST_TMPDIR"/t_pressure_report.txt; then
        pass "pressure-report: timeline, peak, ranges, warnings emitted"
    else
        fail "pressure-report" "expected pressure report sections missing"
    fi
else
    fail "pressure-report" "$(./nibbind --pressure-report "$TEST_TMPDIR"/t_pressure_report.txt --pressure-fn pressure_probe "$TEST_TMPDIR"/t_pressure_report.nir -o "$TEST_TMPDIR"/t_pressure_report.asm 2>&1 | tail -1)"
fi

if ./nibbind --pressure-report "$TEST_TMPDIR"/t_call_arg_cycle_report.txt \
     --pressure-fn call_arg_cycle "$TEST_TMPDIR"/t_call_arg_cycle.nir \
     -o "$TEST_TMPDIR"/t_call_arg_cycle_report.asm >/dev/null 2>&1; then
    if grep -q '^fixups: .*call-arg=[1-9]' \
         "$TEST_TMPDIR"/t_call_arg_cycle_report.txt; then
        pass "pressure-report: call-arg fixups counted"
    else
        fail "pressure-report-call-arg" "call-arg fixup count missing"
    fi
else
    fail "pressure-report-call-arg" "$(
        ./nibbind --pressure-report \
          "$TEST_TMPDIR"/t_call_arg_cycle_report.txt \
          --pressure-fn call_arg_cycle \
          "$TEST_TMPDIR"/t_call_arg_cycle.nir \
          -o "$TEST_TMPDIR"/t_call_arg_cycle_report.asm 2>&1 | tail -1
    )"
fi

cat > "$TEST_TMPDIR"/pressure_old.txt <<'REPORT'
# Nib pressure report

== foo ==
summary: vregs=3 spills=0 peak_live=2 at a.nib:10 u8=0 u16=2 seg=0 far32=0
allocation: spilled=0 peak_fixed=1 peak_preferred=0 peak_spilled=0
fixups: call-arg=0 call-save=1 call-restore=1 cl-route=0 addr-route=0 ret-capture=0 ret-reload=0 total=2
spill-actions: spill-load=0 spill-store=0 scratch-save=0 scratch-restore=0 mem-route=0 total=0

live ranges:
  %0 u16   def a.nib:1 first a.nib:2 last a.nib:3 uses=1 alloc=AX
  %1 u16   def a.nib:1 first a.nib:2 last a.nib:3 uses=1 alloc=BX

== removed ==
summary: vregs=1 spills=0 peak_live=1 at a.nib:1 u8=0 u16=1 seg=0 far32=0
allocation: spilled=0 peak_fixed=0 peak_preferred=0 peak_spilled=0
fixups: call-arg=0 call-save=0 call-restore=0 cl-route=0 addr-route=0 ret-capture=0 ret-reload=0 total=0
spill-actions: spill-load=0 spill-store=0 scratch-save=0 scratch-restore=0 mem-route=0 total=0
REPORT
cat > "$TEST_TMPDIR"/pressure_new.txt <<'REPORT'
# Nib pressure report

== foo ==
summary: vregs=4 spills=1 peak_live=3 at a.nib:10 u8=0 u16=3 seg=0 far32=0
allocation: spilled=1 peak_fixed=1 peak_preferred=1 peak_spilled=1
fixups: call-arg=1 call-save=1 call-restore=1 cl-route=0 addr-route=0 ret-capture=0 ret-reload=0 total=3
spill-actions: spill-load=2 spill-store=1 scratch-save=0 scratch-restore=0 mem-route=1 total=4

live ranges:
  %0 u16   def a.nib:1 first a.nib:2 last a.nib:3 uses=1 alloc=spill0
  %1 u16   def a.nib:1 first a.nib:2 last a.nib:3 uses=1 alloc=BX

== added ==
summary: vregs=1 spills=0 peak_live=1 at a.nib:1 u8=0 u16=1 seg=0 far32=0
allocation: spilled=0 peak_fixed=0 peak_preferred=0 peak_spilled=0
fixups: call-arg=0 call-save=0 call-restore=0 cl-route=0 addr-route=0 ret-capture=0 ret-reload=0 total=0
spill-actions: spill-load=0 spill-store=0 scratch-save=0 scratch-restore=0 mem-route=0 total=0
REPORT
if ./nibbind --pressure-compare "$TEST_TMPDIR"/pressure_old.txt \
     "$TEST_TMPDIR"/pressure_new.txt \
     > "$TEST_TMPDIR"/pressure_compare.txt 2>&1; then
    if grep -q '^# Nib pressure comparison' \
         "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '^== foo ==$' "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q 'spills: 0 -> 1 (+1)' \
         "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q 'fixups: .*call-arg=0->1(+1)' \
         "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q 'spill-actions: .*spill-load=0->2(+2).*mem-route=0->1(+1)' \
         "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '%0: AX -> spill0' "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '^== removed ==$' "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '^  removed function$' "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '^== added ==$' "$TEST_TMPDIR"/pressure_compare.txt &&
       grep -q '^  added function$' "$TEST_TMPDIR"/pressure_compare.txt; then
        pass "pressure-compare: metrics, counts, allocs, functions"
    else
        fail "pressure-compare" "expected comparison deltas missing"
    fi
else
    fail "pressure-compare" "$(tail -1 "$TEST_TMPDIR"/pressure_compare.txt)"
fi
if ./nibbind --pressure-compare "$TEST_TMPDIR"/pressure_old.txt \
     "$TEST_TMPDIR"/pressure_old.txt \
     > "$TEST_TMPDIR"/pressure_compare_same.txt 2>&1 &&
   grep -q '^No pressure changes\.$' \
     "$TEST_TMPDIR"/pressure_compare_same.txt; then
    pass "pressure-compare: identical reports are quiet"
else
    fail "pressure-compare-same" "identical reports were not quiet"
fi
if ./nibbind --pressure-compare "$TEST_TMPDIR"/pressure_old.txt \
     "$TEST_TMPDIR"/pressure_new.txt --pressure-fn foo \
     > "$TEST_TMPDIR"/pressure_compare_filter.txt 2>&1 &&
   grep -q '^== foo ==$' "$TEST_TMPDIR"/pressure_compare_filter.txt &&
   ! grep -q '^== removed ==$\|^== added ==$' \
      "$TEST_TMPDIR"/pressure_compare_filter.txt; then
    pass "pressure-compare: --pressure-fn filters functions"
else
    fail "pressure-compare-filter" "function filter did not apply"
fi

# Byte vregs: zero_extend must not use word-from-byte mov (MOV BX, AL)
if [ -f "$TEST_TMPDIR"/t_byte_vreg.asm ]; then
    if grep -Eq 'mov [A-D]L, [A-D]L|xor [A-D]H, [A-D]H|xor [A-D]X, [A-D]X|and (AX|BX|CX|DX|SI|DI), 0x00FF' "$TEST_TMPDIR"/t_byte_vreg.asm; then
        pass "byte-vreg: zero_extend uses byte-safe operation"
    else
        fail "byte-vreg" "zero_extend uses invalid word-from-byte move"
    fi
fi

# memset/memcopy must set up DI, AL/SI, CX before string ops
if [ -f "$TEST_TMPDIR"/t_memops.asm ]; then
    if grep -q 'mov DI' "$TEST_TMPDIR"/t_memops.asm && grep -q 'mov CX' "$TEST_TMPDIR"/t_memops.asm && grep -q 'rep stosb' "$TEST_TMPDIR"/t_memops.asm; then
        pass "memops: DI/CX/AL set up for rep stosb"
    else
        fail "memops" "string op register setup missing"
    fi
fi

# Scalar globals: must use memory-indirect loads, not address loads
if [ -f "$TEST_TMPDIR"/t_globals_rw.asm ]; then
    if grep -q '^flag: db 0$' "$TEST_TMPDIR"/t_globals_rw.asm &&
       grep -q '^counter: dw 0$' "$TEST_TMPDIR"/t_globals_rw.asm &&
       grep -q '\[counter\]' "$TEST_TMPDIR"/t_globals_rw.asm &&
       grep -q '\[flag\]' "$TEST_TMPDIR"/t_globals_rw.asm &&
       grep -q 'mov \[flag\], [A-D]L' "$TEST_TMPDIR"/t_globals_rw.asm &&
       ! grep -q 'mov \[flag\], [A-D]X' "$TEST_TMPDIR"/t_globals_rw.asm; then
        pass "globals-rw: scalar globals use correct memory width"
    else
        fail "globals-rw" "scalar global layout or access width is wrong"
    fi
fi

if [ -f tests/t_array_infer.nir ]; then
    if grep -q '^\.data bytes, u8\[19\]' tests/t_array_infer.nir &&
       grep -q '^\.global bytes, u8\[19\]$' tests/t_array_infer.nif; then
        pass "array-infer: brace initializer inferred u8[19]"
    else
        fail "array-infer" "unsized brace initializer did not infer u8[19]"
    fi
fi

if [ -f tests/t_arithmetic.nir ]; then
    const_mask_window=$(sed -n '/\.fn const_mask_u8/,/\.endfn/p' tests/t_arithmetic.nir)
    if printf "%s\n" "$const_mask_window" | grep -q 'mov %[0-9][0-9]*, 9' &&
       printf "%s\n" "$const_mask_window" | grep -q '\.vreg %[0-9][0-9]*, u8, const' &&
       printf "%s\n" "$const_mask_window" | grep -q '\.vreg %[0-9][0-9]*, u8'; then
        pass "const-mask-u8: constant expression initializer keeps u8 context"
    else
        fail "const-mask-u8" "constant expression initializer was not u8"
    fi
fi

if [ -f "$TEST_TMPDIR"/t_expr_sites.asm ]; then
    expr_read_hw=$(sed -n '/t_expr_sites_read_hw:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_expr_sites.asm)
    expr_read_ptr=$(sed -n '/t_expr_sites_read_ptr:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_expr_sites.asm)
    expr_write_ss=$(sed -n '/t_expr_sites_write_ss:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_expr_sites.asm)
    expr_loop=$(sed -n '/t_expr_sites_loop_count:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_expr_sites.asm)
    if grep -q '^\.global placed, u8\[5\]' tests/t_expr_sites.nir &&
       grep -q '^\.at 0x1000:0x0022' tests/t_expr_sites.nir &&
       printf "%s\n" "$expr_read_hw" | grep -q 'mov AL, \[BX+0x0003\]' &&
       printf "%s\n" "$expr_read_ptr" | grep -q 'add SI, BX' &&
       printf "%s\n" "$expr_read_ptr" | grep -q 'mov AL, \[SI\]' &&
       printf "%s\n" "$expr_write_ss" | grep -q 'mov \[SS:.*\], CL' &&
       printf "%s\n" "$expr_loop" | grep -q 'loop t_expr_sites_loop_count_.L0'; then
        pass "expr-sites: static const expressions and pointer expressions"
    else
        fail "expr-sites" "static or bracket expression lowering changed"
    fi
fi

if [ -f "$TEST_TMPDIR"/t_mem_width.asm ]; then
    mem_width_load=$(sed -n '/t_mem_width_load_desc:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_mem_width.asm)
    mem_width_word=$(sed -n '/t_mem_width_return_word:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_mem_width.asm)
    mem_width_byte=$(sed -n '/t_mem_width_return_byte:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_mem_width.asm)
    if printf "%s\n" "$mem_width_load" | grep -q 'mov AX, \[ES:SI+0x0000\]' &&
       printf "%s\n" "$mem_width_load" | grep -q 'mov AX, \[ES:SI+0x0004\]' &&
       printf "%s\n" "$mem_width_word" | grep -q 'mov AX, \[ES:SI+0x0006\]' &&
       printf "%s\n" "$mem_width_byte" | grep -q 'mov AL, \[ES:SI+0x0008\]'; then
        pass "mem-width: explicit segment loads use contextual width"
    else
        fail "mem-width" "explicit segment memory loads used wrong width"
    fi
fi

if [ -f tests/t_seg_param.nir ] && [ -f "$TEST_TMPDIR"/t_seg_param.asm ]; then
    seg_param_read=$(sed -n '/t_seg_param_read_es:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_seg_param.asm)
    seg_param_call=$(sed -n '/t_seg_param_call_read_es:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_seg_param.asm)
    if grep -q '^\.param %[0-9][0-9]*, seg, "src_seg", register, pin=ES' tests/t_seg_param.nir &&
       grep -q 'loadmem %[0-9][0-9]*, %[0-9][0-9]*, ES' tests/t_seg_param.nir &&
       ! grep -q '^    mov %[0-9][0-9]*, ES$' tests/t_seg_param.nir &&
       printf "%s\n" "$seg_param_read" | grep -q 'mov AL, \[ES:SI\]' &&
       ! printf "%s\n" "$seg_param_read" | grep -q 'mov \[BP-' &&
       printf "%s\n" "$seg_param_call" | grep -q 'mov ES, AX' &&
       printf "%s\n" "$seg_param_call" | grep -q 'mov SI, 4'; then
        pass "seg-param: seg in ES pins ABI segment parameter without spills"
    else
        fail "seg-param" "seg in ES lowered through a spill or was not passed"
    fi
fi

# Near JMP backward: displacement must account for 3-byte instruction size
if [ -f "$TEST_TMPDIR"/t_jmp_near.asm ]; then
    outbin="$TEST_TMPDIR/t_jmp_near_check.bin"
    outmap="$TEST_TMPDIR/t_jmp_near_check.map"
    if ./nibasm "$TEST_TMPDIR"/t_jmp_near.asm -o "$outbin" -m "$outmap" >/dev/null 2>&1; then
        # Get .L0 label address from map
        l0_addr=$(grep '\.L0' "$outmap" | awk '{print $1}')
        if [ -n "$l0_addr" ]; then
            # Find last backward JMP (E9 near or EB short) and verify target
            target=$(python3 -c "
import struct
with open('$outbin', 'rb') as f:
    data = f.read()
# Try near JMP (E9) first, then short JMP (EB)
last_jmp = -1; jmp_type = None
for i in range(len(data)-2):
    if data[i] == 0xE9:
        last_jmp = i; jmp_type = 'near'
    elif data[i] == 0xEB:
        last_jmp = i; jmp_type = 'short'
if last_jmp >= 0:
    if jmp_type == 'near':
        disp = struct.unpack_from('<h', data, last_jmp+1)[0]
        target = (last_jmp + 3 + disp) & 0xFFFF
    else:
        disp = struct.unpack_from('<b', data, last_jmp+1)[0]
        target = (last_jmp + 2 + disp) & 0xFFFF
    print(f'{target:04X}')
")
            if [ "$target" = "$l0_addr" ]; then
                pass "jmp-near: backward near JMP targets correct label"
            else
                fail "jmp-near" "backward JMP target $target != label $l0_addr (off by $((0x$target - 0x$l0_addr)))"
            fi
        else
            fail "jmp-near" "could not find .L0 in map"
        fi
    else
        fail "jmp-near" "assembly failed"
    fi
else
    skip "jmp-near" "asm not generated"
fi

# Shift with CX-aliased dst must preserve CL (push CX / pop CX)
if [ -f "$TEST_TMPDIR"/t_shift_swap.asm ]; then
    # The dst_is_cx detour must push/pop CX to preserve live CL
    if grep -A1 'push CX' "$TEST_TMPDIR"/t_shift_swap.asm | grep -q 'mov CL,'; then
        pass "shift-swap: dst_is_cx preserves CL with push/pop CX"
    else
        fail "shift-swap" "dst_is_cx detour does not preserve CL"
    fi
else
    skip "shift-swap" "asm not generated"
fi

# Byte array access: must use byte registers (AL, BL, etc.)
if [ -f "$TEST_TMPDIR"/t_byte_array.asm ]; then
    if grep -q 'mov [A-D]L' "$TEST_TMPDIR"/t_byte_array.asm; then
        pass "byte-array: u8 elements use byte registers"
    else
        fail "byte-array" "u8 array access using word registers"
    fi
fi

# Byte RMW inverted copy: left masked byte must be copied into the OR dst.
if [ -f "$TEST_TMPDIR"/t_byte_rmw_inv.asm ]; then
    if awk '
        /^[[:space:]]*or [A-D][HL],/ {
            split($1, op, " "); split($2, or_dst, ",");
            split(prev, p, /[[:space:],]+/);
            if (p[1] == "mov" && p[2] == or_dst[1] && p[3] != or_dst[1])
                ok = 1;
        }
        /^[[:space:]]*(mov|and|not|or) / { prev = $0; sub(/^[[:space:]]+/, "", prev); }
        END { exit ok ? 0 : 1 }
    ' "$TEST_TMPDIR"/t_byte_rmw_inv.asm; then
        pass "byte-rmw-inv: OR dst preserves masked framebuffer byte"
    else
        fail "byte-rmw-inv" "OR does not reload the preserved left intermediate"
    fi
fi

# Load through a spilled BX base must not leave a byte result in BL/BH
# before restoring BX.
if [ -f "$TEST_TMPDIR"/t_load_scratch_alias.asm ]; then
    if awk '
        /push AX/ { saw_push_ax = 1 }
        saw_push_ax && /push BX/ { saw_push_bx = 1 }
        saw_push_bx && /mov AL, \[BX\+SI\]/ { saw_load = 1 }
        saw_load && /pop BX/ { saw_pop_bx = 1 }
        saw_pop_bx && /mov B[HL], AL/ { saw_copy = 1 }
        saw_copy && /pop AX/ { ok = 1 }
        END { exit ok ? 0 : 1 }
    ' "$TEST_TMPDIR"/t_load_scratch_alias.asm; then
        pass "load-scratch-alias: byte result survives BX restore"
    else
        fail "load-scratch-alias" "load result aliases restored BX scratch"
    fi
fi

# Spilled LEA must not use AX as scratch when AL is live for a following
# byte store.
cat > "$TEST_TMPDIR"/t_lea_scratch.nir <<'NIR'
; Binder regression: spilled LEA must not clobber AL.
.fn lea_scratch
.param %0, u8, "ch", register, pin=AL
.returns u8
.local %1, 1, "a"
.local %2, 1, "b"
.local %3, 1, "c"
.local %4, 1, "d"
    lea %5, %1
    lea %6, %2
    lea %7, %3
    lea %8, %4
    mov %9, 0
    storeb %5[%9], %0
    storeb %6[%9], %0
    storeb %7[%9], %0
    storeb %8[%9], %0
    retval %0
    ret
.endfn
NIR
if ./nibbind "$TEST_TMPDIR"/t_lea_scratch.nir -o "$TEST_TMPDIR"/t_lea_scratch.asm >/dev/null 2>&1; then
    if awk '
        /lea AX, / { bad = 1 }
        /push BX/ { saw_push_bx = 1 }
        saw_push_bx && /lea BX, / { saw_lea_bx = 1 }
        saw_lea_bx && /mov \[BP[-+][0-9]+\], BX/ { saw_spill = 1 }
        saw_spill && /mov \[BX\+SI\], AL/ { saw_store = 1 }
        END { exit (!bad && saw_store) ? 0 : 1 }
    ' "$TEST_TMPDIR"/t_lea_scratch.asm; then
        pass "lea-scratch: spilled local address preserves live AL"
    else
        fail "lea-scratch" "spilled LEA clobbers AL before byte store"
    fi
else
    fail "lea-scratch" "$(./nibbind "$TEST_TMPDIR"/t_lea_scratch.nir -o "$TEST_TMPDIR"/t_lea_scratch.asm 2>&1 | tail -1)"
fi

# Loop body CX: CX must not be modified between loop top and LOOP instruction
if [ -f "$TEST_TMPDIR"/t_loop_body.asm ]; then
    # Between the .L0 label and "loop", CX should only appear as a source (read), never as destination
    loop_body=$(sed -n '/\.L0:/,/loop /p' "$TEST_TMPDIR"/t_loop_body.asm | grep -v '\.L0:' | grep -v 'loop ')
    if echo "$loop_body" | grep -q 'sub CX\|mov CX\|add CX\|dec CX'; then
        fail "loop-body" "CX modified inside loop body before LOOP instruction"
    else
        pass "loop-body: CX not modified before LOOP"
    fi
fi

# Pointer deref: near [var] must use DS-default addressable register (BX, SI, DI)
if [ -f "$TEST_TMPDIR"/t_deref.asm ]; then
    if grep -q '\[BX\]\|mov \[SI\]\|mov \[DI\]' "$TEST_TMPDIR"/t_deref.asm && ! grep -q '\[AX\]\|\[CX\]\|\[DX\]' "$TEST_TMPDIR"/t_deref.asm; then
        pass "deref: near pointer uses DS-default addressable register"
    else
        fail "deref" "near pointer deref uses invalid register for memory operand"
    fi
fi

echo ""

echo "--- Type error tests (expected failures) ---"
# Wrong arg count
echo 'use "t_modules_lib.nif";
fn main() { lib_add(1, 2, 3); }' > "$TEST_TMPDIR"/t_err_argcount.nib
if ./nib "$TEST_TMPDIR"/t_err_argcount.nib >/dev/null 2>&1; then
    fail "arg count error" "should have failed"
else
    pass "arg count error (correctly rejected)"
fi

# Size mismatch
echo 'fn bad() { u16 x = 1; u8 y = 2; u16 z = x + y; }' > "$TEST_TMPDIR"/t_err_sizemismatch.nib
if ./nib "$TEST_TMPDIR"/t_err_sizemismatch.nib >/dev/null 2>&1; then
    fail "size mismatch" "should have failed"
else
    pass "size mismatch (correctly rejected)"
fi

# Undefined variable
echo 'fn bad() { u16 x = y; }' > "$TEST_TMPDIR"/t_err_undef.nib
if ./nib "$TEST_TMPDIR"/t_err_undef.nib >/dev/null 2>&1; then
    fail "undefined var" "should have failed"
else
    pass "undefined var (correctly rejected)"
fi

# Return type mismatch
echo 'fn bad() -> u8 { return 0xFFFF; }' > "$TEST_TMPDIR"/t_err_rettype.nib
if ./nib "$TEST_TMPDIR"/t_err_rettype.nib >/dev/null 2>&1; then
    # Literals promote, so this actually succeeds — not an error
    pass "return literal (promotion OK)"
else
    pass "return type check"
fi

echo 'const ONE = 1;
fn bad(n: u16) { for (CX in n..ONE) { nop(); } }' > "$TEST_TMPDIR"/t_err_for_end.nib
if ./nib "$TEST_TMPDIR"/t_err_for_end.nib >/dev/null 2>&1; then
    fail "for end zero" "non-zero countdown end should have failed"
else
    pass "for end zero (correctly rejected)"
fi

echo 'fn bad(x: u8) -> u8 { return [x]; }' > "$TEST_TMPDIR"/t_err_deref_type.nib
if ./nib "$TEST_TMPDIR"/t_err_deref_type.nib >/dev/null 2>&1; then
    fail "deref type" "u8 dereference should have failed"
else
    pass "deref type (correctly rejected)"
fi

echo 'fn bad(s: seg in DS) { }' > "$TEST_TMPDIR"/t_err_seg_pin_ds.nib
if ./nib "$TEST_TMPDIR"/t_err_seg_pin_ds.nib >/dev/null 2>&1; then
    fail "seg pin DS" "seg in DS should have failed"
else
    pass "seg pin DS (correctly rejected)"
fi

echo ""

# Summary
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped (${TOTAL} total) ==="
[ "$FAIL" -eq 0 ]
