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

/* ================================================================
 * Symbol table / scoping
 * ================================================================ */

#define MAX_SYMBOLS 256

typedef struct symbol {
    char        name[64];
    type_t     *type;
    int         vreg;           /* virtual register ID (%0, %1, ...) */
    int         vreg_seg;       /* segment vreg for far params (-1 = none) */
    bool        is_pinned;      /* variable name matches a register */
    int         pinned_reg;
    reg_class_t pin_class;
    bool        is_global;
    bool        has_at;         /* global with at() placement */
    int         at_seg;
    int         at_off;
} symbol_t;

typedef struct scope {
    symbol_t     syms[MAX_SYMBOLS];
    int          nsyms;
    struct scope *parent;
} scope_t;

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
    fn_modifiers_t cur_fn_mods;

    /* Known functions (for call checking) */
    struct {
        char    name[64];
        int     nparams;        /* source-level param count */
        int     nparams_ir;     /* IR param count (far splits add 1) */
        type_t *return_type;
        bool    param_is_far[16]; /* which params are far type */
    } functions[512];
    int nfunctions;

    /* Known structs */
    struct {
        char    name[64];
        field_t *fields;
        bool    aligned;
    } structs[128];
    int nstructs;

    /* Named constants */
    struct {
        char name[64];
        int  value;
    } constants[512];
    int nconstants;
} compiler_t;

static compiler_t C;

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
    s->parent = C.scope;
    C.scope = s;
}

static void pop_scope(void) {
    scope_t *old = C.scope;
    C.scope = old->parent;
    free(old);
}

static symbol_t *sym_lookup(const char *name) {
    for (scope_t *s = C.scope; s; s = s->parent)
        for (int i = 0; i < s->nsyms; i++)
            if (strcmp(s->syms[i].name, name) == 0)
                return &s->syms[i];
    return NULL;
}

static symbol_t *sym_add(const char *name, type_t *type, bool is_global) {
    if (C.scope->nsyms >= MAX_SYMBOLS) {
        fprintf(stderr, "too many symbols in scope\n");
        exit(1);
    }
    symbol_t *sym = &C.scope->syms[C.scope->nsyms++];
    strncpy(sym->name, name, 63);
    sym->name[63] = '\0';
    sym->type = type;
    sym->vreg = C.next_vreg++;
    sym->vreg_seg = -1;
    sym->is_global = is_global;
    sym->is_pinned = false;
    sym->pinned_reg = REG_NONE;
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

/* ---- Struct lookup ---- */

static int find_struct(const char *name) {
    for (int i = 0; i < C.nstructs; i++)
        if (strcmp(C.structs[i].name, name) == 0)
            return i;
    return -1;
}

/* ---- Constant lookup ---- */

static int find_constant(const char *name, int *value) {
    for (int i = 0; i < C.nconstants; i++)
        if (strcmp(C.constants[i].name, name) == 0) {
            if (value) *value = C.constants[i].value;
            return i;
        }
    return -1;
}

static void register_constant(const char *name, int value) {
    if (C.nconstants >= 512) return;
    strncpy(C.constants[C.nconstants].name, name, 63);
    C.constants[C.nconstants].value = value;
    C.nconstants++;
}

/* ---- Function lookup ---- */

static int find_function(const char *name) {
    for (int i = 0; i < C.nfunctions; i++)
        if (strcmp(C.functions[i].name, name) == 0)
            return i;
    return -1;
}

static void register_function(const char *name, int nparams, type_t *ret,
                              param_t *params) {
    if (C.nfunctions >= 512) return;
    int fi = C.nfunctions++;
    strncpy(C.functions[fi].name, name, 63);
    C.functions[fi].nparams = nparams;
    C.functions[fi].return_type = ret;
    /* Track which params are far (for call-site splitting) */
    int ir_count = 0;
    int pi = 0;
    for (param_t *p = params; p && pi < 16; p = p->next, pi++) {
        C.functions[fi].param_is_far[pi] = (p->type && p->type->kind == TYPE_FAR);
        ir_count++;
        if (C.functions[fi].param_is_far[pi]) ir_count++; /* far splits into 2 */
    }
    C.functions[fi].nparams_ir = ir_count;
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
    case TYPE_STRUCT:   return 0; /* would need struct lookup */
    case TYPE_FAR:      return 4;
    case TYPE_VOID:     return 0;
    }
    return 0;
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
    case TYPE_FAR:      return "far";
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

typedef struct { int vreg; type_t *type; } typed_vreg_t;

static typed_vreg_t emit_expr_typed(expr_t *e);

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
    case EXPR_INDEX:
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
            resolve_constants_stmt(s->u.for_stmt.body);
            break;
        case STMT_RETURN:
            resolve_constants_expr(s->u.expr);
            break;
        case STMT_TAILCALL:
            resolve_constants_expr(s->u.tailcall_expr);
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
    typed_vreg_t tv = { vreg, type };
    if (type && type->kind == TYPE_U8 && C.nir)
        fprintf(C.nir, ".byte %%%d\n", vreg);
    return tv;
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
                if (sym->has_at)
                    fprintf(C.nir, "    loadmem %%%d, [0x%04X]\n", r, sym->at_off);
                else
                    fprintf(C.nir, "    loadmem %%%d, [%s]\n", r, sym->name);
            } else {
                /* Aggregate global: load address (passed by reference) */
                if (sym->has_at)
                    fprintf(C.nir, "    mov %%%d, 0x%04X\n", r, sym->at_off);
                else
                    fprintf(C.nir, "    mov %%%d, %s\n", r, sym->name);
            }
            return TV(r, sym->type);
        }
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
            fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg, name);
        }
        return TV(sym->vreg, sym->type);
    }
    case EXPR_SREG: {
        const char *name = sreg_name(e->u.reg.id);
        symbol_t *sym = sym_lookup(name);
        if (!sym) {
            /* Auto-declare segment register on first use */
            sym = sym_add_pinned(mk_type(TYPE_SEG), e->u.reg.id, REGCLASS_SEG);
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg, name);
            fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg, name);
        }
        return TV(sym->vreg, sym->type);
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
        bool is_cmp = (op >= NIB_EQ && op <= NIB_SGTE);
        bool is_shift = (op == NIB_SHL || op == NIB_SHR || op == NIB_SRSHR ||
                         op == NIB_ROL || op == NIB_ROR || op == NIB_RCL || op == NIB_RCR);

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
            int dst = alloc_vreg();
            int cid = C.next_const++;
            fprintf(C.nir, ".const _C%d, far.ref %s\n",
                    cid, e->u.unop.operand->u.ident);
            fprintf(C.nir, "    mov %%%d, _C%d\n", dst, cid);
            return TV(dst, mk_type(TYPE_FAR));
        }
        /* @global — far pointer to global variable */
        if (e->u.unop.op == NIB_FAR_ADDR &&
            e->u.unop.operand->kind == EXPR_IDENT) {
            symbol_t *sym = sym_lookup(e->u.unop.operand->u.ident);
            if (sym && sym->is_global) {
                int dst = alloc_vreg();
                int cid = C.next_const++;
                fprintf(C.nir, ".const _C%d, far.ref %s\n",
                        cid, e->u.unop.operand->u.ident);
                fprintf(C.nir, "    mov %%%d, _C%d\n", dst, cid);
                return TV(dst, mk_type(TYPE_FAR));
            }
            cerr(e->line, "@ requires a function or global name");
            return TV(alloc_vreg(), mk_type(TYPE_FAR));
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
        /* Emit arguments */
        int argc = 0;
        int arg_vregs[16];
        type_t *arg_types[16];
        for (expr_t *a = e->u.call.args; a; a = a->next) {
            if (argc < 16) {
                typed_vreg_t av = emit_expr_typed(a);
                arg_vregs[argc] = av.vreg;
                arg_types[argc] = av.type;
            }
            argc++;
        }
        /* Get function name */
        const char *fn_name = "?";
        type_t *ret_type = mk_type(TYPE_VOID);
        if (e->u.call.func->kind == EXPR_IDENT)
            fn_name = e->u.call.func->u.ident;

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
        if (strcmp(fn_name, "salc") == 0) {
            fprintf(C.nir, "    salc %%%d\n", dst);
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "port_in") == 0) {
            if (argc >= 1) {
                expr_t *port_expr = e->u.call.args;
                if (port_expr && port_expr->kind == EXPR_LIT_INT)
                    fprintf(C.nir, "    inb %%%d, %d\n", dst, port_expr->u.lit_int);
                else
                    fprintf(C.nir, "    inb %%%d, %%%d\n", dst, arg_vregs[0]);
            }
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "port_out") == 0) {
            if (argc >= 2) {
                /* Use outb for byte data, out for word.
                 * Literals (NULL type) default to byte if they fit */
                bool byte_io = !arg_types[1] ||
                               type_size(arg_types[1]) == 1;
                const char *op = byte_io ? "outb" : "out";
                expr_t *port_expr = e->u.call.args;
                if (port_expr && port_expr->kind == EXPR_LIT_INT)
                    fprintf(C.nir, "    %s %d, %%%d\n", op, port_expr->u.lit_int, arg_vregs[1]);
                else
                    fprintf(C.nir, "    %s %%%d, %%%d\n", op, arg_vregs[0], arg_vregs[1]);
            }
            return TV(dst, mk_type(TYPE_VOID));
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
                fprintf(C.nir, ".byte %%%d\n", al);
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
        int fi = -1;
        if (e->u.call.func->kind == EXPR_IDENT) {
            fi = find_function(fn_name);
            if (fi >= 0) {
                if (C.functions[fi].nparams != argc)
                    cerr(e->line, "'%s' expects %d arguments, got %d",
                         fn_name, C.functions[fi].nparams, argc);
                ret_type = C.functions[fi].return_type;
            }
        }
        /* Build IR arg list, splitting far params into off+seg vregs.
         * Pre-extract far components before emitting the call. */
        int ir_args[32];
        int nir_args = 0;
        {
            expr_t *arg_expr = e->u.call.args;
            for (int i = 0; i < argc && i < 16; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
                if (fi >= 0 && C.functions[fi].param_is_far[i]) {
                    /* Far param — split into offset + segment vregs */
                    symbol_t *asym = NULL;
                    if (arg_expr && arg_expr->kind == EXPR_IDENT)
                        asym = sym_lookup(arg_expr->u.ident);
                    if (asym && asym->vreg_seg >= 0) {
                        /* Register-split far — pass both directly */
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

        /* Emit arguments */
        int argc = 0;
        int arg_vregs[16];
        for (expr_t *a = e->u.indirect_call.args; a; a = a->next) {
            if (argc < 16) {
                typed_vreg_t av = emit_expr_typed(a);
                arg_vregs[argc] = av.vreg;
            }
            argc++;
        }

        int dst = alloc_vreg();
        type_t *ret_type = mk_type(TYPE_VOID);

        /* Look up the extern for far-splitting info and return type */
        int fi = find_function(ext_name);
        if (fi >= 0)
            ret_type = C.functions[fi].return_type;

        /* Build IR arg list with far splitting */
        int ir_args[32];
        int nir_args = 0;
        expr_t *arg_expr = e->u.indirect_call.args;
        for (int i = 0; i < argc && i < 16; i++, arg_expr = arg_expr ? arg_expr->next : NULL) {
            if (fi >= 0 && C.functions[fi].param_is_far[i]) {
                symbol_t *asym = NULL;
                if (arg_expr && arg_expr->kind == EXPR_IDENT)
                    asym = sym_lookup(arg_expr->u.ident);
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
            } else {
                ir_args[nir_args++] = arg_vregs[i];
            }
        }

        /* Emit icall: icall %dst, %addr, extern_name, %arg1, ... */
        fprintf(C.nir, "    icall %%%d, %%%d, %s", dst, addr_val.vreg, ext_name);
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

        int dst = alloc_vreg();
        const char *op = (elem && elem->kind == TYPE_U8) ? "loadb" : "load";
        fprintf(C.nir, "    %s %%%d, %%%d[%%%d]\n", op, dst, arr.vreg, idx.vreg);
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

        int dst = alloc_vreg();
        const char *lop = (elem && elem->kind == TYPE_U8) ? "loadb" : "load";
        fprintf(C.nir, "    bound %%%d, %%%d\n", idx.vreg, arr.vreg);
        fprintf(C.nir, "    %s %%%d, %%%d[%%%d]\n", lop, dst, arr.vreg, idx.vreg);
        return TV(dst, elem);
    }
    case EXPR_FIELD: {
        typed_vreg_t obj = emit_expr_typed(e->u.field.object);

        /* far type: use backtick for component access (ptr`seg, ptr`off) */
        if (obj.type && obj.type->kind == TYPE_FAR) {
            cerr(e->line, "use backtick for far components: ptr`seg, ptr`off");
            return TV(alloc_vreg(), mk_type(TYPE_U16));
        }

        /* Verify the struct type has this field */
        type_t *field_type = NULL;
        if (obj.type && obj.type->kind == TYPE_STRUCT && obj.type->struct_name) {
            int si = find_struct(obj.type->struct_name);
            if (si >= 0) {
                bool found = false;
                for (field_t *f = C.structs[si].fields; f; f = f->next) {
                    if (f->name && strcmp(f->name, e->u.field.field_name) == 0) {
                        found = true;
                        if (f->as_type) {
                            /* Typed pointer field — return the as type */
                            field_type = f->as_type;
                        } else if (f->is_bits) {
                            field_type = (f->bits <= 8) ?
                                         mk_type(TYPE_U8) : mk_type(TYPE_U16);
                        } else {
                            field_type = f->type;
                        }
                        break;
                    }
                }
                if (!found)
                    cerr(e->line, "struct '%s' has no field '%s'",
                         obj.type->struct_name, e->u.field.field_name);
            } else {
                cerr(e->line, "unknown struct type '%s'", obj.type->struct_name);
            }
        } else if (obj.type) {
            cerr(e->line, "field access on non-struct type '%s'", type_str(obj.type));
        }
        if (!field_type) field_type = mk_type(TYPE_U16);

        int dst = alloc_vreg();
        fprintf(C.nir, "    field %%%d, %%%d, %s\n", dst, obj.vreg, e->u.field.field_name);
        return TV(dst, field_type);
    }
    case EXPR_MEM: {
        /* Memory access type depends on context — default to u8 */
        int dst = alloc_vreg();
        fprintf(C.nir, "    loadmem %%%d, [", dst);
        if (e->u.mem.abs_seg) {
            fprintf(C.nir, "0x%04X:", e->u.mem.abs_seg_val);
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
            if (need_plus) fprintf(C.nir, "+");
            fprintf(C.nir, "0x%04X", e->u.mem.disp);
        }
        fprintf(C.nir, "]\n");
        /* Type inferred from declaration context — caller must check */
        return TV(dst, mk_type(TYPE_U8));
    }
    case EXPR_FAR_LIT: {
        /* Far literal — emit as constant pool entry (off, seg in LDS format) */
        int cid = C.next_const++;
        int r = alloc_vreg();
        fprintf(C.nir, ".const _C%d, far 0x%04X:0x%04X\n",
                cid, e->u.far_lit.seg, e->u.far_lit.off);
        fprintf(C.nir, "    mov %%%d, _C%d\n", r, cid);
        return TV(r, mk_type(TYPE_FAR));
    }
    case EXPR_RAW_FIELD: {
        /* Same as EXPR_FIELD but returns storage type, ignoring as annotation.
         * Also handles far type component access: ptr`seg, ptr`off */
        typed_vreg_t obj = emit_expr_typed(e->u.field.object);

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

        type_t *field_type = NULL;
        if (obj.type && obj.type->kind == TYPE_STRUCT && obj.type->struct_name) {
            int si = find_struct(obj.type->struct_name);
            if (si >= 0) {
                bool found = false;
                for (field_t *f = C.structs[si].fields; f; f = f->next) {
                    if (f->name && strcmp(f->name, e->u.field.field_name) == 0) {
                        found = true;
                        /* Raw access: always return the storage type, not as_type */
                        if (f->is_bits)
                            field_type = (f->bits <= 8) ? mk_type(TYPE_U8) : mk_type(TYPE_U16);
                        else
                            field_type = f->type;
                        break;
                    }
                }
                if (!found)
                    cerr(e->line, "struct '%s' has no field '%s'",
                         obj.type->struct_name, e->u.field.field_name);
            } else {
                cerr(e->line, "unknown struct type '%s'", obj.type->struct_name);
            }
        } else if (obj.type) {
            cerr(e->line, "raw field access on non-struct type '%s'", type_str(obj.type));
        }
        if (!field_type) field_type = mk_type(TYPE_U16);
        int dst = alloc_vreg();
        fprintf(C.nir, "    field %%%d, %%%d, %s\n", dst, obj.vreg, e->u.field.field_name);
        return TV(dst, field_type);
    }
    case EXPR_CAST: {
        /* as — zero-instruction type reinterpretation */
        typed_vreg_t val = emit_expr_typed(e->u.cast.operand);
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

/* ================================================================
 * NIR emission — statement compilation
 * ================================================================ */

static void emit_stmt(stmt_t *s);
static void emit_stmts(stmt_t *list);

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
        symbol_t *sym;
        if (s->u.vardecl.name) {
            sym = sym_add(s->u.vardecl.name, s->u.vardecl.type, false);
        } else {
            sym = sym_add_pinned(s->u.vardecl.type,
                                 s->u.vardecl.pinned_reg,
                                 s->u.vardecl.pin_class);
        }
        if (s->u.vardecl.type && s->u.vardecl.type->kind == TYPE_ARRAY &&
            s->u.vardecl.type->array_size == 0 && !s->u.vardecl.init) {
            cerr(s->line, "unsized array requires an initializer");
        }
        if (sym->is_pinned) {
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
            fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
        }
        if (s->u.vardecl.init) {
            typed_vreg_t val = emit_expr_typed(s->u.vardecl.init);
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
            fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
        }
        break;
    }
    case STMT_ASSIGN: {
        /* For literal RHS assigned to a register, emit immediate directly */
        expr_t *val_expr = s->u.assign.value;
        expr_t *t = s->u.assign.target;
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
                    fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg, name);
                }
            }
            if (sym) {
                if (sym->is_global && sym->type &&
                    (sym->type->kind == TYPE_U8 || sym->type->kind == TYPE_U16 ||
                     sym->type->kind == TYPE_U32 || sym->type->kind == TYPE_SEG)) {
                    /* Scalar global: store literal to memory */
                    int tmp = alloc_vreg();
                    fprintf(C.nir, "    mov %%%d, %d\n", tmp, val_expr->u.lit_int);
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

        typed_vreg_t val = emit_expr_typed(val_expr);
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
                if (sym->is_global && sym->type &&
                    (sym->type->kind == TYPE_U8 || sym->type->kind == TYPE_U16 ||
                     sym->type->kind == TYPE_U32 || sym->type->kind == TYPE_SEG)) {
                    /* Scalar global: store value to memory */
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
                fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg, name);
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
            if (t->u.mem.abs_seg)
                fprintf(C.nir, "0x%04X:", t->u.mem.abs_seg_val);
            else if (t->u.mem.seg != REG_NONE)
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
                if (np) fprintf(C.nir, "+");
                fprintf(C.nir, "0x%04X", t->u.mem.disp);
            }
            fprintf(C.nir, "], %%%d\n", val.vreg);
        } else if (t->kind == EXPR_INDEX) {
            typed_vreg_t arr_tv = emit_expr_typed(t->u.index.array);
            int idx = emit_expr(t->u.index.index);
            type_t *elem = type_of_element(arr_tv.type);
            const char *sop = (elem && elem->kind == TYPE_U8) ? "storeb" : "store";
            fprintf(C.nir, "    %s %%%d[%%%d], %%%d\n", sop, arr_tv.vreg, idx, val.vreg);
        } else if (t->kind == EXPR_FIELD) {
            int obj = emit_expr(t->u.field.object);
            fprintf(C.nir, "    storefield %%%d, %s, %%%d\n",
                    obj, t->u.field.field_name, val.vreg);
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
        emit_stmts(s->u.for_stmt.body);
        pop_scope();
        C.loop_depth--;
        fprintf(C.nir, "    loop .L%d\n", lbl_top);
        fprintf(C.nir, ".L%d:\n", lbl_end);
        C.loop_break_label = save_break;
        C.loop_continue_label = save_continue;
        break;
    }
    case STMT_RETURN: {
        if (s->u.ret_expr) {
            typed_vreg_t val = emit_expr_typed(s->u.ret_expr);
            if (C.cur_fn_ret) {
                if (val.type && !types_equal(C.cur_fn_ret, val.type)) {
                    if (!(type_is_integer(C.cur_fn_ret) && type_is_integer(val.type) &&
                          type_size(val.type) <= type_size(C.cur_fn_ret)) &&
                        val.type->kind != TYPE_VOID)
                        cerr(s->line, "return type mismatch: function returns %s, got %s",
                             type_str(C.cur_fn_ret), type_str(val.type));
                }
            } else {
                cerr(s->line, "return with value in void function");
            }
            fprintf(C.nir, "    retval %%%d\n", val.vreg);
        } else if (C.cur_fn_ret) {
            cerr(s->line, "return without value in function returning %s",
                 type_str(C.cur_fn_ret));
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
        if (e->kind != EXPR_CALL) {
            cerr(s->line, "tailcall requires a function call");
            break;
        }
        int argc = 0;
        int arg_vregs[16];
        for (expr_t *a = e->u.call.args; a; a = a->next) {
            if (argc < 16)
                arg_vregs[argc] = emit_expr(a);
            argc++;
        }
        const char *fn_name = "?";
        if (e->u.call.func->kind == EXPR_IDENT)
            fn_name = e->u.call.func->u.ident;
        fprintf(C.nir, "    tailcall %s", fn_name);
        for (int i = 0; i < argc && i < 16; i++)
            fprintf(C.nir, ", %%%d", arg_vregs[i]);
        fprintf(C.nir, "\n");
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
    }
}

/* ================================================================
 * Top-level declaration compilation
 * ================================================================ */

/* Emit .preserves directive, inverting clobbers if needed */
static void emit_preserves(FILE *f, fn_modifiers_t *mods) {
    if (!mods->has_preserves) return;

    if (mods->is_clobbers) {
        static const struct { const char *name; int id; } all_regs[] = {
            {"AX",0}, {"CX",1}, {"DX",2}, {"BX",3},
            {"BP",5}, {"SI",6}, {"DI",7},
        };
        bool clobbered[8] = {0};
        bool clobber_flags = false;
        for (reg_list_t *r = mods->preserves; r; r = r->next) {
            if (r->is_flags_all) clobber_flags = true;
            else if (r->rclass == REGCLASS_WORD) clobbered[r->id] = true;
            else if (r->rclass == REGCLASS_BYTE) {
                static const int parent[] = {0,1,2,3,0,1,2,3};
                clobbered[parent[r->id]] = true;
            }
        }
        if (mods->has_ret_pin)
            clobbered[mods->ret_pinned_reg] = true;

        fprintf(f, ".preserves ");
        bool first = true;
        for (int i = 0; i < 7; i++) {
            if (!clobbered[all_regs[i].id]) {
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

static void compile_fn(decl_t *d) {
    C.next_vreg = 0;
    /* Don't reset next_label — keep it global so labels are unique across functions */
    C.loop_depth = 0;
    C.last_emitted_line = 0;
    C.cur_fn_name = d->u.fn.name;
    C.cur_fn_params = d->u.fn.params;
    C.cur_fn_ret = d->u.fn.return_type;
    C.cur_fn_mods = d->u.fn.mods;

    /* Count params */
    int nparams = 0;
    for (param_t *p = d->u.fn.params; p; p = p->next) nparams++;

    /* Register this function */
    register_function(d->u.fn.name, nparams, d->u.fn.return_type,
                       d->u.fn.params);

    /* Emit .nir function header */
    fprintf(C.nir, "\n.fn %s", d->u.fn.name);
    if (d->is_pub) fprintf(C.nir, ", pub");
    if (d->u.fn.mods.is_far) fprintf(C.nir, ", far");
    if (d->u.fn.mods.is_interrupt) {
        fprintf(C.nir, ", interrupt(0x%02X)", d->u.fn.mods.interrupt_vector);
        if (d->u.fn.mods.has_chain)
            fprintf(C.nir, ", chain(%s)", d->u.fn.mods.chain_name);
    }
    if (d->u.fn.mods.is_reentrant) fprintf(C.nir, ", reentrant");
    if (d->u.fn.mods.has_at)
        fprintf(C.nir, ", at(0x%04X:0x%04X)", d->u.fn.mods.at_seg, d->u.fn.mods.at_off);
    fprintf(C.nir, "\n");

    emit_preserves(C.nir, &d->u.fn.mods);

    /* Emit .nif function header (only for pub declarations) */
    bool nif = d->is_pub;
    if (nif) {
        fprintf(C.nif, ".fn %s", d->u.fn.name);
        if (d->u.fn.mods.is_far) fprintf(C.nif, ", far");
        if (d->u.fn.mods.is_interrupt)
            fprintf(C.nif, ", interrupt(0x%02X)", d->u.fn.mods.interrupt_vector);
        if (d->u.fn.mods.is_reentrant) fprintf(C.nif, ", reentrant");
        fprintf(C.nif, "\n");

        emit_preserves(C.nif, &d->u.fn.mods);
    }

    /* Create function scope and add parameters */
    push_scope();

    for (param_t *p = d->u.fn.params; p; p = p->next) {
        symbol_t *sym = sym_add(p->name, p->type, false);

        /* Far params split into two vregs: offset (word) + segment */
        if (p->type && p->type->kind == TYPE_FAR) {
            /* Offset vreg (sym->vreg) */
            fprintf(C.nir, ".param %%%d, u16, \"%s_off\"", sym->vreg, p->name);
            if (p->has_pin)
                fprintf(C.nir, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nir, "\n");
            /* Segment vreg */
            sym->vreg_seg = C.next_vreg++;
            fprintf(C.nir, ".param %%%d, seg, \"%s_seg\"", sym->vreg_seg, p->name);
            if (p->has_seg_pin)
                fprintf(C.nir, ", in %s", reg_name_str(p->pinned_seg, REGCLASS_SEG));
            fprintf(C.nir, "\n");
            /* .nif: emit as far with pin */
            if (nif) {
                fprintf(C.nif, ".param %%%d, far, \"%s\"", sym->vreg, p->name);
                if (p->has_pin && p->has_seg_pin)
                    fprintf(C.nif, ", in %s:%s",
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
        if (p->is_value) fprintf(C.nir, ", value");
        if (p->has_pin)
            fprintf(C.nir, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
        if (is_ref && !p->is_value)
            fprintf(C.nir, " ; ref %s", type_str(p->type));
        fprintf(C.nir, "\n");

        /* .nif keeps the full type for cross-module type checking */
        if (nif) {
            fprintf(C.nif, ".param %%%d, %s, \"%s\"", sym->vreg, type_str(p->type), p->name);
            if (p->is_value) fprintf(C.nif, ", value");
            if (p->has_pin)
                fprintf(C.nif, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nif, "\n");
        }
    }

    if (d->u.fn.return_type) {
        fprintf(C.nir, ".returns %s", type_str(d->u.fn.return_type));
        if (d->u.fn.mods.has_ret_pin)
            fprintf(C.nir, ", in %s",
                    reg_name_str(d->u.fn.mods.ret_pinned_reg,
                                 d->u.fn.mods.ret_pin_class));
        fprintf(C.nir, "\n");
        if (nif) {
            fprintf(C.nif, ".returns %s", type_str(d->u.fn.return_type));
            if (d->u.fn.mods.has_ret_pin)
                fprintf(C.nif, ", in %s",
                        reg_name_str(d->u.fn.mods.ret_pinned_reg,
                                     d->u.fn.mods.ret_pin_class));
            fprintf(C.nif, "\n");
        }
    }

    /* Add chain variable if present */
    if (d->u.fn.mods.has_chain) {
        sym_add(d->u.fn.mods.chain_name, mk_type(TYPE_VOID), false);
    }

    /* Resolve all constant references to literals before emission */
    resolve_constants_stmt(d->u.fn.body);

    /* Emit body */
    emit_stmts(d->u.fn.body);

    pop_scope();

    fprintf(C.nir, ".endfn\n");
    if (nif) fprintf(C.nif, ".endfn\n\n");
}

static void compile_struct(decl_t *d) {
    if (C.nstructs >= 128) return;
    strncpy(C.structs[C.nstructs].name, d->u.struc.name, 63);
    C.structs[C.nstructs].fields = d->u.struc.fields;
    C.structs[C.nstructs].aligned = d->u.struc.aligned;
    C.nstructs++;

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

    /* Add to the current (global) scope */
    symbol_t *gsym = sym_add(name, d->u.global.type, true);
    if (d->u.global.has_at) {
        gsym->has_at = true;
        gsym->at_seg = d->u.global.at_seg;
        gsym->at_off = d->u.global.at_off;
    }

    /* Emit .nif entry (always simple) */
    if (d->is_pub)
        fprintf(C.nif, ".global %s, %s\n", name, type_str(d->u.global.type));

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

        /* Emit .data block */
        fprintf(C.nir, "\n.data %s, %s", name, type_str(ty));
        if (d->u.global.has_at)
            fprintf(C.nir, ", at(0x%04X:0x%04X)",
                    d->u.global.at_seg, d->u.global.at_off);
        fprintf(C.nir, "\n");

        int elem_sz = type_size(elem_type);

        for (expr_t *e = d->u.global.init->u.array_init.elements; e; e = e->next) {
            /* &function_name → far.ref */
            if (e->kind == EXPR_UNOP &&
                (e->u.unop.op == NIB_ADDR || e->u.unop.op == NIB_FAR_ADDR) &&
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
                fprintf(C.nir, "  far 0x%04X:0x%04X\n",
                        e->u.far_lit.seg, e->u.far_lit.off);
            }
            /* integer literal */
            else if (e->kind == EXPR_LIT_INT) {
                if (elem_sz == 1)
                    fprintf(C.nir, "  db 0x%02X\n", e->u.lit_int & 0xFF);
                else if (elem_sz == 2)
                    fprintf(C.nir, "  dw 0x%04X\n", e->u.lit_int & 0xFFFF);
                else if (elem_sz == 4)
                    fprintf(C.nir, "  dd 0x%08X\n", e->u.lit_int);
                else
                    fprintf(C.nir, "  dw 0x%04X\n", e->u.lit_int & 0xFFFF);
            }
            else {
                cerr(e->line,
                     "global initializer must be a constant (literal, &fn, or far)");
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
    int nparams = 0;
    for (param_t *p = d->u.extern_fn.params; p; p = p->next) nparams++;
    register_function(d->u.extern_fn.name, nparams, d->u.extern_fn.return_type,
                       d->u.extern_fn.params);

    /* Emit to .nir so the binder knows how to call it */
    fprintf(C.nir, "\n.extern %s", d->u.extern_fn.name);
    if (d->u.extern_fn.mods.is_far) fprintf(C.nir, ", far");
    if (d->u.extern_fn.mods.is_interrupt)
        fprintf(C.nir, ", interrupt(0x%02X)", d->u.extern_fn.mods.interrupt_vector);
    if (d->u.extern_fn.has_address)
        fprintf(C.nir, ", addr(0x%04X:0x%04X)",
                d->u.extern_fn.addr_seg, d->u.extern_fn.addr_off);
    else
        fprintf(C.nir, " ; WARNING: no address — unbindable");
    fprintf(C.nir, "\n");
    for (param_t *p = d->u.extern_fn.params; p; p = p->next) {
        if (p->type && p->type->kind == TYPE_FAR) {
            /* Far param splits into offset + segment */
            fprintf(C.nir, ".eparam u16, \"%s_off\"", p->name);
            if (p->has_pin)
                fprintf(C.nir, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nir, "\n");
            fprintf(C.nir, ".eparam seg, \"%s_seg\"", p->name);
            if (p->has_seg_pin)
                fprintf(C.nir, ", in %s", reg_name_str(p->pinned_seg, REGCLASS_SEG));
            fprintf(C.nir, "\n");
        } else {
            fprintf(C.nir, ".eparam %s, \"%s\"", type_str(p->type), p->name);
            if (p->has_pin)
                fprintf(C.nir, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
            fprintf(C.nir, "\n");
        }
    }
    fprintf(C.nir, ".endextern\n");

    /* Also emit to .nif for cross-module type checking */
    fprintf(C.nif, ".extern %s", d->u.extern_fn.name);
    if (d->u.extern_fn.mods.is_far) fprintf(C.nif, ", far");
    if (d->u.extern_fn.mods.is_interrupt)
        fprintf(C.nif, ", interrupt(0x%02X)", d->u.extern_fn.mods.interrupt_vector);
    if (d->u.extern_fn.has_address)
        fprintf(C.nif, ", addr(0x%04X:0x%04X)",
                d->u.extern_fn.addr_seg, d->u.extern_fn.addr_off);
    fprintf(C.nif, "\n");

    for (param_t *p = d->u.extern_fn.params; p; p = p->next) {
        fprintf(C.nif, ".param %s, \"%s\"", type_str(p->type), p->name);
        if (p->has_pin)
            fprintf(C.nif, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
        fprintf(C.nif, "\n");
    }
    if (d->u.extern_fn.return_type) {
        fprintf(C.nif, ".returns %s", type_str(d->u.extern_fn.return_type));
        if (d->u.extern_fn.has_ret_pin)
            fprintf(C.nif, ", in %s",
                    reg_name_str(d->u.extern_fn.ret_pinned_reg,
                                 d->u.extern_fn.ret_pin_class));
        fprintf(C.nif, "\n");
    }
    if (d->u.extern_fn.preserves) {
        fprintf(C.nif, ".preserves ");
        for (reg_list_t *r = d->u.extern_fn.preserves; r; r = r->next) {
            if (r->is_flags_all)
                fprintf(C.nif, "FLAGS");
            else
                fprintf(C.nif, "%s", reg_name_str(r->id, r->rclass));
            if (r->next) fprintf(C.nif, ", ");
        }
        fprintf(C.nif, "\n");
    }
    fprintf(C.nif, ".endextern\n\n");
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
    while (*p && !isspace(*p) && *p != ',' && *p != ')' && i < bufsz - 1)
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
    /* u8[N], u16[N], bcd[N] */
    if (strncmp(s, "u8[", 3) == 0)
        return mk_type_array(mk_type(TYPE_U8), atoi(s + 3));
    if (strncmp(s, "u16[", 4) == 0)
        return mk_type_array(mk_type(TYPE_U16), atoi(s + 4));
    if (strncmp(s, "bcd[", 4) == 0)
        return mk_type_array(mk_type(TYPE_BCD), atoi(s + 4));
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
    type_t *cur_ret = NULL;
    bool cur_param_is_far[16] = {0};
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
            cur_ret = NULL;
            memset(cur_param_is_far, 0, sizeof(cur_param_is_far));
            continue;
        }

        /* .extern name */
        if (strncmp(p, ".extern ", 8) == 0) {
            p += 8;
            char name[64];
            nif_read_word(p, name, sizeof(name));
            strncpy(cur_fn, name, 63);
            cur_nparams = 0;
            cur_ret = NULL;
            memset(cur_param_is_far, 0, sizeof(cur_param_is_far));
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
            if (cur_nparams < 16)
                cur_param_is_far[cur_nparams] = (strcmp(ptype, "far") == 0);
            cur_nparams++;
            continue;
        }

        /* .returns */
        if (strncmp(p, ".returns", 8) == 0) {
            p += 8;
            char type[64];
            nif_read_word(p, type, sizeof(type));
            cur_ret = nif_parse_type(type);
            continue;
        }

        /* .endfn / .endextern — register the function */
        if (strncmp(p, ".endfn", 6) == 0 || strncmp(p, ".endextern", 10) == 0) {
            if (cur_fn[0]) {
                int fi = C.nfunctions;
                register_function(cur_fn, cur_nparams, cur_ret, NULL);
                /* Apply far flags parsed from .param lines */
                if (fi < C.nfunctions) {
                    int ir_count = 0;
                    for (int pi = 0; pi < cur_nparams && pi < 16; pi++) {
                        C.functions[fi].param_is_far[pi] = cur_param_is_far[pi];
                        ir_count++;
                        if (cur_param_is_far[pi]) ir_count++;
                    }
                    C.functions[fi].nparams_ir = ir_count;
                }
            }
            cur_fn[0] = '\0';
            continue;
        }

        /* .struct name */
        if (strncmp(p, ".struct ", 8) == 0) {
            p += 8;
            char name[64];
            nif_read_word(p, name, sizeof(name));
            /* Register struct name so type checker knows it */
            if (C.nstructs < 128) {
                strncpy(C.structs[C.nstructs].name, name, 63);
                C.structs[C.nstructs].fields = NULL;
                C.structs[C.nstructs].aligned = false;
                C.nstructs++;
            }
            continue;
        }
        if (strncmp(p, ".endstruct", 10) == 0) {
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
            nif_read_word(p, type, sizeof(type));
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
}

/* ================================================================
 * Main compile entry point
 * ================================================================ */

int compile(program_t *prog, const char *nir_path, const char *nif_path,
            const char *src_dir, const char *src_file) {
    memset(&C, 0, sizeof(C));
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
            register_constant(d->u.konst.name, d->u.konst.value);
            if (d->is_pub)
                fprintf(C.nif, ".const %s, %d\n", d->u.konst.name, d->u.konst.value);
            break;
        case DECL_AT:
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
