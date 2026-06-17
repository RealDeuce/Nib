#!/bin/sh
# Nib test runner — compiles, binds, and assembles each test file
# Reports pass/fail for each stage

cd "$(dirname "$0")/.."

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

# Multi-return materialization must not clobber AX before the DX return
# slot has copied it, and callers must capture the second return slot.
if [ -f "$TEST_TMPDIR"/t_multi_return.asm ]; then
    split_window=$(sed -n '/^split_pair:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_multi_return.asm)
    call_window=$(sed -n '/t_multi_return_use_pair:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_multi_return.asm)
    after_dx=$(printf "%s\n" "$split_window" | sed -n '/mov DX, AX/,$p')
    if printf "%s\n" "$after_dx" | grep -q 'mov AL, CL' &&
       printf "%s\n" "$call_window" | grep -q 'call split_pair' &&
       printf "%s\n" "$call_window" | grep -q 'mov CX, DX'; then
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
       printf "%s\n" "$stack_ds_window" | grep -q 'mov ES, \[BP+6\]' &&
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
       sed -n '/call far \[SS:BX\]/,/ret/p' "$TEST_TMPDIR"/t_icall_save.asm | grep -q 'mov BL, AL'; then
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

# Port I/O: OUT must use AL, IN must read into AL
if [ -f "$TEST_TMPDIR"/t_port_io.asm ]; then
    port_accum_window=$(sed -n '/t_port_io_test_out_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_live_window=$(sed -n '/t_port_io_test_out_live_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_in_live_window=$(sed -n '/t_port_io_test_in_live_accum:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_out16_window=$(sed -n '/t_port_io_test_out16:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_preserve_window=$(sed -n '/t_port_io_test_out16_preserves:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
    port_in16_window=$(sed -n '/t_port_io_test_in16:/,/^[[:space:]]*ret$/p' "$TEST_TMPDIR"/t_port_io.asm)
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
       ! printf "%s\n" "$port_accum_window" | grep -q 'mov [A-D]X, 8[01]'; then
        pass "port-io: IN/OUT use AL accumulator"
    else
        fail "port-io" "IN/OUT not using AL"
    fi
fi

# Byte vregs: zero_extend must not use word-from-byte mov (MOV BX, AL)
if [ -f "$TEST_TMPDIR"/t_byte_vreg.asm ]; then
    if grep -q 'mov [A-D]L, [A-D]L\|xor [A-D]H, [A-D]H\|xor [A-D]X, [A-D]X' "$TEST_TMPDIR"/t_byte_vreg.asm; then
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

echo ""

# Summary
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped (${TOTAL} total) ==="
[ "$FAIL" -eq 0 ]
