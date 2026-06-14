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
    bool        is_pinned;      /* variable name matches a register */
    int         pinned_reg;
    reg_class_t pin_class;
    bool        is_global;
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
    int         next_const;         /* constant pool counter */

    /* Current function info for .nif emission */
    const char *cur_fn_name;
    param_t    *cur_fn_params;
    type_t     *cur_fn_ret;
    fn_modifiers_t cur_fn_mods;

    /* Known functions (for call checking) */
    struct {
        char    name[64];
        int     nparams;
        type_t *return_type;
    } functions[512];
    int nfunctions;

    /* Known structs */
    struct {
        char    name[64];
        field_t *fields;
        bool    aligned;
    } structs[128];
    int nstructs;
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

/* ---- Function lookup ---- */

static int find_function(const char *name) {
    for (int i = 0; i < C.nfunctions; i++)
        if (strcmp(C.functions[i].name, name) == 0)
            return i;
    return -1;
}

static void register_function(const char *name, int nparams, type_t *ret) {
    if (C.nfunctions >= 512) return;
    strncpy(C.functions[C.nfunctions].name, name, 63);
    C.functions[C.nfunctions].nparams = nparams;
    C.functions[C.nfunctions].return_type = ret;
    C.nfunctions++;
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
    case TYPE_ARRAY_U8: return t->array_size;
    case TYPE_ARRAY_U16:return t->array_size * 2;
    case TYPE_BCD:      return t->array_size;
    case TYPE_STRUCT:   return 0; /* would need struct lookup */
    case TYPE_FAR:      return 4;
    case TYPE_VOID:     return 0;
    }
    return 0;
}

static const char *type_str(type_t *t) {
    static char buf[64];
    if (!t) return "void";
    switch (t->kind) {
    case TYPE_U8:       return "u8";
    case TYPE_U16:      return "u16";
    case TYPE_U32:      return "u32";
    case TYPE_SEG:      return "seg";
    case TYPE_BOOL:     return "bool";
    case TYPE_ARRAY_U8: snprintf(buf, sizeof(buf), "u8[%d]", t->array_size); return buf;
    case TYPE_ARRAY_U16:snprintf(buf, sizeof(buf), "u16[%d]", t->array_size); return buf;
    case TYPE_BCD:      snprintf(buf, sizeof(buf), "bcd[%d]", t->array_size); return buf;
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
    case NIB_ADDR: return "lea"; case NIB_LNOT: return "lnot";
    }
    return "??";
}

/* ================================================================
 * Type checking helpers
 * ================================================================ */

static bool types_equal(type_t *a, type_t *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    if (a->kind == TYPE_ARRAY_U8 || a->kind == TYPE_ARRAY_U16 || a->kind == TYPE_BCD)
        return a->array_size == b->array_size;
    if (a->kind == TYPE_STRUCT)
        return a->struct_name && b->struct_name &&
               strcmp(a->struct_name, b->struct_name) == 0;
    return true;
}

static bool type_is_scalar(type_t *t) {
    if (!t) return false;
    return t->kind == TYPE_U8 || t->kind == TYPE_U16 ||
           t->kind == TYPE_U32 || t->kind == TYPE_SEG;
}

static bool type_is_aggregate(type_t *t) {
    if (!t) return false;
    return t->kind == TYPE_ARRAY_U8 || t->kind == TYPE_ARRAY_U16 ||
           t->kind == TYPE_BCD || t->kind == TYPE_STRUCT ||
           t->kind == TYPE_FAR;
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
    if (t->kind == TYPE_ARRAY_U8) return mk_type(TYPE_U8);
    if (t->kind == TYPE_ARRAY_U16) return mk_type(TYPE_U16);
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

/* Emit a vreg reference: %N or pinned register name */
static void emit_vreg(int vreg) {
    fprintf(C.nir, "%%%d", vreg);
}

static int alloc_vreg(void) {
    return C.next_vreg++;
}

/* Convenience wrapper that discards the type */
static int emit_expr(expr_t *e) {
    return emit_expr_typed(e).vreg;
}

static typed_vreg_t TV(int vreg, type_t *type) {
    typed_vreg_t tv = { vreg, type };
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
        return TV(r, mk_type_array(TYPE_ARRAY_U8, slen));
    }
    case EXPR_IDENT: {
        symbol_t *sym = sym_lookup(e->u.ident);
        if (!sym) {
            cerr(e->line, "undefined variable '%s'", e->u.ident);
            return TV(alloc_vreg(), mk_type(TYPE_U16));
        }
        return TV(sym->vreg, sym->type);
    }
    case EXPR_REG: {
        const char *name = reg_name_str(e->u.reg.id, e->u.reg.rclass);
        symbol_t *sym = sym_lookup(name);
        if (sym) return TV(sym->vreg, sym->type);
        cerr(e->line, "undeclared register variable '%s'", name);
        int r = alloc_vreg();
        type_t *t = (e->u.reg.rclass == REGCLASS_BYTE) ?
                    mk_type(TYPE_U8) : mk_type(TYPE_U16);
        return TV(r, t);
    }
    case EXPR_SREG: {
        const char *name = sreg_name(e->u.reg.id);
        symbol_t *sym = sym_lookup(name);
        if (sym) return TV(sym->vreg, sym->type);
        cerr(e->line, "undeclared segment register '%s'", name);
        return TV(alloc_vreg(), mk_type(TYPE_SEG));
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
                /* Check if port argument was a literal */
                expr_t *port_expr = e->u.call.args;
                if (port_expr && port_expr->kind == EXPR_LIT_INT)
                    fprintf(C.nir, "    in %%%d, %d\n", dst, port_expr->u.lit_int);
                else
                    fprintf(C.nir, "    in %%%d, %%%d\n", dst, arg_vregs[0]);
            }
            return TV(dst, mk_type(TYPE_U8));
        }
        if (strcmp(fn_name, "port_out") == 0) {
            if (argc >= 2) {
                expr_t *port_expr = e->u.call.args;
                if (port_expr && port_expr->kind == EXPR_LIT_INT)
                    fprintf(C.nir, "    out %d, %%%d\n", port_expr->u.lit_int, arg_vregs[1]);
                else
                    fprintf(C.nir, "    out %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "memcopy") == 0) {
            if (argc >= 2) {
                fprintf(C.nir, "    ; memcopy %%%d <- %%%d\n", arg_vregs[0], arg_vregs[1]);
                fprintf(C.nir, "    rep movsb\n");
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "memset") == 0) {
            if (argc >= 2) {
                fprintf(C.nir, "    ; memset %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
                fprintf(C.nir, "    rep stosb\n");
            }
            return TV(dst, mk_type(TYPE_VOID));
        }
        if (strcmp(fn_name, "memcmp") == 0) {
            if (argc >= 2) {
                fprintf(C.nir, "    ; memcmp %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
                fprintf(C.nir, "    repe cmpsb\n");
            }
            return TV(dst, mk_type(TYPE_BOOL));
        }
        if (strcmp(fn_name, "memscan") == 0) {
            if (argc >= 2) {
                fprintf(C.nir, "    ; memscan %%%d, %%%d\n", arg_vregs[0], arg_vregs[1]);
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

        /* Not a builtin — regular function call */
        if (e->u.call.func->kind == EXPR_IDENT) {
            int fi = find_function(fn_name);
            if (fi >= 0) {
                if (C.functions[fi].nparams != argc)
                    cerr(e->line, "'%s' expects %d arguments, got %d",
                         fn_name, C.functions[fi].nparams, argc);
                ret_type = C.functions[fi].return_type;
            }
        }
        fprintf(C.nir, "    call %%%d, %s", dst, fn_name);
        for (int i = 0; i < argc && i < 16; i++)
            fprintf(C.nir, ", %%%d", arg_vregs[i]);
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
        fprintf(C.nir, "    load %%%d, %%%d[%%%d]\n", dst, arr.vreg, idx.vreg);
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
        fprintf(C.nir, "    bound %%%d, %%%d\n", idx.vreg, arr.vreg);
        fprintf(C.nir, "    load %%%d, %%%d[%%%d]\n", dst, arr.vreg, idx.vreg);
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
static const char *flag_id_to_name(reg_id_t id) {
    static const char *names[] = {"CF","PF","AF","ZF","SF","TF","DF","OF","IF"};
    return (id >= 0 && id < 9) ? names[id] : "??";
}

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
        if (sym->is_pinned) {
            fprintf(C.nir, "    ; pin %%%d -> %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
            fprintf(C.nir, ".prefer %%%d, %s\n", sym->vreg,
                    reg_name_str(sym->pinned_reg, sym->pin_class));
        }
        if (s->u.vardecl.init) {
            typed_vreg_t val = emit_expr_typed(s->u.vardecl.init);
            /* Type check initializer against declaration type.
             * NULL val.type means literal — always compatible. */
            if (val.type && s->u.vardecl.type) {
                bool ok = types_equal(s->u.vardecl.type, val.type);
                if (!ok && type_is_integer(s->u.vardecl.type) &&
                    type_is_integer(val.type) &&
                    type_size(val.type) <= type_size(s->u.vardecl.type))
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
        typed_vreg_t val = emit_expr_typed(s->u.assign.value);
        /* Target could be a variable, register, memory, or field */
        expr_t *t = s->u.assign.target;
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
                fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val.vreg);
            }
        } else if (t->kind == EXPR_REG || t->kind == EXPR_SREG) {
            const char *name = reg_name_str(t->u.reg.id, t->u.reg.rclass);
            symbol_t *sym = sym_lookup(name);
            if (sym) {
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
            } else {
                cerr(s->line, "undeclared register variable '%s'", name);
            }
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
            int arr = emit_expr(t->u.index.array);
            int idx = emit_expr(t->u.index.index);
            fprintf(C.nir, "    store %%%d[%%%d], %%%d\n", arr, idx, val.vreg);
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
        int start = emit_expr(s->u.for_stmt.start);
        int lbl_top = C.next_label++;
        int lbl_end = C.next_label++;
        int save_break = C.loop_break_label;
        int save_continue = C.loop_continue_label;
        C.loop_break_label = lbl_end;
        C.loop_continue_label = lbl_top;
        int cx_vreg = alloc_vreg();
        fprintf(C.nir, ".prefer %%%d, CX\n", cx_vreg);
        fprintf(C.nir, "    mov %%%d, %%%d\n", cx_vreg, start);
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

static void compile_fn(decl_t *d) {
    C.next_vreg = 0;
    /* Don't reset next_label — keep it global so labels are unique across functions */
    C.loop_depth = 0;
    C.cur_fn_name = d->u.fn.name;
    C.cur_fn_params = d->u.fn.params;
    C.cur_fn_ret = d->u.fn.return_type;
    C.cur_fn_mods = d->u.fn.mods;

    /* Count params */
    int nparams = 0;
    for (param_t *p = d->u.fn.params; p; p = p->next) nparams++;

    /* Register this function */
    register_function(d->u.fn.name, nparams, d->u.fn.return_type);

    /* Emit .nir function header */
    fprintf(C.nir, "\n.fn %s", d->u.fn.name);
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

    if (d->u.fn.mods.has_preserves) {
        fprintf(C.nir, ".preserves ");
        for (reg_list_t *r = d->u.fn.mods.preserves; r; r = r->next) {
            if (r->is_flags_all)
                fprintf(C.nir, "FLAGS");
            else
                fprintf(C.nir, "%s", reg_name_str(r->id, r->rclass));
            if (r->next) fprintf(C.nir, ", ");
        }
        fprintf(C.nir, "\n");
    }

    /* Emit .nif function header */
    fprintf(C.nif, ".fn %s", d->u.fn.name);
    if (d->u.fn.mods.is_far) fprintf(C.nif, ", far");
    if (d->u.fn.mods.is_interrupt)
        fprintf(C.nif, ", interrupt(0x%02X)", d->u.fn.mods.interrupt_vector);
    if (d->u.fn.mods.is_reentrant) fprintf(C.nif, ", reentrant");
    fprintf(C.nif, "\n");

    if (d->u.fn.mods.has_preserves) {
        fprintf(C.nif, ".preserves ");
        for (reg_list_t *r = d->u.fn.mods.preserves; r; r = r->next) {
            if (r->is_flags_all)
                fprintf(C.nif, "FLAGS");
            else
                fprintf(C.nif, "%s", reg_name_str(r->id, r->rclass));
            if (r->next) fprintf(C.nif, ", ");
        }
        fprintf(C.nif, "\n");
    }

    /* Create function scope and add parameters */
    push_scope();

    for (param_t *p = d->u.fn.params; p; p = p->next) {
        symbol_t *sym = sym_add(p->name, p->type, false);
        /* In the IR, aggregate params are references — the vreg holds
         * a u16 address, not the aggregate itself. The full type goes
         * in the .nif for type checking, but the .nir type reflects
         * what the register actually contains. */
        bool is_ref = (p->type && (p->type->kind == TYPE_ARRAY_U8 ||
                                    p->type->kind == TYPE_ARRAY_U16 ||
                                    p->type->kind == TYPE_BCD ||
                                    p->type->kind == TYPE_STRUCT ||
                                    p->type->kind == TYPE_FAR));
        const char *ir_type = (is_ref && !p->is_value) ? "u16" : type_str(p->type);
        fprintf(C.nir, ".param %%%d, %s, \"%s\"", sym->vreg, ir_type, p->name);
        if (p->is_value) fprintf(C.nir, ", value");
        if (is_ref && !p->is_value)
            fprintf(C.nir, " ; ref %s", type_str(p->type));
        fprintf(C.nir, "\n");

        /* .nif keeps the full type for cross-module type checking */
        fprintf(C.nif, ".param %%%d, %s, \"%s\"", sym->vreg, type_str(p->type), p->name);
        if (p->is_value) fprintf(C.nif, ", value");
        fprintf(C.nif, "\n");
    }

    if (d->u.fn.return_type) {
        fprintf(C.nir, ".returns %s\n", type_str(d->u.fn.return_type));
        fprintf(C.nif, ".returns %s\n", type_str(d->u.fn.return_type));
    }

    /* Add chain variable if present */
    if (d->u.fn.mods.has_chain) {
        sym_add(d->u.fn.mods.chain_name, mk_type(TYPE_VOID), false);
    }

    /* Emit body */
    emit_stmts(d->u.fn.body);

    pop_scope();

    fprintf(C.nir, ".endfn\n");
    fprintf(C.nif, ".endfn\n\n");
}

static void compile_struct(decl_t *d) {
    if (C.nstructs >= 128) return;
    strncpy(C.structs[C.nstructs].name, d->u.struc.name, 63);
    C.structs[C.nstructs].fields = d->u.struc.fields;
    C.structs[C.nstructs].aligned = d->u.struc.aligned;
    C.nstructs++;

    /* Emit to .nif so other modules know the struct layout */
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

static void compile_global(decl_t *d) {
    const char *name = d->u.global.name;
    if (!name) name = "?";

    /* Add to the current (global) scope */
    sym_add(name, d->u.global.type, true);

    fprintf(C.nir, ".global %s, %s", name,
            type_str(d->u.global.type));
    if (d->u.global.init) {
        fprintf(C.nir, " ; has initializer");
    }
    fprintf(C.nir, "\n");

    fprintf(C.nif, ".global %s, %s\n", name,
            type_str(d->u.global.type));
}

static void compile_extern_fn(decl_t *d) {
    int nparams = 0;
    for (param_t *p = d->u.extern_fn.params; p; p = p->next) nparams++;
    register_function(d->u.extern_fn.name, nparams, d->u.extern_fn.return_type);

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
        fprintf(C.nir, ".eparam %s, \"%s\"", type_str(p->type), p->name);
        if (p->has_pin)
            fprintf(C.nir, ", in %s", reg_name_str(p->pinned_reg, p->pin_class));
        fprintf(C.nir, "\n");
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
        return mk_type_array(TYPE_ARRAY_U8, atoi(s + 3));
    if (strncmp(s, "u16[", 4) == 0)
        return mk_type_array(TYPE_ARRAY_U16, atoi(s + 4));
    if (strncmp(s, "bcd[", 4) == 0)
        return mk_type_array(TYPE_BCD, atoi(s + 4));
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
    bool in_fn = false;
    bool in_extern = false;
    bool in_struct = false;

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
            in_fn = true;
            in_extern = false;
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
            in_extern = true;
            in_fn = false;
            continue;
        }

        /* .param */
        if (strncmp(p, ".param", 6) == 0) {
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
                register_function(cur_fn, cur_nparams, cur_ret);
            }
            cur_fn[0] = '\0';
            in_fn = false;
            in_extern = false;
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
            in_struct = true;
            continue;
        }
        if (strncmp(p, ".endstruct", 10) == 0) {
            in_struct = false;
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

        /* .preserves — skip (used by binder, not compiler) */
        /* field declarations inside struct — skip for now */
    }

    fclose(fp);
}

/* ================================================================
 * Main compile entry point
 * ================================================================ */

int compile(program_t *prog, const char *nir_path, const char *nif_path,
            const char *src_dir) {
    memset(&C, 0, sizeof(C));
    if (src_dir)
        strncpy(C.src_dir, src_dir, sizeof(C.src_dir) - 1);

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
            fprintf(C.nir, "; use \"%s\"\n", d->u.use_path);
            import_nif(d->u.use_path, d->line);
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
