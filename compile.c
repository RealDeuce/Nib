/*
 * compile.c — Nib compiler backend
 *
 * Takes a parsed AST and emits:
 *   .nir — pseudo-assembly with virtual registers
 *   .nif — interface file (function signatures for cross-module use)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "ast.h"
#include "compile.h"
#include "table.h"

/* ================================================================
 * Symbol table / scoping
 * ================================================================ */

static abi_place_t effective_param_place(param_t *p) {
    if (!p)
        return ABI_PLACE_DEFAULT;
    if (p->has_pin || p->has_seg_pin)
        return ABI_PLACE_REGISTER;
    if (p->place != ABI_PLACE_DEFAULT)
        return p->place;
    if (p->type && p->type->kind == TYPE_FAR)
        return ABI_PLACE_STACK;
    return ABI_PLACE_DEFAULT;
}

static abi_place_t effective_return_place(return_t *r) {
    if (!r)
        return ABI_PLACE_DEFAULT;
    if (r->has_pin)
        return ABI_PLACE_REGISTER;
    if (r->place != ABI_PLACE_DEFAULT)
        return r->place;
    if (r->type && r->type->kind == TYPE_FAR)
        return ABI_PLACE_STACK;
    return ABI_PLACE_DEFAULT;
}

static void emit_abi_place(FILE *f, abi_place_t place) {
    if (place == ABI_PLACE_STACK)
        fprintf(f, ", stack");
    else if (place == ABI_PLACE_REGISTER)
        fprintf(f, ", register");
}

typedef struct symbol {
    char        name[64];
    type_t     *type;
    int         vreg;           /* virtual register ID (%0, %1, ...) */
    int         vreg_seg;       /* segment vreg for far params (-1 = none) */
    bool        is_pinned;      /* variable name matches a register */
    int         pinned_reg;
    reg_class_t pin_class;
    bool        is_global;
    bool        is_const;       /* const qualifier — prevents reassignment */
    bool        has_at;         /* global with at() placement */
    int         at_seg;
    int         at_off;
    bool        is_init_data;   /* initialized global emitted as a .data block */
    bool        is_stack_local; /* source local stored in this function frame */
    int         stack_size;
} symbol_t;

typedef NIB_VEC(symbol_t) symbol_vec_t;

typedef struct scope {
    symbol_vec_t syms;
    struct scope *parent;
} scope_t;

typedef struct {
    char    name[64];
    int     nparams;        /* source-level param count */
    int     nparams_ir;     /* IR param count (far splits add 1) */
    type_t *return_type;
    int     nreturns;
    type_t **return_types;
    type_t **param_types;
    bool    *param_is_far;  /* which params are far type */
} function_sig_t;

typedef struct {
    char    name[64];
    field_t *fields;
    bool    aligned;
} struct_sig_t;

typedef struct {
    char name[64];
    int  value;
} const_sig_t;

typedef NIB_VEC(function_sig_t) function_vec_t;
typedef NIB_VEC(struct_sig_t) struct_vec_t;
typedef NIB_VEC(const_sig_t) const_vec_t;
typedef NIB_VEC(char *) str_vec_t;
typedef NIB_VEC(int) int_vec_t;
typedef NIB_VEC(bool) bool_vec_t;
typedef NIB_VEC(type_t *) type_ptr_vec_t;

typedef struct {
    int vreg;
    int vreg_seg;
    type_t *type;
    expr_t *expr;
} call_arg_t;

typedef NIB_VEC(call_arg_t) call_arg_vec_t;

/* ================================================================
 * Compiler state
 * ================================================================ */

typedef struct {
    FILE       *nir;            /* .nir output */
    FILE       *nif;            /* .nif output */
    scope_t    *scope;          /* current scope */
    int         next_vreg;      /* virtual register counter per function */
    int         next_label;     /* label counter for branches */
    int         errors;
    int         loop_depth;     /* for break/continue validation */
    int         loop_break_label;   /* label to jump to for break */
    int         loop_continue_label; /* label to jump to for continue */
    char        src_dir[256];       /* directory of source file for use resolution */
    char        src_file[256];      /* source filename for debug info */
    int         last_emitted_line;  /* avoid duplicate line comments */
    int         next_const;         /* constant pool counter */

    /* Current function info for .nif emission */
    const char *cur_fn_name;
    param_t    *cur_fn_params;
    type_t     *cur_fn_ret;
    return_t   *cur_fn_returns;
    int         cur_fn_nreturns;
    fn_modifiers_t cur_fn_mods;

    function_vec_t functions;  /* Known functions (for call checking) */
    struct_vec_t structs;      /* Known structs */
    const_vec_t constants;     /* Named constants */
    str_vec_t isr_names;       /* Interrupt handlers as far32 constants */
    str_vec_t addr_taken;      /* Address-taken locals in current function */
} compiler_t;

static compiler_t C;

static char *xstrdup_checked(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = nib_xmalloc(len, "string");
    memcpy(p, s, len);
    return p;
}

/* ---- Error reporting ---- */

static void cerr(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "line %d: error: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    C.errors++;
}

/* ---- Scope management ---- */

static void push_scope(void) {
    scope_t *s = calloc(1, sizeof(scope_t));
    if (!s) {
        fprintf(stderr, "out of memory allocating scope\n");
        exit(1);
    }
    NIB_VEC_INIT(&s->syms);
    s->parent = C.scope;
    C.scope = s;
}

static void pop_scope(void) {
    scope_t *old = C.scope;
    C.scope = old->parent;
    NIB_VEC_FREE(&old->syms);
    free(old);
}

static symbol_t *sym_lookup(const char *name) {
    for (scope_t *s = C.scope; s; s = s->parent)
        for (int i = 0; i < s->syms.len; i++)
            if (strcmp(s->syms.items[i].name, name) == 0)
                return &s->syms.items[i];
    return NULL;
}

static symbol_t *sym_add(const char *name, type_t *type, bool is_global) {
    symbol_t *sym = NIB_VEC_PUSH(&C.scope->syms, "symbols");
    memset(sym, 0, sizeof(*sym));
    strncpy(sym->name, name, 63);
    sym->name[63] = '\0';
    sym->type = type;
    sym->vreg = C.next_vreg++;
    sym->vreg_seg = -1;
    sym->is_global = is_global;
    sym->is_const = false;
    sym->is_pinned = false;
    sym->pinned_reg = REG_NONE;
    sym->is_stack_local = false;
    sym->stack_size = 0;
    sym->is_init_data = false;
    return sym;
}

static symbol_t *sym_add_pinned(type_t *type, int reg, reg_class_t rc) {
    /* Generate a name from the register */
    static const char *wreg_names[] = {"AX","CX","DX","BX","SP","BP","SI","DI"};
    static const char *breg_names[] = {"AL","CL","DL","BL","AH","CH","DH","BH"};
    static const char *sreg_names[] = {"ES","CS","SS","DS"};
    const char *name;
    if (rc == REGCLASS_WORD) name = wreg_names[reg];
    else if (rc == REGCLASS_BYTE) name = breg_names[reg];
    else name = sreg_names[reg];

    symbol_t *sym = sym_add(name, type, false);
    sym->is_pinned = true;
    sym->pinned_reg = reg;
    sym->pin_class = rc;
    return sym;
}

static bool addr_taken_contains(const char *name) {
    for (int i = 0; i < C.addr_taken.len; i++)
        if (strcmp(C.addr_taken.items[i], name) == 0)
            return true;
    return false;
}

static void addr_taken_add(const char *name) {
    if (!name || addr_taken_contains(name))
        return;
    *NIB_VEC_PUSH(&C.addr_taken, "address-taken names") =
        xstrdup_checked(name);
}

/* ---- Struct lookup ---- */

static int find_struct(const char *name) {
    for (int i = 0; i < C.structs.len; i++)
        if (strcmp(C.structs.items[i].name, name) == 0)
            return i;
    return -1;
}

/* ---- Constant lookup ---- */

static int find_constant(const char *name, int *value) {
    for (int i = 0; i < C.constants.len; i++)
        if (strcmp(C.constants.items[i].name, name) == 0) {
            if (value) *value = C.constants.items[i].value;
            return i;
        }
    return -1;
}

static void register_constant(const char *name, int value) {
    const_sig_t *c = NIB_VEC_PUSH(&C.constants, "constants");
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, 63);
    c->value = value;
}

/* Evaluate a constant expression at compile time.
 * Supports literals, named constants, and arithmetic on them. */
static bool eval_const_expr(expr_t *e, int *result) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_LIT_INT:
        *result = e->u.lit_int;
        return true;
    case EXPR_IDENT: {
        int val;
        if (find_constant(e->u.ident, &val) >= 0) {
            *result = val;
            return true;
        }
        return false;
    }
    case EXPR_BINOP: {
        int l, r;
        if (!eval_const_expr(e->u.binop.left, &l)) return false;
        if (!eval_const_expr(e->u.binop.right, &r)) return false;
        switch (e->u.binop.op) {
        case NIB_ADD: *result = l + r; return true;
        case NIB_SUB: *result = l - r; return true;
        case NIB_MUL: *result = l * r; return true;
        case NIB_DIV: if (r == 0) return false; *result = l / r; return true;
        case NIB_MOD: if (r == 0) return false; *result = l % r; return true;
        case NIB_AND: *result = l & r; return true;
        case NIB_OR:  *result = l | r; return true;
        case NIB_XOR: *result = l ^ r; return true;
        case NIB_SHL: *result = l << r; return true;
        case NIB_SHR: *result = (unsigned)l >> r; return true;
        default: return false;
        }
    }
    case EXPR_UNOP: {
        int v;
        if (!eval_const_expr(e->u.unop.operand, &v)) return false;
        switch (e->u.unop.op) {
        case NIB_NEG: *result = -v; return true;
        case NIB_NOT: *result = ~v; return true;
        default: return false;
        }
    }
    default:
        return false;
    }
}

static int require_const_expr(expr_t *e, int line, const char *what) {
    int value = 0;
    if (!eval_const_expr(e, &value))
        cerr(line, "%s must be a constant expression", what);
    return value;
}

static void resolve_type_constants(type_t *t, int line) {
    if (!t) return;
    if (t->kind == TYPE_ARRAY && t->array_size_expr) {
        t->array_size = require_const_expr(t->array_size_expr, line,
                                           "array size");
        t->array_size_expr = NULL;
    }
    if (t->element_type)
        resolve_type_constants(t->element_type, line);
}

static void resolve_param_type_constants(param_t *p, int line) {
    for (; p; p = p->next)
        resolve_type_constants(p->type, line);
}

static void resolve_return_type_constants(return_t *r, int line) {
    for (; r; r = r->next)
        resolve_type_constants(r->type, line);
}

static void resolve_fn_modifier_constants(fn_modifiers_t *mods, int line) {
    if (mods->has_at && mods->at_seg_expr) {
        mods->at_seg = require_const_expr(mods->at_seg_expr, line,
                                          "function at() segment");
        mods->at_off = require_const_expr(mods->at_off_expr, line,
                                          "function at() offset");
        mods->at_seg_expr = NULL;
        mods->at_off_expr = NULL;
    }
    if (mods->ds_policy == DS_POLICY_LITERAL && mods->ds_literal_expr) {
        mods->ds_literal = require_const_expr(mods->ds_literal_expr, line,
                                              "ds() literal");
        mods->ds_literal_expr = NULL;
    }
}

/* ---- Function lookup ---- */

static bool is_isr(const char *name) {
    for (int i = 0; i < C.isr_names.len; i++)
        if (strcmp(C.isr_names.items[i], name) == 0)
            return true;
    return false;
}

static int find_function(const char *name) {
    for (int i = 0; i < C.functions.len; i++)
        if (strcmp(C.functions.items[i].name, name) == 0)
            return i;
    return -1;
}

static int find_indirect_descriptor(const char *name, const char *module,
                                    const char **descriptor_name) {
    int fi = find_function(name);
    if (fi >= 0) {
        if (descriptor_name)
            *descriptor_name = name;
        return fi;
    }

    if (module && module[0]) {
        size_t mlen = strlen(module);
        if (strncmp(name, module, mlen) == 0 && name[mlen] == '_') {
            const char *local_name = name + mlen + 1;
            fi = find_function(local_name);
            if (fi >= 0) {
                if (descriptor_name)
                    *descriptor_name = local_name;
                return fi;
            }
        }
    }

    if (descriptor_name)
        *descriptor_name = name;
    return -1;
}

static void register_function_returns(const char *name, int nparams,
                                      return_t *rets, param_t *params) {
    function_sig_t *fn = NIB_VEC_PUSH(&C.functions, "function signatures");
    memset(fn, 0, sizeof(*fn));
    strncpy(fn->name, name, 63);
    fn->nparams = nparams;
    fn->return_type = rets ? rets->type : NULL;
    fn->nreturns = return_list_count(rets);
    if (fn->nreturns > 0)
        fn->return_types = nib_xcalloc((size_t)fn->nreturns,
                                       sizeof(*fn->return_types),
                                       "function return types");
    if (nparams > 0)
        fn->param_is_far = nib_xcalloc((size_t)nparams,
                                       sizeof(*fn->param_is_far),
                                       "function parameter metadata");
    if (nparams > 0)
        fn->param_types = nib_xcalloc((size_t)nparams,
                                      sizeof(*fn->param_types),
                                      "function parameter types");
    int ri = 0;
    for (return_t *r = rets; r; r = r->next, ri++)
        fn->return_types[ri] = r->type;
    /* Track which params are far (for call-site splitting) */
    int ir_count = 0;
    int pi = 0;
    for (param_t *p = params; p; p = p->next, pi++) {
        fn->param_types[pi] = p->type;
        fn->param_is_far[pi] = (p->type && p->type->kind == TYPE_FAR);
        ir_count++;
        if (fn->param_is_far[pi]) ir_count++; /* far splits into 2 */
    }
    fn->nparams_ir = ir_count;
}

/* ---- Register name helpers ---- */

static const char *wreg_name(int id) {
    static const char *n[] = {"AX","CX","DX","BX","SP","BP","SI","DI"};
    return (id >= 0 && id < 8) ? n[id] : "??";
}

static const char *breg_name(int id) {
    static const char *n[] = {"AL","CL","DL","BL","AH","CH","DH","BH"};
    return (id >= 0 && id < 8) ? n[id] : "??";
}

static const char *sreg_name(int id) {
    static const char *n[] = {"ES","CS","SS","DS"};
    return (id >= 0 && id < 4) ? n[id] : "??";
}

static const char *flag_name(int id) {
    static const char *n[] = {"CF","PF","AF","ZF","SF","TF","DF","OF","IF"};
    return (id >= 0 && id < 9) ? n[id] : "??";
}

static const char *reg_name_str(int id, reg_class_t rc) {
    switch (rc) {
    case REGCLASS_WORD: return wreg_name(id);
    case REGCLASS_BYTE: return breg_name(id);
    case REGCLASS_SEG:  return sreg_name(id);
    case REGCLASS_FLAG: return flag_name(id);
    }
    return "??";
}

/* ---- Type size ---- */

static bool type_is_aggregate(type_t *t);
static bool type_is_integer(type_t *t);
static const char *type_str(type_t *t);

static bool type_needs_word_alignment(type_t *t) {
    if (!t) return false;
    return t->kind == TYPE_U16 || t->kind == TYPE_U32 ||
           t->kind == TYPE_SEG || t->kind == TYPE_FAR ||
           t->kind == TYPE_STRUCT;
}

static int type_size(type_t *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_U8:       return 1;
    case TYPE_U16:      return 2;
    case TYPE_U32:      return 4;
    case TYPE_SEG:      return 2;
    case TYPE_BOOL:     return 0;
    case TYPE_ARRAY:    return t->element_type ? type_size(t->element_type) * t->array_size : 0;
    case TYPE_BCD:      return t->array_size;
    case TYPE_STRUCT: {
        if (!t->struct_name) return 0;
        int si = find_struct(t->struct_name);
        if (si < 0) return 0;
        int bit_pos = 0;
        for (field_t *f = C.structs.items[si].fields; f; f = f->next) {
            if (f->is_bits) {
                bit_pos += f->bits;
                continue;
            }
            if (bit_pos & 7)
                bit_pos = (bit_pos + 7) & ~7;
            int byte_pos = bit_pos / 8;
            if (C.structs.items[si].aligned &&
                type_needs_word_alignment(f->type) && (byte_pos & 1))
                byte_pos++;
            bit_pos = (byte_pos + type_size(f->type)) * 8;
        }
        return (bit_pos + 7) / 8;
    }
    case TYPE_FAR:      return 4;
    case TYPE_VOID:     return 0;
    }
    return 0;
}

typedef struct {
    bool found;
    bool is_bits;
    int byte_offset;
    int bit_offset;
    int bits;
    type_t *storage_type;
    type_t *access_type;
} field_layout_t;

static field_layout_t struct_field_layout(type_t *struct_type,
                                           const char *field_name,
                                           bool raw_access,
                                           int line) {
    field_layout_t out;
    memset(&out, 0, sizeof(out));
    if (!struct_type || struct_type->kind != TYPE_STRUCT ||
        !struct_type->struct_name) {
        if (struct_type)
            cerr(line, "field access on non-struct type '%s'",
                 type_str(struct_type));
        return out;
    }

    int si = find_struct(struct_type->struct_name);
    if (si < 0) {
        cerr(line, "unknown struct type '%s'", struct_type->struct_name);
        return out;
    }

    int bit_pos = 0;
    for (field_t *f = C.structs.items[si].fields; f; f = f->next) {
        int field_bit_pos;
        if (f->is_bits) {
            field_bit_pos = bit_pos;
            bit_pos += f->bits;
        } else {
            if (bit_pos & 7)
                bit_pos = (bit_pos + 7) & ~7;
            int byte_pos = bit_pos / 8;
            if (C.structs.items[si].aligned &&
                type_needs_word_alignment(f->type) && (byte_pos & 1))
                byte_pos++;
            field_bit_pos = byte_pos * 8;
            bit_pos = (byte_pos + type_size(f->type)) * 8;
        }

        if (!f->name || strcmp(f->name, field_name) != 0)
            continue;

        out.found = true;
        out.is_bits = f->is_bits;
        out.byte_offset = field_bit_pos / 8;
        out.bit_offset = field_bit_pos & 7;
        out.bits = f->bits;
        if (f->is_bits) {
            out.storage_type = (f->bits <= 8) ?
                               mk_type(TYPE_U8) : mk_type(TYPE_U16);
            out.access_type = out.storage_type;
        } else {
            out.storage_type = f->type;
            out.access_type = (f->as_type && !raw_access) ?
                              f->as_type : f->type;
        }
        return out;
    }

    cerr(line, "struct '%s' has no field '%s'",
         struct_type->struct_name, field_name);
    return out;
}

static bool should_stack_allocate_local(const char *name, type_t *type) {
    if (!name || !type)
        return false;
    return addr_taken_contains(name);
}

static const char *type_str(type_t *t) {
    static char bufs[2][64];
    static int which = 0;
    char *buf = bufs[which];
    which = 1 - which;
    if (!t) return "void";
    switch (t->kind) {
    case TYPE_U8:       return "u8";
    case TYPE_U16:      return "u16";
    case TYPE_U32:      return "u32";
    case TYPE_SEG:      return "seg";
    case TYPE_BOOL:     return "bool";
    case TYPE_ARRAY:
        snprintf(buf, 64, "%s[%d]",
                 t->element_type ? type_str(t->element_type) : "?",
                 t->array_size);
        return buf;
    case TYPE_BCD:      snprintf(buf, 64, "bcd[%d]", t->array_size); return buf;
    case TYPE_STRUCT:   return t->struct_name ? t->struct_name : "struct";
    case TYPE_FAR:      return "far32";
    case TYPE_VOID:     return "void";
    }
    return "?";
}

/* ---- Operator names ---- */

static const char *op_str(op_kind_t op) {
    switch (op) {
    case NIB_ADD: return "add"; case NIB_SUB: return "sub";
    case NIB_MUL: return "mul"; case NIB_DIV: return "div";
    case NIB_MOD: return "mod";
    case NIB_SMUL: return "imul"; case NIB_SDIV: return "idiv";
    case NIB_SMOD: return "imod";
    case NIB_AND: return "and"; case NIB_OR: return "or";
    case NIB_XOR: return "xor";
    case NIB_SHL: return "shl"; case NIB_SHR: return "shr";
    case NIB_SRSHR: return "sar";
    case NIB_ROL: return "rol"; case NIB_ROR: return "ror";
    case NIB_RCL: return "rcl"; case NIB_RCR: return "rcr";
    case NIB_EQ: return "cmp.eq"; case NIB_NEQ: return "cmp.ne";
    case NIB_LT: return "cmp.b"; case NIB_GT: return "cmp.a";
    case NIB_LTE: return "cmp.be"; case NIB_GTE: return "cmp.ae";
    case NIB_SLT: return "cmp.l"; case NIB_SGT: return "cmp.g";
    case NIB_SLTE: return "cmp.le"; case NIB_SGTE: return "cmp.ge";
    case NIB_XCHG: return "xchg";
    case NIB_NEG: return "neg"; case NIB_NOT: return "not";
    case NIB_ADDR: return "lea"; case NIB_FAR_ADDR: return "far_lea";
    case NIB_LNOT: return "lnot";
    }
    return "??";
}

/* ================================================================
 * Type checking helpers
 * ================================================================ */

static bool types_equal(type_t *a, type_t *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    if (a->kind == TYPE_ARRAY)
        return a->array_size == b->array_size &&
               types_equal(a->element_type, b->element_type);
    if (a->kind == TYPE_BCD)
        return a->array_size == b->array_size;
    if (a->kind == TYPE_STRUCT)
        return a->struct_name && b->struct_name &&
               strcmp(a->struct_name, b->struct_name) == 0;
    return true;
}

static bool type_is_aggregate(type_t *t) {
    if (!t) return false;
    return t->kind == TYPE_ARRAY || t->kind == TYPE_BCD ||
           t->kind == TYPE_STRUCT || t->kind == TYPE_FAR;
}

static bool type_is_integer(type_t *t) {
    if (!t) return false;
    return t->kind == TYPE_U8 || t->kind == TYPE_U16 || t->kind == TYPE_U32;
}

static bool type_is_bcd(type_t *t) {
    return t && t->kind == TYPE_BCD;
}

static type_t *type_of_element(type_t *t) {
    if (!t) return NULL;
    if (t->kind == TYPE_ARRAY) return t->element_type;
    return NULL;
}

static int expr_list_count(expr_t *list) {
    int n = 0;
    for (expr_t *e = list; e; e = e->next) n++;
    return n;
}

static return_t *return_nth(return_t *list, int idx) {
    for (return_t *r = list; r; r = r->next, idx--)
        if (idx == 0) return r;
    return NULL;
}

static bool type_assignable(type_t *dst, type_t *src) {
    if (!src || !dst) return true;
    if (types_equal(dst, src)) return true;
    return type_is_integer(dst) && type_is_integer(src) &&
           type_size(src) <= type_size(dst);
}

/* Check two types are compatible for a binary arithmetic operation.
 * NULL type = literal, promotes to whatever the other side is. */
static type_t *check_arith(type_t *l, type_t *r, op_kind_t op, int line) {
    /* Literal promotion: NULL matches anything */
    if (!l && !r) return mk_type(TYPE_U16); /* two literals — default u16 */
    if (!l) return r;  /* left is literal, adopt right's type */
    if (!r) return l;  /* right is literal, adopt left's type */

    /* BCD + BCD */
    if (type_is_bcd(l) && type_is_bcd(r)) {
        if (!types_equal(l, r))
            cerr(line, "BCD operands must have same size: %s vs %s",
                 type_str(l), type_str(r));
        if (op != NIB_ADD && op != NIB_SUB)
            cerr(line, "BCD only supports + and -");
        return l;
    }

    /* Can't do arithmetic on aggregates */
    if (type_is_aggregate(l) || type_is_aggregate(r)) {
        cerr(line, "cannot perform arithmetic on aggregate types: %s and %s",
             type_str(l), type_str(r));
        return mk_type(TYPE_U16);
    }

    /* Scalar arithmetic — must match sizes */
    if (type_is_integer(l) && type_is_integer(r)) {
        if (l->kind != r->kind)
            cerr(line, "operand size mismatch: %s vs %s",
                 type_str(l), type_str(r));
        /* Multiply produces wider result */
        if (op == NIB_MUL || op == NIB_SMUL) {
            if (l->kind == TYPE_U16) return mk_type(TYPE_U32);
        }
        return l;
    }

    return l;
}

/* Check a comparison — operands must match, result is bool */
static type_t *check_compare(type_t *l, type_t *r, int line) {
    /* Literal promotion */
    if (!l || !r) return mk_type(TYPE_BOOL);
    if (type_is_bcd(l) && type_is_bcd(r)) {
        if (!types_equal(l, r))
            cerr(line, "BCD comparison operands must match: %s vs %s",
                 type_str(l), type_str(r));
        return mk_type(TYPE_BOOL);
    }
    if (type_is_integer(l) && type_is_integer(r)) {
        if (l->kind != r->kind)
            cerr(line, "comparison operand size mismatch: %s vs %s",
                 type_str(l), type_str(r));
        return mk_type(TYPE_BOOL);
    }
    if (type_is_aggregate(l) || type_is_aggregate(r))
        cerr(line, "cannot compare aggregate types");
    return mk_type(TYPE_BOOL);
}

/* ================================================================
 * NIR emission — expression compilation
 * ================================================================ */

typedef struct { int vreg; int vreg_seg; type_t *type; } typed_vreg_t;

static typed_vreg_t emit_expr_typed(expr_t *e);
static typed_vreg_t emit_expr_typed_for(expr_t *e, type_t *target);
static typed_vreg_t emit_initializer_expr_typed(expr_t *e, type_t *target);

static bool binop_is_shift(op_kind_t op) {
    return op == NIB_SHL || op == NIB_SHR || op == NIB_SRSHR ||
           op == NIB_ROL || op == NIB_ROR || op == NIB_RCL ||
           op == NIB_RCR;
}

static bool binop_is_compare(op_kind_t op) {
    return op >= NIB_EQ && op <= NIB_SGTE;
}

/* Recursively resolve all constant references to literals in an expression tree */
static void resolve_constants_expr(expr_t *e) {
    if (!e) return;
    if (e->kind == EXPR_IDENT) {
        int cv;
        if (find_constant(e->u.ident, &cv) >= 0) {
            e->kind = EXPR_LIT_INT;
            e->u.lit_int = cv;
        }
        return;
    }
    switch (e->kind) {
    case EXPR_BINOP:
        resolve_constants_expr(e->u.binop.left);
        resolve_constants_expr(e->u.binop.right);
        break;
    case EXPR_UNOP:
        resolve_constants_expr(e->u.unop.operand);
        break;
    case EXPR_CALL:
        resolve_constants_expr(e->u.call.func);
        for (expr_t *a = e->u.call.args; a; a = a->next)
            resolve_constants_expr(a);
        break;
    case EXPR_INDIRECT_CALL:
    case EXPR_NEAR_INDIRECT_CALL:
        resolve_constants_expr(e->u.indirect_call.addr);
        for (expr_t *a = e->u.indirect_call.args; a; a = a->next)
            resolve_constants_expr(a);
        break;
    case EXPR_INDEX:
    case EXPR_CHECKED_INDEX:
        resolve_constants_expr(e->u.index.array);
        resolve_constants_expr(e->u.index.index);
        break;
    case EXPR_FIELD:
    case EXPR_RAW_FIELD:
        resolve_constants_expr(e->u.field.object);
        break;
    case EXPR_CAST:
        resolve_constants_expr(e->u.cast.operand);
        break;
    case EXPR_ARRAY_INIT:
        for (expr_t *el = e->u.array_init.elements; el; el = el->next)
            resolve_constants_expr(el);
        break;
    case EXPR_MEM:
        resolve_constants_expr(e->u.mem.disp_expr);
        resolve_constants_expr(e->u.mem.abs_seg_expr);
        break;
    case EXPR_FAR_LIT:
        resolve_constants_expr(e->u.far_lit.seg_expr);
        resolve_constants_expr(e->u.far_lit.off_expr);
        break;
    case EXPR_DEREF:
        resolve_constants_expr(e->u.deref.expr);
        break;
    default:
        break;
    }
}

static void resolve_constants_stmt(stmt_t *s) {
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_VARDECL:
            if (s->u.vardecl.init) resolve_constants_expr(s->u.vardecl.init);
            break;
        case STMT_ASSIGN:
        case STMT_TOGGLE_ASSIGN:
            resolve_constants_expr(s->u.assign.target);
            resolve_constants_expr(s->u.assign.value);
            break;
        case STMT_EXPR:
            resolve_constants_expr(s->u.expr);
            break;
        case STMT_IF:
            resolve_constants_expr(s->u.if_stmt.cond);
            resolve_constants_stmt(s->u.if_stmt.then_body);
            resolve_constants_stmt(s->u.if_stmt.else_body);
            break;
        case STMT_WHILE:
            resolve_constants_expr(s->u.while_stmt.cond);
            resolve_constants_stmt(s->u.while_stmt.body);
            break;
        case STMT_FOR:
            resolve_constants_expr(s->u.for_stmt.start);
            resolve_constants_expr(s->u.for_stmt.end);
            resolve_constants_stmt(s->u.for_stmt.body);
            break;
        case STMT_RETURN:
            resolve_constants_expr(s->u.ret_expr);
            break;
        case STMT_TAILCALL:
            resolve_constants_expr(s->u.tailcall_expr);
            break;
        default:
            break;
        }
    }
}

static void scan_addr_taken_expr(expr_t *e);

static void scan_addr_taken_stmt(stmt_t *s) {
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_VARDECL:
            scan_addr_taken_expr(s->u.vardecl.init);
            break;
        case STMT_ASSIGN:
        case STMT_TOGGLE_ASSIGN:
            scan_addr_taken_expr(s->u.assign.target);
            scan_addr_taken_expr(s->u.assign.value);
            break;
        case STMT_EXPR:
            scan_addr_taken_expr(s->u.expr);
            break;
        case STMT_IF:
            scan_addr_taken_expr(s->u.if_stmt.cond);
            scan_addr_taken_stmt(s->u.if_stmt.then_body);
            scan_addr_taken_stmt(s->u.if_stmt.else_body);
            break;
        case STMT_WHILE:
            scan_addr_taken_expr(s->u.while_stmt.cond);
            scan_addr_taken_stmt(s->u.while_stmt.body);
            break;
        case STMT_FOR:
            scan_addr_taken_expr(s->u.for_stmt.start);
            scan_addr_taken_expr(s->u.for_stmt.end);
            scan_addr_taken_stmt(s->u.for_stmt.body);
            break;
        case STMT_RETURN:
            scan_addr_taken_expr(s->u.ret_expr);
            break;
        case STMT_TAILCALL:
            scan_addr_taken_expr(s->u.tailcall_expr);
            break;
        case STMT_CONST:
            scan_addr_taken_expr(s->u.konst.init);
            break;
        default:
            break;
        }
    }
}

static void scan_addr_taken_expr(expr_t *e) {
    for (; e; e = e->next) {
        switch (e->kind) {
        case EXPR_BINOP:
            scan_addr_taken_expr(e->u.binop.left);
            scan_addr_taken_expr(e->u.binop.right);
            break;
        case EXPR_UNOP:
            if ((e->u.unop.op == NIB_ADDR ||
                 e->u.unop.op == NIB_FAR_ADDR) &&
                e->u.unop.operand &&
                e->u.unop.operand->kind == EXPR_IDENT)
                addr_taken_add(e->u.unop.operand->u.ident);
            scan_addr_taken_expr(e->u.unop.operand);
            break;
        case EXPR_CALL:
            scan_addr_taken_expr(e->u.call.func);
            scan_addr_taken_expr(e->u.call.args);
            break;
        case EXPR_INDEX:
        case EXPR_CHECKED_INDEX:
            scan_addr_taken_expr(e->u.index.array);
            scan_addr_taken_expr(e->u.index.index);
            break;
        case EXPR_FIELD:
        case EXPR_RAW_FIELD:
            scan_addr_taken_expr(e->u.field.object);
            break;
        case EXPR_CAST:
            scan_addr_taken_expr(e->u.cast.operand);
            break;
        case EXPR_PAREN:
            scan_addr_taken_expr(e->u.unop.operand);
            break;
        case EXPR_ARRAY_INIT:
            scan_addr_taken_expr(e->u.array_init.elements);
            break;
        case EXPR_INDIRECT_CALL:
        case EXPR_NEAR_INDIRECT_CALL:
            scan_addr_taken_expr(e->u.indirect_call.addr);
            scan_addr_taken_expr(e->u.indirect_call.args);
            break;
        case EXPR_DEREF:
            scan_addr_taken_expr(e->u.deref.expr);
            break;
        case EXPR_MEM:
            scan_addr_taken_expr(e->u.mem.disp_expr);
            scan_addr_taken_expr(e->u.mem.abs_seg_expr);
            break;
        case EXPR_FAR_LIT:
            scan_addr_taken_expr(e->u.far_lit.seg_expr);
            scan_addr_taken_expr(e->u.far_lit.off_expr);
            break;
        default:
            break;
        }
    }
}

/* Emit a vreg reference: %N or pinned register name */
static int alloc_vreg(void) {
    return C.next_vreg++;
}

/* Convenience wrapper that discards the type */
static int emit_expr(expr_t *e) {
    return emit_expr_typed(e).vreg;
}

static typed_vreg_t TV(int vreg, type_t *type) {
    typed_vreg_t tv = { vreg, -1, type };
    if (type && C.nir) {
        if (type->kind == TYPE_U8)
            fprintf(C.nir, ".vreg %%%d, u8\n", vreg);
        else if (type->kind == TYPE_SEG)
            fprintf(C.nir, ".vreg %%%d, seg\n", vreg);
    }
    return tv;
}

static typed_vreg_t TV_FAR(int off_vreg, int seg_vreg) {
    typed_vreg_t tv = { off_vreg, seg_vreg, mk_type(TYPE_FAR) };
    return tv;
}

static typed_vreg_t TV_PIN(int vreg, type_t *type, const char *pin) {
    typed_vreg_t tv = { vreg, -1, type };
    const char *type_name = "u16";
    if (type && type->kind == TYPE_U8)
        type_name = "u8";
    else if (type && type->kind == TYPE_SEG)
        type_name = "seg";
    fprintf(C.nir, ".vreg %%%d, %s, pin=%s\n", vreg, type_name, pin);
    return tv;
}

typedef struct {
    int base;
    int index;
    int disp;
    bool has_disp;
} addr_parts_t;

static bool addr_add_reg(addr_parts_t *p, reg_id_t reg) {
    if (reg == WREG_BX || reg == WREG_BP) {
        if (p->base != REG_NONE) return false;
        p->base = reg;
        return true;
    }
    if (reg == WREG_SI || reg == WREG_DI) {
        if (p->index != REG_NONE) return false;
        p->index = reg;
        return true;
    }
    return false;
}

static bool addr_collect(expr_t *e, addr_parts_t *p) {
    int cv;
    if (eval_const_expr(e, &cv)) {
        p->disp += cv;
        p->has_disp = true;
        return true;
    }
    if (!e) return false;
    if (e->kind == EXPR_REG && e->u.reg.rclass == REGCLASS_WORD)
        return addr_add_reg(p, e->u.reg.id);
    if (e->kind == EXPR_CAST)
        return addr_collect(e->u.cast.operand, p);
    if (e->kind == EXPR_BINOP && e->u.binop.op == NIB_ADD)
        return addr_collect(e->u.binop.left, p) &&
               addr_collect(e->u.binop.right, p);
    if (e->kind == EXPR_BINOP && e->u.binop.op == NIB_SUB) {
        int rv;
        if (!eval_const_expr(e->u.binop.right, &rv))
            return false;
        if (!addr_collect(e->u.binop.left, p))
            return false;
        p->disp -= rv;
        p->has_disp = true;
        return true;
    }
    return false;
}

static void format_direct_addr(expr_t *e, reg_id_t seg, char *buf, size_t bufsz) {
    if (e && e->kind == EXPR_FAR_LIT && seg == REG_NONE) {
        int s = e->u.far_lit.seg_expr
            ? require_const_expr(e->u.far_lit.seg_expr, e->line,
                                 "far literal segment")
            : e->u.far_lit.seg;
        int o = e->u.far_lit.off_expr
            ? require_const_expr(e->u.far_lit.off_expr, e->line,
                                 "far literal offset")
            : e->u.far_lit.off;
        snprintf(buf, bufsz, "[0x%04X:0x%04X]", s & 0xFFFF, o & 0xFFFF);
        return;
    }

    addr_parts_t p = { REG_NONE, REG_NONE, 0, false };
    if (!addr_collect(e, &p)) {
        buf[0] = '\0';
        return;
    }

    char inner[96] = "";
    bool need_plus = false;
    if (p.base != REG_NONE) {
        snprintf(inner + strlen(inner), sizeof(inner) - strlen(inner),
                 "%s", wreg_name(p.base));
        need_plus = true;
    }
    if (p.index != REG_NONE) {
        snprintf(inner + strlen(inner), sizeof(inner) - strlen(inner),
                 "%s%s", need_plus ? "+" : "", wreg_name(p.index));
        need_plus = true;
    }
    if (p.has_disp || !need_plus) {
        unsigned v = (unsigned)p.disp & 0xFFFF;
        snprintf(inner + strlen(inner), sizeof(inner) - strlen(inner),
                 "%s0x%04X", need_plus ? "+" : "", v);
    }

    if (seg != REG_NONE)
        snprintf(buf, bufsz, "[%s:%s]", sreg_name(seg), inner);
    else
        snprintf(buf, bufsz, "[%s]", inner);
}

static void emit_loadmem_with_segment(int dst, int off, reg_id_t seg_id) {
    fprintf(C.nir, "    loadmem %%%d, %%%d, %s\n",
            dst, off, sreg_name(seg_id));
}

static void emit_storemem_with_segment(int off, reg_id_t seg_id, int val) {
    fprintf(C.nir, "    storemem %%%d, %s, %%%d\n",
            off, sreg_name(seg_id), val);
}

static bool is_contextual_mem_load_type(type_t *type);

static typed_vreg_t emit_deref_load(expr_t *e, type_t *target) {
    type_t *load_type = is_contextual_mem_load_type(target)
        ? target : mk_type(TYPE_U8);
    int dst = alloc_vreg();
    char addr[128];
    format_direct_addr(e->u.deref.expr, e->u.deref.seg, addr, sizeof(addr));
    if (addr[0]) {
        fprintf(C.nir, "    loadmem %%%d, %s\n", dst, addr);
        return TV(dst, load_type);
    }

    typed_vreg_t ptr = emit_expr_typed(e->u.deref.expr);
    if (ptr.type && ptr.type->kind == TYPE_FAR) {
        if (e->u.deref.seg != REG_NONE) {
            emit_loadmem_with_segment(dst, ptr.vreg, e->u.deref.seg);
        } else if (ptr.vreg_seg >= 0) {
            fprintf(C.nir, "    loadmem %%%d, %%%d, %%%d\n",
                    dst, ptr.vreg, ptr.vreg_seg);
        } else {
            int off = alloc_vreg();
            int seg = alloc_vreg();
            fprintf(C.nir, "    far.off %%%d, %%%d\n", off, ptr.vreg);
            fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg, ptr.vreg);
            fprintf(C.nir, "    loadmem %%%d, %%%d, %%%d\n", dst, off, seg);
        }
    } else if (!ptr.type || type_is_integer(ptr.type)) {
        if (ptr.type && ptr.type->kind != TYPE_U16)
            cerr(e->line, "pointer dereference requires u16 or far32, got %s",
                 type_str(ptr.type));
        if (e->u.deref.seg == REG_NONE || e->u.deref.seg == SREG_DS) {
            fprintf(C.nir, "    loadmem %%%d, %%%d\n", dst, ptr.vreg);
        } else if (e->u.deref.seg == SREG_CS) {
            fprintf(C.nir, ".vreg %%%d, u16, csref\n", ptr.vreg);
            fprintf(C.nir, "    loadmem %%%d, %%%d\n", dst, ptr.vreg);
        } else {
            emit_loadmem_with_segment(dst, ptr.vreg, e->u.deref.seg);
        }
    } else {
        cerr(e->line, "pointer dereference requires u16 or far32, got %s",
             type_str(ptr.type));
    }
    return TV(dst, load_type);
}

static bool is_contextual_mem_load_type(type_t *type) {
    return type && (type->kind == TYPE_U8 || type->kind == TYPE_U16 ||
                    type->kind == TYPE_SEG);
}

static typed_vreg_t emit_mem_load_expr_typed(expr_t *e, type_t *target) {
    type_t *load_type = is_contextual_mem_load_type(target)
        ? target : mk_type(TYPE_U8);
    int dst = alloc_vreg();
    fprintf(C.nir, "    loadmem %%%d, [", dst);
    if (e->u.mem.abs_seg) {
        int seg = e->u.mem.abs_seg_expr
            ? require_const_expr(e->u.mem.abs_seg_expr, e->line,
                                 "absolute memory segment")
            : e->u.mem.abs_seg_val;
        fprintf(C.nir, "0x%04X:", seg & 0xFFFF);
    } else if (e->u.mem.seg != REG_NONE) {
        fprintf(C.nir, "%s:", sreg_name(e->u.mem.seg));
    }
    bool need_plus = false;
    if (e->u.mem.base != REG_NONE) {
        fprintf(C.nir, "%s", wreg_name(e->u.mem.base));
        need_plus = true;
    }
    if (e->u.mem.index != REG_NONE) {
        if (need_plus) fprintf(C.nir, "+");
        fprintf(C.nir, "%s", wreg_name(e->u.mem.index));
        need_plus = true;
    }
    if (e->u.mem.has_disp) {
        int disp = e->u.mem.disp_expr
            ? require_const_expr(e->u.mem.disp_expr, e->line,
                                 "memory displacement")
            : e->u.mem.disp;
        if (need_plus) fprintf(C.nir, "+");
        fprintf(C.nir, "0x%04X", disp & 0xFFFF);
    }
    fprintf(C.nir, "]\n");
    return TV(dst, load_type);
}

static void emit_deref_store(expr_t *e, typed_vreg_t val) {
    char addr[128];
    format_direct_addr(e->u.deref.expr, e->u.deref.seg, addr, sizeof(addr));
    if (addr[0]) {
        fprintf(C.nir, "    storemem %s, %%%d\n", addr, val.vreg);
        return;
    }

    typed_vreg_t ptr = emit_expr_typed(e->u.deref.expr);
    if (ptr.type && ptr.type->kind == TYPE_FAR) {
        if (e->u.deref.seg != REG_NONE) {
            emit_storemem_with_segment(ptr.vreg, e->u.deref.seg, val.vreg);
        } else if (ptr.vreg_seg >= 0) {
            fprintf(C.nir, "    storemem %%%d, %%%d, %%%d\n",
                    ptr.vreg, ptr.vreg_seg, val.vreg);
        } else {
            int off = alloc_vreg();
            int seg = alloc_vreg();
            fprintf(C.nir, "    far.off %%%d, %%%d\n", off, ptr.vreg);
            fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg, ptr.vreg);
            fprintf(C.nir, "    storemem %%%d, %%%d, %%%d\n",
                    off, seg, val.vreg);
        }
    } else if (!ptr.type || type_is_integer(ptr.type)) {
        if (ptr.type && ptr.type->kind != TYPE_U16)
            cerr(e->line, "pointer dereference requires u16 or far32, got %s",
                 type_str(ptr.type));
        if (e->u.deref.seg == REG_NONE || e->u.deref.seg == SREG_DS) {
            fprintf(C.nir, "    storemem %%%d, %%%d\n", ptr.vreg, val.vreg);
        } else if (e->u.deref.seg == SREG_CS) {
            fprintf(C.nir, ".vreg %%%d, u16, csref\n", ptr.vreg);
            fprintf(C.nir, "    storemem %%%d, %%%d\n", ptr.vreg, val.vreg);
        } else {
            emit_storemem_with_segment(ptr.vreg, e->u.deref.seg, val.vreg);
        }
    } else {
        cerr(e->line, "pointer dereference requires u16 or far32, got %s",
             type_str(ptr.type));
    }
}

static int emit_stack_addr(symbol_t *sym) {
    int dst = alloc_vreg();
    fprintf(C.nir, "    lea %%%d, %%%d\n", dst, sym->vreg);
    return dst;
}

static typed_vreg_t emit_stack_local_value(symbol_t *sym, int line) {
    int addr = emit_stack_addr(sym);
    if (type_is_aggregate(sym->type))
        return TV(addr, sym->type);

    if (!sym->type || !type_is_integer(sym->type)) {
        cerr(line, "cannot load stack local '%s' of type %s",
             sym->name, type_str(sym->type));
        return TV(addr, sym->type);
    }

    int dst = alloc_vreg();
    const char *op = (sym->type->kind == TYPE_U8) ? "loadb" : "load";
    fprintf(C.nir, "    %s %%%d, %%%d\n", op, dst, addr);
    return TV(dst, sym->type);
}

static typed_vreg_t emit_stack_far_addr(symbol_t *sym) {
    int off = emit_stack_addr(sym);
    int seg_tmp = alloc_vreg();
    int seg = alloc_vreg();
    fprintf(C.nir, "    mov %%%d, SS\n", seg_tmp);
    fprintf(C.nir, ".vreg %%%d, seg\n", seg);
    fprintf(C.nir, "    mov %%%d, %%%d\n", seg, seg_tmp);
    return TV_FAR(off, seg);
}

static int emit_addr_plus_const(int base, int offset) {
    if (offset == 0)
        return base;
    int addr = alloc_vreg();
    fprintf(C.nir, "    add %%%d, %%%d, %d\n", addr, base, offset);
    return addr;
}

static typed_vreg_t emit_struct_field_value(typed_vreg_t obj,
                                            const char *field_name,
                                            bool raw_access,
                                            int line) {
    if (obj.type && obj.type->kind == TYPE_FAR) {
        cerr(line, "use backtick for far components: ptr`seg, ptr`off");
        return TV(alloc_vreg(), mk_type(TYPE_U16));
    }

    field_layout_t fl = struct_field_layout(obj.type, field_name,
                                            raw_access, line);
    if (!fl.found)
        return TV(alloc_vreg(), mk_type(TYPE_U16));
    if (fl.is_bits) {
        int dst = alloc_vreg();
        fprintf(C.nir, "    field %%%d, %%%d, %s\n",
                dst, obj.vreg, field_name);
        return TV(dst, fl.access_type);
    }

    int addr = emit_addr_plus_const(obj.vreg, fl.byte_offset);
    if (fl.storage_type && fl.storage_type->kind == TYPE_FAR) {
        int off_v = alloc_vreg();
        int seg_tmp = alloc_vreg();
        int seg_v = alloc_vreg();
        fprintf(C.nir, "    load %%%d, %%%d\n", off_v, addr);
        int seg_addr = emit_addr_plus_const(addr, 2);
        fprintf(C.nir, "    load %%%d, %%%d\n", seg_tmp, seg_addr);
        fprintf(C.nir, ".vreg %%%d, seg\n", seg_v);
        fprintf(C.nir, "    mov %%%d, %%%d\n", seg_v, seg_tmp);
        return TV_FAR(off_v, seg_v);
    }
    if (type_is_aggregate(fl.storage_type))
        return TV(addr, fl.access_type);

    int dst = alloc_vreg();
    const char *op = (fl.storage_type && fl.storage_type->kind == TYPE_U8)
        ? "loadb" : "load";
    fprintf(C.nir, "    %s %%%d, %%%d\n", op, dst, addr);
    return TV(dst, fl.access_type);
}

static void emit_struct_field_store(expr_t *target, typed_vreg_t val) {
    typed_vreg_t obj = emit_expr_typed(target->u.field.object);
    field_layout_t fl = struct_field_layout(obj.type, target->u.field.field_name,
                                            false, target->line);
    if (!fl.found)
        return;
    if (fl.is_bits) {
        fprintf(C.nir, "    storefield %%%d, %s, %%%d\n",
                obj.vreg, target->u.field.field_name, val.vreg);
        return;
    }

    int addr = emit_addr_plus_const(obj.vreg, fl.byte_offset);
    if (fl.storage_type && fl.storage_type->kind == TYPE_FAR) {
        int off_v = val.vreg;
        int seg_v = val.vreg_seg;
        if (seg_v < 0) {
            off_v = alloc_vreg();
            seg_v = alloc_vreg();
            fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, val.vreg);
            fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, val.vreg);
        }
        fprintf(C.nir, "    store %%%d, %%%d\n", addr, off_v);
        int seg_addr = emit_addr_plus_const(addr, 2);
        fprintf(C.nir, "    store %%%d, %%%d\n", seg_addr, seg_v);
        return;
    }
    if (type_is_aggregate(fl.storage_type)) {
        cerr(target->line, "aggregate field assignment is not supported");
        return;
    }

    const char *op = (fl.storage_type && fl.storage_type->kind == TYPE_U8)
        ? "storeb" : "store";
    fprintf(C.nir, "    %s %%%d, %%%d\n", op, addr, val.vreg);
}

static typed_vreg_t emit_expr_typed(expr_t *e) {
    if (!e) return TV(-1, NULL);

    switch (e->kind) {
    case EXPR_LIT_INT: {
        int r = alloc_vreg();
        /* Integer literals are untyped — they promote to whatever context needs.
           We use NULL type to signal "literal, promote me". */
        fprintf(C.nir, "    mov %%%d, %d\n", r, e->u.lit_int);
        return TV(r, NULL);
    }
    case EXPR_LIT_STR: {
        int r = alloc_vreg();
        int slen = strlen(e->u.lit_str);
        if (slen == 1) {
            /* Single character — just a u8 immediate */
            fprintf(C.nir, "    mov %%%d, %d\n", r, (unsigned char)e->u.lit_str[0]);
            return TV(r, NULL);
        }
        /* Multi-char string — emit as constant and load address */
        int cid = C.next_const++;
        fprintf(C.nir, ".const _C%d, \"", cid);
        for (int i = 0; i < slen; i++) {
            unsigned char ch = e->u.lit_str[i];
            if (ch >= 0x20 && ch < 0x7F && ch != '"' && ch != '\\')
                fprintf(C.nir, "%c", ch);
            else
                fprintf(C.nir, "\\x%02X", ch);
        }
        fprintf(C.nir, "\"\n");
        fprintf(C.nir, "    mov %%%d, _C%d\n", r, cid);
        return TV(r, mk_type_array(mk_type(TYPE_U8), slen));
    }
    case EXPR_IDENT: {
        /* Interrupt handler name — far32 constant (label + SEG) */
        if (is_isr(e->u.ident)) {
            int seg_tmp = alloc_vreg();
            int seg = alloc_vreg();
            int off = alloc_vreg();
            fprintf(C.nir, "    mov %%%d, SEG %s\n", seg_tmp, e->u.ident);
            fprintf(C.nir, ".vreg %%%d, seg\n", seg);
            fprintf(C.nir, "    mov %%%d, %%%d\n", seg, seg_tmp);
            fprintf(C.nir, "    mov %%%d, %s\n", off, e->u.ident);
            return TV_FAR(off, seg);
        }
        symbol_t *sym = sym_lookup(e->u.ident);
        if (!sym) {
            cerr(e->line, "undefined variable '%s'", e->u.ident);
            return TV(alloc_vreg(), mk_type(TYPE_U16));
        }
        /* Globals don't have function-scoped vregs — load fresh each time */
        if (sym->is_global) {
            int r = alloc_vreg();
            bool is_scalar = sym->type && (sym->type->kind == TYPE_U8 ||
                             sym->type->kind == TYPE_U16 ||
                             sym->type->kind == TYPE_U32 ||
                             sym->type->kind == TYPE_SEG);
            if (is_scalar) {
                /* Scalar global: load value from memory */
                if (sym->has_at && !sym->is_init_data)
                    fprintf(C.nir, "    loadmem %%%d, [0x%04X]\n", r, sym->at_off);
                else
                    fprintf(C.nir, "    loadmem %%%d, [%s]\n", r, sym->name);
            } else {
                /* Aggregate global: load address (passed by reference) */
                if (sym->has_at && !sym->is_init_data)
                    fprintf(C.nir, "    mov %%%d, 0x%04X\n", r, sym->at_off);
                else
                    fprintf(C.nir, "    mov %%%d, %s\n", r, sym->name);
            }
            return TV(r, sym->type);
        }
        if (sym->vreg_seg >= 0)
            return TV_FAR(sym->vreg, sym->vreg_seg);
        if (sym->is_stack_local)
            return emit_stack_local_value(sym, e->line);
        return TV(sym->vreg, sym->type);
    }
    case EXPR_REG: {
        const char *name = reg_name_str(e->u.reg.id, e->u.reg.rclass);
        symbol_t *sym = sym_lookup(name);
        if (!sym) {
            /* Auto-declare register variable on first use */
            type_t *t = (e->u.reg.rclass == REGCLASS_BYTE) ?
                        mk_type(TYPE_U8) : mk_type(TYPE_U16);
            sym = sym_add_pinned(t, e->u.reg.id, e->u.reg.rclass);
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
        }
        return TV_PIN(sym->vreg, sym->type, name);
    }
    case EXPR_SREG: {
        const char *name = sreg_name(e->u.reg.id);
        symbol_t *sym = sym_lookup(name);
        if (!sym) {
            /* Auto-declare segment register on first use */
            sym = sym_add_pinned(mk_type(TYPE_SEG), e->u.reg.id, REGCLASS_SEG);
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
        }
        return TV_PIN(sym->vreg, sym->type, name);
    }
    case EXPR_FLAG: {
        int r = alloc_vreg();
        fprintf(C.nir, "    getflag %%%d, %s\n", r, flag_name(e->u.flag_id));
        return TV(r, mk_type(TYPE_BOOL));
    }
    case EXPR_BINOP: {
        op_kind_t op = e->u.binop.op;

        /* Check if right operand is an integer literal — emit as immediate */
        bool right_is_imm = (e->u.binop.right->kind == EXPR_LIT_INT);
        int right_imm = right_is_imm ? e->u.binop.right->u.lit_int : 0;

        typed_vreg_t l = emit_expr_typed(e->u.binop.left);
        typed_vreg_t r;
        if (right_is_imm) {
            /* Don't allocate a vreg for the immediate */
            r = TV(-1, NULL);
        } else {
            r = emit_expr_typed(e->u.binop.right);
        }

        int dst = alloc_vreg();
        type_t *result_type;

        /* Type check based on operator category */
        bool is_cmp = binop_is_compare(op);
        bool is_shift = binop_is_shift(op);

        if (is_cmp) {
            result_type = check_compare(l.type, r.type, e->line);
        } else if (op == NIB_XCHG) {
            if (!types_equal(l.type, r.type))
                cerr(e->line, "exchange operands must be same type: %s vs %s",
                     type_str(l.type), type_str(r.type));
            result_type = l.type;
        } else if (is_shift) {
            if (l.type && !type_is_integer(l.type))
                cerr(e->line, "shift/rotate operand must be integer, got %s",
                     type_str(l.type));
            result_type = l.type;
        } else {
            result_type = check_arith(l.type, r.type, op, e->line);
        }

        /* Byte multiply: IMUL only operates on word registers, so
         * zero-extend operands to word, do word multiply, truncate. */
        if ((op == NIB_MUL || op == NIB_SMUL) &&
            l.type && l.type->kind == TYPE_U8) {
            int wd = alloc_vreg();
            fprintf(C.nir, "    zext %%%d, %%%d\n", wd, l.vreg);
            if (right_is_imm) {
                fprintf(C.nir, "    %s %%%d, %%%d, %d\n", op_str(op), wd, wd, right_imm);
            } else {
                int wd2 = alloc_vreg();
                fprintf(C.nir, "    zext %%%d, %%%d\n", wd2, r.vreg);
                fprintf(C.nir, "    %s %%%d, %%%d, %%%d\n", op_str(op), wd, wd, wd2);
            }
            fprintf(C.nir, ".vreg %%%d, u8\n", dst);
            fprintf(C.nir, "    mov %%%d, %%%d\n", dst, wd);
            return TV(dst, result_type);
        }
        if (right_is_imm) {
            fprintf(C.nir, "    %s %%%d, %%%d, %d\n", op_str(op), dst, l.vreg, right_imm);
        } else {
            fprintf(C.nir, "    %s %%%d, %%%d, %%%d\n", op_str(op), dst, l.vreg, r.vreg);
        }
        return TV(dst, result_type);
    }
    case EXPR_UNOP: {
        /* Special case: &function_name — must check before evaluating operand,
         * since function names aren't in the symbol table as variables */
        /* &fn_name — near offset to function */
        if (e->u.unop.op == NIB_ADDR &&
            e->u.unop.operand->kind == EXPR_IDENT &&
            find_function(e->u.unop.operand->u.ident) >= 0) {
            int dst = alloc_vreg();
            fprintf(C.nir, "    mov %%%d, %s\n", dst,
                    e->u.unop.operand->u.ident);
            return TV(dst, mk_type(TYPE_U16));
        }
        /* @fn_name — far pointer to function */
        if (e->u.unop.op == NIB_FAR_ADDR &&
            e->u.unop.operand->kind == EXPR_IDENT &&
            find_function(e->u.unop.operand->u.ident) >= 0) {
            int seg_tmp = alloc_vreg();
            int seg = alloc_vreg();
            int off = alloc_vreg();
            fprintf(C.nir, "    mov %%%d, SEG %s\n", seg_tmp, e->u.unop.operand->u.ident);
            fprintf(C.nir, ".vreg %%%d, seg\n", seg);
            fprintf(C.nir, "    mov %%%d, %%%d\n", seg, seg_tmp);
            fprintf(C.nir, "    mov %%%d, %s\n", off, e->u.unop.operand->u.ident);
            return TV_FAR(off, seg);
        }
        /* @global — far pointer to global variable */
        if (e->u.unop.op == NIB_FAR_ADDR &&
            e->u.unop.operand->kind == EXPR_IDENT) {
            symbol_t *sym = sym_lookup(e->u.unop.operand->u.ident);
            if (sym && sym->is_stack_local)
                return emit_stack_far_addr(sym);
            if (sym && sym->is_global) {
                int seg_tmp = alloc_vreg();
                int seg = alloc_vreg();
                int off = alloc_vreg();
                fprintf(C.nir, "    mov %%%d, SEG %s\n", seg_tmp, sym->name);
                fprintf(C.nir, ".vreg %%%d, seg\n", seg);
                fprintf(C.nir, "    mov %%%d, %%%d\n", seg, seg_tmp);
                fprintf(C.nir, "    mov %%%d, %s\n", off, sym->name);
                return TV_FAR(off, seg);
            }
            cerr(e->line, "@ requires a function, global, or stack local name");
            return TV_FAR(alloc_vreg(), alloc_vreg());
        }

        if (e->u.unop.op == NIB_ADDR &&
            e->u.unop.operand->kind == EXPR_IDENT) {
            symbol_t *sym = sym_lookup(e->u.unop.operand->u.ident);
            if (sym && sym->is_stack_local) {
                int dst = emit_stack_addr(sym);
                return TV(dst, mk_type(TYPE_U16));
            }
        }

        typed_vreg_t operand = emit_expr_typed(e->u.unop.operand);
        int dst = alloc_vreg();
        type_t *result_type = operand.type;

        switch (e->u.unop.op) {
        case NIB_NEG:
            if (operand.type && !type_is_integer(operand.type))
                cerr(e->line, "neg requires integer operand, got %s",
                     type_str(operand.type));
            break;
        case NIB_NOT:
            /* NOT works on integers (bitwise) and bools (complement) */
            if (operand.type && !type_is_integer(operand.type) &&
                operand.type->kind != TYPE_BOOL)
                cerr(e->line, "not requires integer or bool operand, got %s",
                     type_str(operand.type));
            break;
        case NIB_ADDR:
            result_type = mk_type(TYPE_U16);
            /* For reference params, &x is just the reference value — a mov, not LEA */
            fprintf(C.nir, "    mov %%%d, %%%d\n", dst, operand.vreg);
            return TV(dst, result_type);
        case NIB_LNOT:
            if (operand.type && operand.type->kind != TYPE_BOOL)
                cerr(e->line, "logical NOT requires bool operand, got %s",
                     type_str(operand.type));
            result_type = mk_type(TYPE_BOOL);
            break;
        default:
            break;
        }

        fprintf(C.nir, "    %s %%%d, %%%d\n", op_str(e->u.unop.op), dst, operand.vreg);
        return TV(dst, result_type);
    }
    case EXPR_CALL: {
        const char *early_fn_name = "?";
        if (e->u.call.func->kind == EXPR_IDENT)
            early_fn_name = e->u.call.func->u.ident;

        if (strcmp(early_fn_name, "assume") == 0) {
            int argc = expr_list_count(e->u.call.args);
            if (argc != 1) {
                cerr(e->line, "assume() takes exactly one argument");
                return TV(-1, mk_type(TYPE_VOID));
            }
            typed_vreg_t cond = emit_expr_typed(e->u.call.args);
            if (cond.type && cond.type->kind != TYPE_BOOL)
                cerr(e->line, "assume() requires bool, got %s",
                     type_str(cond.type));
            fprintf(C.nir, "    assume %%%d\n", cond.vreg);
            return TV(-1, mk_type(TYPE_VOID));
        }

        if (strcmp(early_fn_name, "port_in") == 0) {
            int dst = alloc_vreg();
            expr_t *port_expr = e->u.call.args;
            if (port_expr) {
                int port_imm;
                if (eval_const_expr(port_expr, &port_imm)) {
                    fprintf(C.nir, "    inb %%%d, %d\n", dst, port_imm);
                } else {
                    typed_vreg_t port = emit_expr_typed(port_expr);
                    fprintf(C.nir, "    inb %%%d, %%%d\n", dst, port.vreg);
                }
            }
            return TV(dst, mk_type(TYPE_U8));
        }

        if (strcmp(early_fn_name, "port_in16") == 0) {
            int dst = alloc_vreg();
            expr_t *port_expr = e->u.call.args;
            if (port_expr) {
                int port_imm;
                if (eval_const_expr(port_expr, &port_imm)) {
                    fprintf(C.nir, "    in %%%d, %d\n", dst, port_imm);
                } else {
                    typed_vreg_t port = emit_expr_typed(port_expr);
                    fprintf(C.nir, "    in %%%d, %%%d\n", dst, port.vreg);
                }
            }
            return TV(dst, mk_type(TYPE_U16));
        }

        if (strcmp(early_fn_name, "port_out") == 0) {
            int dst = alloc_vreg();
            expr_t *port_expr = e->u.call.args;
            expr_t *value_expr = port_expr ? port_expr->next : NULL;
            if (port_expr && value_expr) {
                int port_imm;
                typed_vreg_t port = { -1, -1, NULL };
                bool have_imm = eval_const_expr(port_expr, &port_imm);
                if (!have_imm)
                    port = emit_expr_typed(port_expr);

                typed_vreg_t value = emit_expr_typed(value_expr);
                bool byte_io = !value.type || type_size(value.type) == 1;
                const char *op = byte_io ? "outb" : "out";
                if (have_imm)
                    fprintf(C.nir, "    %s %d, %%%d\n", op, port_imm,
                            value.vreg);
                else
                    fprintf(C.nir, "    %s %%%d, %%%d\n", op, port.vreg,
                            value.vreg);
            }
            return TV(dst, mk_type(TYPE_VOID));
        }

        if (strcmp(early_fn_name, "port_out16") == 0) {
            int dst = alloc_vreg();
            expr_t *port_expr = e->u.call.args;
            expr_t *value_expr = port_expr ? port_expr->next : NULL;
            if (port_expr && value_expr) {
                int port_imm;
                typed_vreg_t port = { -1, -1, NULL };
                bool have_imm = eval_const_expr(port_expr, &port_imm);
                if (!have_imm)
                    port = emit_expr_typed(port_expr);

                typed_vreg_t value = emit_expr_typed(value_expr);
                if (value.type && type_size(value.type) != 2)
                    cerr(e->line, "port_out16 value must be u16, got %s",
                         type_str(value.type));
                if (have_imm)
                    fprintf(C.nir, "    out %d, %%%d\n", port_imm,
                            value.vreg);
                else
                    fprintf(C.nir, "    out %%%d, %%%d\n", port.vreg,
                            value.vreg);
            }
            return TV(dst, mk_type(TYPE_VOID));
        }

        /* Get function name and signature before emitting arguments so
         * parameter types can provide literal/expression context. */
        const char *fn_name = "?";
        if (e->u.call.func->kind == EXPR_IDENT)
            fn_name = e->u.call.func->u.ident;
        int fi = find_function(fn_name);

        /* Emit arguments */
        int argc = expr_list_count(e->u.call.args);
        int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                     "call arguments");
        int *arg_seg_vregs = nib_xcalloc((size_t)argc,
                                         sizeof(*arg_seg_vregs),
                                         "call segment arguments");
        type_t **arg_types = nib_xcalloc((size_t)argc, sizeof(*arg_types),
                                         "call argument types");
        for (int i = 0; i < argc; i++)
            arg_seg_vregs[i] = -1;
        int ai = 0;
        for (expr_t *a = e->u.call.args; a; a = a->next, ai++) {
            type_t *param_type = (fi >= 0 &&
                                  ai < C.functions.items[fi].nparams)
                ? C.functions.items[fi].param_types[ai] : NULL;
            typed_vreg_t av = emit_expr_typed_for(a, param_type);
            arg_vregs[ai] = av.vreg;
            arg_seg_vregs[ai] = av.vreg_seg;
            arg_types[ai] = av.type;
        }
        type_t *ret_type = mk_type(TYPE_VOID);

        /* Check for builtins — expand inline instead of call */
        int dst = alloc_vreg();

        if (strcmp(fn_name, "halt") == 0) {
            fprintf(C.nir, "    hlt\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "nop") == 0) {
            fprintf(C.nir, "    nop\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "pushf") == 0) {
            fprintf(C.nir, "    pushf\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "popf") == 0) {
            fprintf(C.nir, "    popf\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "cli") == 0) {
            fprintf(C.nir, "    cli\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "sti") == 0) {
            fprintf(C.nir, "    sti\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "frame_enter") == 0) {
            if (!C.cur_fn_mods.is_bare)
                cerr(e->line, "frame_enter() is only valid in bare functions");
            fprintf(C.nir, "    frame_enter\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "frame_leave") == 0) {
            if (!C.cur_fn_mods.is_bare)
                cerr(e->line, "frame_leave() is only valid in bare functions");
            fprintf(C.nir, "    frame_leave\n");
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "salc") == 0) {
            fprintf(C.nir, "    salc %%%d\n", dst);
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "memcopy") == 0) {
            /* REP MOVSB: ES:DI=dst, DS:SI=src, CX=count.
             * Must set ES=DS since MOVSB writes through ES. */
            if (argc >= 2) {
                int sz = arg_types[0] ? type_size(arg_types[0]) : 0;
                int di = alloc_vreg();
                int si = alloc_vreg();
                int cx = alloc_vreg();
                fprintf(C.nir, ".prefer %%%d, DI\n", di);
                fprintf(C.nir, ".prefer %%%d, SI\n", si);
                fprintf(C.nir, ".prefer %%%d, CX\n", cx);
                fprintf(C.nir, "    mov %%%d, %%%d\n", di, arg_vregs[0]);
                fprintf(C.nir, "    mov %%%d, %%%d\n", si, arg_vregs[1]);
                fprintf(C.nir, "    mov %%%d, %d\n", cx, sz);
                fprintf(C.nir, "    push DS\n");
                fprintf(C.nir, "    pop ES\n");
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    rep movsb\n");
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "memset") == 0) {
            /* REP STOSB: ES:DI=dst, AL=fill, CX=count.
             * Must set ES=DS since STOSB uses ES segment. */
            if (argc >= 2) {
                int sz = arg_types[0] ? type_size(arg_types[0]) : 0;
                int di = alloc_vreg();
                int al = alloc_vreg();
                int cx = alloc_vreg();
                fprintf(C.nir, ".prefer %%%d, DI\n", di);
                fprintf(C.nir, ".prefer %%%d, AL\n", al);
                fprintf(C.nir, ".vreg %%%d, u8\n", al);
                fprintf(C.nir, ".prefer %%%d, CX\n", cx);
                fprintf(C.nir, "    mov %%%d, %%%d\n", di, arg_vregs[0]);
                fprintf(C.nir, "    mov %%%d, %%%d\n", al, arg_vregs[1]);
                fprintf(C.nir, "    mov %%%d, %d\n", cx, sz);
                fprintf(C.nir, "    push DS\n");
                fprintf(C.nir, "    pop ES\n");
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    rep stosb\n");
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "memcmp") == 0) {
            /* REPE CMPSB: ES:DI=a, DS:SI=b, CX=count.
             * Must set ES=DS since CMPSB reads through ES:DI. */
            if (argc >= 2) {
                int sz = arg_types[0] ? type_size(arg_types[0]) : 0;
                int di = alloc_vreg();
                int si = alloc_vreg();
                int cx = alloc_vreg();
                fprintf(C.nir, ".prefer %%%d, DI\n", di);
                fprintf(C.nir, ".prefer %%%d, SI\n", si);
                fprintf(C.nir, ".prefer %%%d, CX\n", cx);
                fprintf(C.nir, "    mov %%%d, %%%d\n", di, arg_vregs[0]);
                fprintf(C.nir, "    mov %%%d, %%%d\n", si, arg_vregs[1]);
                fprintf(C.nir, "    mov %%%d, %d\n", cx, sz);
                fprintf(C.nir, "    push DS\n");
                fprintf(C.nir, "    pop ES\n");
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    repe cmpsb\n");
            }
            return TV(dst, mk_type(TYPE_BOOL));
        }
        if (strcmp(fn_name, "memscan") == 0) {
            /* REPNE SCASB: ES:DI=haystack, AL=needle, CX=count.
             * Must set ES=DS since SCASB reads through ES:DI. */
            if (argc >= 2) {
                fprintf(C.nir, "    ; memscan %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
                fprintf(C.nir, "    push DS\n");
                fprintf(C.nir, "    pop ES\n");
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    repne scasb\n");
            }
            return TV(dst, mk_type(TYPE_U16));
        }
        if (strcmp(fn_name, "sign_extend") == 0) {
            if (argc >= 1)
                fprintf(C.nir, "    cbw %%%d, %%%d\n", dst, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U16));
        }
        if (strcmp(fn_name, "zero_extend") == 0) {
            if (argc >= 1)
                fprintf(C.nir, "    zext %%%d, %%%d\n", dst, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U16));
        }
        if (strcmp(fn_name, "xlat") == 0) {
            if (argc >= 2) {
                fprintf(C.nir, "    ; xlat %%%d[%%%d]\n", arg_vregs[0], arg_vregs[1]);
                fprintf(C.nir, "    xlat %%%d, %%%d, %%%d\n", dst, arg_vregs[0], arg_vregs[1]);
            }
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "bound") == 0) {
            if (argc >= 3)
                fprintf(C.nir, "    bound %%%d, %%%d, %%%d\n",
                        arg_vregs[0], arg_vregs[1], arg_vregs[2]);
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "daa") == 0 || strcmp(fn_name, "das") == 0 ||
            strcmp(fn_name, "aaa") == 0 || strcmp(fn_name, "aas") == 0 ||
            strcmp(fn_name, "aam") == 0 || strcmp(fn_name, "aad") == 0) {
            if (argc >= 1)
                fprintf(C.nir, "    %s %%%d\n", fn_name, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "emulate") == 0) {
            if (argc >= 2)
                fprintf(C.nir, "    brkem %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "divmod") == 0 || strcmp(fn_name, "sdivmod") == 0) {
            if (argc >= 4) {
                const char *op = (fn_name[0] == 's') ? "idiv" : "div";
                fprintf(C.nir, "    %s %%%d, %%%d, %%%d, %%%d\n",
                        op, arg_vregs[0], arg_vregs[1], arg_vregs[2], arg_vregs[3]);
            }
            return TV(dst, mk_type(TYPE_VOID));
        }

        if (strcmp(fn_name, "load") == 0) {
            /* LODSB/LODSW — load from [SI], advance SI */
            if (argc >= 1) {
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    lods %%%d, %%%d\n", dst, arg_vregs[0]);
            }
            /* Return type matches the source type */
            type_t *src_type = (argc >= 1) ? arg_types[0] : NULL;
            type_t *elem = src_type ? type_of_element(src_type) : mk_type(TYPE_U8);
            return TV(dst, elem ? elem : mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "store") == 0) {
            /* STOSB/STOSW — store to [DI], advance DI */
            if (argc >= 2) {
                fprintf(C.nir, "    setflag DF, 0\n");
                fprintf(C.nir, "    stos %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "extract") == 0) {
            /* V20 EXT — extract bit field */
            if (argc >= 3)
                fprintf(C.nir, "    bext %%%d, %%%d, %%%d, %%%d\n",
                        dst, arg_vregs[0], arg_vregs[1], arg_vregs[2]);
            return TV(dst, mk_type(TYPE_U16));
        }
        if (strcmp(fn_name, "insert") == 0) {
            /* V20 INS — insert bit field */
            if (argc >= 4)
                fprintf(C.nir, "    bins %%%d, %%%d, %%%d, %%%d\n",
                        arg_vregs[0], arg_vregs[1], arg_vregs[2], arg_vregs[3]);
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "nibble_rol") == 0) {
            /* V20 ROL4 — rotate nibbles left through AL */
            if (argc >= 1)
                fprintf(C.nir, "    rol4 %%%d, %%%d\n", dst, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "nibble_ror") == 0) {
            /* V20 ROR4 — rotate nibbles right through AL */
            if (argc >= 1)
                fprintf(C.nir, "    ror4 %%%d, %%%d\n", dst, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "swap_flags") == 0) {
            /* LAHF/SAHF — exchange AH with flags */
            if (argc >= 1)
                fprintf(C.nir, "    swap_flags %%%d, %%%d\n", dst, arg_vregs[0]);
            return TV(dst, mk_type(TYPE_U8));
        }

        /* Not a builtin — regular function call */
        if (e->u.call.func->kind == EXPR_IDENT) {
            if (fi >= 0) {
                if (C.functions.items[fi].nparams != argc)
                    cerr(e->line, "'%s' expects %d arguments, got %d",
                         fn_name, C.functions.items[fi].nparams, argc);
                if (C.functions.items[fi].nreturns > 1)
                    cerr(e->line, "'%s' returns multiple values; use destructuring assignment",
                         fn_name);
                ret_type = C.functions.items[fi].return_type;
            }
        }
        /* Build IR arg list, splitting far params into off+seg vregs.
         * Pre-extract far components before emitting the call. */
        int *ir_args = nib_xcalloc((size_t)argc * 2 + 2,
                                   sizeof(*ir_args), "IR call arguments");
        int nir_args = 0;
        {
            expr_t *arg_expr = e->u.call.args;
            for (int i = 0; i < argc; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
                if (fi >= 0 && i < C.functions.items[fi].nparams && C.functions.items[fi].param_is_far[i]) {
                    /* Far param — split into offset + segment vregs */
                    symbol_t *asym = NULL;
                    if (arg_seg_vregs[i] >= 0) {
                        /* Expression returned a vreg pair — use directly */
                        ir_args[nir_args++] = arg_vregs[i];
                        ir_args[nir_args++] = arg_seg_vregs[i];
                    } else {
                        if (arg_expr && arg_expr->kind == EXPR_IDENT)
                            asym = sym_lookup(arg_expr->u.ident);
                        if (asym && asym->vreg_seg >= 0) {
                            /* Symbol has split vregs — pass both directly */
                            ir_args[nir_args++] = asym->vreg;
                            ir_args[nir_args++] = asym->vreg_seg;
                        } else {
                            /* Memory-based far — extract via far.off/far.seg */
                            int off_v = alloc_vreg();
                            int seg_v = alloc_vreg();
                            fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, arg_vregs[i]);
                            fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, arg_vregs[i]);
                            ir_args[nir_args++] = off_v;
                            ir_args[nir_args++] = seg_v;
                        }
                    }
                } else {
                    ir_args[nir_args++] = arg_vregs[i];
                }
            }
        }
        fprintf(C.nir, "    call %%%d, %s", dst, fn_name);
        for (int i = 0; i < nir_args; i++)
            fprintf(C.nir, ", %%%d", ir_args[i]);
        fprintf(C.nir, "\n");
        return TV(dst, ret_type);
    }
    case EXPR_INDIRECT_CALL: {
        /* addr as name from module(args...) */
        typed_vreg_t addr_val = emit_expr_typed(e->u.indirect_call.addr);
        const char *ext_name = e->u.indirect_call.extern_name;
        const char *descriptor_name = ext_name;
        int fi = find_indirect_descriptor(ext_name,
                                          e->u.indirect_call.module_name,
                                          &descriptor_name);

        /* Emit arguments */
        int argc = expr_list_count(e->u.indirect_call.args);
        int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                     "indirect call arguments");
        int *arg_seg_vregs = nib_xcalloc((size_t)argc,
                                         sizeof(*arg_seg_vregs),
                                         "indirect call segment arguments");
        for (int i = 0; i < argc; i++)
            arg_seg_vregs[i] = -1;
        int ai = 0;
        for (expr_t *a = e->u.indirect_call.args; a; a = a->next, ai++) {
            type_t *param_type = (fi >= 0 &&
                                  ai < C.functions.items[fi].nparams)
                ? C.functions.items[fi].param_types[ai] : NULL;
            typed_vreg_t av = emit_expr_typed_for(a, param_type);
            arg_vregs[ai] = av.vreg;
            arg_seg_vregs[ai] = av.vreg_seg;
        }

        int dst = alloc_vreg();
        type_t *ret_type = mk_type(TYPE_VOID);

        if (fi >= 0)
            ret_type = C.functions.items[fi].return_type;

        /* Build IR arg list with far splitting */
        int *ir_args = nib_xcalloc((size_t)argc * 2 + 2,
                                   sizeof(*ir_args),
                                   "IR indirect call arguments");
        int nir_args = 0;
        expr_t *arg_expr = e->u.indirect_call.args;
        for (int i = 0; i < argc; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
            if (fi >= 0 && i < C.functions.items[fi].nparams && C.functions.items[fi].param_is_far[i]) {
                symbol_t *asym = NULL;
                if (arg_expr && arg_expr->kind == EXPR_IDENT)
                    asym = sym_lookup(arg_expr->u.ident);
                if (arg_seg_vregs[i] >= 0) {
                    ir_args[nir_args++] = arg_vregs[i];
                    ir_args[nir_args++] = arg_seg_vregs[i];
                } else if (asym && asym->vreg_seg >= 0) {
                    ir_args[nir_args++] = asym->vreg;
                    ir_args[nir_args++] = asym->vreg_seg;
                } else {
                    int off_v = alloc_vreg();
                    int seg_v = alloc_vreg();
                    fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, arg_vregs[i]);
                    fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, arg_vregs[i]);
                    ir_args[nir_args++] = off_v;
                    ir_args[nir_args++] = seg_v;
                }
            } else {
                ir_args[nir_args++] = arg_vregs[i];
            }
        }

        /* Emit icall with both offset and seg vregs when available.
         * Format: icall %dst, %off, %seg, name, args...
         * If no seg vreg, %off is a pointer to a memory-resident far32. */
        if (addr_val.vreg_seg >= 0) {
            fprintf(C.nir, "    icall %%%d, %%%d, %%%d, %s",
                    dst, addr_val.vreg, addr_val.vreg_seg,
                    descriptor_name);
        } else {
            fprintf(C.nir, "    icall %%%d, %%%d, %s",
                    dst, addr_val.vreg, descriptor_name);
        }
        for (int i = 0; i < nir_args; i++)
            fprintf(C.nir, ", %%%d", ir_args[i]);
        fprintf(C.nir, "\n");
        return TV(dst, ret_type);
    }
    case EXPR_NEAR_INDIRECT_CALL: {
        /* addr as name(args...) */
        typed_vreg_t addr_val = emit_expr_typed(e->u.indirect_call.addr);
        const char *ext_name = e->u.indirect_call.extern_name;
        const char *descriptor_name = ext_name;
        int fi = find_indirect_descriptor(ext_name, NULL, &descriptor_name);

        if (addr_val.type && addr_val.type->kind == TYPE_FAR)
            cerr(e->line, "near indirect call target must be u16, got %s",
                 type_str(addr_val.type));

        int argc = expr_list_count(e->u.indirect_call.args);
        int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                     "near indirect call arguments");
        int *arg_seg_vregs = nib_xcalloc((size_t)argc,
                                         sizeof(*arg_seg_vregs),
                                         "near indirect call segment arguments");
        for (int i = 0; i < argc; i++)
            arg_seg_vregs[i] = -1;
        int ai = 0;
        for (expr_t *a = e->u.indirect_call.args; a; a = a->next, ai++) {
            type_t *param_type = (fi >= 0 &&
                                  ai < C.functions.items[fi].nparams)
                ? C.functions.items[fi].param_types[ai] : NULL;
            typed_vreg_t av = emit_expr_typed_for(a, param_type);
            arg_vregs[ai] = av.vreg;
            arg_seg_vregs[ai] = av.vreg_seg;
        }

        int dst = alloc_vreg();
        type_t *ret_type = mk_type(TYPE_VOID);

        if (fi >= 0)
            ret_type = C.functions.items[fi].return_type;

        int *ir_args = nib_xcalloc((size_t)argc * 2 + 2,
                                   sizeof(*ir_args),
                                   "IR near indirect call arguments");
        int nir_args = 0;
        expr_t *arg_expr = e->u.indirect_call.args;
        for (int i = 0; i < argc; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
            if (fi >= 0 && i < C.functions.items[fi].nparams && C.functions.items[fi].param_is_far[i]) {
                symbol_t *asym = NULL;
                if (arg_expr && arg_expr->kind == EXPR_IDENT)
                    asym = sym_lookup(arg_expr->u.ident);
                if (arg_seg_vregs[i] >= 0) {
                    ir_args[nir_args++] = arg_vregs[i];
                    ir_args[nir_args++] = arg_seg_vregs[i];
                } else if (asym && asym->vreg_seg >= 0) {
                    ir_args[nir_args++] = asym->vreg;
                    ir_args[nir_args++] = asym->vreg_seg;
                } else {
                    int off_v = alloc_vreg();
                    int seg_v = alloc_vreg();
                    fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, arg_vregs[i]);
                    fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, arg_vregs[i]);
                    ir_args[nir_args++] = off_v;
                    ir_args[nir_args++] = seg_v;
                }
            } else {
                ir_args[nir_args++] = arg_vregs[i];
            }
        }

        fprintf(C.nir, "    ncall %%%d, %%%d, %s",
                dst, addr_val.vreg, descriptor_name);
        for (int i = 0; i < nir_args; i++)
            fprintf(C.nir, ", %%%d", ir_args[i]);
        fprintf(C.nir, "\n");
        return TV(dst, ret_type);
    }
    case EXPR_INDEX: {
        typed_vreg_t arr = emit_expr_typed(e->u.index.array);
        typed_vreg_t idx = emit_expr_typed(e->u.index.index);

        if (arr.type && !type_is_aggregate(arr.type))
            cerr(e->line, "indexing requires array type, got %s", type_str(arr.type));
        if (idx.type && !type_is_integer(idx.type))
            cerr(e->line, "array index must be integer, got %s", type_str(idx.type));

        type_t *elem = type_of_element(arr.type);
        if (!elem) elem = mk_type(TYPE_U8);

        /* Scale index by element size for non-byte elements */
        int elem_shift = 0;
        if (elem->kind == TYPE_U16 || elem->kind == TYPE_SEG) elem_shift = 1;
        else if (elem->kind == TYPE_FAR || elem->kind == TYPE_U32) elem_shift = 2;
        int scaled_idx = idx.vreg;
        if (elem_shift > 0) {
            scaled_idx = alloc_vreg();
            fprintf(C.nir, "    shl %%%d, %%%d, %d\n", scaled_idx, idx.vreg, elem_shift);
        }

        int dst = alloc_vreg();
        if (elem->kind == TYPE_FAR) {
            /* Far32 element: load both offset and segment halves */
            int addr = alloc_vreg();
            fprintf(C.nir, "    add %%%d, %%%d, %%%d\n", addr, arr.vreg, scaled_idx);
            int off_v = alloc_vreg();
            int seg_v = alloc_vreg();
            fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, addr);
            fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, addr);
            return TV_FAR(off_v, seg_v);
        }
        if (type_is_aggregate(elem)) {
            /* Aggregate elements: return address of element (by reference) */
            fprintf(C.nir, "    add %%%d, %%%d, %%%d\n", dst, arr.vreg, scaled_idx);
            return TV(dst, elem);
        }
        const char *op = (elem->kind == TYPE_U8) ? "loadb" : "load";
        fprintf(C.nir, "    %s %%%d, %%%d[%%%d]\n", op, dst, arr.vreg, scaled_idx);
        return TV(dst, elem);
    }
    case EXPR_CHECKED_INDEX: {
        typed_vreg_t arr = emit_expr_typed(e->u.index.array);
        typed_vreg_t idx = emit_expr_typed(e->u.index.index);

        if (arr.type && !type_is_aggregate(arr.type))
            cerr(e->line, "indexing requires array type, got %s", type_str(arr.type));
        if (idx.type && !type_is_integer(idx.type))
            cerr(e->line, "array index must be integer, got %s", type_str(idx.type));

        type_t *elem = type_of_element(arr.type);
        if (!elem) elem = mk_type(TYPE_U8);

        /* Scale index by element size for non-byte elements */
        int elem_shift2 = 0;
        if (elem->kind == TYPE_U16 || elem->kind == TYPE_SEG) elem_shift2 = 1;
        else if (elem->kind == TYPE_FAR || elem->kind == TYPE_U32) elem_shift2 = 2;
        int scaled_idx2 = idx.vreg;
        if (elem_shift2 > 0) {
            scaled_idx2 = alloc_vreg();
            fprintf(C.nir, "    shl %%%d, %%%d, %d\n", scaled_idx2, idx.vreg, elem_shift2);
        }

        int dst = alloc_vreg();
        const char *lop = (elem && elem->kind == TYPE_U8) ? "loadb" : "load";
        int arr_len = (arr.type && arr.type->array_size > 0) ? arr.type->array_size : 0;
        fprintf(C.nir, "    bound %%%d, %d\n", idx.vreg, arr_len);
        fprintf(C.nir, "    %s %%%d, %%%d[%%%d]\n", lop, dst, arr.vreg, scaled_idx2);
        return TV(dst, elem);
    }
    case EXPR_FIELD: {
        typed_vreg_t obj = emit_expr_typed(e->u.field.object);
        return emit_struct_field_value(obj, e->u.field.field_name,
                                       false, e->line);
    }
    case EXPR_MEM: {
        return emit_mem_load_expr_typed(e, NULL);
    }
    case EXPR_DEREF: {
        return emit_deref_load(e, NULL);
    }
    case EXPR_FAR_LIT: {
        /* Far literal — load segment via word vreg, then offset */
        int seg_tmp = alloc_vreg();
        int seg = alloc_vreg();
        int off = alloc_vreg();
        int seg_val = e->u.far_lit.seg_expr
            ? require_const_expr(e->u.far_lit.seg_expr, e->line,
                                 "far literal segment")
            : e->u.far_lit.seg;
        int off_val = e->u.far_lit.off_expr
            ? require_const_expr(e->u.far_lit.off_expr, e->line,
                                 "far literal offset")
            : e->u.far_lit.off;
        fprintf(C.nir, "    mov %%%d, 0x%04X\n", seg_tmp, seg_val & 0xFFFF);
        fprintf(C.nir, ".vreg %%%d, seg\n", seg);
        fprintf(C.nir, "    mov %%%d, %%%d\n", seg, seg_tmp);
        fprintf(C.nir, "    mov %%%d, 0x%04X\n", off, off_val & 0xFFFF);
        return TV_FAR(off, seg);
    }
    case EXPR_RAW_FIELD: {
        /* Same as EXPR_FIELD but returns storage type, ignoring as annotation.
         * Also handles far type component access: ptr`seg, ptr`off
         * and array metadata: arr`len, arr`sz */
        typed_vreg_t obj = emit_expr_typed(e->u.field.object);

        /* Array type: `len (element count) and `sz (byte size) */
        if (obj.type && obj.type->kind == TYPE_ARRAY) {
            const char *fname = e->u.field.field_name;
            if (strcmp(fname, "len") == 0) {
                int dst = alloc_vreg();
                fprintf(C.nir, "    mov %%%d, %d\n", dst, obj.type->array_size);
                return TV(dst, mk_type(TYPE_U16));
            } else if (strcmp(fname, "sz") == 0) {
                int elem_sz = 1;
                if (obj.type->element_type) {
                    switch (obj.type->element_type->kind) {
                    case TYPE_U16: case TYPE_SEG: elem_sz = 2; break;
                    case TYPE_FAR: case TYPE_U32: elem_sz = 4; break;
                    default: elem_sz = 1; break;
                    }
                }
                int dst = alloc_vreg();
                fprintf(C.nir, "    mov %%%d, %d\n", dst, obj.type->array_size * elem_sz);
                return TV(dst, mk_type(TYPE_U16));
            } else {
                cerr(e->line, "array type only has `len and `sz, not `%s", fname);
            }
        }

        /* far type: `seg and `off */
        if (obj.type && obj.type->kind == TYPE_FAR) {
            /* Check if this is a far param with register-split vregs */
            if (e->u.field.object->kind == EXPR_IDENT) {
                symbol_t *sym = sym_lookup(e->u.field.object->u.ident);
                if (sym && sym->vreg_seg >= 0) {
                    if (strcmp(e->u.field.field_name, "off") == 0)
                        return TV(sym->vreg, mk_type(TYPE_U16));
                    else if (strcmp(e->u.field.field_name, "seg") == 0)
                        return TV(sym->vreg_seg, mk_type(TYPE_SEG));
                    else
                        cerr(e->line, "far type only has `seg and `off");
                    return TV(sym->vreg, mk_type(TYPE_U16));
                }
            }
            /* Check if expression itself returned a register pair */
            if (obj.vreg_seg >= 0) {
                if (strcmp(e->u.field.field_name, "off") == 0)
                    return TV(obj.vreg, mk_type(TYPE_U16));
                else if (strcmp(e->u.field.field_name, "seg") == 0)
                    return TV(obj.vreg_seg, mk_type(TYPE_SEG));
                else
                    cerr(e->line, "far type only has `seg and `off");
                return TV(obj.vreg, mk_type(TYPE_U16));
            }
            /* Memory-based far value — load from [ptr] or [ptr+2] */
            int dst = alloc_vreg();
            if (strcmp(e->u.field.field_name, "off") == 0) {
                fprintf(C.nir, "    far.off %%%d, %%%d\n", dst, obj.vreg);
                return TV(dst, mk_type(TYPE_U16));
            } else if (strcmp(e->u.field.field_name, "seg") == 0) {
                fprintf(C.nir, "    far.seg %%%d, %%%d\n", dst, obj.vreg);
                return TV(dst, mk_type(TYPE_SEG));
            } else {
                cerr(e->line, "far type only has `seg and `off");
            }
            return TV(dst, mk_type(TYPE_U16));
        }

        return emit_struct_field_value(obj, e->u.field.field_name,
                                       true, e->line);
    }
    case EXPR_CAST: {
        /* as — zero-instruction type reinterpretation */
        typed_vreg_t val = emit_expr_typed_for(e->u.cast.operand,
                                               e->u.cast.target_type);
        return TV(val.vreg, e->u.cast.target_type);
    }
    case EXPR_PAREN:
        return TV(-1, NULL);
    case EXPR_ARRAY_INIT:
        cerr(e->line, "array initializer not valid in expression context");
        return TV(alloc_vreg(), mk_type(TYPE_VOID));
    }
    return TV(-1, NULL);
}

static typed_vreg_t emit_expr_typed_for(expr_t *e, type_t *target) {
    if (e && e->kind == EXPR_MEM)
        return emit_mem_load_expr_typed(e, target);
    if (e && e->kind == EXPR_DEREF)
        return emit_deref_load(e, target);
    if (e && e->kind == EXPR_CAST && e->u.cast.target_type &&
        is_contextual_mem_load_type(e->u.cast.target_type)) {
        typed_vreg_t val = emit_expr_typed_for(e->u.cast.operand,
                                               e->u.cast.target_type);
        val.type = e->u.cast.target_type;
        return val;
    }
    if (target && type_is_integer(target) && e &&
        e->kind == EXPR_LIT_INT) {
        int r = alloc_vreg();
        fprintf(C.nir, "    mov %%%d, %d\n", r, e->u.lit_int);
        return TV(r, target);
    }
    if (target && type_is_integer(target) && e &&
        e->kind == EXPR_UNOP &&
        (e->u.unop.op == NIB_NEG || e->u.unop.op == NIB_NOT)) {
        typed_vreg_t operand = emit_expr_typed_for(e->u.unop.operand,
                                                   target);
        if (e->u.unop.op == NIB_NEG) {
            if (operand.type && !type_is_integer(operand.type))
                cerr(e->line, "neg requires integer operand, got %s",
                     type_str(operand.type));
        } else if (operand.type && !type_is_integer(operand.type) &&
                   operand.type->kind != TYPE_BOOL) {
            cerr(e->line, "not requires integer or bool operand, got %s",
                 type_str(operand.type));
        }
        int dst = alloc_vreg();
        fprintf(C.nir, "    %s %%%d, %%%d\n",
                op_str(e->u.unop.op), dst, operand.vreg);
        return TV(dst, operand.type ? operand.type : target);
    }
    if (target && type_is_integer(target) && e &&
        e->kind == EXPR_BINOP && !binop_is_compare(e->u.binop.op) &&
        e->u.binop.op != NIB_XCHG) {
        op_kind_t op = e->u.binop.op;
        bool is_shift = binop_is_shift(op);
        bool right_is_imm = (e->u.binop.right->kind == EXPR_LIT_INT);
        int right_imm = right_is_imm ? e->u.binop.right->u.lit_int : 0;
        typed_vreg_t l = emit_expr_typed_for(e->u.binop.left, target);
        typed_vreg_t r = TV(-1, NULL);
        if (!right_is_imm) {
            type_t *right_target = is_shift ? NULL : target;
            r = emit_expr_typed_for(e->u.binop.right, right_target);
        }

        type_t *result_type;
        if (is_shift) {
            if (l.type && !type_is_integer(l.type))
                cerr(e->line, "shift/rotate operand must be integer, got %s",
                     type_str(l.type));
            result_type = l.type ? l.type : target;
        } else {
            result_type = check_arith(l.type, r.type, op, e->line);
            if (!result_type)
                result_type = target;
        }

        int dst = alloc_vreg();
        if (right_is_imm)
            fprintf(C.nir, "    %s %%%d, %%%d, %d\n",
                    op_str(op), dst, l.vreg, right_imm);
        else
            fprintf(C.nir, "    %s %%%d, %%%d, %%%d\n",
                    op_str(op), dst, l.vreg, r.vreg);
        return TV(dst, result_type);
    }

    typed_vreg_t val = emit_expr_typed(e);
    if (target && type_is_integer(target) && !val.type) {
        val.type = target;
        TV(val.vreg, target);
    }
    return val;
}

static typed_vreg_t emit_initializer_expr_typed(expr_t *e, type_t *target) {
    int value;

    if (target && type_is_integer(target) && eval_const_expr(e, &value)) {
        int r = alloc_vreg();
        fprintf(C.nir, "    mov %%%d, %d\n", r, value);
        return TV(r, target);
    }

    return emit_expr_typed_for(e, target);
}

/* ================================================================
 * NIR emission — statement compilation
 * ================================================================ */

static void emit_stmt(stmt_t *s);
static void emit_stmts(stmt_t *list);

static void emit_assign_simple(expr_t *t, typed_vreg_t val, int line) {
    if (t->kind == EXPR_IDENT) {
        symbol_t *sym = sym_lookup(t->u.ident);
        if (!sym) {
            cerr(line, "undefined variable '%s'", t->u.ident);
            return;
        }
        if (!type_assignable(sym->type, val.type))
            cerr(line, "assignment type mismatch: '%s' is %s, got %s",
                 t->u.ident, type_str(sym->type), type_str(val.type));
        if (sym->is_global && sym->type &&
            (sym->type->kind == TYPE_U8 || sym->type->kind == TYPE_U16 ||
             sym->type->kind == TYPE_U32 || sym->type->kind == TYPE_SEG)) {
            if (sym->type->kind == TYPE_U8 && !val.type)
                fprintf(C.nir, ".vreg %%%d, u8\n", val.vreg);
            if (sym->has_at)
                fprintf(C.nir, "    storemem [0x%04X], %%%d\n", sym->at_off, val.vreg);
            else
                fprintf(C.nir, "    storemem [%s], %%%d\n", sym->name, val.vreg);
        } else {
            fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
        }
        return;
    }

    if (t->kind == EXPR_REG || t->kind == EXPR_SREG) {
        const char *name = (t->kind == EXPR_REG) ?
            reg_name_str(t->u.reg.id, t->u.reg.rclass) :
            sreg_name(t->u.reg.id);
        symbol_t *sym = sym_lookup(name);
        if (!sym) {
            if (t->kind == EXPR_SREG) {
                sym = sym_add_pinned(mk_type(TYPE_SEG), t->u.reg.id, REGCLASS_SEG);
            } else {
                type_t *rt = (t->u.reg.rclass == REGCLASS_BYTE) ?
                             mk_type(TYPE_U8) : mk_type(TYPE_U16);
                sym = sym_add_pinned(rt, t->u.reg.id, t->u.reg.rclass);
            }
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
            TV_PIN(sym->vreg, sym->type, name);
        }
        if (!type_assignable(sym->type, val.type))
            cerr(line, "assignment type mismatch: '%s' is %s, got %s",
                 name, type_str(sym->type), type_str(val.type));
        fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
        return;
    }

    cerr(line, "multi-return assignment target must be a variable or register");
}

static bool emit_multi_call_assign(expr_t *targets, expr_t *value, int line) {
    if (!targets || !targets->next)
        return false;
    if (!value || value->kind != EXPR_CALL ||
        value->u.call.func->kind != EXPR_IDENT) {
        cerr(line, "multi-return assignment requires a direct function call");
        return true;
    }

    const char *fn_name = value->u.call.func->u.ident;
    int fi = find_function(fn_name);
    if (fi < 0) {
        cerr(line, "unknown function '%s'", fn_name);
        return true;
    }

    int ntargets = expr_list_count(targets);
    if (C.functions.items[fi].nreturns != ntargets)
        cerr(line, "'%s' returns %d values, assignment has %d targets",
             fn_name, C.functions.items[fi].nreturns, ntargets);
    int argc = expr_list_count(value->u.call.args);
    int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                 "multi-call arguments");
    int *arg_seg_vregs = nib_xcalloc((size_t)argc,
                                     sizeof(*arg_seg_vregs),
                                     "multi-call segment arguments");
    expr_t **arg_exprs = nib_xcalloc((size_t)argc, sizeof(*arg_exprs),
                                     "multi-call argument expressions");
    for (int i = 0; i < argc; i++)
        arg_seg_vregs[i] = -1;
    int ai = 0;
    for (expr_t *a = value->u.call.args; a; a = a->next, ai++) {
        type_t *param_type = (ai < C.functions.items[fi].nparams)
            ? C.functions.items[fi].param_types[ai] : NULL;
        typed_vreg_t av = emit_expr_typed_for(a, param_type);
        arg_vregs[ai] = av.vreg;
        arg_seg_vregs[ai] = av.vreg_seg;
        arg_exprs[ai] = a;
    }
    if (C.functions.items[fi].nparams != argc)
        cerr(line, "'%s' expects %d arguments, got %d",
             fn_name, C.functions.items[fi].nparams, argc);

    int *ir_args = nib_xcalloc((size_t)argc * 2 + 2,
                               sizeof(*ir_args), "IR multi-call arguments");
    int nir_args = 0;
    for (int i = 0; i < argc; i++) {
        if (i < C.functions.items[fi].nparams && C.functions.items[fi].param_is_far[i]) {
            symbol_t *asym = NULL;
            if (arg_seg_vregs[i] >= 0) {
                ir_args[nir_args++] = arg_vregs[i];
                ir_args[nir_args++] = arg_seg_vregs[i];
            } else {
                if (arg_exprs[i] && arg_exprs[i]->kind == EXPR_IDENT)
                    asym = sym_lookup(arg_exprs[i]->u.ident);
                if (asym && asym->vreg_seg >= 0) {
                    ir_args[nir_args++] = asym->vreg;
                    ir_args[nir_args++] = asym->vreg_seg;
                } else {
                    int off_v = alloc_vreg();
                    int seg_v = alloc_vreg();
                    fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, arg_vregs[i]);
                    fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, arg_vregs[i]);
                    ir_args[nir_args++] = off_v;
                    ir_args[nir_args++] = seg_v;
                }
            }
        } else {
            ir_args[nir_args++] = arg_vregs[i];
        }
    }

    int *ret_vregs = nib_xcalloc((size_t)ntargets, sizeof(*ret_vregs),
                                 "multi-call return vregs");
    fprintf(C.nir, "    mcall");
    for (int i = 0; i < ntargets; i++) {
        ret_vregs[i] = alloc_vreg();
        fprintf(C.nir, " %%%d,", ret_vregs[i]);
    }
    fprintf(C.nir, " %s", fn_name);
    for (int i = 0; i < nir_args; i++)
        fprintf(C.nir, ", %%%d", ir_args[i]);
    fprintf(C.nir, "\n");
    for (int i = 0; i < ntargets; i++) {
        type_t *rt = C.functions.items[fi].return_types[i];
        if (rt)
            fprintf(C.nir, ".vreg %%%d, %s\n", ret_vregs[i], type_str(rt));
    }

    int i = 0;
    for (expr_t *t = targets; t && i < ntargets; t = t->next, i++) {
        typed_vreg_t tv = TV(ret_vregs[i], C.functions.items[fi].return_types[i]);
        emit_assign_simple(t, tv, line);
    }
    return true;
}

/* Emit flag expression as conditional jumps.
 * Emits code that jumps to skip_label if the condition is NOT met. */

static const char *flag_id_to_jcc(reg_id_t id) {
    /* Jump if flag IS set */
    static const char *jcc[] = {"jc","jp","???","jz","js","???","???","jo","???"};
    return (id >= 0 && id < 9) ? jcc[id] : "???";
}

static const char *flag_id_to_jncc(reg_id_t id) {
    /* Jump if flag is NOT set */
    static const char *jncc[] = {"jnc","jnp","???","jnz","jns","???","???","jno","???"};
    return (id >= 0 && id < 9) ? jncc[id] : "???";
}

static void emit_flag_checks(flag_case_t *cases) {
    if (!cases) return;

    for (flag_case_t *c = cases; c; c = c->next) {
        int lbl_skip = C.next_label++;

        /* Emit condition check — jump to skip if NOT met */
        flag_expr_t *fe = c->condition;

        if (fe->kind == FEXPR_FLAG) {
            /* Single flag: jump past if not set */
            fprintf(C.nir, "    %s .L%d\n",
                    flag_id_to_jncc(fe->flag_id), lbl_skip);
        } else if (fe->kind == FEXPR_NOT && fe->left->kind == FEXPR_FLAG) {
            /* !flag: jump past if set */
            fprintf(C.nir, "    %s .L%d\n",
                    flag_id_to_jcc(fe->left->flag_id), lbl_skip);
        } else if (fe->kind == FEXPR_OR) {
            /* a | b: enter if either set — skip only if BOTH clear */
            int lbl_enter = C.next_label++;
            if (fe->left->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jcc(fe->left->flag_id), lbl_enter);
            if (fe->right->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jcc(fe->right->flag_id), lbl_enter);
            fprintf(C.nir, "    jmp .L%d\n", lbl_skip);
            fprintf(C.nir, ".L%d:\n", lbl_enter);
        } else if (fe->kind == FEXPR_AND) {
            /* a & b: enter only if both set — skip if either clear */
            if (fe->left->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jncc(fe->left->flag_id), lbl_skip);
            if (fe->right->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jncc(fe->right->flag_id), lbl_skip);
        } else if (fe->kind == FEXPR_XOR) {
            /* a ^ b: enter if exactly one set */
            int lbl_a_set = C.next_label++;
            if (fe->left->kind == FEXPR_FLAG) {
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jcc(fe->left->flag_id), lbl_a_set);
            }
            /* A clear: enter if B set */
            if (fe->right->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jncc(fe->right->flag_id), lbl_skip);
            fprintf(C.nir, "    jmp .L%d\n", lbl_skip + 1);
            fprintf(C.nir, ".L%d:\n", lbl_a_set);
            /* A set: enter if B clear */
            if (fe->right->kind == FEXPR_FLAG)
                fprintf(C.nir, "    %s .L%d\n",
                        flag_id_to_jcc(fe->right->flag_id), lbl_skip);
            C.next_label++; /* consumed lbl_skip + 1 */
        }

        /* Emit body or trap */
        if (c->is_trap) {
            if (fe->kind == FEXPR_FLAG && fe->flag_id == FLG_OF) {
                fprintf(C.nir, "    into\n");
            } else {
                fprintf(C.nir, "    int 4\n"); /* generic trap */
            }
        } else {
            push_scope();
            emit_stmts(c->body);
            pop_scope();
        }

        fprintf(C.nir, ".L%d:\n", lbl_skip);
    }
}

static void emit_stmts(stmt_t *list) {
    for (stmt_t *s = list; s; s = s->next)
        emit_stmt(s);
}

static void emit_stmt(stmt_t *s) {
    if (!s) return;

    /* Emit source line comment for debug info */
    if (s->line > 0 && s->line != C.last_emitted_line && C.src_file[0]) {
        fprintf(C.nir, "; @%s:%d\n", C.src_file, s->line);
        C.last_emitted_line = s->line;
    }

    switch (s->kind) {
    case STMT_VARDECL: {
        resolve_type_constants(s->u.vardecl.type, s->line);
        symbol_t *sym;
        if (s->u.vardecl.name) {
            sym = sym_add(s->u.vardecl.name, s->u.vardecl.type, false);
        } else {
            sym = sym_add_pinned(s->u.vardecl.type,
                                 s->u.vardecl.pinned_reg,
                                 s->u.vardecl.pin_class);
        }
        if (s->u.vardecl.is_const)
            sym->is_const = true;
        if (!sym->is_pinned && !sym->is_global &&
            should_stack_allocate_local(s->u.vardecl.name, s->u.vardecl.type)) {
            int sz = type_size(s->u.vardecl.type);
            if (sz <= 0) {
                cerr(s->line, "cannot take address of zero-sized local '%s'",
                     s->u.vardecl.name);
            } else {
                sym->is_stack_local = true;
                sym->stack_size = sz;
                fprintf(C.nir, ".local %%%d, %d, \"%s\"\n",
                        sym->vreg, sz, sym->name);
            }
        }
        if (s->u.vardecl.type && s->u.vardecl.type->kind == TYPE_ARRAY &&
            s->u.vardecl.type->array_size == 0 && !s->u.vardecl.init) {
            cerr(s->line, "unsized array requires an initializer");
        }
        if (sym->is_const && !sym->is_stack_local)
            fprintf(C.nir, ".vreg %%%d, %s, const\n", sym->vreg,
                    (s->u.vardecl.type && s->u.vardecl.type->kind == TYPE_U8) ? "u8" : "u16");
        if (sym->is_pinned) {
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
            fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
        }
        if (s->u.vardecl.init) {
            typed_vreg_t val = emit_initializer_expr_typed(s->u.vardecl.init,
                                                           s->u.vardecl.type);
            /* Unsized array: infer size from initializer */
            if (s->u.vardecl.type && s->u.vardecl.type->kind == TYPE_ARRAY &&
                s->u.vardecl.type->array_size == 0 && val.type &&
                val.type->kind == TYPE_ARRAY && val.type->array_size > 0) {
                s->u.vardecl.type->array_size = val.type->array_size;
                sym->type = s->u.vardecl.type;
            }
            /* Type check initializer against declaration type.
             * NULL val.type means literal — always compatible. */
            if (val.type && s->u.vardecl.type) {
                bool ok = types_equal(s->u.vardecl.type, val.type);
                if (!ok && type_is_integer(s->u.vardecl.type) &&
                    type_is_integer(val.type) &&
                    type_size(val.type) <= type_size(s->u.vardecl.type))
                    ok = true;
                /* Array initializer: same element type, init fits */
                if (!ok && s->u.vardecl.type->kind == TYPE_ARRAY &&
                    val.type->kind == TYPE_ARRAY &&
                    types_equal(s->u.vardecl.type->element_type,
                                val.type->element_type) &&
                    val.type->array_size <= s->u.vardecl.type->array_size)
                    ok = true;
                if (!ok && val.type->kind == TYPE_VOID)
                    ok = true;
                if (!ok)
                    cerr(s->line, "initializer type mismatch: declared %s, got %s",
                         type_str(s->u.vardecl.type), type_str(val.type));
            }
            if (sym->is_stack_local) {
                int addr = emit_stack_addr(sym);
                if (type_is_aggregate(sym->type)) {
                    cerr(s->line, "stack local aggregate initializers are not supported");
                } else {
                    const char *op = (sym->type && sym->type->kind == TYPE_U8)
                        ? "storeb" : "store";
                    fprintf(C.nir, "    %s %%%d, %%%d\n", op, addr, val.vreg);
                }
            } else {
                fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
            }
            /* Far32 init: also copy segment vreg */
            if (!sym->is_stack_local &&
                val.vreg_seg >= 0 && s->u.vardecl.type &&
                s->u.vardecl.type->kind == TYPE_FAR) {
                if (sym->vreg_seg < 0) {
                    sym->vreg_seg = alloc_vreg();
                    fprintf(C.nir, ".vreg %%%d, seg\n", sym->vreg_seg);
                }
                fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg_seg, val.vreg_seg);
            }
        }
        break;
    }
    case STMT_ASSIGN: {
        if (emit_multi_call_assign(s->u.assign.target, s->u.assign.value, s->line))
            break;
        /* Check for assignment to const variable */
        expr_t *t = s->u.assign.target;
        if (t->kind == EXPR_IDENT) {
            symbol_t *cs = sym_lookup(t->u.ident);
            if (cs && cs->is_const)
                cerr(s->line, "cannot reassign const variable '%s'", t->u.ident);
        }
        /* For literal RHS assigned to a flag, emit setflag with immediate */
        expr_t *val_expr = s->u.assign.value;
        if (val_expr->kind == EXPR_LIT_INT && t->kind == EXPR_FLAG) {
            fprintf(C.nir, "    setflag %s, %d\n",
                    flag_name(t->u.flag_id), val_expr->u.lit_int);
            break;
        }
        /* For literal RHS assigned to a register, emit immediate directly */
        if (val_expr->kind == EXPR_LIT_INT &&
            (t->kind == EXPR_REG || t->kind == EXPR_SREG ||
             t->kind == EXPR_IDENT)) {
            /* Get target vreg */
            symbol_t *sym = NULL;
            if (t->kind == EXPR_IDENT) {
                sym = sym_lookup(t->u.ident);
            } else {
                const char *name = (t->kind == EXPR_REG) ?
                    reg_name_str(t->u.reg.id, t->u.reg.rclass) :
                    sreg_name(t->u.reg.id);
                sym = sym_lookup(name);
                if (!sym) {
                    if (t->kind == EXPR_SREG) {
                        sym = sym_add_pinned(mk_type(TYPE_SEG), t->u.reg.id, REGCLASS_SEG);
                    } else {
                        type_t *rt = (t->u.reg.rclass == REGCLASS_BYTE) ?
                                     mk_type(TYPE_U8) : mk_type(TYPE_U16);
                        sym = sym_add_pinned(rt, t->u.reg.id, t->u.reg.rclass);
                    }
                    fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
                    TV_PIN(sym->vreg, sym->type, name);
                }
            }
            if (sym) {
                if (sym->is_stack_local) {
                    int tmp = alloc_vreg();
                    fprintf(C.nir, "    mov %%%d, %d\n", tmp, val_expr->u.lit_int);
                    int addr = emit_stack_addr(sym);
                    const char *op = (sym->type && sym->type->kind == TYPE_U8)
                        ? "storeb" : "store";
                    fprintf(C.nir, "    %s %%%d, %%%d\n", op, addr, tmp);
                } else if (sym->is_global && sym->type &&
                    (sym->type->kind == TYPE_U8 || sym->type->kind == TYPE_U16 ||
                     sym->type->kind == TYPE_U32 || sym->type->kind == TYPE_SEG)) {
                    /* Scalar global: store literal to memory */
                    int tmp = alloc_vreg();
                    fprintf(C.nir, "    mov %%%d, %d\n", tmp, val_expr->u.lit_int);
                    if (sym->type->kind == TYPE_U8)
                        fprintf(C.nir, ".vreg %%%d, u8\n", tmp);
                    if (sym->has_at)
                        fprintf(C.nir, "    storemem [0x%04X], %%%d\n", sym->at_off, tmp);
                    else
                        fprintf(C.nir, "    storemem [%s], %%%d\n", sym->name, tmp);
                } else {
                    fprintf(C.nir, "    mov %%%d, %d\n", sym->vreg, val_expr->u.lit_int);
                }
                break;
            }
        }

        type_t *assign_target_type = NULL;
        if (t->kind == EXPR_IDENT) {
            symbol_t *target_sym = sym_lookup(t->u.ident);
            if (target_sym)
                assign_target_type = target_sym->type;
        } else if (t->kind == EXPR_REG) {
            assign_target_type = (t->u.reg.rclass == REGCLASS_BYTE)
                ? mk_type(TYPE_U8) : mk_type(TYPE_U16);
        } else if (t->kind == EXPR_SREG) {
            assign_target_type = mk_type(TYPE_SEG);
        } else if (t->kind == EXPR_FLAG) {
            assign_target_type = mk_type(TYPE_BOOL);
        }
        typed_vreg_t val = emit_expr_typed_for(val_expr, assign_target_type);
        if (t->kind == EXPR_IDENT) {
            symbol_t *sym = sym_lookup(t->u.ident);
            if (!sym) {
                cerr(s->line, "undefined variable '%s'", t->u.ident);
            } else {
                /* Type check — NULL val.type means literal, always ok */
                if (val.type && sym->type && !types_equal(sym->type, val.type)) {
                    bool ok = false;
                    if (type_is_integer(sym->type) && type_is_integer(val.type) &&
                        type_size(val.type) <= type_size(sym->type))
                        ok = true;
                    if (val.type->kind == TYPE_VOID) ok = true;
                    if (!ok)
                        cerr(s->line, "assignment type mismatch: '%s' is %s, got %s",
                             t->u.ident, type_str(sym->type), type_str(val.type));
                }
                if (sym->is_stack_local) {
                    int addr = emit_stack_addr(sym);
                    const char *op = (sym->type && sym->type->kind == TYPE_U8)
                        ? "storeb" : "store";
                    fprintf(C.nir, "    %s %%%d, %%%d\n", op, addr, val.vreg);
                } else if (sym->is_global && sym->type &&
                    (sym->type->kind == TYPE_U8 || sym->type->kind == TYPE_U16 ||
                     sym->type->kind == TYPE_U32 || sym->type->kind == TYPE_SEG)) {
                    /* Scalar global: store value to memory */
                    if (sym->type->kind == TYPE_U8 && !val.type)
                        fprintf(C.nir, ".vreg %%%d, u8\n", val.vreg);
                    if (sym->has_at)
                        fprintf(C.nir, "    storemem [0x%04X], %%%d\n", sym->at_off, val.vreg);
                    else
                        fprintf(C.nir, "    storemem [%s], %%%d\n", sym->name, val.vreg);
                } else {
                    fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
                }
            }
        } else if (t->kind == EXPR_REG || t->kind == EXPR_SREG) {
            const char *name = (t->kind == EXPR_REG) ?
                reg_name_str(t->u.reg.id, t->u.reg.rclass) :
                sreg_name(t->u.reg.id);
            symbol_t *sym = sym_lookup(name);
            if (!sym) {
                /* Auto-declare register on first use */
                if (t->kind == EXPR_SREG) {
                    sym = sym_add_pinned(mk_type(TYPE_SEG), t->u.reg.id, REGCLASS_SEG);
                } else {
                    type_t *rt = (t->u.reg.rclass == REGCLASS_BYTE) ?
                                 mk_type(TYPE_U8) : mk_type(TYPE_U16);
                    sym = sym_add_pinned(rt, t->u.reg.id, t->u.reg.rclass);
                }
                fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
                TV_PIN(sym->vreg, sym->type, name);
            }
            if (val.type && sym->type && !types_equal(sym->type, val.type)) {
                bool ok = false;
                if (type_is_integer(sym->type) && type_is_integer(val.type) &&
                    type_size(val.type) <= type_size(sym->type))
                    ok = true;
                if (val.type->kind == TYPE_VOID) ok = true;
                if (!ok)
                    cerr(s->line, "assignment type mismatch: '%s' is %s, got %s",
                         name, type_str(sym->type), type_str(val.type));
            }
            fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
        } else if (t->kind == EXPR_FLAG) {
            /* Flags accept any value — 0 or 1 */
            fprintf(C.nir, "    setflag %s, %%%d\n",
                    flag_name(t->u.flag_id), val.vreg);
        } else if (t->kind == EXPR_MEM) {
            fprintf(C.nir, "    storemem [");
            if (t->u.mem.abs_seg) {
                int seg = t->u.mem.abs_seg_expr
                    ? require_const_expr(t->u.mem.abs_seg_expr, t->line,
                                         "absolute memory segment")
                    : t->u.mem.abs_seg_val;
                fprintf(C.nir, "0x%04X:", seg & 0xFFFF);
            } else if (t->u.mem.seg != REG_NONE)
                fprintf(C.nir, "%s:", sreg_name(t->u.mem.seg));
            bool np = false;
            if (t->u.mem.base != REG_NONE) {
                fprintf(C.nir, "%s", wreg_name(t->u.mem.base));
                np = true;
            }
            if (t->u.mem.index != REG_NONE) {
                if (np) fprintf(C.nir, "+");
                fprintf(C.nir, "%s", wreg_name(t->u.mem.index));
                np = true;
            }
            if (t->u.mem.has_disp) {
                int disp = t->u.mem.disp_expr
                    ? require_const_expr(t->u.mem.disp_expr, t->line,
                                         "memory displacement")
                    : t->u.mem.disp;
                if (np) fprintf(C.nir, "+");
                fprintf(C.nir, "0x%04X", disp & 0xFFFF);
            }
            fprintf(C.nir, "], %%%d\n", val.vreg);
        } else if (t->kind == EXPR_DEREF) {
            emit_deref_store(t, val);
        } else if (t->kind == EXPR_INDEX) {
            typed_vreg_t arr_tv = emit_expr_typed(t->u.index.array);
            int idx = emit_expr(t->u.index.index);
            type_t *elem = type_of_element(arr_tv.type);
            /* Scale index by element size */
            int es = 0;
            if (elem && (elem->kind == TYPE_U16 || elem->kind == TYPE_SEG)) es = 1;
            else if (elem && (elem->kind == TYPE_FAR || elem->kind == TYPE_U32)) es = 2;
            int sidx = idx;
            if (es > 0) {
                sidx = alloc_vreg();
                fprintf(C.nir, "    shl %%%d, %%%d, %d\n", sidx, idx, es);
            }
            if (elem && elem->kind == TYPE_FAR) {
                /* Far32 element: store both offset and segment halves */
                int off_v, seg_v;
                if (val.vreg_seg >= 0) {
                    /* Register pair — use directly */
                    off_v = val.vreg;
                    seg_v = val.vreg_seg;
                } else {
                    /* Memory-based — extract via far.off/far.seg */
                    off_v = alloc_vreg();
                    seg_v = alloc_vreg();
                    fprintf(C.nir, "    far.off %%%d, %%%d\n", off_v, val.vreg);
                    fprintf(C.nir, "    far.seg %%%d, %%%d\n", seg_v, val.vreg);
                }
                fprintf(C.nir, "    store %%%d[%%%d], %%%d\n",
                        arr_tv.vreg, sidx, off_v);
                int sidx2 = alloc_vreg();
                fprintf(C.nir, "    add %%%d, %%%d, 2\n", sidx2, sidx);
                fprintf(C.nir, "    store %%%d[%%%d], %%%d\n",
                        arr_tv.vreg, sidx2, seg_v);
            } else {
                const char *sop = (elem && elem->kind == TYPE_U8) ? "storeb" : "store";
                fprintf(C.nir, "    %s %%%d[%%%d], %%%d\n", sop, arr_tv.vreg, sidx, val.vreg);
            }
        } else if (t->kind == EXPR_FIELD) {
            emit_struct_field_store(t, val);
        } else {
            cerr(s->line, "invalid assignment target");
        }
        /* Emit flag-check block if present */
        emit_flag_checks(s->u.assign.flag_checks);
        break;
    }
    case STMT_TOGGLE_ASSIGN: {
        typed_vreg_t val_tv = emit_expr_typed(s->u.assign.value);
        int val = val_tv.vreg;
        expr_t *t = s->u.assign.target;
        if (t->kind == EXPR_FLAG) {
            fprintf(C.nir, "    toggleflag %s\n", flag_name(t->u.flag_id));
        } else if (t->kind == EXPR_FIELD) {
            int obj = emit_expr(t->u.field.object);
            fprintf(C.nir, "    togglefield %%%d, %s, %%%d\n",
                    obj, t->u.field.field_name, val);
        } else {
            cerr(s->line, "toggle-assign only valid for flags and bit fields");
        }
        emit_flag_checks(s->u.assign.flag_checks);
        break;
    }
    case STMT_EXPR: {
        emit_expr(s->u.expr);
        break;
    }
    case STMT_IF: {
        typed_vreg_t cond_tv = emit_expr_typed(s->u.if_stmt.cond);
        int cond = cond_tv.vreg;
        if (cond_tv.type && cond_tv.type->kind != TYPE_BOOL)
            cerr(s->line, "if condition must be bool, got %s", type_str(cond_tv.type));
        int lbl_else = C.next_label++;
        int lbl_end = C.next_label++;
        fprintf(C.nir, "    jz %%%d, .L%d\n", cond, lbl_else);
        push_scope();
        emit_stmts(s->u.if_stmt.then_body);
        pop_scope();
        if (s->u.if_stmt.else_body) {
            fprintf(C.nir, "    jmp .L%d\n", lbl_end);
            fprintf(C.nir, ".L%d:\n", lbl_else);
            push_scope();
            emit_stmts(s->u.if_stmt.else_body);
            pop_scope();
            fprintf(C.nir, ".L%d:\n", lbl_end);
        } else {
            fprintf(C.nir, ".L%d:\n", lbl_else);
        }
        break;
    }
    case STMT_WHILE: {
        int lbl_top = C.next_label++;
        int lbl_end = C.next_label++;
        int save_break = C.loop_break_label;
        int save_continue = C.loop_continue_label;
        C.loop_break_label = lbl_end;
        C.loop_continue_label = lbl_top;
        fprintf(C.nir, ".L%d:\n", lbl_top);
        typed_vreg_t cond_tv = emit_expr_typed(s->u.while_stmt.cond);
        int cond = cond_tv.vreg;
        if (cond_tv.type && cond_tv.type->kind != TYPE_BOOL)
            cerr(s->line, "while condition must be bool, got %s", type_str(cond_tv.type));
        fprintf(C.nir, "    jz %%%d, .L%d\n", cond, lbl_end);
        C.loop_depth++;
        push_scope();
        emit_stmts(s->u.while_stmt.body);
        pop_scope();
        C.loop_depth--;
        fprintf(C.nir, "    jmp .L%d\n", lbl_top);
        fprintf(C.nir, ".L%d:\n", lbl_end);
        C.loop_break_label = save_break;
        C.loop_continue_label = save_continue;
        break;
    }
    case STMT_FOR: {
        /* for (CX in start..0) — LOOP instruction */
        int end_val = require_const_expr(s->u.for_stmt.end, s->line,
                                         "for loop end");
        if (end_val != 0)
            cerr(s->line, "for loop end must be 0 for LOOP lowering");
        int lbl_top = C.next_label++;
        int lbl_end = C.next_label++;
        int save_break = C.loop_break_label;
        int save_continue = C.loop_continue_label;
        C.loop_break_label = lbl_end;
        C.loop_continue_label = lbl_top;
        int cx_vreg = alloc_vreg();
        fprintf(C.nir, ".prefer %%%d, CX\n", cx_vreg);
        if (s->u.for_stmt.start->kind == EXPR_LIT_INT) {
            fprintf(C.nir, "    mov %%%d, %d\n", cx_vreg, s->u.for_stmt.start->u.lit_int);
        } else {
            int start = emit_expr(s->u.for_stmt.start);
            fprintf(C.nir, "    mov %%%d, %%%d\n", cx_vreg, start);
        }
        fprintf(C.nir, ".L%d:\n", lbl_top);
        C.loop_depth++;
        push_scope();
        /* Add CX as a scoped symbol so body refs to CX use the loop vreg */
        {
            symbol_t *cx_sym = NIB_VEC_PUSH(&C.scope->syms, "symbols");
            memset(cx_sym, 0, sizeof(*cx_sym));
            strncpy(cx_sym->name, "CX", 63);
            cx_sym->type = mk_type(TYPE_U16);
            cx_sym->vreg = cx_vreg;
            cx_sym->vreg_seg = -1;
            cx_sym->is_pinned = true;
            cx_sym->pinned_reg = 1; /* REG_CX */
            cx_sym->pin_class = REGCLASS_WORD;
        }
        emit_stmts(s->u.for_stmt.body);
        pop_scope();
        C.loop_depth--;
        /* loop references the CX vreg so the binder extends its liveness */
        fprintf(C.nir, "    loop .L%d, %%%d\n", lbl_top, cx_vreg);
        fprintf(C.nir, ".L%d:\n", lbl_end);
        C.loop_break_label = save_break;
        C.loop_continue_label = save_continue;
        break;
    }
    case STMT_RETURN: {
        if (s->u.ret_expr) {
            int nexprs = expr_list_count(s->u.ret_expr);
            if (C.cur_fn_nreturns == 0) {
                cerr(s->line, "return with value in void function");
            } else if (nexprs != C.cur_fn_nreturns) {
                cerr(s->line, "function returns %d values, got %d",
                     C.cur_fn_nreturns, nexprs);
            }
            int *vals = nib_xcalloc((size_t)nexprs, sizeof(*vals),
                                    "return value vregs");
            int i = 0;
            for (expr_t *e = s->u.ret_expr; e; e = e->next, i++) {
                return_t *ret = return_nth(C.cur_fn_returns, i);
                typed_vreg_t val = emit_expr_typed_for(e, ret ? ret->type : NULL);
                if (ret && val.type && !type_assignable(ret->type, val.type) &&
                    val.type->kind != TYPE_VOID)
                    cerr(s->line, "return type mismatch: return %d is %s, got %s",
                         i + 1, type_str(ret->type), type_str(val.type));
                vals[i] = val.vreg;
            }
            fprintf(C.nir, "    retval");
            for (int j = 0; j < i; j++) {
                fprintf(C.nir, "%s %%%d", j == 0 ? "" : ",", vals[j]);
            }
            fprintf(C.nir, "\n");
        } else if (C.cur_fn_nreturns > 0) {
            cerr(s->line, "return without value in function returning %d value%s",
                 C.cur_fn_nreturns, C.cur_fn_nreturns == 1 ? "" : "s");
        }
        fprintf(C.nir, "    ret\n");
        break;
    }
    case STMT_BREAK: {
        if (C.loop_depth == 0)
            cerr(s->line, "break outside loop");
        else
            fprintf(C.nir, "    jmp .L%d\n", C.loop_break_label);
        break;
    }
    case STMT_CONTINUE: {
        if (C.loop_depth == 0)
            cerr(s->line, "continue outside loop");
        else
            fprintf(C.nir, "    jmp .L%d\n", C.loop_continue_label);
        break;
    }
    case STMT_GOTO: {
        /* Check if target is a function name */
        if (find_function(s->u.goto_label) >= 0)
            fprintf(C.nir, "    goto.fn %s\n", s->u.goto_label);
        else
            fprintf(C.nir, "    jmp %s\n", s->u.goto_label);
        break;
    }
    case STMT_TAILCALL: {
        /* tailcall expr — must be a function call */
        expr_t *e = s->u.tailcall_expr;
        if (e->kind == EXPR_CALL) {
            const char *fn_name = "?";
            if (e->u.call.func->kind == EXPR_IDENT)
                fn_name = e->u.call.func->u.ident;
            int fi = find_function(fn_name);
            int argc = expr_list_count(e->u.call.args);
            int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                         "tailcall arguments");
            int ai = 0;
            for (expr_t *a = e->u.call.args; a; a = a->next, ai++) {
                type_t *param_type = (fi >= 0 &&
                                      ai < C.functions.items[fi].nparams)
                    ? C.functions.items[fi].param_types[ai] : NULL;
                arg_vregs[ai] = emit_expr_typed_for(a, param_type).vreg;
            }
            fprintf(C.nir, "    tailcall %s", fn_name);
            for (int i = 0; i < argc; i++)
                fprintf(C.nir, ", %%%d", arg_vregs[i]);
            fprintf(C.nir, "\n");
            break;
        }
        if (e->kind == EXPR_NEAR_INDIRECT_CALL) {
            typed_vreg_t target = emit_expr_typed(e->u.indirect_call.addr);
            if (target.type && target.type->kind == TYPE_FAR)
                cerr(s->line, "near indirect tailcall target must be u16, got %s",
                     type_str(target.type));
            const char *descriptor_name = e->u.indirect_call.extern_name;
            int fi = find_indirect_descriptor(e->u.indirect_call.extern_name,
                                              NULL, &descriptor_name);

            int argc = expr_list_count(e->u.indirect_call.args);
            int *arg_vregs = nib_xcalloc((size_t)argc, sizeof(*arg_vregs),
                                         "near indirect tailcall arguments");
            int *arg_seg_vregs = nib_xcalloc((size_t)argc,
                                             sizeof(*arg_seg_vregs),
                                             "near indirect tailcall segment arguments");
            for (int i = 0; i < argc; i++)
                arg_seg_vregs[i] = -1;
            int ai = 0;
            for (expr_t *a = e->u.indirect_call.args; a; a = a->next, ai++) {
                type_t *param_type = (fi >= 0 &&
                                      ai < C.functions.items[fi].nparams)
                    ? C.functions.items[fi].param_types[ai] : NULL;
                typed_vreg_t av = emit_expr_typed_for(a, param_type);
                arg_vregs[ai] = av.vreg;
                arg_seg_vregs[ai] = av.vreg_seg;
            }

            int *ir_args = nib_xcalloc((size_t)argc * 2 + 2,
                                       sizeof(*ir_args),
                                       "IR near indirect tailcall arguments");
            int nir_args = 0;
            expr_t *arg_expr = e->u.indirect_call.args;
            for (int i = 0; i < argc; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
                if (fi >= 0 && i < C.functions.items[fi].nparams &&
                    C.functions.items[fi].param_is_far[i]) {
                    symbol_t *asym = NULL;
                    if (arg_expr && arg_expr->kind == EXPR_IDENT)
                        asym = sym_lookup(arg_expr->u.ident);
                    if (arg_seg_vregs[i] >= 0) {
                        ir_args[nir_args++] = arg_vregs[i];
                        ir_args[nir_args++] = arg_seg_vregs[i];
                    } else if (asym && asym->vreg_seg >= 0) {
                        ir_args[nir_args++] = asym->vreg;
                        ir_args[nir_args++] = asym->vreg_seg;
                    } else {
                        int off_v = alloc_vreg();
                        int seg_v = alloc_vreg();
                        fprintf(C.nir, "    far.off %%%d, %%%d\n",
                                off_v, arg_vregs[i]);
                        fprintf(C.nir, "    far.seg %%%d, %%%d\n",
                                seg_v, arg_vregs[i]);
                        ir_args[nir_args++] = off_v;
                        ir_args[nir_args++] = seg_v;
                    }
                } else {
                    ir_args[nir_args++] = arg_vregs[i];
                }
            }

            fprintf(C.nir, "    ntailcall %%%d, %s",
                    target.vreg, descriptor_name);
            for (int i = 0; i < nir_args; i++)
                fprintf(C.nir, ", %%%d", ir_args[i]);
            fprintf(C.nir, "\n");
            break;
        }
        cerr(s->line, "tailcall requires a function call");
        break;
    }
    case STMT_LABEL: {
        fprintf(C.nir, "%s:\n", s->u.label_name);
        break;
    }
    case STMT_ASM: {
        fprintf(C.nir, "    asm");
        if (s->u.asm_stmt.has_annotation) {
            fprintf(C.nir, " %s(",
                    s->u.asm_stmt.is_clobbers ? "clobbers" : "preserves");
            for (reg_list_t *r = s->u.asm_stmt.is_clobbers ?
                     s->u.asm_stmt.clobbers : s->u.asm_stmt.preserves;
                 r; r = r->next) {
                if (r->is_flags_all)
                    fprintf(C.nir, "FLAGS");
                else
                    fprintf(C.nir, "%s", reg_name_str(r->id, r->rclass));
                if (r->next) fprintf(C.nir, ", ");
            }
            fprintf(C.nir, ")");
        }
        fprintf(C.nir, " {%s}\n", s->u.asm_stmt.body);
        break;
    }
    case STMT_CONST: {
        /* File-scope style const with literal value in function scope */
        if (s->u.konst.init) {
            int val;
            if (eval_const_expr(s->u.konst.init, &val))
                register_constant(s->u.konst.name, val);
            else
                cerr(s->line, "const initializer must be a constant expression");
        } else {
            register_constant(s->u.konst.name, s->u.konst.value);
        }
        break;
    }
    }
}

/* ================================================================
 * Top-level declaration compilation
 * ================================================================ */

/* Emit .preserves directive, inverting clobbers if needed */
static void emit_preserves(FILE *f, fn_modifiers_t *mods, return_t *rets) {
    if (!mods->has_preserves) return;

    if (mods->is_clobbers) {
        static const struct {
            const char *name;
            int id;
            reg_class_t rclass;
        } all_regs[] = {
            {"AX", WREG_AX, REGCLASS_WORD},
            {"CX", WREG_CX, REGCLASS_WORD},
            {"DX", WREG_DX, REGCLASS_WORD},
            {"BX", WREG_BX, REGCLASS_WORD},
            {"BP", WREG_BP, REGCLASS_WORD},
            {"SI", WREG_SI, REGCLASS_WORD},
            {"DI", WREG_DI, REGCLASS_WORD},
            {"ES", SREG_ES, REGCLASS_SEG},
            {"CS", SREG_CS, REGCLASS_SEG},
            {"SS", SREG_SS, REGCLASS_SEG},
            {"DS", SREG_DS, REGCLASS_SEG},
        };
        bool clobbered[8] = {0};
        bool clobbered_seg[4] = {0};
        bool clobber_flags = false;
        for (reg_list_t *r = mods->preserves; r; r = r->next) {
            if (r->is_flags_all) clobber_flags = true;
            else if (r->rclass == REGCLASS_WORD) clobbered[r->id] = true;
            else if (r->rclass == REGCLASS_BYTE) {
                static const int parent[] = {0,1,2,3,0,1,2,3};
                clobbered[parent[r->id]] = true;
            } else if (r->rclass == REGCLASS_SEG) {
                clobbered_seg[r->id] = true;
            }
        }
        for (return_t *r = rets; r; r = r->next) {
            if (!r->has_pin) continue;
            if (r->pin_class == REGCLASS_WORD)
                clobbered[r->pinned_reg] = true;
            else if (r->pin_class == REGCLASS_BYTE) {
                static const int parent[] = {0,1,2,3,0,1,2,3};
                clobbered[parent[r->pinned_reg]] = true;
            }
        }

        fprintf(f, ".preserves ");
        bool first = true;
        for (size_t i = 0; i < sizeof(all_regs) / sizeof(all_regs[0]); i++) {
            bool is_clobbered = all_regs[i].rclass == REGCLASS_SEG ?
                clobbered_seg[all_regs[i].id] : clobbered[all_regs[i].id];
            if (!is_clobbered) {
                if (!first) fprintf(f, ", ");
                fprintf(f, "%s", all_regs[i].name);
                first = false;
            }
        }
        if (!clobber_flags) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "FLAGS");
        }
        fprintf(f, "\n");
    } else {
        fprintf(f, ".preserves ");
        for (reg_list_t *r = mods->preserves; r; r = r->next) {
            if (r->is_flags_all)
                fprintf(f, "FLAGS");
            else
                fprintf(f, "%s", reg_name_str(r->id, r->rclass));
            if (r->next) fprintf(f, ", ");
        }
        fprintf(f, "\n");
    }
}

static void validate_clobbers(fn_modifiers_t *mods, int line) {
    if (!mods->has_preserves || !mods->is_clobbers)
        return;
    for (reg_list_t *r = mods->preserves; r; r = r->next) {
        if (r->rclass == REGCLASS_SEG &&
            (r->id == SREG_CS || r->id == SREG_SS))
            cerr(line, "functions cannot clobber CS or SS");
    }
}

static void validate_ds_policy(fn_modifiers_t *mods, int line) {
    if (mods->ds_policy == DS_POLICY_LITERAL) {
        if (mods->ds_literal < 0 || mods->ds_literal > 0xFFFF)
            cerr(line, "ds() segment literal must be in range 0x0000..0xFFFF");
    } else if (mods->ds_policy == DS_POLICY_SYMBOL) {
        symbol_t *sym = sym_lookup(mods->ds_symbol);
        if (sym && !sym->is_global)
            cerr(line, "ds(%s) must name a global or data object",
                 mods->ds_symbol);
    }
}

static void emit_ds_policy(FILE *f, fn_modifiers_t *mods) {
    switch (mods->ds_policy) {
    case DS_POLICY_CALLER:
        fprintf(f, ", ds=caller");
        break;
    case DS_POLICY_NONE:
        fprintf(f, ", ds=none");
        break;
    case DS_POLICY_SYMBOL:
        fprintf(f, ", ds=%s", mods->ds_symbol);
        break;
    case DS_POLICY_LITERAL:
        fprintf(f, ", ds=0x%04X", mods->ds_literal & 0xFFFF);
        break;
    case DS_POLICY_UNSPEC:
        break;
    }
}

static void emit_extern_params(FILE *f, param_t *params, bool nir) {
    int pn = 0;
    for (param_t *p = params; p; p = p->next, pn++) {
        abi_place_t place = effective_param_place(p);
        if (nir) {
            if (p->type && p->type->kind == TYPE_FAR) {
                fprintf(f, ".eparam u16, \"%s_off\"", p->name);
                emit_abi_place(f, place);
                if (p->has_pin)
                    fprintf(f, ", pin=%s",
                            reg_name_str(p->pinned_reg, p->pin_class));
                fprintf(f, "\n");
                fprintf(f, ".eparam seg, \"%s_seg\"", p->name);
                emit_abi_place(f, place);
                if (p->has_seg_pin)
                    fprintf(f, ", pin=%s",
                            reg_name_str(p->pinned_seg, REGCLASS_SEG));
                fprintf(f, "\n");
            } else {
                fprintf(f, ".eparam %s, \"%s\"", type_str(p->type), p->name);
                emit_abi_place(f, place);
                if (p->has_pin)
                    fprintf(f, ", pin=%s",
                            reg_name_str(p->pinned_reg, p->pin_class));
                fprintf(f, "\n");
            }
        } else {
            fprintf(f, ".param %%%d, %s, \"%s\"", pn, type_str(p->type),
                    p->name);
            emit_abi_place(f, place);
            if (p->has_pin)
                fprintf(f, ", pin=%s",
                        reg_name_str(p->pinned_reg, p->pin_class));
            if (p->type && p->type->kind == TYPE_FAR && p->has_seg_pin)
                fprintf(f, ":%s",
                        reg_name_str(p->pinned_seg, REGCLASS_SEG));
            fprintf(f, "\n");
            if (p->type && p->type->kind == TYPE_FAR)
                pn++;
        }
    }
}

static void emit_extern_returns(FILE *f, return_t *returns) {
    for (return_t *r = returns; r; r = r->next) {
        fprintf(f, ".returns %s", type_str(r->type));
        emit_abi_place(f, effective_return_place(r));
        if (r->has_pin)
            fprintf(f, ", pin=%s",
                    reg_name_str(r->pinned_reg, r->pin_class));
        fprintf(f, "\n");
    }
}

static void emit_extern_descriptor(FILE *f, const char *name,
                                   fn_modifiers_t *mods,
                                   param_t *params, return_t *returns,
                                   bool has_address, int addr_seg,
                                   int addr_off, bool nir) {
    fprintf(f, "\n.extern %s", name);
    if (mods->is_far)
        fprintf(f, ", far");
    emit_ds_policy(f, mods);
    if (has_address) {
        fprintf(f, ", addr_seg=0x%04X, addr_off=0x%04X",
                addr_seg, addr_off);
    } else if (nir) {
        fprintf(f, " ; WARNING: no address — unbindable");
    }
    fprintf(f, "\n");
    emit_extern_params(f, params, nir);
    emit_extern_returns(f, returns);
    emit_preserves(f, mods, returns);
    fprintf(f, ".endextern\n");
    if (!nir)
        fprintf(f, "\n");
}

static void compile_fn(decl_t *d) {
    C.next_vreg = 0;
    /* Don't reset next_label — keep it global so labels are unique across functions */
    C.loop_depth = 0;
    C.last_emitted_line = 0;
    C.cur_fn_name = d->u.fn.name;
    C.cur_fn_params = d->u.fn.params;
    C.cur_fn_ret = d->u.fn.return_type;
    C.cur_fn_returns = d->u.fn.returns;
    C.cur_fn_nreturns = d->u.fn.nreturns;
    C.cur_fn_mods = d->u.fn.mods;
    C.addr_taken.len = 0;
    scan_addr_taken_stmt(d->u.fn.body);
    resolve_fn_modifier_constants(&d->u.fn.mods, d->line);
    resolve_param_type_constants(d->u.fn.params, d->line);
    resolve_return_type_constants(d->u.fn.returns, d->line);
    C.cur_fn_mods = d->u.fn.mods;
    validate_ds_policy(&d->u.fn.mods, d->line);
    validate_clobbers(&d->u.fn.mods, d->line);
    if ((d->is_pub || d->u.fn.mods.is_api) && d->u.fn.mods.is_far &&
        d->u.fn.mods.ds_policy == DS_POLICY_UNSPEC)
        cerr(d->line, "public/api far functions must declare ds(...)");
    if (d->u.fn.mods.is_api &&
        (!d->u.fn.mods.has_preserves || !d->u.fn.mods.is_clobbers))
        cerr(d->line, "api functions must declare clobbers(...)");
    if (d->u.fn.mods.is_bare &&
        (d->u.fn.mods.ds_policy == DS_POLICY_SYMBOL ||
         d->u.fn.mods.ds_policy == DS_POLICY_LITERAL))
        cerr(d->line, "bare functions cannot use ds() setup policies");

    /* Validate and register */
    if (d->u.fn.mods.is_interrupt) {
        if (d->is_pub)
            cerr(d->line, "interrupt handlers cannot be pub");
        if (d->u.fn.mods.is_api)
            cerr(d->line, "interrupt handlers cannot be api");
        if (d->u.fn.mods.is_far)
            cerr(d->line, "interrupt handlers cannot be far");
        if (d->u.fn.mods.has_at)
            cerr(d->line, "interrupt handlers cannot have at()");
        if (d->u.fn.params)
            cerr(d->line, "interrupt handlers cannot have parameters");
        if (d->u.fn.nreturns > 0)
            cerr(d->line, "interrupt handlers cannot have a return type");
        /* Record as far32 constant, not a callable function */
        *NIB_VEC_PUSH(&C.isr_names, "interrupt handler names") =
            xstrdup_checked(d->u.fn.name);
    } else {
        int nparams = 0;
        for (param_t *p = d->u.fn.params; p; p = p->next) nparams++;
        register_function_returns(d->u.fn.name, nparams, d->u.fn.returns,
                                  d->u.fn.params);
    }

    if (d->u.fn.mods.is_api) {
        emit_extern_descriptor(C.nir, d->u.fn.name, &d->u.fn.mods,
                               d->u.fn.params, d->u.fn.returns,
                               false, 0, 0, true);
        emit_extern_descriptor(C.nif, d->u.fn.name, &d->u.fn.mods,
                               d->u.fn.params, d->u.fn.returns,
                               false, 0, 0, false);
    }

    /* Emit .nir function header */
    fprintf(C.nir, "\n.fn %s", d->u.fn.name);
    if (d->is_pub) fprintf(C.nir, ", pub");
    if (d->u.fn.mods.is_far) fprintf(C.nir, ", far");
    if (d->u.fn.mods.is_interrupt)
        fprintf(C.nir, ", interrupt");
    if (d->u.fn.mods.is_bare)
        fprintf(C.nir, ", bare");
    if (d->u.fn.mods.has_at)
        fprintf(C.nir, ", at(0x%04X:0x%04X)", d->u.fn.mods.at_seg, d->u.fn.mods.at_off);
    emit_ds_policy(C.nir, &d->u.fn.mods);
    fprintf(C.nir, "\n");

    emit_preserves(C.nir, &d->u.fn.mods, d->u.fn.returns);

    /* Emit .nif function header (only for pub declarations) */
    bool nif = d->is_pub;
    if (nif) {
        fprintf(C.nif, ".fn %s", d->u.fn.name);
        if (d->u.fn.mods.is_far) fprintf(C.nif, ", far");
        emit_ds_policy(C.nif, &d->u.fn.mods);
        fprintf(C.nif, "\n");

        emit_preserves(C.nif, &d->u.fn.mods, d->u.fn.returns);
    }

    /* Create function scope and add parameters */
    push_scope();

    for (param_t *p = d->u.fn.params; p; p = p->next) {
        symbol_t *sym = sym_add(p->name, p->type, false);
        abi_place_t place = effective_param_place(p);

        /* Far params split into two vregs: offset (word) + segment */
        if (p->type && p->type->kind == TYPE_FAR) {
            /* Offset vreg (sym->vreg) */
            fprintf(C.nir, ".param %%%d, u16, \"%s_off\"", sym->vreg, p->name);
            emit_abi_place(C.nir, place);
            if (p->has_pin)
                fprintf(C.nir, ", pin=%s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nir, "\n");
            /* Segment vreg */
            sym->vreg_seg = C.next_vreg++;
            fprintf(C.nir, ".param %%%d, seg, \"%s_seg\"", sym->vreg_seg, p->name);
            emit_abi_place(C.nir, place);
            if (p->has_seg_pin)
                fprintf(C.nir, ", pin=%s", reg_name_str(p->pinned_seg, REGCLASS_SEG));
            fprintf(C.nir, "\n");
            /* .nif: emit as far with pin */
            if (nif) {
                fprintf(C.nif, ".param %%%d, far, \"%s\"", sym->vreg, p->name);
                emit_abi_place(C.nif, place);
                if (p->has_pin && p->has_seg_pin)
                    fprintf(C.nif, ", pin=%s:%s",
                            reg_name_str(p->pinned_seg, REGCLASS_SEG),
                            reg_name_str(p->pinned_reg, p->pin_class));
                fprintf(C.nif, "\n");
            }
            continue;
        }

        /* In the IR, aggregate params are references — the vreg holds
         * a u16 address, not the aggregate itself. The full type goes
         * in the .nif for type checking, but the .nir type reflects
         * what the register actually contains. */
        bool is_ref = (p->type && type_is_aggregate(p->type));
        const char *ir_type = (is_ref && !p->is_value) ? "u16" : type_str(p->type);
        fprintf(C.nir, ".param %%%d, %s, \"%s\"", sym->vreg, ir_type, p->name);
        emit_abi_place(C.nir, place);
        if (p->is_value) fprintf(C.nir, ", value");
        if (p->has_pin)
            fprintf(C.nir, ", pin=%s", reg_name_str(p->pinned_reg, p->pin_class));
        if (is_ref && !p->is_value)
            fprintf(C.nir, " ; ref %s", type_str(p->type));
        fprintf(C.nir, "\n");

        /* .nif keeps the full type for cross-module type checking */
        if (nif) {
            fprintf(C.nif, ".param %%%d, %s, \"%s\"", sym->vreg, type_str(p->type), p->name);
            emit_abi_place(C.nif, place);
            if (p->is_value) fprintf(C.nif, ", value");
            if (p->has_pin)
                fprintf(C.nif, ", pin=%s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nif, "\n");
        }
    }

    for (return_t *r = d->u.fn.returns; r; r = r->next) {
        fprintf(C.nir, ".returns %s", type_str(r->type));
        emit_abi_place(C.nir, effective_return_place(r));
        if (r->has_pin)
            fprintf(C.nir, ", pin=%s",
                    reg_name_str(r->pinned_reg, r->pin_class));
        fprintf(C.nir, "\n");
        if (nif) {
            fprintf(C.nif, ".returns %s", type_str(r->type));
            emit_abi_place(C.nif, effective_return_place(r));
            if (r->has_pin)
                fprintf(C.nif, ", pin=%s",
                        reg_name_str(r->pinned_reg, r->pin_class));
            fprintf(C.nif, "\n");
        }
    }

    /* Add chain variable if present */
    /* Resolve all constant references to literals before emission */
    resolve_constants_stmt(d->u.fn.body);

    /* Emit body */
    emit_stmts(d->u.fn.body);

    pop_scope();

    fprintf(C.nir, ".endfn\n");
    if (nif) fprintf(C.nif, ".endfn\n\n");
}

static void compile_struct(decl_t *d) {
    for (field_t *f = d->u.struc.fields; f; f = f->next) {
        resolve_type_constants(f->type, d->line);
        resolve_type_constants(f->as_type, d->line);
        if (f->bits_expr) {
            f->bits = require_const_expr(f->bits_expr, d->line,
                                         "bit field width");
            f->bits_expr = NULL;
        }
    }
    struct_sig_t *st = NIB_VEC_PUSH(&C.structs, "structs");
    memset(st, 0, sizeof(*st));
    strncpy(st->name, d->u.struc.name, 63);
    st->fields = d->u.struc.fields;
    st->aligned = d->u.struc.aligned;

    /* Emit to .nif so other modules know the struct layout */
    if (d->is_pub) {
        fprintf(C.nif, ".struct %s", d->u.struc.name);
        if (d->u.struc.aligned) fprintf(C.nif, ", aligned");
        fprintf(C.nif, "\n");
        for (field_t *f = d->u.struc.fields; f; f = f->next) {
            if (f->is_bits) {
                fprintf(C.nif, "    %s: bits(%d)\n",
                        f->name ? f->name : "_", f->bits);
            } else {
                if (f->as_type)
                    fprintf(C.nif, "    %s: %s as %s\n", f->name,
                            type_str(f->type), type_str(f->as_type));
                else
                    fprintf(C.nif, "    %s: %s\n", f->name, type_str(f->type));
            }
        }
        fprintf(C.nif, ".endstruct\n\n");
    }
}

static void compile_global(decl_t *d) {
    const char *name = d->u.global.name;
    if (!name) name = "?";
    resolve_type_constants(d->u.global.type, d->line);
    if (d->u.global.has_at && d->u.global.at_seg_expr) {
        d->u.global.at_seg =
            require_const_expr(d->u.global.at_seg_expr, d->line,
                               "global at() segment");
        d->u.global.at_off =
            require_const_expr(d->u.global.at_off_expr, d->line,
                               "global at() offset");
        d->u.global.at_seg_expr = NULL;
        d->u.global.at_off_expr = NULL;
    }

    if (d->u.global.type && d->u.global.type->kind == TYPE_ARRAY &&
        d->u.global.type->array_size == 0 && d->u.global.init) {
        if (d->u.global.init->kind == EXPR_ARRAY_INIT) {
            int nelem = 0;
            for (expr_t *e = d->u.global.init->u.array_init.elements;
                 e; e = e->next)
                nelem++;
            d->u.global.type->array_size = nelem;
        } else if (d->u.global.init->kind == EXPR_LIT_STR) {
            d->u.global.type->array_size =
                strlen(d->u.global.init->u.lit_str);
        }
    }

    /* Add to the current (global) scope */
    symbol_t *gsym = sym_add(name, d->u.global.type, true);
    if (d->u.global.has_at) {
        gsym->has_at = true;
        gsym->at_seg = d->u.global.at_seg;
        gsym->at_off = d->u.global.at_off;
    }
    /* Emit .nif entry */
    if (d->is_pub) {
        fprintf(C.nif, ".global %s, %s", name, type_str(d->u.global.type));
        fprintf(C.nif, "\n");
    }

    /* Check for array initializer */
    if (d->u.global.init && d->u.global.init->kind == EXPR_ARRAY_INIT) {
        type_t *ty = d->u.global.type;
        type_t *elem_type = type_of_element(ty);
        int arr_size = ty ? ty->array_size : 0;

        if (!elem_type) {
            cerr(d->line, "array initializer on non-array type '%s'",
                 type_str(ty));
            return;
        }

        /* Count initializer elements */
        int nelem = 0;
        for (expr_t *e = d->u.global.init->u.array_init.elements; e; e = e->next)
            nelem++;

        if (nelem > arr_size) {
            cerr(d->line, "too many initializer elements (%d) for %s",
                 nelem, type_str(ty));
            return;
        }

        gsym->is_init_data = true;

        /* Emit .data block */
        fprintf(C.nir, "\n.data %s, %s", name, type_str(ty));
        if (d->u.global.has_at)
            fprintf(C.nir, ", at(0x%04X:0x%04X)",
                    d->u.global.at_seg, d->u.global.at_off);
        fprintf(C.nir, "\n");

        int elem_sz = type_size(elem_type);

        for (expr_t *e = d->u.global.init->u.array_init.elements; e; e = e->next) {
            /* &function_name -> near offset */
            if (e->kind == EXPR_UNOP &&
                e->u.unop.op == NIB_ADDR &&
                e->u.unop.operand->kind == EXPR_IDENT &&
                find_function(e->u.unop.operand->u.ident) >= 0) {
                if (elem_sz != 2) {
                    cerr(e->line, "near function reference in non-u16 array");
                    continue;
                }
                fprintf(C.nir, "  near.ref %s\n",
                        e->u.unop.operand->u.ident);
            }
            /* @function_name -> far.ref */
            else if (e->kind == EXPR_UNOP &&
                e->u.unop.op == NIB_FAR_ADDR &&
                e->u.unop.operand->kind == EXPR_IDENT &&
                find_function(e->u.unop.operand->u.ident) >= 0) {
                if (elem_type->kind != TYPE_FAR)
                    cerr(e->line, "function reference in non-far array");
                fprintf(C.nir, "  far.ref %s\n",
                        e->u.unop.operand->u.ident);
            }
            /* far literal seg:off */
            else if (e->kind == EXPR_FAR_LIT) {
                if (elem_type->kind != TYPE_FAR)
                    cerr(e->line, "far literal in non-far array");
                int seg = e->u.far_lit.seg_expr
                    ? require_const_expr(e->u.far_lit.seg_expr, e->line,
                                         "far literal segment")
                    : e->u.far_lit.seg;
                int off = e->u.far_lit.off_expr
                    ? require_const_expr(e->u.far_lit.off_expr, e->line,
                                         "far literal offset")
                    : e->u.far_lit.off;
                fprintf(C.nir, "  far 0x%04X:0x%04X\n",
                        seg & 0xFFFF, off & 0xFFFF);
            }
            /* integer literal */
            else {
                int val;
                if (!eval_const_expr(e, &val)) {
                    cerr(e->line,
                         "global initializer must be a constant (literal, &fn, or far)");
                    continue;
                }
                if (elem_sz == 1)
                    fprintf(C.nir, "  db 0x%02X\n", val & 0xFF);
                else if (elem_sz == 2)
                    fprintf(C.nir, "  dw 0x%04X\n", val & 0xFFFF);
                else if (elem_sz == 4)
                    fprintf(C.nir, "  dd 0x%08X\n", val);
                else
                    fprintf(C.nir, "  dw 0x%04X\n", val & 0xFFFF);
            }
        }

        /* Zero-fill remaining elements */
        for (int i = nelem; i < arr_size; i++) {
            if (elem_type->kind == TYPE_FAR)
                fprintf(C.nir, "  far 0x0000:0x0000\n");
            else if (elem_sz == 1)
                fprintf(C.nir, "  db 0x00\n");
            else if (elem_sz == 2)
                fprintf(C.nir, "  dw 0x0000\n");
            else
                fprintf(C.nir, "  dw 0x0000\n");
        }

        fprintf(C.nir, ".enddata\n");
        return;
    }

    /* String-initialized global: u8[N] name = "..."; */
    if (d->u.global.init && d->u.global.init->kind == EXPR_LIT_STR &&
        d->u.global.type && d->u.global.type->kind == TYPE_ARRAY) {
        const char *str = d->u.global.init->u.lit_str;
        int slen = strlen(str);
        type_t *ty = d->u.global.type;
        int arr_size = ty->array_size;
        if (slen > arr_size) {
            cerr(d->line, "string initializer too long for %s", type_str(ty));
            return;
        }
        gsym->is_init_data = true;
        fprintf(C.nir, "\n.data %s, %s", name, type_str(ty));
        if (d->u.global.has_at)
            fprintf(C.nir, ", at(0x%04X:0x%04X)",
                    d->u.global.at_seg, d->u.global.at_off);
        fprintf(C.nir, "\n");
        for (int i = 0; i < slen; i++)
            fprintf(C.nir, "  db 0x%02X\n", (unsigned char)str[i]);
        for (int i = slen; i < arr_size; i++)
            fprintf(C.nir, "  db 0x00\n");
        fprintf(C.nir, ".enddata\n");
        return;
    }

    /* Non-array global */
    fprintf(C.nir, ".global %s, %s", name, type_str(d->u.global.type));
    if (d->u.global.has_at)
        fprintf(C.nir, ", at(0x%04X:0x%04X)",
                d->u.global.at_seg, d->u.global.at_off);
    if (d->u.global.init)
        fprintf(C.nir, " ; has initializer");
    fprintf(C.nir, "\n");
}

static void compile_extern_fn(decl_t *d) {
    resolve_fn_modifier_constants(&d->u.extern_fn.mods, d->line);
    resolve_param_type_constants(d->u.extern_fn.params, d->line);
    resolve_return_type_constants(d->u.extern_fn.returns, d->line);
    if (d->u.extern_fn.has_address && d->u.extern_fn.addr_seg_expr) {
        d->u.extern_fn.addr_seg =
            require_const_expr(d->u.extern_fn.addr_seg_expr, d->line,
                               "extern address segment");
        d->u.extern_fn.addr_off =
            require_const_expr(d->u.extern_fn.addr_off_expr, d->line,
                               "extern address offset");
        d->u.extern_fn.addr_seg_expr = NULL;
        d->u.extern_fn.addr_off_expr = NULL;
    }
    int nparams = 0;
    for (param_t *p = d->u.extern_fn.params; p; p = p->next) nparams++;
    register_function_returns(d->u.extern_fn.name, nparams,
                              d->u.extern_fn.returns,
                              d->u.extern_fn.params);
    validate_ds_policy(&d->u.extern_fn.mods, d->line);
    validate_clobbers(&d->u.extern_fn.mods, d->line);
    if (d->is_pub && d->u.extern_fn.mods.is_far &&
        d->u.extern_fn.mods.ds_policy == DS_POLICY_UNSPEC)
        cerr(d->line, "public far functions must declare ds(...)");
    if (!d->u.extern_fn.mods.has_preserves ||
        !d->u.extern_fn.mods.is_clobbers)
        cerr(d->line, "extern functions must declare clobbers(...)");

    fn_modifiers_t desc_mods = d->u.extern_fn.mods;
    if (d->u.extern_fn.mods.has_preserves) {
        desc_mods.has_preserves = true;
        desc_mods.preserves = d->u.extern_fn.preserves;
    }

    emit_extern_descriptor(C.nir, d->u.extern_fn.name, &desc_mods,
                           d->u.extern_fn.params, d->u.extern_fn.returns,
                           d->u.extern_fn.has_address,
                           d->u.extern_fn.addr_seg,
                           d->u.extern_fn.addr_off, true);
    emit_extern_descriptor(C.nif, d->u.extern_fn.name, &desc_mods,
                           d->u.extern_fn.params, d->u.extern_fn.returns,
                           d->u.extern_fn.has_address,
                           d->u.extern_fn.addr_seg,
                           d->u.extern_fn.addr_off, false);

    /* If this extern has a body, compile it as a regular function.
     * This is the "implementation form" — defines calling convention
     * (extern signature above) AND provides the implementation. */
    if (d->u.extern_fn.body) {
        decl_t fn_decl;
        memset(&fn_decl, 0, sizeof(fn_decl));
        fn_decl.kind = DECL_FN;
        fn_decl.line = d->line;
        fn_decl.is_pub = d->is_pub;
        fn_decl.u.fn.name = d->u.extern_fn.name;
        fn_decl.u.fn.mods = d->u.extern_fn.mods;
        fn_decl.u.fn.params = d->u.extern_fn.params;
        fn_decl.u.fn.return_type = d->u.extern_fn.return_type;
        fn_decl.u.fn.returns = d->u.extern_fn.returns;
        fn_decl.u.fn.nreturns = d->u.extern_fn.nreturns;
        fn_decl.u.fn.body = d->u.extern_fn.body;
        /* Transfer preserves/clobbers to fn mods */
        if (d->u.extern_fn.mods.has_preserves) {
            fn_decl.u.fn.mods.has_preserves = true;
            fn_decl.u.fn.mods.preserves = d->u.extern_fn.preserves;
        }
        compile_fn(&fn_decl);
    }
}

/* ================================================================
 * .nif import (use directive)
 * ================================================================ */

static char *nif_skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *nif_read_word(char *p, char *buf, int bufsz) {
    p = nif_skip_ws(p);
    int i = 0;
    while (*p && !isspace(*p) && *p != ',' && *p != ')' &&
           *p != ':' && i < bufsz - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    return p;
}

static type_t *nif_parse_type(const char *s) {
    if (strcmp(s, "u8") == 0) return mk_type(TYPE_U8);
    if (strcmp(s, "u16") == 0) return mk_type(TYPE_U16);
    if (strcmp(s, "u32") == 0) return mk_type(TYPE_U32);
    if (strcmp(s, "seg") == 0) return mk_type(TYPE_SEG);
    if (strcmp(s, "bool") == 0) return mk_type(TYPE_BOOL);
    if (strcmp(s, "far") == 0 || strcmp(s, "far32") == 0) return mk_type(TYPE_FAR);
    /* u8[N], u16[N], bcd[N], far[N] */
    if (strncmp(s, "u8[", 3) == 0)
        return mk_type_array(mk_type(TYPE_U8), atoi(s + 3));
    if (strncmp(s, "u16[", 4) == 0)
        return mk_type_array(mk_type(TYPE_U16), atoi(s + 4));
    if (strncmp(s, "bcd[", 4) == 0)
        return mk_type_array(mk_type(TYPE_BCD), atoi(s + 4));
    if (strncmp(s, "far[", 4) == 0)
        return mk_type_array(mk_type(TYPE_FAR), atoi(s + 4));
    if (strncmp(s, "far32[", 6) == 0)
        return mk_type_array(mk_type(TYPE_FAR), atoi(s + 6));
    /* struct name */
    return mk_type_struct(s);
}

static void import_nif(const char *path, int use_line) {
    /* Try path as-is first, then resolve relative to source directory */
    char resolved[512];
    FILE *fp = fopen(path, "r");
    if (!fp && C.src_dir[0]) {
        snprintf(resolved, sizeof(resolved), "%s/%s", C.src_dir, path);
        fp = fopen(resolved, "r");
    }
    if (!fp) {
        cerr(use_line, "cannot open '%s'", path);
        return;
    }

    char line[512];
    char cur_fn[64] = "";
    int cur_nparams = 0;
    return_t *cur_rets = NULL;
    bool_vec_t cur_param_is_far;
    type_ptr_vec_t cur_param_types;
    NIB_VEC_INIT(&cur_param_is_far);
    NIB_VEC_INIT(&cur_param_types);
    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char *p = nif_skip_ws(line);
        if (!*p || *p == ';') continue;

        /* .fn name */
        if (strncmp(p, ".fn ", 4) == 0) {
            p += 4;
            char name[64];
            nif_read_word(p, name, sizeof(name));
            strncpy(cur_fn, name, 63);
            cur_nparams = 0;
            cur_rets = NULL;
            cur_param_is_far.len = 0;
            cur_param_types.len = 0;
            continue;
        }

        /* .extern name */
        if (strncmp(p, ".extern ", 8) == 0) {
            p += 8;
            char name[64];
            nif_read_word(p, name, sizeof(name));
            strncpy(cur_fn, name, 63);
            cur_nparams = 0;
            cur_rets = NULL;
            cur_param_is_far.len = 0;
            cur_param_types.len = 0;
            continue;
        }

        /* .param %N, type, "name" */
        if (strncmp(p, ".param", 6) == 0) {
            p += 6;
            /* Skip vreg */
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
            p = nif_skip_ws(p);
            /* Read type */
            char ptype[32];
            nif_read_word(p, ptype, sizeof(ptype));
            type_t *param_type = nif_parse_type(ptype);
            *NIB_VEC_PUSH(&cur_param_is_far, "imported parameter metadata") =
                (param_type && param_type->kind == TYPE_FAR);
            *NIB_VEC_PUSH(&cur_param_types, "imported parameter types") =
                param_type;
            cur_nparams++;
            continue;
        }

        /* .returns */
        if (strncmp(p, ".returns", 8) == 0) {
            p += 8;
            char type[64];
            nif_read_word(p, type, sizeof(type));
            cur_rets = return_list_append(cur_rets, mk_return(nif_parse_type(type)));
            continue;
        }

        /* .endfn / .endextern — register the function */
        if (strncmp(p, ".endfn", 6) == 0 || strncmp(p, ".endextern", 10) == 0) {
            if (cur_fn[0]) {
                int fi = C.functions.len;
                register_function_returns(cur_fn, cur_nparams, cur_rets, NULL);
                /* Apply far flags parsed from .param lines */
                if (fi < C.functions.len) {
                    int ir_count = 0;
                    for (int pi = 0; pi < cur_nparams; pi++) {
                        C.functions.items[fi].param_is_far[pi] =
                            cur_param_is_far.items[pi];
                        C.functions.items[fi].param_types[pi] =
                            cur_param_types.items[pi];
                        ir_count++;
                        if (cur_param_is_far.items[pi]) ir_count++;
                    }
                    C.functions.items[fi].nparams_ir = ir_count;
                }
            }
            cur_fn[0] = '\0';
            continue;
        }

        /* .struct name [, aligned] */
        if (strncmp(p, ".struct ", 8) == 0) {
            p += 8;
            char name[64];
            p = nif_read_word(p, name, sizeof(name));
            bool aligned = false;
            p = nif_skip_ws(p);
            if (*p == ',') { p++; char q[64]; nif_read_word(p, q, sizeof(q));
                if (strcmp(q, "aligned") == 0) aligned = true; }
            /* Parse fields until .endstruct */
            field_t *fields = NULL, *tail = NULL;
            char sline[512];
            while (fgets(sline, sizeof(sline), fp)) {
                char *sp = sline;
                while (*sp == ' ' || *sp == '\t') sp++;
                int slen = strlen(sp);
                while (slen > 0 && (sp[slen-1] == '\n' || sp[slen-1] == '\r'))
                    sp[--slen] = '\0';
                if (strncmp(sp, ".endstruct", 10) == 0) break;
                /* Parse "name: type" or "name: bits(N)" or "_: bits(N)" */
                char fname[64], ftype[64];
                sp = nif_read_word(sp, fname, sizeof(fname));
                /* Skip colon */
                sp = nif_skip_ws(sp);
                if (*sp == ':') sp++;
                sp = nif_skip_ws(sp);
                sp = nif_read_word(sp, ftype, sizeof(ftype));
                field_t *f;
                if (strncmp(ftype, "bits(", 5) == 0) {
                    int bits = atoi(ftype + 5);
                    f = mk_field_bits(strcmp(fname, "_") == 0 ? NULL : fname, bits);
                } else {
                    /* Check for "as type" */
                    sp = nif_skip_ws(sp);
                    if (strncmp(sp, "as ", 3) == 0) {
                        sp += 3;
                        char astype[64];
                        nif_read_word(sp, astype, sizeof(astype));
                        f = mk_field_typed_ptr(fname, nif_parse_type(ftype),
                                               nif_parse_type(astype));
                    } else {
                        f = mk_field(fname, nif_parse_type(ftype));
                    }
                }
                if (!fields) fields = f;
                else tail->next = f;
                tail = f;
            }
            struct_sig_t *st = NIB_VEC_PUSH(&C.structs, "imported structs");
            memset(st, 0, sizeof(*st));
            strncpy(st->name, name, 63);
            st->fields = fields;
            st->aligned = aligned;
            continue;
        }

        /* .global name, type */
        if (strncmp(p, ".global ", 8) == 0) {
            p += 8;
            char name[64];
            p = nif_read_word(p, name, sizeof(name));
            /* skip comma */
            p = nif_skip_ws(p);
            if (*p == ',') p++;
            char type[64];
            p = nif_read_word(p, type, sizeof(type));
            /* Add to global scope */
            sym_add(name, nif_parse_type(type), true);
            continue;
        }

        /* .const name, value */
        if (strncmp(p, ".const ", 7) == 0) {
            p += 7;
            char name[64];
            p = nif_read_word(p, name, sizeof(name));
            p = nif_skip_ws(p);
            if (*p == ',') p++;
            p = nif_skip_ws(p);
            int value = (int)strtol(p, NULL, 0);
            register_constant(name, value);
            continue;
        }

        /* .preserves — skip (used by binder, not compiler) */
        /* field declarations inside struct — skip for now */
    }

    fclose(fp);
    NIB_VEC_FREE(&cur_param_is_far);
    NIB_VEC_FREE(&cur_param_types);
}

/* ================================================================
 * Main compile entry point
 * ================================================================ */

int compile(program_t *prog, const char *nir_path, const char *nif_path,
            const char *src_dir, const char *src_file) {
    memset(&C, 0, sizeof(C));
    NIB_VEC_INIT(&C.functions);
    NIB_VEC_INIT(&C.structs);
    NIB_VEC_INIT(&C.constants);
    NIB_VEC_INIT(&C.isr_names);
    NIB_VEC_INIT(&C.addr_taken);
    if (src_dir)
        strncpy(C.src_dir, src_dir, sizeof(C.src_dir) - 1);
    if (src_file) {
        /* Store just the basename for debug info */
        const char *slash = strrchr(src_file, '/');
        strncpy(C.src_file, slash ? slash + 1 : src_file, sizeof(C.src_file) - 1);
    }

    C.nir = fopen(nir_path, "w");
    if (!C.nir) { perror(nir_path); return 1; }

    C.nif = fopen(nif_path, "w");
    if (!C.nif) { perror(nif_path); fclose(C.nir); return 1; }

    fprintf(C.nir, "; Nib IR — generated by nib compile\n");
    fprintf(C.nif, "; Nib interface — generated by nib compile\n\n");

    /* Create global scope */
    push_scope();

    for (decl_t *d = prog->decls; d; d = d->next) {
        switch (d->kind) {
        case DECL_FN:
            compile_fn(d);
            break;
        case DECL_STRUCT:
            compile_struct(d);
            break;
        case DECL_GLOBAL:
            compile_global(d);
            break;
        case DECL_EXTERN_GLOBAL:
            compile_global(d);
            break;
        case DECL_EXTERN_FN:
            compile_extern_fn(d);
            break;
        case DECL_USE:
            fprintf(C.nir, ".use \"%s\"\n", d->u.use_path);
            import_nif(d->u.use_path, d->line);
            break;
        case DECL_CONST:
            if (d->u.konst.init)
                d->u.konst.value = require_const_expr(d->u.konst.init,
                                                       d->line,
                                                       "const initializer");
            register_constant(d->u.konst.name, d->u.konst.value);
            if (d->is_pub)
                fprintf(C.nif, ".const %s, %d\n", d->u.konst.name, d->u.konst.value);
            break;
        case DECL_AT:
            if (d->u.at.seg_expr) {
                d->u.at.seg = require_const_expr(d->u.at.seg_expr, d->line,
                                                 "at() segment");
                d->u.at.off = require_const_expr(d->u.at.off_expr, d->line,
                                                 "at() offset");
                d->u.at.seg_expr = NULL;
                d->u.at.off_expr = NULL;
            }
            fprintf(C.nir, "\n.at 0x%04X:0x%04X\n", d->u.at.seg, d->u.at.off);
            break;
        case DECL_ENDAT:
            fprintf(C.nir, ".endat\n");
            break;
        }
    }

    pop_scope();

    fclose(C.nir);
    fclose(C.nif);

    if (C.errors > 0) {
        fprintf(stderr, "%d error(s)\n", C.errors);
        return 1;
    }

    return 0;
}
