/*
 * ast.c — AST node constructors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

static void *xalloc(size_t sz) {
    void *p = calloc(1, sz);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

/* ---- Types ---- */

type_t *mk_type(type_kind_t kind) {
    type_t *t = xalloc(sizeof(type_t));
    t->kind = kind;
    return t;
}

type_t *mk_type_array(type_kind_t kind, int size) {
    type_t *t = mk_type(kind);
    t->array_size = size;
    return t;
}

type_t *mk_type_struct(const char *name) {
    type_t *t = mk_type(TYPE_STRUCT);
    t->struct_name = xstrdup(name);
    return t;
}

/* ---- Expressions ---- */

static expr_t *mk_expr(expr_kind_t kind, int line) {
    expr_t *e = xalloc(sizeof(expr_t));
    e->kind = kind;
    e->line = line;
    return e;
}

expr_t *mk_expr_int(int val, int line) {
    expr_t *e = mk_expr(EXPR_LIT_INT, line);
    e->u.lit_int = val;
    return e;
}

expr_t *mk_expr_str(const char *s, int line) {
    expr_t *e = mk_expr(EXPR_LIT_STR, line);
    e->u.lit_str = xstrdup(s);
    return e;
}

expr_t *mk_expr_ident(const char *name, int line) {
    expr_t *e = mk_expr(EXPR_IDENT, line);
    e->u.ident = xstrdup(name);
    return e;
}

expr_t *mk_expr_reg(reg_id_t id, reg_class_t rc, int line) {
    expr_t *e = mk_expr(rc == REGCLASS_SEG ? EXPR_SREG : EXPR_REG, line);
    e->u.reg.id = id;
    e->u.reg.rclass = rc;
    return e;
}

expr_t *mk_expr_flag(reg_id_t id, int line) {
    expr_t *e = mk_expr(EXPR_FLAG, line);
    e->u.flag_id = id;
    return e;
}

expr_t *mk_expr_binop(op_kind_t op, expr_t *l, expr_t *r, int line) {
    expr_t *e = mk_expr(EXPR_BINOP, line);
    e->u.binop.op = op;
    e->u.binop.left = l;
    e->u.binop.right = r;
    return e;
}

expr_t *mk_expr_unop(op_kind_t op, expr_t *operand, int line) {
    expr_t *e = mk_expr(EXPR_UNOP, line);
    e->u.unop.op = op;
    e->u.unop.operand = operand;
    return e;
}

expr_t *mk_expr_call(expr_t *func, expr_t *args, int line) {
    expr_t *e = mk_expr(EXPR_CALL, line);
    e->u.call.func = func;
    e->u.call.args = args;
    return e;
}

expr_t *mk_expr_index(expr_t *arr, expr_t *idx, bool checked, int line) {
    expr_t *e = mk_expr(checked ? EXPR_CHECKED_INDEX : EXPR_INDEX, line);
    e->u.index.array = arr;
    e->u.index.index = idx;
    return e;
}

expr_t *mk_expr_field(expr_t *obj, const char *field, int line) {
    expr_t *e = mk_expr(EXPR_FIELD, line);
    e->u.field.object = obj;
    e->u.field.field_name = xstrdup(field);
    return e;
}

expr_t *mk_expr_mem(reg_id_t seg, reg_id_t base, reg_id_t idx,
                     int disp, bool has_disp, int line) {
    expr_t *e = mk_expr(EXPR_MEM, line);
    e->u.mem.seg = seg;
    e->u.mem.base = base;
    e->u.mem.index = idx;
    e->u.mem.disp = disp;
    e->u.mem.has_disp = has_disp;
    e->u.mem.abs_seg = false;
    return e;
}

expr_t *mk_expr_mem_abs(int seg, int off, int line) {
    expr_t *e = mk_expr(EXPR_MEM, line);
    e->u.mem.seg = REG_NONE;
    e->u.mem.base = REG_NONE;
    e->u.mem.index = REG_NONE;
    e->u.mem.disp = off;
    e->u.mem.has_disp = true;
    e->u.mem.abs_seg = true;
    e->u.mem.abs_seg_val = seg;
    return e;
}

/* ---- Statements ---- */

static stmt_t *mk_stmt(stmt_kind_t kind, int line) {
    stmt_t *s = xalloc(sizeof(stmt_t));
    s->kind = kind;
    s->line = line;
    return s;
}

stmt_t *mk_stmt_vardecl(type_t *type, const char *name,
                         int pinned_reg, reg_class_t pin_class,
                         expr_t *init, int line) {
    stmt_t *s = mk_stmt(STMT_VARDECL, line);
    s->u.vardecl.type = type;
    s->u.vardecl.name = xstrdup(name);
    s->u.vardecl.pinned_reg = pinned_reg;
    s->u.vardecl.pin_class = pin_class;
    s->u.vardecl.init = init;
    return s;
}

stmt_t *mk_stmt_assign(expr_t *target, expr_t *value, int line) {
    stmt_t *s = mk_stmt(STMT_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value = value;
    return s;
}

stmt_t *mk_stmt_toggle(expr_t *target, expr_t *value, int line) {
    stmt_t *s = mk_stmt(STMT_TOGGLE_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value = value;
    return s;
}

stmt_t *mk_stmt_expr(expr_t *e, int line) {
    stmt_t *s = mk_stmt(STMT_EXPR, line);
    s->u.expr = e;
    return s;
}

stmt_t *mk_stmt_if(expr_t *cond, stmt_t *then_b, stmt_t *else_b, int line) {
    stmt_t *s = mk_stmt(STMT_IF, line);
    s->u.if_stmt.cond = cond;
    s->u.if_stmt.then_body = then_b;
    s->u.if_stmt.else_body = else_b;
    return s;
}

stmt_t *mk_stmt_while(expr_t *cond, stmt_t *body, int line) {
    stmt_t *s = mk_stmt(STMT_WHILE, line);
    s->u.while_stmt.cond = cond;
    s->u.while_stmt.body = body;
    return s;
}

stmt_t *mk_stmt_for(expr_t *start, int end_val, stmt_t *body, int line) {
    stmt_t *s = mk_stmt(STMT_FOR, line);
    s->u.for_stmt.start = start;
    s->u.for_stmt.end_val = end_val;
    s->u.for_stmt.body = body;
    return s;
}

stmt_t *mk_stmt_return(expr_t *e, int line) {
    stmt_t *s = mk_stmt(STMT_RETURN, line);
    s->u.ret_expr = e;
    return s;
}

stmt_t *mk_stmt_break(int line) {
    return mk_stmt(STMT_BREAK, line);
}

stmt_t *mk_stmt_continue(int line) {
    return mk_stmt(STMT_CONTINUE, line);
}

stmt_t *mk_stmt_goto(const char *label, int line) {
    stmt_t *s = mk_stmt(STMT_GOTO, line);
    s->u.goto_label = xstrdup(label);
    return s;
}

stmt_t *mk_stmt_label(const char *name, int line) {
    stmt_t *s = mk_stmt(STMT_LABEL, line);
    s->u.label_name = xstrdup(name);
    return s;
}

stmt_t *mk_stmt_asm(const char *body, reg_list_t *annotation,
                     bool is_clobbers, bool has_annotation, int line) {
    stmt_t *s = mk_stmt(STMT_ASM, line);
    s->u.asm_stmt.body = xstrdup(body);
    s->u.asm_stmt.has_annotation = has_annotation;
    s->u.asm_stmt.is_clobbers = is_clobbers;
    if (has_annotation) {
        if (is_clobbers)
            s->u.asm_stmt.clobbers = annotation;
        else
            s->u.asm_stmt.preserves = annotation;
    }
    return s;
}

/* ---- Parameters ---- */

param_t *mk_param(const char *name, type_t *type, bool is_value) {
    param_t *p = xalloc(sizeof(param_t));
    p->name = xstrdup(name);
    p->type = type;
    p->is_value = is_value;
    p->pinned_reg = REG_NONE;
    p->has_pin = false;
    return p;
}

param_t *mk_param_pinned(const char *name, type_t *type,
                          int pinned_reg, reg_class_t pin_class) {
    param_t *p = mk_param(name, type, false);
    p->pinned_reg = pinned_reg;
    p->pin_class = pin_class;
    p->has_pin = true;
    return p;
}

/* ---- Fields ---- */

field_t *mk_field(const char *name, type_t *type) {
    field_t *f = xalloc(sizeof(field_t));
    f->name = xstrdup(name);
    f->type = type;
    f->is_bits = false;
    return f;
}

field_t *mk_field_bits(const char *name, int nbits) {
    field_t *f = xalloc(sizeof(field_t));
    f->name = name ? xstrdup(name) : NULL;
    f->type = NULL;
    f->bits = nbits;
    f->is_bits = true;
    return f;
}

/* ---- Register lists ---- */

reg_list_t *mk_reg_list(reg_id_t id, reg_class_t rc) {
    reg_list_t *r = xalloc(sizeof(reg_list_t));
    r->id = id;
    r->rclass = rc;
    r->is_flags_all = false;
    return r;
}

reg_list_t *mk_reg_list_flags_all(void) {
    reg_list_t *r = xalloc(sizeof(reg_list_t));
    r->is_flags_all = true;
    return r;
}

/* ---- Declarations ---- */

static decl_t *mk_decl(decl_kind_t kind, int line) {
    decl_t *d = xalloc(sizeof(decl_t));
    d->kind = kind;
    d->line = line;
    return d;
}

decl_t *mk_decl_fn(const char *name, fn_modifiers_t mods,
                    param_t *params, type_t *ret, stmt_t *body, int line) {
    decl_t *d = mk_decl(DECL_FN, line);
    d->u.fn.name = xstrdup(name);
    d->u.fn.mods = mods;
    d->u.fn.params = params;
    d->u.fn.return_type = ret;
    d->u.fn.body = body;
    return d;
}

decl_t *mk_decl_struct(const char *name, bool aligned,
                        field_t *fields, int line) {
    decl_t *d = mk_decl(DECL_STRUCT, line);
    d->u.struc.name = xstrdup(name);
    d->u.struc.aligned = aligned;
    d->u.struc.fields = fields;
    return d;
}

decl_t *mk_decl_global(type_t *type, const char *name,
                        int pinned_reg, reg_class_t pin_class,
                        expr_t *init, int line) {
    decl_t *d = mk_decl(DECL_GLOBAL, line);
    d->u.global.type = type;
    d->u.global.name = xstrdup(name);
    d->u.global.pinned_reg = pinned_reg;
    d->u.global.pin_class = pin_class;
    d->u.global.init = init;
    return d;
}

decl_t *mk_decl_extern_global(type_t *type, const char *name, int line) {
    decl_t *d = mk_decl(DECL_EXTERN_GLOBAL, line);
    d->u.global.type = type;
    d->u.global.name = xstrdup(name);
    d->u.global.pinned_reg = REG_NONE;
    d->u.global.init = NULL;
    return d;
}

decl_t *mk_decl_extern_fn(const char *name, fn_modifiers_t mods,
                           param_t *params, type_t *ret,
                           int ret_pin, reg_class_t ret_pin_class,
                           bool has_ret_pin, reg_list_t *preserves,
                           bool has_addr, int addr_seg, int addr_off,
                           int line) {
    decl_t *d = mk_decl(DECL_EXTERN_FN, line);
    d->u.extern_fn.name = xstrdup(name);
    d->u.extern_fn.mods = mods;
    d->u.extern_fn.params = params;
    d->u.extern_fn.return_type = ret;
    d->u.extern_fn.ret_pinned_reg = ret_pin;
    d->u.extern_fn.ret_pin_class = ret_pin_class;
    d->u.extern_fn.has_ret_pin = has_ret_pin;
    d->u.extern_fn.preserves = preserves;
    d->u.extern_fn.has_address = has_addr;
    d->u.extern_fn.addr_seg = addr_seg;
    d->u.extern_fn.addr_off = addr_off;
    return d;
}

decl_t *mk_decl_use(const char *path, int line) {
    decl_t *d = mk_decl(DECL_USE, line);
    d->u.use_path = xstrdup(path);
    return d;
}

/* ---- Program ---- */

program_t *mk_program(void) {
    return xalloc(sizeof(program_t));
}

void program_add(program_t *p, decl_t *d) {
    if (!p->decls) {
        p->decls = d;
        p->decls_tail = d;
    } else {
        p->decls_tail->next = d;
        p->decls_tail = d;
    }
}
