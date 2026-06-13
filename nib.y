%{
#include <stdio.h>
#include <stdlib.h>
#include "ast.h"

extern int yylex(void);
extern int yyline;
extern FILE *yyin;
void yyerror(const char *s) {
    fprintf(stderr, "line %d: %s\n", yyline, s);
}
%}

%union {
    int    ival;
    char  *sval;
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
%token KW_ASM KW_VALUE
%token KW_PRESERVES KW_CLOBBERS
%token KW_BITS

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
%left '.' '[' '('

/* ---- Dangling else ---- */
%nonassoc LOWER_THAN_ELSE
%nonassoc KW_ELSE

%%

/* ==== Top level ==== */

program
    : /* empty */
    | program top_decl
    ;

top_decl
    : function_def
    | struct_def
    | extern_decl
    | global_decl
    ;

/* ==== Global variable declarations ==== */

global_decl
    : type IDENT '=' expr ';'
    | type IDENT ';'
    | type reg_name '=' expr ';'
    | type reg_name ';'
    | KW_EXTERN type IDENT ';'
    | KW_EXTERN type reg_name ';'
    ;

/* ==== Struct definitions ==== */

struct_def
    : KW_STRUCT IDENT '{' struct_fields '}'
    | KW_STRUCT KW_ALIGNED IDENT '{' struct_fields '}'
    ;

struct_fields
    : struct_field
    | struct_fields struct_field
    ;

struct_field
    : type IDENT ';'
    | IDENT ':' type ';'
    | IDENT ':' KW_BITS '(' LIT_INT ')' ';'
    | '_' ':' KW_BITS '(' LIT_INT ')' ';'
    ;

/* ==== Function definitions ==== */

function_def
    : KW_FN fn_modifiers IDENT '(' param_list ')' return_clause '{' stmt_list '}'
    | KW_FN fn_modifiers IDENT '(' ')' return_clause '{' stmt_list '}'
    ;

fn_modifiers
    : /* empty */
    | fn_modifiers fn_modifier
    ;

fn_modifier
    : KW_FAR
    | KW_REENTRANT
    | interrupt_clause
    ;

interrupt_clause
    : KW_INTERRUPT '(' LIT_INT ')'
    | KW_INTERRUPT '(' LIT_INT ',' KW_CHAIN IDENT ')'
    ;

return_clause
    : /* empty */
    | OP_ARROW type
    ;

/* ==== Extern declarations ==== */

extern_decl
    : KW_EXTERN KW_FN extern_modifiers IDENT '(' extern_param_list ')' return_clause_extern preserves_clause ';'
    | KW_EXTERN KW_FN extern_modifiers IDENT '(' ')' return_clause_extern preserves_clause ';'
    | KW_EXTERN KW_FN extern_modifiers '[' LIT_INT ':' LIT_INT ']' IDENT '(' extern_param_list ')' return_clause_extern preserves_clause ';'
    | KW_EXTERN KW_FN extern_modifiers '[' LIT_INT ':' LIT_INT ']' IDENT '(' ')' return_clause_extern preserves_clause ';'
    ;

extern_modifiers
    : /* empty */
    | extern_modifiers extern_modifier
    ;

extern_modifier
    : KW_FAR
    | KW_INTERRUPT '(' LIT_INT ')'
    ;

return_clause_extern
    : /* empty */
    | OP_ARROW type KW_IN reg_name
    ;

preserves_clause
    : /* empty */
    | KW_PRESERVES '(' reg_flag_list ')'
    ;

/* ==== Parameters ==== */

param_list
    : param
    | param_list ',' param
    ;

param
    : IDENT ':' type
    | KW_VALUE IDENT ':' type
    ;

extern_param_list
    : extern_param
    | extern_param_list ',' extern_param
    ;

extern_param
    : IDENT ':' type KW_IN reg_name
    ;

/* ==== Statements ==== */

stmt_list
    : /* empty */
    | stmt_list stmt
    ;

stmt
    : var_decl ';'
    | assignment ';'
    | expr ';'
    | if_stmt
    | while_stmt
    | for_stmt
    | KW_RETURN expr ';'
    | KW_RETURN ';'
    | KW_BREAK ';'
    | KW_CONTINUE ';'
    | asm_block
    ;

/* ---- Variable declarations ---- */

var_decl
    : type IDENT '=' expr
    | type IDENT
    | type reg_name '=' expr
    | type reg_name
    | TY_SEG seg_reg '=' expr
    | TY_SEG seg_reg
    ;

/* ---- Assignment ---- */

assignment
    : expr OP_ASSIGN expr
    | expr OP_TOGGLEASSIGN expr
    ;

/* ---- Control flow ---- */

if_stmt
    : KW_IF '(' expr ')' '{' stmt_list '}' %prec LOWER_THAN_ELSE
    | KW_IF '(' expr ')' '{' stmt_list '}' KW_ELSE '{' stmt_list '}'
    | KW_IF '(' expr ')' '{' stmt_list '}' KW_ELSE if_stmt
    ;

while_stmt
    : KW_WHILE '(' expr ')' '{' stmt_list '}'
    ;

for_stmt
    : KW_FOR '(' REG_CX KW_IN expr OP_DOTDOT LIT_INT ')' '{' stmt_list '}'
    ;

/* ---- ASM blocks ---- */

asm_block
    : KW_ASM asm_annotation '{' ASM_BODY
    | KW_ASM '{' ASM_BODY
    ;

asm_annotation
    : KW_CLOBBERS '(' reg_flag_list ')'
    | KW_PRESERVES '(' reg_flag_list ')'
    ;

/* ==== Expressions ==== */

expr
    : expr OP_XCHG expr                        /* exchange */
    | expr '|' expr                             /* bitwise or */
    | expr '^' expr                             /* bitwise xor */
    | expr '&' expr                             /* bitwise and */
    | expr OP_EQ expr                           /* equality */
    | expr OP_NEQ expr
    | expr '<' expr                             /* comparison unsigned */
    | expr '>' expr
    | expr OP_LTE expr
    | expr OP_GTE expr
    | expr OP_SLT expr                          /* comparison signed */
    | expr OP_SGT expr
    | expr OP_SLTE expr
    | expr OP_SGTE expr
    | expr OP_SHL expr                          /* shift */
    | expr OP_SHR expr
    | expr OP_SRSHR expr                        /* signed shift */
    | expr OP_ROL expr                          /* rotate */
    | expr OP_ROR expr
    | expr OP_RCL expr                          /* rotate through carry */
    | expr OP_RCR expr
    | expr '+' expr                             /* additive */
    | expr '-' expr
    | expr '*' expr                             /* multiplicative */
    | expr '/' expr
    | expr '%' expr
    | expr OP_SMUL expr                         /* signed multiplicative */
    | expr OP_SDIV expr
    | expr OP_SMOD expr
    | '-' expr %prec UNARY                      /* unary */
    | '~' expr %prec UNARY
    | '&' expr %prec UNARY
    | '!' expr %prec UNARY
    | postfix_expr
    ;

postfix_expr
    : postfix_expr '.' IDENT                    /* field access */
    | postfix_expr '[' expr ']'                 /* array index */
    | postfix_expr '!' '[' expr ']'              /* checked array index */
    | postfix_expr '(' arg_list ')'             /* function call */
    | postfix_expr '(' ')'                      /* function call no args */
    | primary_expr
    ;

primary_expr
    : LIT_INT
    | LIT_STRING
    | IDENT
    | reg_name
    | seg_reg
    | flag
    | '(' expr ')'
    | mem_access
    ;

/* ---- Memory access [seg:expr] or [expr] ---- */

mem_access
    : '[' mem_inner ']'
    | '[' seg_reg ':' mem_inner ']'
    | '[' LIT_INT ':' LIT_INT ']'
    ;

mem_inner
    : mem_base
    | mem_base '+' mem_base
    | mem_base '+' LIT_INT
    | mem_base '+' mem_base '+' LIT_INT
    | LIT_INT
    ;

mem_base
    : REG_BX | REG_SI | REG_DI | REG_BP
    ;

/* ---- Function call arguments ---- */

arg_list
    : expr
    | arg_list ',' expr
    ;

/* ==== Register and flag name groups ==== */

reg_name
    : word_reg
    | byte_reg
    ;

word_reg
    : REG_AX | REG_BX | REG_CX | REG_DX
    | REG_SI | REG_DI | REG_BP | REG_SP
    ;

byte_reg
    : REG_AL | REG_AH | REG_BL | REG_BH
    | REG_CL | REG_CH | REG_DL | REG_DH
    ;

seg_reg
    : REG_DS | REG_ES | REG_SS | REG_CS
    ;

flag
    : FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF
    | FLAG_SF | FLAG_TF | FLAG_DF | FLAG_OF
    | FLAG_IF
    ;

reg_flag_list
    : reg_or_flag
    | reg_flag_list ',' reg_or_flag
    ;

reg_or_flag
    : reg_name
    | seg_reg
    | flag
    | FLAG_ALL
    ;

/* ==== Types ==== */

type
    : TY_U8
    | TY_U16
    | TY_U32
    | TY_SEG
    | TY_BOOL
    | TY_U8 '[' LIT_INT ']'
    | TY_U16 '[' LIT_INT ']'
    | TY_BCD '[' LIT_INT ']'
    | IDENT
    ;

%%

int main(int argc, char **argv) {
    if (argc > 1) {
        yyin = fopen(argv[1], "r");
        if (!yyin) {
            perror(argv[1]);
            return 1;
        }
    }
    int result = yyparse();
    if (result == 0)
        printf("Parse OK\n");
    return result;
}
