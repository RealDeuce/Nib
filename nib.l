%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ast.h"
#include "nib.tab.h"

/* Track brace depth for asm blocks */
static int asm_brace_depth = 0;
static char *asm_buf = NULL;
static size_t asm_buf_len = 0;
static size_t asm_buf_cap = 0;

static void asm_buf_init(void) {
    asm_buf_cap = 256;
    asm_buf_len = 0;
    asm_buf = (char *)malloc(asm_buf_cap);
    asm_buf[0] = '\0';
}

static void asm_buf_append(const char *text) {
    size_t tlen = strlen(text);
    while (asm_buf_len + tlen + 1 > asm_buf_cap) {
        asm_buf_cap *= 2;
        asm_buf = (char *)realloc(asm_buf, asm_buf_cap);
    }
    memcpy(asm_buf + asm_buf_len, text, tlen);
    asm_buf_len += tlen;
    asm_buf[asm_buf_len] = '\0';
}

int yyline = 1;
static int asm_pending = 0;     /* set after KW_ASM or clobbers/preserves clause */
%}

%option noyywrap noinput nounput

%x ASM

%%

 /* ---- Whitespace and comments ---- */
[ \t\r]+                { /* skip */ }
\n                      { yyline++; }
"//"[^\n]*              { /* line comment */ }
"/*"([^*]|\*+[^*/])*\*+"/"  { /* block comment — count newlines */
    for (char *p = yytext; *p; p++)
        if (*p == '\n') yyline++;
}

 /* ---- Keywords ---- */
"fn"                    { return KW_FN; }
"struct"                { return KW_STRUCT; }
"aligned"               { return KW_ALIGNED; }
"extern"                { return KW_EXTERN; }
"far"                   { return KW_FAR; }
"interrupt"             { return KW_INTERRUPT; }
"chain"                 { return KW_CHAIN; }
"reentrant"             { return KW_REENTRANT; }
"if"                    { return KW_IF; }
"else"                  { return KW_ELSE; }
"while"                 { return KW_WHILE; }
"for"                   { return KW_FOR; }
"in"                    { return KW_IN; }
"return"                { return KW_RETURN; }
"break"                 { return KW_BREAK; }
"continue"              { return KW_CONTINUE; }
"value"                 { return KW_VALUE; }
"use"                   { return KW_USE; }
"preserves"             { return KW_PRESERVES; }
"clobbers"              { return KW_CLOBBERS; }
"bits"                  { return KW_BITS; }
"as"                    { return KW_AS; }
"trap"                  { return KW_TRAP; }
"goto"                  { return KW_GOTO; }
"at"                    { return KW_AT; }
"tailcall"              { return KW_TAILCALL; }
"const"                 { return KW_CONST; }
"pub"                   { return KW_PUB; }

 /* ---- Type keywords ---- */
"u8"                    { return TY_U8; }
"u16"                   { return TY_U16; }
"u32"                   { return TY_U32; }
"seg"                   { return TY_SEG; }
"bcd"                   { return TY_BCD; }
"bool"                  { return TY_BOOL; }

 /* ---- ASM block entry ---- */
"asm"                   { asm_pending = 1; return KW_ASM; }

 /* ---- Predefined flag variables (uppercase, checked before registers) ---- */
"CF"                    { return FLAG_CF; }
"PF"                    { return FLAG_PF; }
"AF"                    { return FLAG_AF; }
"ZF"                    { return FLAG_ZF; }
"SF"                    { return FLAG_SF; }
"TF"                    { return FLAG_TF; }
"DF"                    { return FLAG_DF; }
"OF"                    { return FLAG_OF; }
"IF"                    { return FLAG_IF; }
"FLAGS"                 { return FLAG_ALL; }

 /* ---- Predefined register variables (uppercase) ---- */
 /* Word registers */
"AX"                    { return REG_AX; }
"BX"                    { return REG_BX; }
"CX"                    { return REG_CX; }
"DX"                    { return REG_DX; }
"SI"                    { return REG_SI; }
"DI"                    { return REG_DI; }
"BP"                    { return REG_BP; }
"SP"                    { return REG_SP; }
 /* Byte registers */
"AL"                    { return REG_AL; }
"AH"                    { return REG_AH; }
"BL"                    { return REG_BL; }
"BH"                    { return REG_BH; }
"CL"                    { return REG_CL; }
"CH"                    { return REG_CH; }
"DL"                    { return REG_DL; }
"DH"                    { return REG_DH; }
 /* Segment registers */
"DS"                    { return REG_DS; }
"ES"                    { return REG_ES; }
"SS"                    { return REG_SS; }
"CS"                    { return REG_CS; }

 /* ---- Signed operators ($-prefix, longest match first) ---- */
"$>="                   { return OP_SGTE; }
"$<="                   { return OP_SLTE; }
"$>>"                   { return OP_SRSHR; }
"$>"                    { return OP_SGT; }
"$<"                    { return OP_SLT; }
"$*"                    { return OP_SMUL; }
"$/"                    { return OP_SDIV; }
"$%"                    { return OP_SMOD; }

 /* ---- Multi-char operators (longest match first) ---- */
"<<<."                  { return OP_RCL; }
">>>."                  { return OP_RCR; }
"<<<"                   { return OP_ROL; }
">>>"                   { return OP_ROR; }
"<=>"                   { return OP_XCHG; }
"<<"                    { return OP_SHL; }
">>"                    { return OP_SHR; }
"<="                    { return OP_LTE; }
">="                    { return OP_GTE; }
"=="                    { return OP_EQ; }
"!="                    { return OP_NEQ; }
":="                    { return OP_ASSIGN; }
"~="                    { return OP_TOGGLEASSIGN; }
"->"                    { return OP_ARROW; }
".."                    { return OP_DOTDOT; }

 /* ---- Single-char operators and punctuation ---- */
"+"                     { return '+'; }
"-"                     { return '-'; }
"*"                     { return '*'; }
"/"                     { return '/'; }
"%"                     { return '%'; }
"&"                     { return '&'; }
"|"                     { return '|'; }
"^"                     { return '^'; }
"~"                     { return '~'; }
"<"                     { return '<'; }
">"                     { return '>'; }
"="                     { return '='; }
"!"                     { return '!'; }
"("                     { return '('; }
")"                     { return ')'; }
"{"                     {
    if (asm_pending) {
        asm_pending = 0;
        asm_buf_init();
        asm_brace_depth = 0;
        BEGIN(ASM);
        return '{';
    }
    return '{';
}
"}"                     { return '}'; }
"["                     { return '['; }
"]"                     { return ']'; }
":"                     { return ':'; }
";"                     { return ';'; }
","                     { return ','; }
"."                     { return '.'; }
"`"                     { return '`'; }
"_"                     { return '_'; }

 /* ---- Literals ---- */
0[xX][0-9a-fA-F]+       { yylval.ival = (int)strtol(yytext, NULL, 16); return LIT_INT; }
[0-9]+                  { yylval.ival = atoi(yytext); return LIT_INT; }
\"([^"\\]|\\.)*\"       {
    /* Strip surrounding quotes */
    int len = strlen(yytext);
    yylval.sval = strndup(yytext + 1, len - 2);
    return LIT_STRING;
}

 /* ---- Identifiers ---- */
[a-zA-Z_][a-zA-Z0-9_]*  { yylval.sval = strdup(yytext); return IDENT; }

 /* ---- Catch-all ---- */
.                       { fprintf(stderr, "line %d: unexpected character '%c'\n", yyline, yytext[0]); }

 /* ---- ASM state: collect everything until matching closing brace ---- */
<ASM>"{"                { asm_brace_depth++; asm_buf_append(yytext); }
<ASM>"}"                {
    if (asm_brace_depth == 0) {
        yylval.sval = strdup(asm_buf);
        free(asm_buf);
        asm_buf = NULL;
        BEGIN(INITIAL);
        return ASM_BODY;
    }
    asm_brace_depth--;
    asm_buf_append(yytext);
}
<ASM>\n                 { yyline++; asm_buf_append(yytext); }
<ASM>.                  { asm_buf_append(yytext); }

%%

void asm_enter(void) {
    asm_buf_init();
    asm_brace_depth = 0;
    BEGIN(ASM);
}
