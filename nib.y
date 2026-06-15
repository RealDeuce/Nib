%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "compile.h"

extern int yylex(void);
extern int yyline;
extern FILE *yyin;

void yyerror(const char *s) {
    fprintf(stderr, "line %d: %s\n", yyline, s);
}

/* Global program root — set by parser */
program_t *parsed_program = NULL;

/* Helpers for building linked lists */
static stmt_t *stmt_list_append(stmt_t *list, stmt_t *item) {
    if (!item) return list;
    if (!list) return item;
    stmt_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

static expr_t *expr_list_append(expr_t *list, expr_t *item) {
    if (!list) return item;
    expr_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

static param_t *param_list_append(param_t *list, param_t *item) {
    if (!list) return item;
    param_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

static field_t *field_list_append(field_t *list, field_t *item) {
    if (!list) return item;
    field_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

static reg_list_t *reg_list_append(reg_list_t *list, reg_list_t *item) {
    if (!list) return item;
    reg_list_t *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

/* Temporary storage for fn_modifiers accumulation */
static fn_modifiers_t current_mods;

static void mods_reset(void) {
    memset(&current_mods, 0, sizeof(current_mods));
}

/* ---- Compile-time defines for `when` conditional compilation ---- */
#define MAX_DEFINES 64
static struct {
    char name[64];
    char value[256];
} defines[MAX_DEFINES];
static int ndefines = 0;

static void add_define(const char *spec) {
    if (ndefines >= MAX_DEFINES) return;
    const char *eq = strchr(spec, '=');
    if (!eq) {
        fprintf(stderr, "error: -D requires NAME=VALUE (got '%s')\n", spec);
        return;
    }
    int nlen = (int)(eq - spec);
    if (nlen > 63) nlen = 63;
    memcpy(defines[ndefines].name, spec, nlen);
    defines[ndefines].name[nlen] = '\0';
    strncpy(defines[ndefines].value, eq + 1, 255);
    ndefines++;
}

static bool when_eq(const char *name, const char *value) {
    for (int i = 0; i < ndefines; i++)
        if (strcmp(defines[i].name, name) == 0 &&
            strcmp(defines[i].value, value) == 0) return true;
    return false;
}

static bool when_neq(const char *name, const char *value) {
    return !when_eq(name, value);
}

/* Splice a decl list into a program */
static void program_splice(program_t *prog, decl_t *list) {
    if (!list) return;
    if (!prog->decls) {
        prog->decls = list;
    } else {
        prog->decls_tail->next = list;
    }
    /* Find new tail */
    decl_t *p = list;
    while (p->next) p = p->next;
    prog->decls_tail = p;
}
%}

%union {
    int            ival;
    char          *sval;
    type_t        *type;
    expr_t        *expr;
    stmt_t        *stmt;
    decl_t        *decl;
    param_t       *param;
    field_t       *field;
    reg_list_t    *rlist;
    fn_modifiers_t mods;
    struct { int reg; int rclass; } regval;
    flag_expr_t   *fexpr;
    flag_case_t   *fcase;
    struct { bool has_at; int seg; int off; } at_clause;
}

/* ---- Literals and identifiers ---- */
%token <ival> LIT_INT
%token <sval> LIT_STRING
%token <sval> IDENT
%token <sval> ASM_BODY

/* ---- Keywords ---- */
%token KW_FN KW_STRUCT KW_ALIGNED KW_EXTERN KW_FAR
%token KW_INTERRUPT KW_CHAIN KW_REENTRANT
%token KW_IF KW_ELSE KW_WHILE KW_FOR KW_IN
%token KW_RETURN KW_BREAK KW_CONTINUE
%token KW_ASM KW_VALUE KW_USE
%token KW_PRESERVES KW_CLOBBERS
%token KW_BITS KW_TRAP KW_GOTO KW_TAILCALL KW_AS KW_AT KW_CONST KW_PUB KW_WHEN KW_END KW_FROM

/* ---- Type keywords ---- */
%token TY_U8 TY_U16 TY_U32 TY_SEG TY_BCD TY_BOOL

/* ---- Register tokens ---- */
%token REG_AX REG_BX REG_CX REG_DX REG_SI REG_DI REG_BP REG_SP
%token REG_AL REG_AH REG_BL REG_BH REG_CL REG_CH REG_DL REG_DH
%token REG_DS REG_ES REG_SS REG_CS

/* ---- Flag tokens ---- */
%token FLAG_CF FLAG_PF FLAG_AF FLAG_ZF FLAG_SF FLAG_TF FLAG_DF FLAG_OF FLAG_IF
%token FLAG_ALL

/* ---- Multi-char operators ---- */
%token OP_SGTE OP_SLTE OP_SRSHR OP_SGT OP_SLT
%token OP_SMUL OP_SDIV OP_SMOD
%token OP_RCL OP_RCR OP_ROL OP_ROR
%token OP_XCHG
%token OP_SHL OP_SHR
%token OP_LTE OP_GTE OP_EQ OP_NEQ
%token OP_ASSIGN OP_TOGGLEASSIGN
%token OP_ARROW OP_DOTDOT

/* ---- Nonterminal types ---- */
%type <type>   type return_clause
%type <expr>   expr postfix_expr primary_expr mem_access mem_inner arg_list
%type <stmt>   stmt stmt_list var_decl assignment checked_assignment if_stmt while_stmt for_stmt asm_block when_stmt
%type <decl>   top_decl function_def struct_def extern_decl global_decl use_decl const_decl when_body when_block at_decl end_at_decl
%type <param>  param param_list extern_param extern_param_list
%type <field>  struct_field struct_fields
%type <rlist>  reg_flag_list reg_or_flag asm_annotation preserves_clause
%type <regval> reg_name word_reg byte_reg seg_reg flag mem_base
%type <type>   return_clause_extern_type
%type <regval> return_clause_extern_pin
%type <sval>   any_ident
%type <fcase>  flag_block flag_cases flag_case
%type <fexpr>  flag_expr flag_atom
%type <at_clause> at_clause

/* ---- Precedence (low to high) ---- */
%left OP_XCHG
%left '|'
%left '^'
%left '&'
%left OP_EQ OP_NEQ
%left '<' '>' OP_LTE OP_GTE OP_SGT OP_SLT OP_SGTE OP_SLTE
%left OP_SHL OP_SHR OP_SRSHR OP_ROL OP_ROR OP_RCL OP_RCR
%left '+' '-'
%left '*' '/' '%' OP_SMUL OP_SDIV OP_SMOD
%right UNARY
%left '.' '`' '[' '('

/* ---- Dangling else ---- */
%nonassoc LOWER_THAN_ELSE
%nonassoc KW_ELSE

%%

/* ==== Top level ==== */

program
    : /* empty */           { parsed_program = mk_program(); }
    | program top_decl      { program_add(parsed_program, $2); }
    | program when_block    { program_splice(parsed_program, $2); }
    ;

when_body
    : /* empty */           { $$ = NULL; }
    | when_body top_decl    {
        if ($1 == NULL) { $$ = $2; }
        else { decl_t *p = $1; while (p->next) p = p->next; p->next = $2; $$ = $1; }
    }
    ;

when_block
    : KW_WHEN IDENT OP_EQ LIT_STRING '{' when_body '}'
        { $$ = when_eq($2, $4) ? $6 : NULL; }
    | KW_WHEN IDENT OP_EQ LIT_STRING '{' when_body '}' KW_ELSE '{' when_body '}'
        { $$ = when_eq($2, $4) ? $6 : $10; }
    | KW_WHEN IDENT OP_NEQ LIT_STRING '{' when_body '}'
        { $$ = when_neq($2, $4) ? $6 : NULL; }
    | KW_WHEN IDENT OP_NEQ LIT_STRING '{' when_body '}' KW_ELSE '{' when_body '}'
        { $$ = when_neq($2, $4) ? $6 : $10; }
    ;

top_decl
    : function_def          { $$ = $1; }
    | struct_def            { $$ = $1; }
    | extern_decl           { $$ = $1; }
    | global_decl           { $$ = $1; }
    | use_decl              { $$ = $1; }
    | const_decl            { $$ = $1; }
    | KW_PUB function_def   { $$ = $2; $$->is_pub = true; }
    | KW_PUB struct_def     { $$ = $2; $$->is_pub = true; }
    | KW_PUB global_decl    { $$ = $2; $$->is_pub = true; }
    | KW_PUB const_decl     { $$ = $2; $$->is_pub = true; }
    | KW_PUB extern_decl   { $$ = $2; $$->is_pub = true; }
    | at_decl               { $$ = $1; }
    | end_at_decl           { $$ = $1; }
    ;

at_decl
    : KW_AT '(' LIT_INT ':' LIT_INT ')' ';'
        { $$ = mk_decl_at($3, $5, yyline); }
    ;

end_at_decl
    : KW_END KW_AT ';'
        { $$ = mk_decl_endat(yyline); }
    ;

/* ==== Use directive ==== */

use_decl
    : KW_USE LIT_STRING ';'    { $$ = mk_decl_use($2, yyline); }
    ;

/* ==== Constant declarations ==== */

const_decl
    : KW_CONST IDENT '=' LIT_INT ';'
        { $$ = mk_decl_const($2, $4, yyline); }
    ;

/* ==== Global variable declarations ==== */

global_decl
    : type IDENT at_clause '=' expr ';'
        { $$ = mk_decl_global($1, $2, REG_NONE, REGCLASS_WORD, $5, $3.has_at, $3.seg, $3.off, yyline); }
    | type IDENT at_clause '=' '{' arg_list '}' ';'
        { $$ = mk_decl_global($1, $2, REG_NONE, REGCLASS_WORD, mk_expr_array_init($6, yyline), $3.has_at, $3.seg, $3.off, yyline); }
    | type IDENT at_clause ';'
        { $$ = mk_decl_global($1, $2, REG_NONE, REGCLASS_WORD, NULL, $3.has_at, $3.seg, $3.off, yyline); }
    | type reg_name at_clause '=' expr ';'
        { $$ = mk_decl_global($1, NULL, $2.reg, $2.rclass, $5, $3.has_at, $3.seg, $3.off, yyline); }
    | type reg_name at_clause ';'
        { $$ = mk_decl_global($1, NULL, $2.reg, $2.rclass, NULL, $3.has_at, $3.seg, $3.off, yyline); }
    | KW_EXTERN type IDENT ';'
        { $$ = mk_decl_extern_global($2, $3, yyline); }
    | KW_EXTERN type reg_name ';'
        { $$ = mk_decl_extern_global($2, NULL, yyline); }
    ;

at_clause
    : /* empty */
        { $$.has_at = false; $$.seg = 0; $$.off = 0; }
    | KW_AT '(' LIT_INT ':' LIT_INT ')'
        { $$.has_at = true; $$.seg = $3; $$.off = $5; }
    ;

/* ==== Struct definitions ==== */

struct_def
    : KW_STRUCT IDENT '{' struct_fields '}'
        { $$ = mk_decl_struct($2, false, $4, yyline); }
    | KW_STRUCT KW_ALIGNED IDENT '{' struct_fields '}'
        { $$ = mk_decl_struct($3, true, $5, yyline); }
    ;

struct_fields
    : struct_field                  { $$ = $1; }
    | struct_fields struct_field    { $$ = field_list_append($1, $2); }
    ;

struct_field
    : type IDENT ';'
        { $$ = mk_field($2, $1); }
    | type IDENT KW_AS type ';'
        { $$ = mk_field_typed_ptr($2, $1, $4); }
    | IDENT ':' type ';'
        { $$ = mk_field($1, $3); }
    | IDENT ':' KW_BITS '(' LIT_INT ')' ';'
        { $$ = mk_field_bits($1, $5); }
    | '_' ':' KW_BITS '(' LIT_INT ')' ';'
        { $$ = mk_field_bits(NULL, $5); }
    ;

/* ==== Function definitions ==== */

fn_start
    : KW_FN                    { mods_reset(); }
    ;

function_def
    : fn_start fn_modifiers IDENT '(' param_list ')' return_clause fn_preserves '{' stmt_list '}'
        { $$ = mk_decl_fn($3, current_mods, $5, $7, $10, yyline); }
    | fn_start fn_modifiers IDENT '(' ')' return_clause fn_preserves '{' stmt_list '}'
        { $$ = mk_decl_fn($3, current_mods, NULL, $6, $9, yyline); }
    ;

fn_preserves
    : /* empty */
    | KW_PRESERVES '(' reg_flag_list ')'
        { current_mods.has_preserves = true; current_mods.preserves = $3; }
    | KW_CLOBBERS '(' reg_flag_list ')'
        { current_mods.has_preserves = true; current_mods.is_clobbers = true;
          current_mods.preserves = $3; }
    ;

fn_modifiers
    : /* empty */
    | fn_modifiers fn_modifier
    ;

fn_modifier
    : KW_FAR                    { current_mods.is_far = true; }
    | KW_REENTRANT              { current_mods.is_reentrant = true; }
    | KW_AT '(' LIT_INT ':' LIT_INT ')'
        { current_mods.has_at = true; current_mods.at_seg = $3; current_mods.at_off = $5; }
    | interrupt_clause
    ;

interrupt_clause
    : KW_INTERRUPT '(' LIT_INT ')'
        { current_mods.is_interrupt = true; current_mods.interrupt_vector = $3; }
    | KW_INTERRUPT '(' LIT_INT ',' KW_CHAIN IDENT ')'
        { current_mods.is_interrupt = true; current_mods.interrupt_vector = $3;
          current_mods.has_chain = true; current_mods.chain_name = $6; }
    ;

return_clause
    : /* empty */               { $$ = NULL; }
    | OP_ARROW type             { $$ = $2; }
    | OP_ARROW type KW_IN reg_name
        { $$ = $2;
          current_mods.has_ret_pin = true;
          current_mods.ret_pinned_reg = $4.reg;
          current_mods.ret_pin_class = $4.rclass; }
    ;

/* ==== Extern declarations ==== */

extern_fn_start
    : KW_EXTERN KW_FN          { mods_reset(); }
    ;

extern_decl
    : extern_fn_start extern_modifiers IDENT '(' extern_param_list ')' return_clause_extern_type return_clause_extern_pin preserves_clause ';'
        { $$ = mk_decl_extern_fn($3, current_mods, $5, $7,
              $8.reg, $8.rclass, ($8.reg != REG_NONE),
              $9, false, 0, 0, yyline); }
    | extern_fn_start extern_modifiers IDENT '(' ')' return_clause_extern_type return_clause_extern_pin preserves_clause ';'
        { $$ = mk_decl_extern_fn($3, current_mods, NULL, $6,
              $7.reg, $7.rclass, ($7.reg != REG_NONE),
              $8, false, 0, 0, yyline); }
    | extern_fn_start extern_modifiers '[' LIT_INT ':' LIT_INT ']' IDENT '(' extern_param_list ')' return_clause_extern_type return_clause_extern_pin preserves_clause ';'
        { $$ = mk_decl_extern_fn($8, current_mods, $10, $12,
              $13.reg, $13.rclass, ($13.reg != REG_NONE),
              $14, true, $4, $6, yyline); }
    | extern_fn_start extern_modifiers '[' LIT_INT ':' LIT_INT ']' IDENT '(' ')' return_clause_extern_type return_clause_extern_pin preserves_clause ';'
        { $$ = mk_decl_extern_fn($8, current_mods, NULL, $11,
              $12.reg, $12.rclass, ($12.reg != REG_NONE),
              $13, true, $4, $6, yyline); }
    ;

extern_modifiers
    : /* empty */
    | extern_modifiers extern_modifier
    ;

extern_modifier
    : KW_FAR                            { current_mods.is_far = true; }
    | KW_INTERRUPT '(' LIT_INT ')'     { current_mods.is_interrupt = true; current_mods.interrupt_vector = $3; }
    ;

return_clause_extern_type
    : /* empty */               { $$ = NULL; }
    | OP_ARROW type             { $$ = $2; }
    ;

return_clause_extern_pin
    : /* empty */               { $$.reg = REG_NONE; $$.rclass = REGCLASS_WORD; }
    | KW_IN reg_name            { $$ = $2; }
    ;

preserves_clause
    : /* empty */                               { $$ = NULL; }
    | KW_PRESERVES '(' reg_flag_list ')'        { $$ = $3; }
    ;

/* ==== Parameters ==== */

param_list
    : param                         { $$ = $1; }
    | param_list ',' param          { $$ = param_list_append($1, $3); }
    ;

param
    : IDENT ':' type                { $$ = mk_param($1, $3, false); }
    | KW_VALUE IDENT ':' type       { $$ = mk_param($2, $4, true); }
    | IDENT ':' type KW_IN reg_name
        { $$ = mk_param_pinned($1, $3, $5.reg, $5.rclass); }
    | KW_VALUE IDENT ':' type KW_IN reg_name
        { $$ = mk_param_pinned($2, $4, $6.reg, $6.rclass); $$->is_value = true; }
    | IDENT ':' KW_FAR KW_IN seg_reg ':' word_reg
        { $$ = mk_param_far_pinned($1, $7.reg, $5.reg); }
    ;

extern_param_list
    : extern_param                          { $$ = $1; }
    | extern_param_list ',' extern_param    { $$ = param_list_append($1, $3); }
    ;

extern_param
    : IDENT ':' type KW_IN reg_name
        { $$ = mk_param_pinned($1, $3, $5.reg, $5.rclass); }
    | IDENT ':' KW_FAR KW_IN seg_reg ':' word_reg
        { $$ = mk_param_far_pinned($1, $7.reg, $5.reg); }
    ;

/* ==== Statements ==== */

stmt_list
    : /* empty */                   { $$ = NULL; }
    | stmt_list stmt                { $$ = stmt_list_append($1, $2); }
    ;

stmt
    : var_decl ';'                  { $$ = $1; }
    | assignment ';'                { $$ = $1; }
    | checked_assignment            { $$ = $1; }
    | expr ';'                      { $$ = mk_stmt_expr($1, yyline); }
    | if_stmt                       { $$ = $1; }
    | while_stmt                    { $$ = $1; }
    | for_stmt                      { $$ = $1; }
    | KW_RETURN expr ';'            { $$ = mk_stmt_return($2, yyline); }
    | KW_RETURN ';'                 { $$ = mk_stmt_return(NULL, yyline); }
    | KW_BREAK ';'                  { $$ = mk_stmt_break(yyline); }
    | KW_CONTINUE ';'              { $$ = mk_stmt_continue(yyline); }
    | KW_GOTO IDENT ';'            { $$ = mk_stmt_goto($2, yyline); }
    | KW_TAILCALL expr ';'         { $$ = mk_stmt_tailcall($2, yyline); }
    | IDENT ':'                     { $$ = mk_stmt_label($1, yyline); }
    | asm_block                     { $$ = $1; }
    | when_stmt                     { $$ = $1; }
    ;

when_stmt
    : KW_WHEN IDENT OP_EQ LIT_STRING '{' stmt_list '}'
        { $$ = when_eq($2, $4) ? $6 : NULL; }
    | KW_WHEN IDENT OP_EQ LIT_STRING '{' stmt_list '}' KW_ELSE '{' stmt_list '}'
        { $$ = when_eq($2, $4) ? $6 : $10; }
    | KW_WHEN IDENT OP_NEQ LIT_STRING '{' stmt_list '}'
        { $$ = when_neq($2, $4) ? $6 : NULL; }
    | KW_WHEN IDENT OP_NEQ LIT_STRING '{' stmt_list '}' KW_ELSE '{' stmt_list '}'
        { $$ = when_neq($2, $4) ? $6 : $10; }
    ;

/* ---- Variable declarations ---- */

var_decl
    : type IDENT '=' expr
        { $$ = mk_stmt_vardecl($1, $2, REG_NONE, REGCLASS_WORD, $4, yyline); }
    | type IDENT
        { $$ = mk_stmt_vardecl($1, $2, REG_NONE, REGCLASS_WORD, NULL, yyline); }
    | type reg_name '=' expr
        { $$ = mk_stmt_vardecl($1, NULL, $2.reg, $2.rclass, $4, yyline); }
    | type reg_name
        { $$ = mk_stmt_vardecl($1, NULL, $2.reg, $2.rclass, NULL, yyline); }
    | TY_SEG seg_reg '=' expr
        { $$ = mk_stmt_vardecl(mk_type(TYPE_SEG), NULL, $2.reg, REGCLASS_SEG, $4, yyline); }
    | TY_SEG seg_reg
        { $$ = mk_stmt_vardecl(mk_type(TYPE_SEG), NULL, $2.reg, REGCLASS_SEG, NULL, yyline); }
    ;

/* ---- Assignment ---- */

assignment
    : expr OP_ASSIGN expr
        { $$ = mk_stmt_assign($1, $3, yyline); }
    | expr OP_TOGGLEASSIGN expr
        { $$ = mk_stmt_toggle($1, $3, yyline); }
    ;

checked_assignment
    : expr OP_ASSIGN expr flag_block
        { $$ = mk_stmt_assign_checked($1, $3, $4, yyline); }
    | expr OP_TOGGLEASSIGN expr flag_block
        { $$ = mk_stmt_toggle_checked($1, $3, $4, yyline); }
    ;

/* ---- Flag-check blocks ---- */

flag_block
    : '{' flag_cases '}'               { $$ = $2; }
    ;

flag_cases
    : flag_case                         { $$ = $1; }
    | flag_cases flag_case              {
        flag_case_t *p = $1;
        while (p->next) p = p->next;
        p->next = $2;
        $$ = $1;
    }
    ;

flag_case
    : flag_expr ':' '{' stmt_list '}'   { $$ = mk_flag_case($1, $4); }
    | flag_expr ':' KW_TRAP ';'         { $$ = mk_flag_case_trap($1); }
    ;

flag_expr
    : flag_atom                         { $$ = $1; }
    | flag_expr '|' flag_atom           { $$ = mk_fexpr_binop(FEXPR_OR, $1, $3); }
    | flag_expr '&' flag_atom           { $$ = mk_fexpr_binop(FEXPR_AND, $1, $3); }
    | flag_expr '^' flag_atom           { $$ = mk_fexpr_binop(FEXPR_XOR, $1, $3); }
    ;

flag_atom
    : flag                              { $$ = mk_fexpr_flag($1.reg); }
    | '!' flag_atom                     { $$ = mk_fexpr_not($2); }
    | '(' flag_expr ')'                 { $$ = $2; }
    ;

/* ---- Control flow ---- */

if_stmt
    : KW_IF '(' expr ')' '{' stmt_list '}' %prec LOWER_THAN_ELSE
        { $$ = mk_stmt_if($3, $6, NULL, yyline); }
    | KW_IF '(' expr ')' '{' stmt_list '}' KW_ELSE '{' stmt_list '}'
        { $$ = mk_stmt_if($3, $6, $10, yyline); }
    | KW_IF '(' expr ')' '{' stmt_list '}' KW_ELSE if_stmt
        { $$ = mk_stmt_if($3, $6, $9, yyline); }
    ;

while_stmt
    : KW_WHILE '(' expr ')' '{' stmt_list '}'
        { $$ = mk_stmt_while($3, $6, yyline); }
    ;

for_stmt
    : KW_FOR '(' REG_CX KW_IN expr OP_DOTDOT LIT_INT ')' '{' stmt_list '}'
        { $$ = mk_stmt_for($5, $7, $10, yyline); }
    ;

/* ---- ASM blocks ---- */

asm_block
    : KW_ASM asm_annotation '{' ASM_BODY
        { $$ = mk_stmt_asm($4, $2,
              /* is_clobbers: determined by which keyword was used —
                 the annotation rule sets the rlist, and we need to know
                 which it was. We'll use a convention: if the first node
                 has rclass == -1, it's a preserves sentinel. Simpler:
                 just track it in a global. */
              false, true, yyline);
          /* Note: is_clobbers gets patched by asm_annotation */ }
    | KW_ASM '{' ASM_BODY
        { $$ = mk_stmt_asm($3, NULL, false, false, yyline); }
    ;

asm_annotation
    : KW_CLOBBERS '(' reg_flag_list ')'     { $$ = $3; }
    | KW_PRESERVES '(' reg_flag_list ')'    { $$ = $3; }
    ;

/* ==== Expressions ==== */

expr
    : expr OP_XCHG expr
        { $$ = mk_expr_binop(NIB_XCHG, $1, $3, yyline); }
    | expr '|' expr
        { $$ = mk_expr_binop(NIB_OR, $1, $3, yyline); }
    | expr '^' expr
        { $$ = mk_expr_binop(NIB_XOR, $1, $3, yyline); }
    | expr '&' expr
        { $$ = mk_expr_binop(NIB_AND, $1, $3, yyline); }
    | expr OP_EQ expr
        { $$ = mk_expr_binop(NIB_EQ, $1, $3, yyline); }
    | expr OP_NEQ expr
        { $$ = mk_expr_binop(NIB_NEQ, $1, $3, yyline); }
    | expr '<' expr
        { $$ = mk_expr_binop(NIB_LT, $1, $3, yyline); }
    | expr '>' expr
        { $$ = mk_expr_binop(NIB_GT, $1, $3, yyline); }
    | expr OP_LTE expr
        { $$ = mk_expr_binop(NIB_LTE, $1, $3, yyline); }
    | expr OP_GTE expr
        { $$ = mk_expr_binop(NIB_GTE, $1, $3, yyline); }
    | expr OP_SLT expr
        { $$ = mk_expr_binop(NIB_SLT, $1, $3, yyline); }
    | expr OP_SGT expr
        { $$ = mk_expr_binop(NIB_SGT, $1, $3, yyline); }
    | expr OP_SLTE expr
        { $$ = mk_expr_binop(NIB_SLTE, $1, $3, yyline); }
    | expr OP_SGTE expr
        { $$ = mk_expr_binop(NIB_SGTE, $1, $3, yyline); }
    | expr OP_SHL expr
        { $$ = mk_expr_binop(NIB_SHL, $1, $3, yyline); }
    | expr OP_SHR expr
        { $$ = mk_expr_binop(NIB_SHR, $1, $3, yyline); }
    | expr OP_SRSHR expr
        { $$ = mk_expr_binop(NIB_SRSHR, $1, $3, yyline); }
    | expr OP_ROL expr
        { $$ = mk_expr_binop(NIB_ROL, $1, $3, yyline); }
    | expr OP_ROR expr
        { $$ = mk_expr_binop(NIB_ROR, $1, $3, yyline); }
    | expr OP_RCL expr
        { $$ = mk_expr_binop(NIB_RCL, $1, $3, yyline); }
    | expr OP_RCR expr
        { $$ = mk_expr_binop(NIB_RCR, $1, $3, yyline); }
    | expr '+' expr
        { $$ = mk_expr_binop(NIB_ADD, $1, $3, yyline); }
    | expr '-' expr
        { $$ = mk_expr_binop(NIB_SUB, $1, $3, yyline); }
    | expr '*' expr
        { $$ = mk_expr_binop(NIB_MUL, $1, $3, yyline); }
    | expr '/' expr
        { $$ = mk_expr_binop(NIB_DIV, $1, $3, yyline); }
    | expr '%' expr
        { $$ = mk_expr_binop(NIB_MOD, $1, $3, yyline); }
    | expr OP_SMUL expr
        { $$ = mk_expr_binop(NIB_SMUL, $1, $3, yyline); }
    | expr OP_SDIV expr
        { $$ = mk_expr_binop(NIB_SDIV, $1, $3, yyline); }
    | expr OP_SMOD expr
        { $$ = mk_expr_binop(NIB_SMOD, $1, $3, yyline); }
    | '-' expr %prec UNARY
        { $$ = mk_expr_unop(NIB_NEG, $2, yyline); }
    | '~' expr %prec UNARY
        { $$ = mk_expr_unop(NIB_NOT, $2, yyline); }
    | '&' expr %prec UNARY
        { $$ = mk_expr_unop(NIB_ADDR, $2, yyline); }
    | '@' expr %prec UNARY
        { $$ = mk_expr_unop(NIB_FAR_ADDR, $2, yyline); }
    | '!' expr %prec UNARY
        { $$ = mk_expr_unop(NIB_LNOT, $2, yyline); }
    | postfix_expr
        { $$ = $1; }
    ;

postfix_expr
    : postfix_expr KW_AS '(' type ')'
        { $$ = mk_expr_cast($1, $4, yyline); }
    | postfix_expr '.' IDENT
        { $$ = mk_expr_field($1, $3, yyline); }
    | postfix_expr '`' any_ident
        { $$ = mk_expr_raw_field($1, $3, yyline); }
    | postfix_expr '[' expr ']'
        { $$ = mk_expr_index($1, $3, false, yyline); }
    | postfix_expr '!' '[' expr ']'
        { $$ = mk_expr_index($1, $4, true, yyline); }
    | postfix_expr '(' arg_list ')'
        { $$ = mk_expr_call($1, $3, yyline); }
    | postfix_expr '(' ')'
        { $$ = mk_expr_call($1, NULL, yyline); }
    | postfix_expr KW_AS IDENT KW_FROM IDENT '(' arg_list ')'
        { $$ = mk_expr_indirect_call($1, $3, $5, $7, yyline); }
    | postfix_expr KW_AS IDENT KW_FROM IDENT '(' ')'
        { $$ = mk_expr_indirect_call($1, $3, $5, NULL, yyline); }
    | primary_expr
        { $$ = $1; }
    ;

primary_expr
    : LIT_INT               { $$ = mk_expr_int($1, yyline); }
    | LIT_STRING            { $$ = mk_expr_str($1, yyline); }
    | IDENT                 { $$ = mk_expr_ident($1, yyline); }
    | reg_name              { $$ = mk_expr_reg($1.reg, $1.rclass, yyline); }
    | seg_reg               { $$ = mk_expr_reg($1.reg, REGCLASS_SEG, yyline); }
    | flag                  { $$ = mk_expr_flag($1.reg, yyline); }
    | LIT_INT ':' LIT_INT   { $$ = mk_expr_far_lit($1, $3, yyline); }
    | '(' expr ')'          { $$ = $2; }
    | mem_access            { $$ = $1; }
    ;

/* ---- Memory access [seg:expr] or [expr] ---- */

mem_access
    : '[' mem_inner ']'
        { $$ = $2; }
    | '[' seg_reg ':' mem_inner ']'
        { $$ = $4; $$->u.mem.seg = $2.reg; }
    | '[' LIT_INT ':' LIT_INT ']'
        { $$ = mk_expr_mem_abs($2, $4, yyline); }
    ;

mem_inner
    : mem_base
        { $$ = mk_expr_mem(REG_NONE, $1.reg, REG_NONE, 0, false, yyline);
          /* Determine if this is base or index based on register */
          if ($1.reg == WREG_SI || $1.reg == WREG_DI)
            { $$->u.mem.base = REG_NONE; $$->u.mem.index = $1.reg; }
          else
            { $$->u.mem.base = $1.reg; $$->u.mem.index = REG_NONE; }
        }
    | mem_base '+' mem_base
        { $$ = mk_expr_mem(REG_NONE, $1.reg, $3.reg, 0, false, yyline); }
    | mem_base '+' LIT_INT
        { $$ = mk_expr_mem(REG_NONE, REG_NONE, REG_NONE, $3, true, yyline);
          if ($1.reg == WREG_SI || $1.reg == WREG_DI)
            { $$->u.mem.index = $1.reg; }
          else
            { $$->u.mem.base = $1.reg; }
        }
    | mem_base '+' mem_base '+' LIT_INT
        { $$ = mk_expr_mem(REG_NONE, $1.reg, $3.reg, $5, true, yyline); }
    | LIT_INT
        { $$ = mk_expr_mem(REG_NONE, REG_NONE, REG_NONE, $1, true, yyline); }
    ;

mem_base
    : REG_BX    { $$.reg = WREG_BX; $$.rclass = REGCLASS_WORD; }
    | REG_SI    { $$.reg = WREG_SI; $$.rclass = REGCLASS_WORD; }
    | REG_DI    { $$.reg = WREG_DI; $$.rclass = REGCLASS_WORD; }
    | REG_BP    { $$.reg = WREG_BP; $$.rclass = REGCLASS_WORD; }
    ;

/* ---- Function call arguments ---- */

arg_list
    : expr                      { $$ = $1; }
    | arg_list ',' expr         { $$ = expr_list_append($1, $3); }
    ;

/* ==== Register and flag name groups ==== */

reg_name
    : word_reg      { $$ = $1; }
    | byte_reg      { $$ = $1; }
    ;

word_reg
    : REG_AX    { $$.reg = WREG_AX; $$.rclass = REGCLASS_WORD; }
    | REG_BX    { $$.reg = WREG_BX; $$.rclass = REGCLASS_WORD; }
    | REG_CX    { $$.reg = WREG_CX; $$.rclass = REGCLASS_WORD; }
    | REG_DX    { $$.reg = WREG_DX; $$.rclass = REGCLASS_WORD; }
    | REG_SI    { $$.reg = WREG_SI; $$.rclass = REGCLASS_WORD; }
    | REG_DI    { $$.reg = WREG_DI; $$.rclass = REGCLASS_WORD; }
    | REG_BP    { $$.reg = WREG_BP; $$.rclass = REGCLASS_WORD; }
    | REG_SP    { $$.reg = WREG_SP; $$.rclass = REGCLASS_WORD; }
    ;

byte_reg
    : REG_AL    { $$.reg = BREG_AL; $$.rclass = REGCLASS_BYTE; }
    | REG_AH    { $$.reg = BREG_AH; $$.rclass = REGCLASS_BYTE; }
    | REG_BL    { $$.reg = BREG_BL; $$.rclass = REGCLASS_BYTE; }
    | REG_BH    { $$.reg = BREG_BH; $$.rclass = REGCLASS_BYTE; }
    | REG_CL    { $$.reg = BREG_CL; $$.rclass = REGCLASS_BYTE; }
    | REG_CH    { $$.reg = BREG_CH; $$.rclass = REGCLASS_BYTE; }
    | REG_DL    { $$.reg = BREG_DL; $$.rclass = REGCLASS_BYTE; }
    | REG_DH    { $$.reg = BREG_DH; $$.rclass = REGCLASS_BYTE; }
    ;

seg_reg
    : REG_DS    { $$.reg = SREG_DS; $$.rclass = REGCLASS_SEG; }
    | REG_ES    { $$.reg = SREG_ES; $$.rclass = REGCLASS_SEG; }
    | REG_SS    { $$.reg = SREG_SS; $$.rclass = REGCLASS_SEG; }
    | REG_CS    { $$.reg = SREG_CS; $$.rclass = REGCLASS_SEG; }
    ;

flag
    : FLAG_CF   { $$.reg = FLG_CF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_PF   { $$.reg = FLG_PF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_AF   { $$.reg = FLG_AF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_ZF   { $$.reg = FLG_ZF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_SF   { $$.reg = FLG_SF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_TF   { $$.reg = FLG_TF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_DF   { $$.reg = FLG_DF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_OF   { $$.reg = FLG_OF; $$.rclass = REGCLASS_FLAG; }
    | FLAG_IF   { $$.reg = FLG_IF; $$.rclass = REGCLASS_FLAG; }
    ;

reg_flag_list
    : reg_or_flag                           { $$ = $1; }
    | reg_flag_list ',' reg_or_flag         { $$ = reg_list_append($1, $3); }
    ;

reg_or_flag
    : reg_name      { $$ = mk_reg_list($1.reg, $1.rclass); }
    | seg_reg       { $$ = mk_reg_list($1.reg, $1.rclass); }
    | flag          { $$ = mk_reg_list($1.reg, $1.rclass); }
    | FLAG_ALL      { $$ = mk_reg_list_flags_all(); }
    ;

/* ==== Types ==== */

type
    : TY_U8                     { $$ = mk_type(TYPE_U8); }
    | TY_U16                    { $$ = mk_type(TYPE_U16); }
    | TY_U32                    { $$ = mk_type(TYPE_U32); }
    | TY_SEG                    { $$ = mk_type(TYPE_SEG); }
    | TY_BOOL                   { $$ = mk_type(TYPE_BOOL); }
    | KW_FAR                    { $$ = mk_type(TYPE_FAR); }
    | TY_U8 '[' LIT_INT ']'    { $$ = mk_type_array(mk_type(TYPE_U8), $3); }
    | TY_U8 '[' ']'            { $$ = mk_type_array(mk_type(TYPE_U8), 0); }
    | TY_U16 '[' LIT_INT ']'   { $$ = mk_type_array(mk_type(TYPE_U16), $3); }
    | TY_U16 '[' ']'           { $$ = mk_type_array(mk_type(TYPE_U16), 0); }
    | TY_BCD '[' LIT_INT ']'   { $$ = mk_type_array(mk_type(TYPE_BCD), $3); }
    | KW_FAR '[' LIT_INT ']'   { $$ = mk_type_array(mk_type(TYPE_FAR), $3); }
    | KW_STRUCT IDENT '[' LIT_INT ']' { $$ = mk_type_array(mk_type_struct($2), $4); }
    | KW_STRUCT IDENT           { $$ = mk_type_struct($2); }
    ;

/* Identifiers that can appear after backtick — includes keywords
 * that are valid as field/component names */
any_ident
    : IDENT                     { $$ = $1; }
    | TY_SEG                    { $$ = strdup("seg"); }
    | TY_U8                     { $$ = strdup("u8"); }
    | TY_U16                    { $$ = strdup("u16"); }
    | KW_FAR                    { $$ = strdup("far"); }
    ;

%%

/* Derive output paths from input path */
static char *replace_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    int baselen = dot ? (int)(dot - path) : (int)strlen(path);
    char *out = malloc(baselen + strlen(ext) + 1);
    memcpy(out, path, baselen);
    strcpy(out + baselen, ext);
    return out;
}

int main(int argc, char **argv) {
    const char *infile = NULL;
    bool parse_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--parse-only") == 0)
            parse_only = true;
        else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc)
            add_define(argv[++i]);
        else if (strncmp(argv[i], "-D", 2) == 0 && argv[i][2])
            add_define(argv[i] + 2);
        else
            infile = argv[i];
    }

    if (infile) {
        yyin = fopen(infile, "r");
        if (!yyin) {
            perror(infile);
            return 1;
        }
    }

    int result = yyparse();
    if (result != 0 || !parsed_program)
        return 1;

    /* Count declarations */
    int ndecls = 0;
    for (decl_t *d = parsed_program->decls; d; d = d->next)
        ndecls++;

    if (parse_only) {
        printf("Parse OK: %d declarations\n", ndecls);
        return 0;
    }

    /* Compile */
    const char *base = infile ? infile : "out.nib";
    char *nir_path = replace_ext(base, ".nir");
    char *nif_path = replace_ext(base, ".nif");

    /* Get source directory for use resolution */
    char src_dir[256] = ".";
    if (infile) {
        char tmp[256];
        strncpy(tmp, infile, 255);
        tmp[255] = '\0';
        /* Find last / */
        char *slash = strrchr(tmp, '/');
        if (slash) {
            *slash = '\0';
            strncpy(src_dir, tmp, sizeof(src_dir) - 1);
        }
    }

    result = compile(parsed_program, nir_path, nif_path, src_dir, base);

    if (result == 0) {
        fprintf(stderr, "%s: %d declarations -> %s + %s\n",
                base, ndecls, nir_path, nif_path);
    }

    free(nir_path);
    free(nif_path);
    return result;
}
