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

type_t *mk_type_array(type_t *elem, int size) {
    type_t *t = mk_type(TYPE_ARRAY);
    t->element_type = elem;
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

expr_t *mk_expr_raw_field(expr_t *obj, const char *field, int line) {
    expr_t *e = mk_expr(EXPR_RAW_FIELD, line);
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

expr_t *mk_expr_far_lit(int seg, int off, int line) {
    expr_t *e = mk_expr(EXPR_FAR_LIT, line);
    e->u.far_lit.seg = seg;
    e->u.far_lit.off = off;
    return e;
}

expr_t *mk_expr_cast(expr_t *operand, type_t *target, int line) {
    expr_t *e = mk_expr(EXPR_CAST, line);
    e->u.cast.operand = operand;
    e->u.cast.target_type = target;
    return e;
}

expr_t *mk_expr_array_init(expr_t *elements, int line) {
    expr_t *e = mk_expr(EXPR_ARRAY_INIT, line);
    e->u.array_init.elements = elements;
    return e;
}

expr_t *mk_expr_indirect_call(expr_t *addr, const char *extern_name,
                               const char *module_name, expr_t *args, int line) {
    expr_t *e = mk_expr(EXPR_INDIRECT_CALL, line);
    e->u.indirect_call.addr = addr;
    e->u.indirect_call.extern_name = xstrdup(extern_name);
    e->u.indirect_call.module_name = xstrdup(module_name);
    e->u.indirect_call.args = args;
    return e;
}

expr_t *mk_expr_deref(const char *name, int line) {
    expr_t *e = mk_expr(EXPR_DEREF, line);
    e->u.deref.name = xstrdup(name);
    e->u.deref.seg = REG_NONE;
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
    s->u.vardecl.is_const = false;
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

/* ---- Flag expressions ---- */

flag_expr_t *mk_fexpr_flag(reg_id_t id) {
    flag_expr_t *f = xalloc(sizeof(flag_expr_t));
    f->kind = FEXPR_FLAG;
    f->flag_id = id;
    return f;
}

flag_expr_t *mk_fexpr_not(flag_expr_t *e) {
    flag_expr_t *f = xalloc(sizeof(flag_expr_t));
    f->kind = FEXPR_NOT;
    f->left = e;
    return f;
}

flag_expr_t *mk_fexpr_binop(flag_expr_kind_t kind, flag_expr_t *l, flag_expr_t *r) {
    flag_expr_t *f = xalloc(sizeof(flag_expr_t));
    f->kind = kind;
    f->left = l;
    f->right = r;
    return f;
}

flag_case_t *mk_flag_case(flag_expr_t *cond, stmt_t *body) {
    flag_case_t *c = xalloc(sizeof(flag_case_t));
    c->condition = cond;
    c->body = body;
    return c;
}

flag_case_t *mk_flag_case_trap(flag_expr_t *cond) {
    flag_case_t *c = xalloc(sizeof(flag_case_t));
    c->condition = cond;
    c->is_trap = true;
    return c;
}

stmt_t *mk_stmt_assign_checked(expr_t *target, expr_t *value,
                                flag_case_t *checks, int line) {
    stmt_t *s = mk_stmt(STMT_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value = value;
    s->u.assign.flag_checks = checks;
    return s;
}

stmt_t *mk_stmt_toggle_checked(expr_t *target, expr_t *value,
                                flag_case_t *checks, int line) {
    stmt_t *s = mk_stmt(STMT_TOGGLE_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value = value;
    s->u.assign.flag_checks = checks;
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

stmt_t *mk_stmt_tailcall(expr_t *call_expr, int line) {
    stmt_t *s = mk_stmt(STMT_TAILCALL, line);
    s->u.tailcall_expr = call_expr;
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

stmt_t *mk_stmt_const(const char *name, int value, int line) {
    stmt_t *s = mk_stmt(STMT_CONST, line);
    s->u.konst.name = xstrdup(name);
    s->u.konst.value = value;
    s->u.konst.init = NULL;
    return s;
}

stmt_t *mk_stmt_const_expr(const char *name, expr_t *init, int line) {
    stmt_t *s = mk_stmt(STMT_CONST, line);
    s->u.konst.name = xstrdup(name);
    s->u.konst.value = 0;
    s->u.konst.init = init;
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
    p->place = ABI_PLACE_DEFAULT;
    return p;
}

param_t *mk_param_pinned(const char *name, type_t *type,
                          int pinned_reg, reg_class_t pin_class) {
    param_t *p = mk_param(name, type, false);
    p->pinned_reg = pinned_reg;
    p->pin_class = pin_class;
    p->has_pin = true;
    p->place = ABI_PLACE_REGISTER;
    return p;
}

param_t *mk_param_far_pinned(const char *name,
                              int off_reg, int seg_reg) {
    param_t *p = mk_param(name, mk_type(TYPE_FAR), false);
    p->pinned_reg = off_reg;
    p->pin_class = REGCLASS_WORD;
    p->has_pin = true;
    p->pinned_seg = seg_reg;
    p->has_seg_pin = true;
    p->place = ABI_PLACE_REGISTER;
    return p;
}

param_t *mk_param_placed(const char *name, type_t *type,
                          bool is_value, abi_place_t place) {
    param_t *p = mk_param(name, type, is_value);
    p->place = place;
    return p;
}

/* ---- Return values ---- */

return_t *mk_return(type_t *type) {
    return_t *r = xalloc(sizeof(return_t));
    r->type = type;
    r->pinned_reg = REG_NONE;
    r->pin_class = REGCLASS_WORD;
    r->place = ABI_PLACE_DEFAULT;
    return r;
}

return_t *mk_return_pinned(type_t *type, int pinned_reg,
                            reg_class_t pin_class) {
    return_t *r = mk_return(type);
    r->pinned_reg = pinned_reg;
    r->pin_class = pin_class;
    r->has_pin = true;
    r->place = ABI_PLACE_REGISTER;
    return r;
}

return_t *mk_return_placed(type_t *type, abi_place_t place) {
    return_t *r = mk_return(type);
    r->place = place;
    return r;
}

return_t *return_list_append(return_t *list, return_t *item) {
    if (!list) return item;
    return_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

int return_list_count(return_t *list) {
    int n = 0;
    for (return_t *r = list; r; r = r->next) n++;
    return n;
}

/* ---- Fields ---- */

field_t *mk_field(const char *name, type_t *type) {
    field_t *f = xalloc(sizeof(field_t));
    f->name = xstrdup(name);
    f->type = type;
    f->is_bits = false;
    return f;
}

field_t *mk_field_typed_ptr(const char *name, type_t *storage, type_t *as_type) {
    field_t *f = mk_field(name, storage);
    f->as_type = as_type;
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
                    param_t *params, return_t *rets, stmt_t *body, int line) {
    decl_t *d = mk_decl(DECL_FN, line);
    d->u.fn.name = xstrdup(name);
    d->u.fn.mods = mods;
    d->u.fn.params = params;
    d->u.fn.return_type = rets ? rets->type : NULL;
    d->u.fn.returns = rets;
    d->u.fn.nreturns = return_list_count(rets);
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
                        expr_t *init,
                        bool has_at, int at_seg, int at_off,
                        int line) {
    decl_t *d = mk_decl(DECL_GLOBAL, line);
    d->u.global.type = type;
    d->u.global.name = xstrdup(name);
    d->u.global.pinned_reg = pinned_reg;
    d->u.global.pin_class = pin_class;
    d->u.global.init = init;
    d->u.global.has_at = has_at;
    d->u.global.at_seg = at_seg;
    d->u.global.at_off = at_off;
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
                           param_t *params, return_t *rets,
                           reg_list_t *preserves,
                           bool has_addr, int addr_seg, int addr_off,
                           int line) {
    decl_t *d = mk_decl(DECL_EXTERN_FN, line);
    d->u.extern_fn.name = xstrdup(name);
    d->u.extern_fn.mods = mods;
    d->u.extern_fn.params = params;
    d->u.extern_fn.return_type = rets ? rets->type : NULL;
    d->u.extern_fn.returns = rets;
    d->u.extern_fn.nreturns = return_list_count(rets);
    d->u.extern_fn.ret_pinned_reg = rets && rets->has_pin ? rets->pinned_reg : REG_NONE;
    d->u.extern_fn.ret_pin_class = rets ? rets->pin_class : REGCLASS_WORD;
    d->u.extern_fn.has_ret_pin = rets && rets->has_pin;
    d->u.extern_fn.preserves = preserves;
    d->u.extern_fn.has_address = has_addr;
    d->u.extern_fn.addr_seg = addr_seg;
    d->u.extern_fn.addr_off = addr_off;
    d->u.extern_fn.body = NULL;
    return d;
}

decl_t *mk_decl_use(const char *path, int line) {
    decl_t *d = mk_decl(DECL_USE, line);
    d->u.use_path = xstrdup(path);
    return d;
}

decl_t *mk_decl_const(const char *name, int value, int line) {
    decl_t *d = mk_decl(DECL_CONST, line);
    d->u.konst.name = xstrdup(name);
    d->u.konst.value = value;
    return d;
}

/* ---- Program ---- */

decl_t *mk_decl_at(int seg, int off, int line) {
    decl_t *d = mk_decl(DECL_AT, line);
    d->u.at.seg = seg;
    d->u.at.off = off;
    return d;
}

decl_t *mk_decl_endat(int line) {
    return mk_decl(DECL_ENDAT, line);
}

program_t *mk_program(void) {
    return xalloc(sizeof(program_t));
}

void program_add(program_t *p, decl_t *d) {
    if (!d) return;
    if (!p->decls) {
        p->decls = d;
        p->decls_tail = d;
    } else {
        p->decls_tail->next = d;
        p->decls_tail = d;
    }
}
