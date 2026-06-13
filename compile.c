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
 * NIR emission — expression compilation
 * ================================================================ */

/* Emit an expression, return the vreg holding the result */
static int emit_expr(expr_t *e);

/* Emit a vreg reference: %N or pinned register name */
static void emit_vreg(int vreg) {
    fprintf(C.nir, "%%%d", vreg);
}

static int alloc_vreg(void) {
    return C.next_vreg++;
}

static int emit_expr(expr_t *e) {
    if (!e) return -1;

    switch (e->kind) {
    case EXPR_LIT_INT: {
        int r = alloc_vreg();
        fprintf(C.nir, "    mov %%%d, %d\n", r, e->u.lit_int);
        return r;
    }
    case EXPR_LIT_STR: {
        int r = alloc_vreg();
        fprintf(C.nir, "    mov %%%d, \"%s\"\n", r, e->u.lit_str);
        return r;
    }
    case EXPR_IDENT: {
        symbol_t *sym = sym_lookup(e->u.ident);
        if (!sym) {
            /* Could be a function name for a call */
            cerr(e->line, "undefined variable '%s'", e->u.ident);
            return alloc_vreg();
        }
        return sym->vreg;
    }
    case EXPR_REG: {
        /* Look up the pinned register variable */
        const char *name = reg_name_str(e->u.reg.id, e->u.reg.rclass);
        symbol_t *sym = sym_lookup(name);
        if (sym) return sym->vreg;
        /* Not declared yet — treat as a reference to the raw register.
           The binder will handle this. */
        int r = alloc_vreg();
        fprintf(C.nir, "    ; ref %s -> %%%d\n", name, r);
        return r;
    }
    case EXPR_SREG: {
        const char *name = sreg_name(e->u.reg.id);
        symbol_t *sym = sym_lookup(name);
        if (sym) return sym->vreg;
        int r = alloc_vreg();
        fprintf(C.nir, "    ; ref %s -> %%%d\n", name, r);
        return r;
    }
    case EXPR_FLAG: {
        int r = alloc_vreg();
        fprintf(C.nir, "    getflag %%%d, %s\n", r, flag_name(e->u.flag_id));
        return r;
    }
    case EXPR_BINOP: {
        int l = emit_expr(e->u.binop.left);
        int r = emit_expr(e->u.binop.right);
        int dst = alloc_vreg();
        fprintf(C.nir, "    %s %%%d, %%%d, %%%d\n", op_str(e->u.binop.op), dst, l, r);
        return dst;
    }
    case EXPR_UNOP: {
        int operand = emit_expr(e->u.unop.operand);
        int dst = alloc_vreg();
        fprintf(C.nir, "    %s %%%d, %%%d\n", op_str(e->u.unop.op), dst, operand);
        return dst;
    }
    case EXPR_CALL: {
        /* Emit arguments */
        int argc = 0;
        int arg_vregs[16];
        for (expr_t *a = e->u.call.args; a; a = a->next) {
            if (argc < 16)
                arg_vregs[argc] = emit_expr(a);
            argc++;
        }
        /* Get function name */
        const char *fn_name = "?";
        if (e->u.call.func->kind == EXPR_IDENT)
            fn_name = e->u.call.func->u.ident;
        int dst = alloc_vreg();
        fprintf(C.nir, "    call %%%d, %s", dst, fn_name);
        for (int i = 0; i < argc && i < 16; i++)
            fprintf(C.nir, ", %%%d", arg_vregs[i]);
        fprintf(C.nir, "\n");
        return dst;
    }
    case EXPR_INDEX: {
        int arr = emit_expr(e->u.index.array);
        int idx = emit_expr(e->u.index.index);
        int dst = alloc_vreg();
        fprintf(C.nir, "    load %%%d, %%%d[%%%d]\n", dst, arr, idx);
        return dst;
    }
    case EXPR_CHECKED_INDEX: {
        int arr = emit_expr(e->u.index.array);
        int idx = emit_expr(e->u.index.index);
        int dst = alloc_vreg();
        fprintf(C.nir, "    bound %%%d, %%%d\n", idx, arr);
        fprintf(C.nir, "    load %%%d, %%%d[%%%d]\n", dst, arr, idx);
        return dst;
    }
    case EXPR_FIELD: {
        int obj = emit_expr(e->u.field.object);
        int dst = alloc_vreg();
        fprintf(C.nir, "    field %%%d, %%%d, %s\n", dst, obj, e->u.field.field_name);
        return dst;
    }
    case EXPR_MEM: {
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
        return dst;
    }
    case EXPR_PAREN:
        return -1; /* shouldn't reach here — parens are unwrapped */
    }
    return -1;
}

/* ================================================================
 * NIR emission — statement compilation
 * ================================================================ */

static void emit_stmt(stmt_t *s);

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
            int val = emit_expr(s->u.vardecl.init);
            fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val);
        }
        break;
    }
    case STMT_ASSIGN: {
        int val = emit_expr(s->u.assign.value);
        /* Target could be a variable, register, memory, or field */
        expr_t *t = s->u.assign.target;
        if (t->kind == EXPR_IDENT) {
            symbol_t *sym = sym_lookup(t->u.ident);
            if (!sym) {
                cerr(s->line, "undefined variable '%s'", t->u.ident);
            } else {
                fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val);
            }
        } else if (t->kind == EXPR_REG || t->kind == EXPR_SREG) {
            const char *name = reg_name_str(t->u.reg.id, t->u.reg.rclass);
            symbol_t *sym = sym_lookup(name);
            if (sym) {
                fprintf(C.nir, "    mov %%%d, %%%d\n", sym->vreg, val);
            } else {
                cerr(s->line, "undeclared register variable '%s'", name);
            }
        } else if (t->kind == EXPR_FLAG) {
            fprintf(C.nir, "    setflag %s, %%%d\n",
                    flag_name(t->u.flag_id), val);
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
            fprintf(C.nir, "], %%%d\n", val);
        } else if (t->kind == EXPR_INDEX) {
            int arr = emit_expr(t->u.index.array);
            int idx = emit_expr(t->u.index.index);
            fprintf(C.nir, "    store %%%d[%%%d], %%%d\n", arr, idx, val);
        } else if (t->kind == EXPR_FIELD) {
            int obj = emit_expr(t->u.field.object);
            fprintf(C.nir, "    storefield %%%d, %s, %%%d\n",
                    obj, t->u.field.field_name, val);
        } else {
            cerr(s->line, "invalid assignment target");
        }
        break;
    }
    case STMT_TOGGLE_ASSIGN: {
        int val = emit_expr(s->u.assign.value);
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
        break;
    }
    case STMT_EXPR: {
        emit_expr(s->u.expr);
        break;
    }
    case STMT_IF: {
        int cond = emit_expr(s->u.if_stmt.cond);
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
        fprintf(C.nir, ".L%d:\n", lbl_top);
        int cond = emit_expr(s->u.while_stmt.cond);
        fprintf(C.nir, "    jz %%%d, .L%d\n", cond, lbl_end);
        C.loop_depth++;
        push_scope();
        emit_stmts(s->u.while_stmt.body);
        pop_scope();
        C.loop_depth--;
        fprintf(C.nir, "    jmp .L%d\n", lbl_top);
        fprintf(C.nir, ".L%d:\n", lbl_end);
        break;
    }
    case STMT_FOR: {
        /* for (CX in start..0) — LOOP instruction */
        int start = emit_expr(s->u.for_stmt.start);
        int lbl_top = C.next_label++;
        int lbl_end = C.next_label++;
        fprintf(C.nir, "    mov %%cx, %%%d\n", start);
        fprintf(C.nir, ".L%d:\n", lbl_top);
        C.loop_depth++;
        push_scope();
        emit_stmts(s->u.for_stmt.body);
        pop_scope();
        C.loop_depth--;
        fprintf(C.nir, "    loop .L%d\n", lbl_top);
        fprintf(C.nir, ".L%d:\n", lbl_end);
        break;
    }
    case STMT_RETURN: {
        if (s->u.ret_expr) {
            int val = emit_expr(s->u.ret_expr);
            fprintf(C.nir, "    retval %%%d\n", val);
        }
        fprintf(C.nir, "    ret\n");
        break;
    }
    case STMT_BREAK: {
        if (C.loop_depth == 0)
            cerr(s->line, "break outside loop");
        fprintf(C.nir, "    break\n");
        break;
    }
    case STMT_CONTINUE: {
        if (C.loop_depth == 0)
            cerr(s->line, "continue outside loop");
        fprintf(C.nir, "    continue\n");
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
    C.next_label = 0;
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
    fprintf(C.nir, "\n");

    /* Emit .nif function header */
    fprintf(C.nif, ".fn %s", d->u.fn.name);
    if (d->u.fn.mods.is_far) fprintf(C.nif, ", far");
    if (d->u.fn.mods.is_interrupt)
        fprintf(C.nif, ", interrupt(0x%02X)", d->u.fn.mods.interrupt_vector);
    if (d->u.fn.mods.is_reentrant) fprintf(C.nif, ", reentrant");
    fprintf(C.nif, "\n");

    /* Create function scope and add parameters */
    push_scope();

    for (param_t *p = d->u.fn.params; p; p = p->next) {
        symbol_t *sym = sym_add(p->name, p->type, false);
        fprintf(C.nir, ".param %%%d, %s, \"%s\"", sym->vreg, type_str(p->type), p->name);
        if (p->is_value) fprintf(C.nir, ", value");
        fprintf(C.nir, "\n");

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

    /* Extern functions go directly to .nif — no .nir body */
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
 * Main compile entry point
 * ================================================================ */

int compile(program_t *prog, const char *nir_path, const char *nif_path) {
    memset(&C, 0, sizeof(C));

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
            /* TODO: read .nif and import symbols */
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
