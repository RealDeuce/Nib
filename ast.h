#ifndef NIB_AST_H
#define NIB_AST_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Nib AST — node types for the compiler
 *
 * All nodes are heap-allocated via mk_* constructor functions.
 * Linked lists use ->next pointers.
 * ================================================================ */

/* ---- Forward declarations ---- */
typedef struct type_node    type_t;
typedef struct expr_node    expr_t;
typedef struct stmt_node    stmt_t;
typedef struct decl_node    decl_t;
typedef struct param_node   param_t;
typedef struct field_node   field_t;
typedef struct reg_list_node reg_list_t;
typedef struct flag_expr_node flag_expr_t;
typedef struct flag_case_node flag_case_t;

/* ---- Register / flag IDs ---- */

typedef enum {
    /* Word registers (index matches ModR/M encoding) */
    WREG_AX=0, WREG_CX, WREG_DX, WREG_BX, WREG_SP, WREG_BP, WREG_SI, WREG_DI,
    /* Byte registers */
    BREG_AL=0, BREG_CL, BREG_DL, BREG_BL, BREG_AH, BREG_CH, BREG_DH, BREG_BH,
    /* Segment registers */
    SREG_ES=0, SREG_CS, SREG_SS, SREG_DS,
    /* Flags */
    FLG_CF=0, FLG_PF, FLG_AF, FLG_ZF, FLG_SF, FLG_TF, FLG_DF, FLG_OF, FLG_IF,
    /* None */
    REG_NONE = -1
} reg_id_t;

typedef enum {
    REGCLASS_WORD, REGCLASS_BYTE, REGCLASS_SEG, REGCLASS_FLAG
} reg_class_t;

/* ---- Types ---- */

typedef enum {
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_SEG, TYPE_BOOL,
    TYPE_ARRAY,     /* array: element_type[array_size] */
    TYPE_BCD,
    TYPE_STRUCT,
    TYPE_FAR,
    TYPE_VOID
} type_kind_t;

struct type_node {
    type_kind_t kind;
    int         array_size;     /* for arrays and BCD: element count */
    char       *struct_name;    /* for TYPE_STRUCT */
    type_t     *element_type;   /* for TYPE_ARRAY: element type */
};

/* ---- Expressions ---- */

typedef enum {
    /* Operators for EXPR_BINOP */
    NIB_ADD, NIB_SUB, NIB_MUL, NIB_DIV, NIB_MOD,
    NIB_SMUL, NIB_SDIV, NIB_SMOD,
    NIB_AND, NIB_OR, NIB_XOR,
    NIB_SHL, NIB_SHR, NIB_SRSHR,
    NIB_ROL, NIB_ROR, NIB_RCL, NIB_RCR,
    NIB_EQ, NIB_NEQ,
    NIB_LT, NIB_GT, NIB_LTE, NIB_GTE,
    NIB_SLT, NIB_SGT, NIB_SLTE, NIB_SGTE,
    NIB_XCHG,
    /* Operators for EXPR_UNOP */
    NIB_NEG, NIB_NOT, NIB_ADDR, NIB_FAR_ADDR, NIB_LNOT
} op_kind_t;

typedef enum {
    EXPR_LIT_INT,
    EXPR_LIT_STR,
    EXPR_IDENT,
    EXPR_REG,
    EXPR_SREG,
    EXPR_FLAG,
    EXPR_BINOP,
    EXPR_UNOP,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_CHECKED_INDEX,
    EXPR_FIELD,
    EXPR_RAW_FIELD,
    EXPR_MEM,
    EXPR_FAR_LIT,   /* far literal: seg:off */
    EXPR_CAST,
    EXPR_PAREN,
    EXPR_ARRAY_INIT, /* array initializer: [expr, expr, ...] */
    EXPR_INDIRECT_CALL, /* addr as name from module(args...) */
    EXPR_DEREF          /* [var] — pointer dereference */
} expr_kind_t;

struct expr_node {
    expr_kind_t kind;
    int         line;       /* source line for error reporting */

    union {
        /* LIT_INT */
        int lit_int;

        /* LIT_STR */
        char *lit_str;

        /* IDENT */
        char *ident;

        /* REG / SREG */
        struct { reg_id_t id; reg_class_t rclass; } reg;

        /* FLAG */
        reg_id_t flag_id;

        /* BINOP */
        struct { op_kind_t op; expr_t *left, *right; } binop;

        /* UNOP */
        struct { op_kind_t op; expr_t *operand; } unop;

        /* CALL */
        struct { expr_t *func; expr_t *args; } call;
        /* args is a linked list of expr_t via ->next */

        /* INDEX / CHECKED_INDEX */
        struct { expr_t *array; expr_t *index; } index;

        /* FIELD */
        struct { expr_t *object; char *field_name; } field;

        /* FAR_LIT — seg:off constant */
        struct { int seg; int off; } far_lit;

        /* CAST (as) — reinterpret type, no code */
        struct { expr_t *operand; type_t *target_type; } cast;

        /* MEM — [seg:base+index+disp] */
        struct {
            reg_id_t seg;       /* SREG_* or REG_NONE */
            reg_id_t base;      /* WREG_BX/BP or REG_NONE */
            reg_id_t index;     /* WREG_SI/DI or REG_NONE */
            int      disp;
            bool     has_disp;
            bool     abs_seg;   /* true for [0xB800:0x0000] form */
            int      abs_seg_val;
        } mem;

        /* ARRAY_INIT — [expr, expr, ...] */
        struct { expr_t *elements; } array_init;

        /* INDIRECT_CALL — addr as name from module(args...) */
        struct {
            expr_t *addr;           /* far pointer expression */
            char   *extern_name;    /* function name in extern namespace */
            char   *module_name;    /* module to look up extern in */
            expr_t *args;           /* argument list */
        } indirect_call;

        /* DEREF — [var] pointer dereference */
        struct {
            char    *name;          /* variable name */
            reg_id_t seg;           /* segment override or REG_NONE */
        } deref;
    } u;

    expr_t *next;   /* for argument lists */
};

/* ---- Statements ---- */

typedef enum {
    STMT_VARDECL,
    STMT_ASSIGN,
    STMT_TOGGLE_ASSIGN,
    STMT_EXPR,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_GOTO,
    STMT_TAILCALL,
    STMT_LABEL,
    STMT_ASM,
    STMT_CONST
} stmt_kind_t;

struct stmt_node {
    stmt_kind_t kind;
    int         line;

    union {
        /* VARDECL */
        struct {
            type_t *type;
            char   *name;
            int     pinned_reg;     /* REG_NONE or reg ID */
            reg_class_t pin_class;
            expr_t *init;           /* NULL if uninitialized */
            bool    is_const;       /* const qualifier — prevents reassignment */
        } vardecl;

        /* ASSIGN / TOGGLE_ASSIGN */
        struct { expr_t *target; expr_t *value; flag_case_t *flag_checks; } assign;

        /* EXPR (expression statement — function calls, builtins) */
        expr_t *expr;

        /* IF */
        struct { expr_t *cond; stmt_t *then_body; stmt_t *else_body; } if_stmt;

        /* WHILE */
        struct { expr_t *cond; stmt_t *body; } while_stmt;

        /* FOR (CX countdown) */
        struct { expr_t *start; int end_val; stmt_t *body; } for_stmt;

        /* RETURN */
        expr_t *ret_expr;  /* NULL for bare return */

        /* GOTO */
        char *goto_label;

        /* TAILCALL */
        expr_t *tailcall_expr;

        /* LABEL */
        char *label_name;

        /* CONST */
        struct { char *name; int value; expr_t *init; } konst;

        /* ASM */
        struct {
            char *body;
            reg_list_t *clobbers;   /* NULL = clobbers everything */
            reg_list_t *preserves;  /* NULL = nothing preserved */
            bool has_annotation;
            bool is_clobbers;       /* true = clobbers list, false = preserves list */
        } asm_stmt;
    } u;

    stmt_t *next;
};

/* ---- Parameters ---- */

struct param_node {
    char   *name;
    type_t *type;
    bool    is_value;       /* 'value' keyword for aggregate copy */

    /* For extern declarations: pinned register */
    int         pinned_reg;
    reg_class_t pin_class;
    bool        has_pin;

    /* For far params: segment register pin */
    int         pinned_seg;     /* segment register (SREG_*) */
    bool        has_seg_pin;

    param_t *next;
};

/* ---- Struct fields ---- */

struct field_node {
    char   *name;       /* NULL for padding '_' */
    type_t *type;       /* NULL for bit fields */
    int     bits;       /* >0 for bit fields */
    bool    is_bits;
    type_t *as_type;    /* typed pointer annotation, or NULL */

    field_t *next;
};

/* ---- Flag expressions (for flag-check blocks) ---- */

typedef enum {
    FEXPR_FLAG,     /* single flag */
    FEXPR_NOT,      /* !expr */
    FEXPR_OR,       /* expr | expr */
    FEXPR_AND,      /* expr & expr */
    FEXPR_XOR,      /* expr ^ expr */
} flag_expr_kind_t;

typedef struct flag_expr_node flag_expr_t;
struct flag_expr_node {
    flag_expr_kind_t kind;
    reg_id_t         flag_id;    /* for FEXPR_FLAG */
    flag_expr_t     *left;       /* for binary ops and NOT */
    flag_expr_t     *right;      /* for binary ops */
};

typedef struct flag_case_node flag_case_t;
struct flag_case_node {
    flag_expr_t  *condition;
    stmt_t       *body;          /* NULL for trap */
    bool          is_trap;
    flag_case_t  *next;
};

/* ---- Register/flag list (for preserves/clobbers) ---- */

struct reg_list_node {
    reg_id_t    id;
    reg_class_t rclass;
    bool        is_flags_all;   /* FLAGS keyword */
    reg_list_t *next;
};

/* ---- Top-level declarations ---- */

typedef enum {
    DECL_FN,
    DECL_STRUCT,
    DECL_GLOBAL,
    DECL_EXTERN_GLOBAL,
    DECL_EXTERN_FN,
    DECL_USE,
    DECL_CONST,
    DECL_AT,
    DECL_ENDAT
} decl_kind_t;

typedef struct {
    bool is_far;
    bool is_reentrant;
    bool is_interrupt;
    int  interrupt_vector;
    bool has_chain;
    char *chain_name;
    bool has_preserves;
    bool is_clobbers;       /* true if clobbers() used instead of preserves() */
    reg_list_t *preserves;
    bool has_ret_pin;
    int  ret_pinned_reg;
    reg_class_t ret_pin_class;
    bool has_at;
    int  at_seg;
    int  at_off;
} fn_modifiers_t;

struct decl_node {
    decl_kind_t kind;
    int         line;
    bool        is_pub;     /* visible to other modules via .nif */

    union {
        /* FN */
        struct {
            char          *name;
            fn_modifiers_t mods;
            param_t       *params;
            type_t        *return_type;   /* NULL if void */
            stmt_t        *body;
        } fn;

        /* STRUCT */
        struct {
            char    *name;
            bool     aligned;
            field_t *fields;
        } struc;

        /* GLOBAL / EXTERN_GLOBAL */
        struct {
            type_t *type;
            char   *name;
            int     pinned_reg;
            reg_class_t pin_class;
            expr_t *init;
            bool    has_at;
            int     at_seg;
            int     at_off;
        } global;

        /* EXTERN_FN */
        struct {
            char          *name;
            fn_modifiers_t mods;
            param_t       *params;
            type_t        *return_type;
            int            ret_pinned_reg;
            reg_class_t    ret_pin_class;
            bool           has_ret_pin;
            reg_list_t    *preserves;
            /* For fixed-address externs */
            bool           has_address;
            int            addr_seg;
            int            addr_off;
        } extern_fn;

        /* USE */
        char *use_path;

        /* CONST */
        struct {
            char *name;
            int   value;
        } konst;

        /* AT (standalone placement) */
        struct {
            int seg;
            int off;
        } at;
    } u;

    decl_t *next;
};

/* ---- Program (top-level list of declarations) ---- */

typedef struct {
    decl_t *decls;      /* linked list */
    decl_t *decls_tail; /* for O(1) append */
} program_t;

/* ---- Allocator / constructor functions ---- */

/* Implemented in ast.c */
type_t     *mk_type(type_kind_t kind);
type_t     *mk_type_array(type_t *elem, int size);
type_t     *mk_type_struct(const char *name);

expr_t     *mk_expr_int(int val, int line);
expr_t     *mk_expr_str(const char *s, int line);
expr_t     *mk_expr_ident(const char *name, int line);
expr_t     *mk_expr_reg(reg_id_t id, reg_class_t rc, int line);
expr_t     *mk_expr_flag(reg_id_t id, int line);
expr_t     *mk_expr_binop(op_kind_t op, expr_t *l, expr_t *r, int line);
expr_t     *mk_expr_unop(op_kind_t op, expr_t *e, int line);
expr_t     *mk_expr_call(expr_t *func, expr_t *args, int line);
expr_t     *mk_expr_index(expr_t *arr, expr_t *idx, bool checked, int line);
expr_t     *mk_expr_field(expr_t *obj, const char *field, int line);
expr_t     *mk_expr_raw_field(expr_t *obj, const char *field, int line);
expr_t     *mk_expr_mem(reg_id_t seg, reg_id_t base, reg_id_t idx,
                         int disp, bool has_disp, int line);
expr_t     *mk_expr_mem_abs(int seg, int off, int line);
expr_t     *mk_expr_far_lit(int seg, int off, int line);
expr_t     *mk_expr_cast(expr_t *operand, type_t *target, int line);
expr_t     *mk_expr_array_init(expr_t *elements, int line);
expr_t     *mk_expr_indirect_call(expr_t *addr, const char *extern_name,
                                   const char *module_name, expr_t *args, int line);
expr_t     *mk_expr_deref(const char *name, int line);

stmt_t     *mk_stmt_vardecl(type_t *type, const char *name,
                             int pinned_reg, reg_class_t pin_class,
                             expr_t *init, int line);
stmt_t     *mk_stmt_assign(expr_t *target, expr_t *value, int line);
stmt_t     *mk_stmt_toggle(expr_t *target, expr_t *value, int line);
stmt_t     *mk_stmt_expr(expr_t *e, int line);
stmt_t     *mk_stmt_if(expr_t *cond, stmt_t *then_b, stmt_t *else_b, int line);
stmt_t     *mk_stmt_while(expr_t *cond, stmt_t *body, int line);
stmt_t     *mk_stmt_for(expr_t *start, int end_val, stmt_t *body, int line);
stmt_t     *mk_stmt_return(expr_t *e, int line);
flag_expr_t *mk_fexpr_flag(reg_id_t id);
flag_expr_t *mk_fexpr_not(flag_expr_t *e);
flag_expr_t *mk_fexpr_binop(flag_expr_kind_t kind, flag_expr_t *l, flag_expr_t *r);
flag_case_t *mk_flag_case(flag_expr_t *cond, stmt_t *body);
flag_case_t *mk_flag_case_trap(flag_expr_t *cond);

stmt_t     *mk_stmt_assign_checked(expr_t *target, expr_t *value,
                                    flag_case_t *checks, int line);
stmt_t     *mk_stmt_toggle_checked(expr_t *target, expr_t *value,
                                    flag_case_t *checks, int line);

stmt_t     *mk_stmt_break(int line);
stmt_t     *mk_stmt_continue(int line);
stmt_t     *mk_stmt_goto(const char *label, int line);
stmt_t     *mk_stmt_tailcall(expr_t *call_expr, int line);
stmt_t     *mk_stmt_label(const char *name, int line);
stmt_t     *mk_stmt_asm(const char *body, reg_list_t *annotation,
                         bool is_clobbers, bool has_annotation, int line);
stmt_t     *mk_stmt_const(const char *name, int value, int line);
stmt_t     *mk_stmt_const_expr(const char *name, expr_t *init, int line);

param_t    *mk_param(const char *name, type_t *type, bool is_value);
param_t    *mk_param_pinned(const char *name, type_t *type,
                             int pinned_reg, reg_class_t pin_class);
param_t    *mk_param_far_pinned(const char *name,
                                 int off_reg, int seg_reg);
field_t    *mk_field(const char *name, type_t *type);
field_t    *mk_field_typed_ptr(const char *name, type_t *storage, type_t *as_type);
field_t    *mk_field_bits(const char *name, int nbits);
reg_list_t *mk_reg_list(reg_id_t id, reg_class_t rc);
reg_list_t *mk_reg_list_flags_all(void);

decl_t     *mk_decl_fn(const char *name, fn_modifiers_t mods,
                        param_t *params, type_t *ret, stmt_t *body, int line);
decl_t     *mk_decl_struct(const char *name, bool aligned,
                            field_t *fields, int line);
decl_t     *mk_decl_global(type_t *type, const char *name,
                            int pinned_reg, reg_class_t pin_class,
                            expr_t *init,
                            bool has_at, int at_seg, int at_off,
                            int line);
decl_t     *mk_decl_extern_global(type_t *type, const char *name, int line);
decl_t     *mk_decl_extern_fn(const char *name, fn_modifiers_t mods,
                               param_t *params, type_t *ret,
                               int ret_pin, reg_class_t ret_pin_class,
                               bool has_ret_pin, reg_list_t *preserves,
                               bool has_addr, int addr_seg, int addr_off,
                               int line);
decl_t     *mk_decl_use(const char *path, int line);
decl_t     *mk_decl_const(const char *name, int value, int line);
decl_t     *mk_decl_at(int seg, int off, int line);
decl_t     *mk_decl_endat(int line);

program_t  *mk_program(void);
void        program_add(program_t *p, decl_t *d);

/* ---- Line tracking (set by lexer) ---- */
extern int yyline;

#endif /* NIB_AST_H */
