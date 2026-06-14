/*
 * nib asm — V20 cross-assembler
 *
 * Two-pass, Intel syntax, flat binary output.
 * Supports full 8086/80186 instruction set plus V20 extensions:
 *   bext/bins (bit field extract/insert)
 *   test1/set1/clr1/not1 (single bit operations)
 *   add4s/sub4s/cmp4s (BCD string operations)
 *   rol4/ror4 (nibble rotate)
 *   brkem (break to 8080 emulation)
 *
 * Directives: ORG, SEG, DB, DW, EQU
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define MAX_LABELS   4096
#define MAX_LINE     1024
#define MAX_OUTPUT   1048576  /* 1MB — full V20 address space */
#define MAX_FIXUPS   4096
#define MAX_DBG      8192

/* Source-level debug tracking (for error messages and .dbg output) */
static char pending_dbg_file[64];  /* from last ; @ comment */
static int  pending_dbg_line = 0;

/* ---- Error handling ---- */

static const char *current_file = "<stdin>";
static int current_line = 0;

static void err(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", current_file, current_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (pending_dbg_file[0] && pending_dbg_line > 0)
        fprintf(stderr, " [%s:%d]", pending_dbg_file, pending_dbg_line);
    fprintf(stderr, "\n");
}

static void fatal(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: fatal: ", current_file, current_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (pending_dbg_file[0] && pending_dbg_line > 0)
        fprintf(stderr, " [%s:%d]", pending_dbg_file, pending_dbg_line);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- Output buffer ---- */

static uint8_t *output;
static bool    *written;
static int out_pos = 0;
static int org_base = 0;
static int seg_base = 0; /* current segment for SEG operator */

/* org stack for at()/end at; */
#define MAX_ORG_STACK 16
static struct { int org_base; int out_pos; int seg_base; } org_stack[MAX_ORG_STACK];
static int org_sp = 0;
static int pass = 1;     /* 1 or 2 */
static int errors = 0;
/* Jcc relaxation: track which source lines need 5-byte relaxed form */
#define MAX_RELAXED 256
static int relaxed_lines[MAX_RELAXED];
static int nrelaxed = 0;
static bool jcc_relaxed = false;

static bool is_relaxed_line(int line) {
    for (int i = 0; i < nrelaxed; i++)
        if (relaxed_lines[i] == line) return true;
    return false;
}

static void mark_relaxed_line(int line) {
    if (is_relaxed_line(line)) return;
    if (nrelaxed < MAX_RELAXED)
        relaxed_lines[nrelaxed++] = line;
    jcc_relaxed = true;
}

static void emit(uint8_t b) {
    int addr = org_base + out_pos;
    if (pass == 2) {
        if (addr >= MAX_OUTPUT)
            fatal("output exceeds 1MB address space");
        output[addr] = b;
        written[addr] = true;
    }
    out_pos++;
}

static void emit16(uint16_t w) {
    emit(w & 0xFF);
    emit((w >> 8) & 0xFF);
}

/* ---- Labels / symbol table ---- */

typedef enum { LBL_CODE, LBL_DATA, LBL_EQU } label_type_t;

typedef struct {
    char name[64];
    int  value;
    bool defined;
    label_type_t ltype;
    int  segment;       /* segment value when label was defined */
} label_t;

static label_t labels[MAX_LABELS];
static int nlabels = 0;

/* Debug info entries */
typedef struct {
    int  addr;          /* linear address */
    char file[64];      /* source filename */
    int  line;          /* source line number */
} dbg_entry_t;

static dbg_entry_t dbg_entries[MAX_DBG];
static int ndbg_entries = 0;

static label_t *find_label(const char *name) {
    for (int i = 0; i < nlabels; i++)
        if (strcasecmp(labels[i].name, name) == 0)
            return &labels[i];
    return NULL;
}

static label_t *add_label_typed(const char *name, int value, label_type_t ltype);

static label_t *add_label(const char *name, int value) {
    return add_label_typed(name, value, LBL_CODE);
}

static label_t *add_label_typed(const char *name, int value, label_type_t ltype) {
    label_t *l = find_label(name);
    if (l) {
        if (l->defined && pass == 1 && nrelaxed == 0) {
            err("duplicate label '%s'", name);
            errors++;
            return l;
        }
        l->value = value;
        l->defined = true;
        l->ltype = ltype;
        l->segment = seg_base;
        return l;
    }
    if (nlabels >= MAX_LABELS)
        fatal("too many labels");
    l = &labels[nlabels++];
    strncpy(l->name, name, 63);
    l->name[63] = '\0';
    l->value = value;
    l->defined = true;
    l->ltype = ltype;
    l->segment = seg_base;
    return l;
}

static int lookup_label(const char *name) {
    label_t *l = find_label(name);
    if (!l || !l->defined) {
        if (pass == 2) {
            err("undefined label '%s'", name);
            errors++;
        }
        return 0;
    }
    return l->value;
}

/* ---- Tokenizer ---- */

typedef enum {
    T_EOF, T_EOL, T_IDENT, T_NUMBER, T_STRING,
    T_COMMA, T_COLON, T_LBRAK, T_RBRAK, T_PLUS, T_MINUS, T_STAR,
    T_LPAREN, T_RPAREN
} toktype_t;

typedef struct {
    toktype_t type;
    char      sval[128];
    int       ival;
} token_t;

static const char *line_ptr;

static void skip_ws(void) {
    while (*line_ptr == ' ' || *line_ptr == '\t')
        line_ptr++;
}

static token_t next_token(void) {
    token_t t = {0};
    skip_ws();

    if (*line_ptr == '\0' || *line_ptr == '\n' || *line_ptr == ';') {
        t.type = T_EOL;
        return t;
    }

    char c = *line_ptr;

    if (c == ',') { t.type = T_COMMA; line_ptr++; return t; }
    if (c == ':') { t.type = T_COLON; line_ptr++; return t; }
    if (c == '[') { t.type = T_LBRAK; line_ptr++; return t; }
    if (c == ']') { t.type = T_RBRAK; line_ptr++; return t; }
    if (c == '+') { t.type = T_PLUS; line_ptr++; return t; }
    if (c == '-') { t.type = T_MINUS; line_ptr++; return t; }
    if (c == '*') { t.type = T_STAR; line_ptr++; return t; }
    if (c == '(') { t.type = T_LPAREN; line_ptr++; return t; }
    if (c == ')') { t.type = T_RPAREN; line_ptr++; return t; }

    /* String or character literal */
    if (c == '\'' || c == '"') {
        char delim = c;
        line_ptr++;
        int i = 0;
        while (*line_ptr && *line_ptr != delim && i < 126) {
            if (*line_ptr == '\\' && line_ptr[1]) {
                line_ptr++;
                switch (*line_ptr) {
                case 'n': t.sval[i++] = '\n'; break;
                case 'r': t.sval[i++] = '\r'; break;
                case 't': t.sval[i++] = '\t'; break;
                case '0': t.sval[i++] = '\0'; break;
                case '\\': t.sval[i++] = '\\'; break;
                default: t.sval[i++] = *line_ptr; break;
                }
            } else {
                t.sval[i++] = *line_ptr;
            }
            line_ptr++;
        }
        t.sval[i] = '\0';
        if (*line_ptr == delim) line_ptr++;
        /* Single character in quotes -> treat as number */
        if (i == 1 && delim == '\'') {
            t.type = T_NUMBER;
            t.ival = (uint8_t)t.sval[0];
            return t;
        }
        t.type = T_STRING;
        return t;
    }

    /* Number: hex (0x..), decimal, or trailing 'h' */
    if (isdigit(c)) {
        if (c == '0' && (line_ptr[1] == 'x' || line_ptr[1] == 'X')) {
            line_ptr += 2;
            t.ival = (int)strtol(line_ptr, (char **)&line_ptr, 16);
        } else {
            /* Could be decimal or hex with trailing 'h' */
            const char *start = line_ptr;
            while (isxdigit(*line_ptr)) line_ptr++;
            if (*line_ptr == 'h' || *line_ptr == 'H') {
                t.ival = (int)strtol(start, NULL, 16);
                line_ptr++; /* skip 'h' */
            } else {
                t.ival = (int)strtol(start, NULL, 10);
            }
        }
        t.type = T_NUMBER;
        return t;
    }

    /* Identifier */
    if (isalpha(c) || c == '_' || c == '.') {
        int i = 0;
        while ((isalnum(*line_ptr) || *line_ptr == '_' || *line_ptr == '.') && i < 126) {
            t.sval[i++] = *line_ptr++;
        }
        t.sval[i] = '\0';

        /* Check for hex number with trailing h: like 0FFh */
        /* Already handled above since those start with digit */

        t.type = T_IDENT;
        return t;
    }

    err("unexpected character '%c'", c);
    errors++;
    line_ptr++;
    t.type = T_EOL;
    return t;
}

static token_t peek;
static bool has_peek = false;

static token_t peek_token(void) {
    if (!has_peek) {
        peek = next_token();
        has_peek = true;
    }
    return peek;
}

static token_t consume(void) {
    if (has_peek) {
        has_peek = false;
        return peek;
    }
    return next_token();
}

static void expect(toktype_t type, const char *what) {
    token_t t = consume();
    if (t.type != type) {
        err("expected %s", what);
        errors++;
    }
}

/* ---- Register encoding ---- */

/* Register IDs used for ModR/M encoding */
enum {
    R_AL=0, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH,  /* 8-bit */
    R_AX=0, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI,   /* 16-bit */
    R_ES=0, R_CS, R_SS, R_DS,                             /* segment */
    R_NONE = -1
};

typedef struct {
    const char *name;
    int id;
    int size;  /* 1=byte, 2=word, 4=segment */
} reginfo_t;

static const reginfo_t regs[] = {
    {"al", R_AL, 1}, {"cl", R_CL, 1}, {"dl", R_DL, 1}, {"bl", R_BL, 1},
    {"ah", R_AH, 1}, {"ch", R_CH, 1}, {"dh", R_DH, 1}, {"bh", R_BH, 1},
    {"ax", R_AX, 2}, {"cx", R_CX, 2}, {"dx", R_DX, 2}, {"bx", R_BX, 2},
    {"sp", R_SP, 2}, {"bp", R_BP, 2}, {"si", R_SI, 2}, {"di", R_DI, 2},
    {"es", R_ES, 4}, {"cs", R_CS, 4}, {"ss", R_SS, 4}, {"ds", R_DS, 4},
    {NULL, 0, 0}
};

static const reginfo_t *find_reg(const char *name) {
    for (const reginfo_t *r = regs; r->name; r++)
        if (strcasecmp(r->name, name) == 0)
            return r;
    return NULL;
}

/* ---- Operand parsing ---- */

typedef enum {
    OP_NONE, OP_REG, OP_SREG, OP_IMM, OP_MEM, OP_LABEL
} optype_t;

typedef struct {
    optype_t type;
    int      reg;       /* register id */
    int      size;      /* 1=byte, 2=word */
    int      disp;      /* displacement for memory */
    bool     has_disp;
    int      base;      /* base register for mem (R_BX, R_BP, R_NONE) */
    int      index;     /* index register for mem (R_SI, R_DI, R_NONE) */
    int      seg_ovr;   /* segment override (R_ES..R_DS or R_NONE) */
    int      imm;       /* immediate value */
    char     label[64]; /* label name for fixups */
    bool     is_far;    /* FAR prefix on jmp/call */
} operand_t;

/* Forward declare expression parser */
static int parse_expr(void);

static int parse_primary(void) {
    token_t t = peek_token();
    if (t.type == T_NUMBER) {
        consume();
        return t.ival;
    }
    if (t.type == T_IDENT) {
        consume();
        /* SEG operator: returns the segment a label was defined in */
        if (strcasecmp(t.sval, "SEG") == 0) {
            token_t lt = peek_token();
            if (lt.type == T_IDENT) {
                consume();
                label_t *l = find_label(lt.sval);
                if (!l || !l->defined) {
                    if (pass == 2) {
                        err("undefined label '%s'", lt.sval);
                        errors++;
                    }
                    return 0;
                }
                return l->segment;
            }
        }
        return lookup_label(t.sval);
    }
    if (t.type == T_LPAREN) {
        consume();
        int val = parse_expr();
        expect(T_RPAREN, "')'");
        return val;
    }
    err("expected number or label in expression");
    errors++;
    consume(); /* eat the bad token to avoid infinite loop */
    return 0;
}

static int parse_expr(void) {
    token_t t = peek_token();
    int sign = 1;
    if (t.type == T_MINUS) { consume(); sign = -1; }
    else if (t.type == T_PLUS) { consume(); }

    int val = sign * parse_primary();

    for (;;) {
        t = peek_token();
        if (t.type == T_PLUS) {
            consume();
            val += parse_primary();
        } else if (t.type == T_MINUS) {
            consume();
            val -= parse_primary();
        } else if (t.type == T_STAR) {
            consume();
            val *= parse_primary();
        } else {
            break;
        }
    }
    return val;
}

/* Size override keywords */
static int check_size_prefix(void) {
    token_t t = peek_token();
    if (t.type == T_IDENT) {
        if (strcasecmp(t.sval, "byte") == 0) { consume(); return 1; }
        if (strcasecmp(t.sval, "word") == 0) { consume(); return 2; }
    }
    return 0;
}

static operand_t parse_operand(void) {
    operand_t op = {0};
    op.base = R_NONE;
    op.index = R_NONE;
    op.seg_ovr = R_NONE;

    /* Check for size prefix: BYTE or WORD */
    int size_pfx = check_size_prefix();

    /* Check for PTR keyword after size prefix */
    token_t t = peek_token();
    if (t.type == T_IDENT && strcasecmp(t.sval, "ptr") == 0)
        consume();

    /* Check for FAR keyword */
    t = peek_token();
    if (t.type == T_IDENT && strcasecmp(t.sval, "far") == 0) {
        consume();
        op.is_far = true;
    }

    t = peek_token();

    /* Memory operand: [....] */
    if (t.type == T_LBRAK) {
        consume();
        op.type = OP_MEM;
        op.size = size_pfx ? size_pfx : 0; /* 0 = infer from other operand */

        /* Check for segment override: CS:[...], DS:[...] etc */
        t = peek_token();
        if (t.type == T_IDENT) {
            const reginfo_t *r = find_reg(t.sval);
            if (r && r->size == 4) {
                /* peek ahead for colon */
                token_t saved = t;
                const char *saved_ptr = line_ptr;
                consume();
                t = peek_token();
                if (t.type == T_COLON) {
                    consume();
                    op.seg_ovr = r->id;
                } else {
                    /* not a seg override, push back */
                    has_peek = true;
                    peek = saved;
                    line_ptr = saved_ptr;
                    /* Actually we already consumed, this is tricky.
                       Let's re-parse: it must be a label or something */
                    /* Simpler: just error, segments must have colon */
                    err("expected ':' after segment register");
                    errors++;
                }
            }
        }

        /* Parse the memory expression: reg, reg+reg, reg+disp, etc */
        bool got_component = false;
        for (;;) {
            t = peek_token();
            if (t.type == T_RBRAK) {
                consume();
                break;
            }
            if (t.type == T_PLUS && got_component) {
                consume();
                continue;
            }
            if (t.type == T_MINUS && got_component) {
                consume();
                int val = parse_primary();
                op.disp -= val;
                op.has_disp = true;
                got_component = true;
                continue;
            }
            if (t.type == T_IDENT) {
                const reginfo_t *r = find_reg(t.sval);
                if (r && r->size == 2) {
                    consume();
                    if (r->id == R_BX || r->id == R_BP) {
                        if (op.base == R_NONE) op.base = r->id;
                        else if (op.index == R_NONE) op.index = r->id;
                        else { err("too many registers in memory operand"); errors++; }
                    } else if (r->id == R_SI || r->id == R_DI) {
                        if (op.index == R_NONE) op.index = r->id;
                        else if (op.base == R_NONE) op.base = r->id;
                        else { err("too many registers in memory operand"); errors++; }
                    } else {
                        err("invalid register in memory operand: %s", r->name);
                        errors++;
                    }
                    got_component = true;
                    continue;
                }
            }
            /* Must be a displacement expression */
            int val = parse_expr();
            op.disp += val;
            op.has_disp = true;
            got_component = true;
        }

        /* Direct memory if no registers */
        if (op.base == R_NONE && op.index == R_NONE) {
            op.has_disp = true; /* always 16-bit displacement */
        }

        return op;
    }

    /* Register operand */
    if (t.type == T_IDENT) {
        const reginfo_t *r = find_reg(t.sval);
        if (r) {
            consume();
            if (r->size == 4) {
                op.type = OP_SREG;
                op.reg = r->id;
                op.size = 2;
            } else {
                op.type = OP_REG;
                op.reg = r->id;
                op.size = r->size;
            }
            return op;
        }
    }

    /* Immediate / label */
    if (t.type == T_NUMBER) {
        consume();
        op.type = OP_IMM;
        op.imm = t.ival;
        op.size = size_pfx;
        return op;
    }

    if (t.type == T_IDENT) {
        consume();
        op.type = OP_IMM;
        op.imm = lookup_label(t.sval);
        strncpy(op.label, t.sval, 63);
        op.size = size_pfx ? size_pfx : 2; /* labels are 16-bit by default */
        return op;
    }

    err("expected operand");
    errors++;
    op.type = OP_NONE;
    return op;
}

/* ---- ModR/M encoding ---- */

/*
 * ModR/M byte: [mod:2][reg:3][rm:3]
 * mod=00: no displacement (except [disp16] when rm=110)
 * mod=01: 8-bit displacement
 * mod=10: 16-bit displacement
 * mod=11: register direct
 */

static int modrm_rm_table[8][2] = {
    /* rm=0: [BX+SI] */  {R_BX, R_SI},
    /* rm=1: [BX+DI] */  {R_BX, R_DI},
    /* rm=2: [BP+SI] */  {R_BP, R_SI},
    /* rm=3: [BP+DI] */  {R_BP, R_DI},
    /* rm=4: [SI]    */  {R_NONE, R_SI},
    /* rm=5: [DI]    */  {R_NONE, R_DI},
    /* rm=6: [BP]/d16 */ {R_BP, R_NONE},
    /* rm=7: [BX]    */  {R_BX, R_NONE},
};

static void emit_seg_override(operand_t *op) {
    if (op->seg_ovr != R_NONE) {
        static const uint8_t ovr[] = {0x26, 0x2E, 0x36, 0x3E};
        emit(ovr[op->seg_ovr]);
    }
}

static void emit_modrm_mem(operand_t *op, int reg_field) {
    int base = op->base;
    int idx = op->index;
    int disp = op->disp;
    bool has_d = op->has_disp;

    /* Direct address: no base, no index */
    if (base == R_NONE && idx == R_NONE) {
        emit(0x00 | (reg_field << 3) | 0x06); /* mod=00 rm=110 */
        emit16(disp);
        return;
    }

    /* Find matching rm encoding */
    int rm = -1;
    for (int i = 0; i < 8; i++) {
        int tb = modrm_rm_table[i][0];
        int ti = modrm_rm_table[i][1];
        if ((tb == base && ti == idx) || (tb == idx && ti == base)) {
            rm = i;
            break;
        }
    }

    if (rm < 0) {
        err("invalid memory operand register combination");
        errors++;
        emit(0);
        return;
    }

    /* Special case: [BP] with no displacement encodes as [BP+0] */
    if (rm == 6 && !has_d && base == R_BP && idx == R_NONE) {
        has_d = true;
        disp = 0;
    }

    int mod;
    if (!has_d || (disp == 0 && rm != 6)) {
        mod = 0;
    } else if (disp >= -128 && disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }

    emit((mod << 6) | (reg_field << 3) | rm);

    if (mod == 1) emit((uint8_t)(disp & 0xFF));
    else if (mod == 2) emit16(disp);
}

static void emit_modrm(operand_t *rm_op, int reg_field) {
    if (rm_op->type == OP_REG) {
        emit(0xC0 | (reg_field << 3) | rm_op->reg);
    } else if (rm_op->type == OP_MEM) {
        emit_seg_override(rm_op);
        emit_modrm_mem(rm_op, reg_field);
    } else {
        err("expected register or memory operand");
        errors++;
    }
}

/* ---- Instruction assembly ---- */

/*
 * ALU ops: ADD=0, OR=1, ADC=2, SBB=3, AND=4, SUB=5, XOR=6, CMP=7
 */
static void asm_alu(int aluop, operand_t *dst, operand_t *src) {
    int w = (dst->size == 2) ? 1 : 0;

    /* reg/mem, imm */
    if (src->type == OP_IMM) {
        if (dst->type == OP_REG && dst->reg == R_AX && w) {
            /* AX, imm16 — short form */
            emit(aluop * 8 + 5);
            emit16(src->imm);
        } else if (dst->type == OP_REG && dst->reg == R_AL && !w) {
            /* AL, imm8 — short form */
            emit(aluop * 8 + 4);
            emit(src->imm & 0xFF);
        } else {
            /* r/m, imm — check for sign-extend short form */
            bool short_imm = w && (src->imm >= -128 && src->imm <= 127);
            if (short_imm) {
                emit(0x83);
            } else {
                emit(0x80 | w);
            }
            emit_modrm(dst, aluop);
            if (short_imm) {
                emit(src->imm & 0xFF);
            } else if (w) {
                emit16(src->imm);
            } else {
                emit(src->imm & 0xFF);
            }
        }
        return;
    }

    /* reg, r/m */
    if (dst->type == OP_REG && (src->type == OP_REG || src->type == OP_MEM)) {
        int w2 = (dst->size == 2) ? 1 : 0;
        if (src->type == OP_MEM) emit_seg_override(src);
        emit(aluop * 8 + 2 + w2); /* direction=1: reg,r/m */
        if (src->type == OP_REG)
            emit(0xC0 | (dst->reg << 3) | src->reg);
        else
            emit_modrm_mem(src, dst->reg);
        return;
    }

    /* r/m, reg */
    if ((dst->type == OP_REG || dst->type == OP_MEM) && src->type == OP_REG) {
        int w2 = (src->size == 2) ? 1 : 0;
        if (dst->type == OP_MEM) emit_seg_override(dst);
        emit(aluop * 8 + w2); /* direction=0: r/m,reg */
        if (dst->type == OP_REG)
            emit(0xC0 | (src->reg << 3) | dst->reg);
        else
            emit_modrm_mem(dst, src->reg);
        return;
    }

    err("invalid operand combination for ALU operation");
    errors++;
}

static void asm_shift(int op, operand_t *dst, operand_t *src) {
    int w = (dst->size == 2) ? 1 : 0;

    if (src->type == OP_IMM && src->imm == 1) {
        emit(0xD0 | w);
        emit_modrm(dst, op);
    } else if (src->type == OP_REG && src->reg == R_CL && src->size == 1) {
        emit(0xD2 | w);
        emit_modrm(dst, op);
    } else if (src->type == OP_IMM) {
        /* 80186+ immediate shift */
        emit(0xC0 | w);
        emit_modrm(dst, op);
        emit(src->imm & 0xFF);
    } else {
        err("shift count must be 1, CL, or immediate");
        errors++;
    }
}

static void asm_inc_dec(int op, operand_t *dst) {
    int w = (dst->size == 2) ? 1 : 0;

    /* Word register short form */
    if (dst->type == OP_REG && w) {
        emit(0x40 + op * 8 + dst->reg);
        return;
    }

    /* r/m form */
    emit(0xFE | w);
    emit_modrm(dst, op); /* inc=0, dec=1 */
}

static void asm_push_pop(int is_pop, operand_t *op) {
    if (op->type == OP_REG && op->size == 2) {
        emit((is_pop ? 0x58 : 0x50) + op->reg);
        return;
    }
    if (op->type == OP_SREG) {
        int base = is_pop ? 0x07 : 0x06;
        emit(base + op->reg * 8);
        return;
    }
    if (op->type == OP_MEM || (op->type == OP_REG && op->size == 2)) {
        emit(0x8F | (!is_pop ? 0x70 : 0x00));
        emit_modrm(op, is_pop ? 0 : 6);
        return;
    }
    if (!is_pop && op->type == OP_IMM) {
        /* PUSH imm — 80186+ */
        if (op->imm >= -128 && op->imm <= 127) {
            emit(0x6A);
            emit(op->imm & 0xFF);
        } else {
            emit(0x68);
            emit16(op->imm);
        }
        return;
    }
    err("invalid operand for push/pop");
    errors++;
}

static void asm_mov(operand_t *dst, operand_t *src) {
    /* MOV sreg, r/m16 */
    if (dst->type == OP_SREG && (src->type == OP_REG || src->type == OP_MEM)) {
        emit(0x8E);
        emit_modrm(src, dst->reg);
        return;
    }
    /* MOV r/m16, sreg */
    if (src->type == OP_SREG && (dst->type == OP_REG || dst->type == OP_MEM)) {
        emit(0x8C);
        emit_modrm(dst, src->reg);
        return;
    }
    /* MOV reg, imm */
    if (dst->type == OP_REG && src->type == OP_IMM) {
        int w = (dst->size == 2) ? 1 : 0;
        emit(0xB0 + w * 8 + dst->reg);
        if (w) emit16(src->imm); else emit(src->imm & 0xFF);
        return;
    }
    /* MOV r/m, imm */
    if ((dst->type == OP_REG || dst->type == OP_MEM) && src->type == OP_IMM) {
        int w = (dst->size == 2) ? 1 : 0;
        emit(0xC6 | w);
        emit_modrm(dst, 0);
        if (w) emit16(src->imm); else emit(src->imm & 0xFF);
        return;
    }
    /* MOV reg, r/m */
    if (dst->type == OP_REG && (src->type == OP_REG || src->type == OP_MEM)) {
        int w = (dst->size == 2) ? 1 : 0;
        if (src->type == OP_MEM) emit_seg_override(src);
        emit(0x8A | w);
        if (src->type == OP_REG)
            emit(0xC0 | (dst->reg << 3) | src->reg);
        else
            emit_modrm_mem(src, dst->reg);
        return;
    }
    /* MOV r/m, reg */
    if ((dst->type == OP_REG || dst->type == OP_MEM) && src->type == OP_REG) {
        int w = (src->size == 2) ? 1 : 0;
        if (dst->type == OP_MEM) emit_seg_override(dst);
        emit(0x88 | w);
        if (dst->type == OP_REG)
            emit(0xC0 | (src->reg << 3) | dst->reg);
        else
            emit_modrm_mem(dst, src->reg);
        return;
    }
    /* MOV AL/AX, [addr] (direct) */
    if (dst->type == OP_REG && (dst->reg == R_AX || dst->reg == R_AL) &&
        src->type == OP_MEM && src->base == R_NONE && src->index == R_NONE) {
        int w = (dst->size == 2) ? 1 : 0;
        emit_seg_override(src);
        emit(0xA0 | w);
        emit16(src->disp);
        return;
    }
    /* MOV [addr], AL/AX (direct) */
    if (src->type == OP_REG && (src->reg == R_AX || src->reg == R_AL) &&
        dst->type == OP_MEM && dst->base == R_NONE && dst->index == R_NONE) {
        int w = (src->size == 2) ? 1 : 0;
        emit_seg_override(dst);
        emit(0xA2 | w);
        emit16(dst->disp);
        return;
    }

    err("invalid operand combination for MOV");
    errors++;
}

static void asm_xchg(operand_t *dst, operand_t *src) {
    /* XCHG AX, reg16 (short form) */
    if (dst->type == OP_REG && dst->reg == R_AX && dst->size == 2 &&
        src->type == OP_REG && src->size == 2) {
        emit(0x90 + src->reg);
        return;
    }
    if (src->type == OP_REG && src->reg == R_AX && src->size == 2 &&
        dst->type == OP_REG && dst->size == 2) {
        emit(0x90 + dst->reg);
        return;
    }
    /* XCHG r/m, reg */
    int w = (dst->size == 2 || src->size == 2) ? 1 : 0;
    emit(0x86 | w);
    if (dst->type == OP_REG && src->type == OP_REG) {
        emit(0xC0 | (dst->reg << 3) | src->reg);
    } else if (dst->type == OP_REG) {
        emit_modrm(src, dst->reg);
    } else {
        emit_modrm(dst, src->reg);
    }
}

static void asm_jmp_call(const char *mnemonic, operand_t *op) {
    bool is_call = (strcasecmp(mnemonic, "call") == 0);

    if (op->type == OP_IMM || op->label[0]) {
        if (op->is_far) {
            /* Far direct: JMP FAR seg:off or CALL FAR seg:off */
            label_t *l = find_label(op->label);
            int seg_val = (l && l->defined) ? l->segment : 0;
            emit(is_call ? 0x9A : 0xEA);
            emit16(op->imm & 0xFFFF);
            emit16(seg_val & 0xFFFF);
            return;
        }

        /* Near relative */
        int target = op->imm;
        int next_ip = (out_pos + org_base) + (is_call ? 3 : 2);

        if (!is_call) {
            /* JMP short or near */
            int rel = target - ((out_pos + org_base) + 2);
            if (rel >= -128 && rel <= 127) {
                emit(0xEB);
                emit(rel & 0xFF);
                return;
            }
        }
        /* Near */
        emit(is_call ? 0xE8 : 0xE9);
        int rel = target - next_ip;
        emit16(rel & 0xFFFF);
        return;
    }

    if (op->type == OP_REG || op->type == OP_MEM) {
        /* Indirect */
        emit(0xFF);
        emit_modrm(op, is_call ? 2 : 4);
        return;
    }

    err("invalid operand for %s", mnemonic);
    errors++;
}

static void asm_jcc(int cc) {
    /* Save label name before parse_operand consumes it */
    char jcc_label[64] = {0};
    const char *save = line_ptr;
    while (*save == ' ' || *save == '\t') save++;
    char *lbl = jcc_label;
    while (*save && !isspace(*save) && *save != ';' &&
           (size_t)(lbl - jcc_label) < sizeof(jcc_label) - 1)
        *lbl++ = *save++;
    *lbl = '\0';

    operand_t op = parse_operand();
    int target = op.imm;
    int rel = target - ((out_pos + org_base) + 2);

    /* Check if target label is actually defined (forward refs return 0) */
    label_t *tgt_label = find_label(jcc_label);
    bool label_known = (tgt_label && tgt_label->defined);

    if (is_relaxed_line(current_line)) {
        /* Previously marked for relaxation — always emit 5 bytes */
        emit(0x70 + (cc ^ 1));
        emit(3);
        int rel16 = target - ((out_pos + org_base) + 3);
        emit(0xE9);
        emit(rel16 & 0xFF);
        emit((rel16 >> 8) & 0xFF);
    } else if (!label_known || (rel >= -128 && rel <= 127)) {
        /* Forward ref (assume short) or known short — emit 2 bytes */
        emit(0x70 + cc);
        emit(rel & 0xFF);
    } else {
        /* Known label, out of range: mark for relaxation and emit 5 bytes */
        mark_relaxed_line(current_line);
        emit(0x70 + (cc ^ 1));
        emit(3);
        int rel16 = target - ((out_pos + org_base) + 3);
        emit(0xE9);
        emit(rel16 & 0xFF);
        emit((rel16 >> 8) & 0xFF);
    }
}

static void asm_loop(int opcode) {
    operand_t op = parse_operand();
    int target = op.imm;
    int rel = target - ((out_pos + org_base) + 2);
    if (rel < -128 || rel > 127) {
        err("LOOP target out of range");
        errors++;
    }
    emit(opcode);
    emit(rel & 0xFF);
}

static void asm_ret(const char *mnemonic) {
    bool is_far = (strcasecmp(mnemonic, "retf") == 0);
    token_t t = peek_token();
    if (t.type == T_NUMBER || t.type == T_IDENT) {
        operand_t op = parse_operand();
        emit(is_far ? 0xCA : 0xC2);
        emit16(op.imm);
    } else {
        emit(is_far ? 0xCB : 0xC3);
    }
}

static void asm_int(void) {
    operand_t op = parse_operand();
    if (op.imm == 3) {
        emit(0xCC);
    } else {
        emit(0xCD);
        emit(op.imm & 0xFF);
    }
}

static void asm_in_out(const char *mnemonic) {
    bool is_out = (strcasecmp(mnemonic, "out") == 0);
    operand_t dst = parse_operand();
    expect(T_COMMA, "','");
    operand_t src = parse_operand();

    if (is_out) {
        /* OUT port, AL/AX */
        int w = (src.size == 2) ? 1 : 0;
        if (dst.type == OP_IMM) {
            emit(0xE6 | w);
            emit(dst.imm & 0xFF);
        } else if (dst.type == OP_REG && dst.reg == R_DX) {
            emit(0xEE | w);
        } else {
            err("OUT: port must be immediate or DX");
            errors++;
        }
    } else {
        /* IN AL/AX, port */
        int w = (dst.size == 2) ? 1 : 0;
        if (src.type == OP_IMM) {
            emit(0xE4 | w);
            emit(src.imm & 0xFF);
        } else if (src.type == OP_REG && src.reg == R_DX) {
            emit(0xEC | w);
        } else {
            err("IN: port must be immediate or DX");
            errors++;
        }
    }
}

static void asm_test(operand_t *dst, operand_t *src) {
    int w = (dst->size == 2) ? 1 : 0;

    if (src->type == OP_IMM) {
        if (dst->type == OP_REG && dst->reg == R_AX && w) {
            emit(0xA9);
            emit16(src->imm);
        } else if (dst->type == OP_REG && dst->reg == R_AL && !w) {
            emit(0xA8);
            emit(src->imm & 0xFF);
        } else {
            emit(0xF6 | w);
            emit_modrm(dst, 0);
            if (w) emit16(src->imm); else emit(src->imm & 0xFF);
        }
        return;
    }

    if (src->type == OP_REG) {
        emit(0x84 | w);
        emit_modrm(dst, src->reg);
        return;
    }

    err("invalid operand for TEST");
    errors++;
}

static void asm_lea(operand_t *dst, operand_t *src) {
    if (dst->type != OP_REG || dst->size != 2) {
        err("LEA destination must be 16-bit register");
        errors++;
        return;
    }
    if (src->type != OP_MEM) {
        err("LEA source must be memory operand");
        errors++;
        return;
    }
    emit(0x8D);
    emit_modrm(src, dst->reg);
}

/* ---- V20 extension instructions ---- */

static void asm_v20_bitop(const char *mnemonic) {
    /* TEST1/CLR1/SET1/NOT1 r/m, CL or imm */
    int op;
    if (strcasecmp(mnemonic, "test1") == 0) op = 0;
    else if (strcasecmp(mnemonic, "clr1") == 0) op = 1;
    else if (strcasecmp(mnemonic, "set1") == 0) op = 2;
    else if (strcasecmp(mnemonic, "not1") == 0) op = 3;
    else { err("unknown bit op"); errors++; return; }

    operand_t dst = parse_operand();
    expect(T_COMMA, "','");
    operand_t src = parse_operand();

    int w = (dst.size == 2) ? 1 : 0;

    if (src.type == OP_REG && src.reg == R_CL && src.size == 1) {
        /* r/m, CL */
        emit(0x0F);
        emit(0x10 + op * 2 + w);
        emit_modrm(&dst, 0);
    } else if (src.type == OP_IMM) {
        /* r/m, imm */
        emit(0x0F);
        emit(0x18 + op * 2 + w);
        emit_modrm(&dst, 0);
        emit(src.imm & 0xFF);
    } else {
        err("bit operation source must be CL or immediate");
        errors++;
    }
}

static void asm_v20_bcd_string(const char *mnemonic) {
    /* ADD4S/SUB4S/CMP4S — no operands, uses CL for length, DS:SI/ES:DI */
    emit(0x0F);
    if (strcasecmp(mnemonic, "add4s") == 0) emit(0x20);
    else if (strcasecmp(mnemonic, "sub4s") == 0) emit(0x22);
    else if (strcasecmp(mnemonic, "cmp4s") == 0) emit(0x26);
    else { err("unknown BCD string op"); errors++; }
}

static void asm_v20_nibble(const char *mnemonic) {
    /* ROL4/ROR4 — no operands, uses DS:SI and AL */
    emit(0x0F);
    if (strcasecmp(mnemonic, "rol4") == 0) emit(0x28);
    else if (strcasecmp(mnemonic, "ror4") == 0) emit(0x2A);
    else { err("unknown nibble op"); errors++; }
}

static void asm_v20_bext_bins(const char *mnemonic) {
    /* BEXT (EXT) / BINS (INS) — register form */
    /* Uses ModRM to specify registers for bit offset/length */
    emit(0x0F);
    if (strcasecmp(mnemonic, "bext") == 0) emit(0x33);
    else if (strcasecmp(mnemonic, "bins") == 0) emit(0x31);
    else { err("unknown bit field op"); errors++; return; }

    /* Parse operand: register pair or immediate form */
    token_t t = peek_token();
    if (t.type == T_EOL) {
        /* No operands — use register form with defaults */
        emit(0xC0); /* ModRM for default regs */
        return;
    }

    operand_t op = parse_operand();
    if (op.type == OP_IMM) {
        /* Immediate form: 0x0F 0x3B/0x39 */
        /* Re-emit with correct opcode */
        out_pos -= 2;
        emit(0x0F);
        if (strcasecmp(mnemonic, "bext") == 0) emit(0x3B);
        else emit(0x39);
        emit_modrm(&op, 0);
        expect(T_COMMA, "','");
        operand_t imm = parse_operand();
        emit(imm.imm & 0xFF);
    } else {
        /* Register form — operand specifies the ModRM */
        emit_modrm(&op, 0);
    }
}

static void asm_brkem(void) {
    emit(0x0F);
    emit(0xFF);
    operand_t op = parse_operand();
    emit(op.imm & 0xFF);
}

/* ---- Prefix instructions ---- */

static bool check_prefix(const char *mnemonic) {
    if (strcasecmp(mnemonic, "rep") == 0 || strcasecmp(mnemonic, "repe") == 0 ||
        strcasecmp(mnemonic, "repz") == 0) {
        emit(0xF3);
        return true;
    }
    if (strcasecmp(mnemonic, "repne") == 0 || strcasecmp(mnemonic, "repnz") == 0) {
        emit(0xF2);
        return true;
    }
    if (strcasecmp(mnemonic, "lock") == 0) {
        emit(0xF0);
        return true;
    }
    return false;
}

/* ---- Main instruction dispatch ---- */

static void assemble_instruction(const char *mnemonic) {
    /* --- Prefixes --- */
    if (check_prefix(mnemonic)) {
        token_t t = peek_token();
        if (t.type == T_IDENT) {
            consume();
            assemble_instruction(t.sval);
        }
        return;
    }

    /* --- No-operand instructions --- */
    struct { const char *name; uint8_t op; } simple[] = {
        {"nop",   0x90}, {"cbw",   0x98}, {"cwd",   0x99},
        {"pushf", 0x9C}, {"popf",  0x9D}, {"sahf",  0x9E},
        {"lahf",  0x9F}, {"movsb", 0xA4}, {"movsw", 0xA5},
        {"cmpsb", 0xA6}, {"cmpsw", 0xA7}, {"stosb", 0xAA},
        {"stosw", 0xAB}, {"lodsb", 0xAC}, {"lodsw", 0xAD},
        {"scasb", 0xAE}, {"scasw", 0xAF}, {"xlat",  0xD7},
        {"aaa",   0x37}, {"aas",   0x3F}, {"daa",   0x27},
        {"das",   0x2F}, {"hlt",   0xF4}, {"cmc",   0xF5},
        {"clc",   0xF8}, {"stc",   0xF9}, {"cli",   0xFA},
        {"sti",   0xFB}, {"cld",   0xFC}, {"std",   0xFD},
        {"into",  0xCE}, {"iret",  0xCF}, {"pusha", 0x60},
        {"popa",  0x61}, {"leave", 0xC9}, {"salc",  0xD6},
        {"insb",  0x6C}, {"insw",  0x6D}, {"outsb", 0x6E},
        {"outsw", 0x6F},
        {NULL, 0}
    };

    for (int i = 0; simple[i].name; i++) {
        if (strcasecmp(mnemonic, simple[i].name) == 0) {
            emit(simple[i].op);
            return;
        }
    }

    /* --- AAM/AAD with optional base --- */
    if (strcasecmp(mnemonic, "aam") == 0) {
        emit(0xD4);
        token_t t = peek_token();
        if (t.type == T_NUMBER) { consume(); emit(t.ival & 0xFF); }
        else emit(0x0A);
        return;
    }
    if (strcasecmp(mnemonic, "aad") == 0) {
        emit(0xD5);
        token_t t = peek_token();
        if (t.type == T_NUMBER) { consume(); emit(t.ival & 0xFF); }
        else emit(0x0A);
        return;
    }

    /* --- ALU ops --- */
    struct { const char *name; int op; } alu[] = {
        {"add", 0}, {"or",  1}, {"adc", 2}, {"sbb", 3},
        {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7},
        {NULL, 0}
    };
    for (int i = 0; alu[i].name; i++) {
        if (strcasecmp(mnemonic, alu[i].name) == 0) {
            operand_t dst = parse_operand();
            expect(T_COMMA, "','");
            operand_t src = parse_operand();
            asm_alu(alu[i].op, &dst, &src);
            return;
        }
    }

    /* --- Shift/rotate ops --- */
    struct { const char *name; int op; } shifts[] = {
        {"rol", 0}, {"ror", 1}, {"rcl", 2}, {"rcr", 3},
        {"shl", 4}, {"sal", 4}, {"shr", 5}, {"sar", 7},
        {NULL, 0}
    };
    for (int i = 0; shifts[i].name; i++) {
        if (strcasecmp(mnemonic, shifts[i].name) == 0) {
            operand_t dst = parse_operand();
            expect(T_COMMA, "','");
            operand_t src = parse_operand();
            asm_shift(shifts[i].op, &dst, &src);
            return;
        }
    }

    /* --- MOV --- */
    if (strcasecmp(mnemonic, "mov") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        /* Infer sizes */
        if (dst.size && !src.size) src.size = dst.size;
        if (src.size && !dst.size) dst.size = src.size;
        if (!dst.size && !src.size) dst.size = src.size = 2;
        asm_mov(&dst, &src);
        return;
    }

    /* --- XCHG --- */
    if (strcasecmp(mnemonic, "xchg") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        asm_xchg(&dst, &src);
        return;
    }

    /* --- TEST --- */
    if (strcasecmp(mnemonic, "test") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        asm_test(&dst, &src);
        return;
    }

    /* --- LEA --- */
    if (strcasecmp(mnemonic, "lea") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        asm_lea(&dst, &src);
        return;
    }

    /* --- LDS/LES --- */
    if (strcasecmp(mnemonic, "lds") == 0 || strcasecmp(mnemonic, "les") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        emit(strcasecmp(mnemonic, "les") == 0 ? 0xC4 : 0xC5);
        emit_modrm(&src, dst.reg);
        return;
    }

    /* --- INC/DEC --- */
    if (strcasecmp(mnemonic, "inc") == 0) {
        operand_t dst = parse_operand();
        asm_inc_dec(0, &dst);
        return;
    }
    if (strcasecmp(mnemonic, "dec") == 0) {
        operand_t dst = parse_operand();
        asm_inc_dec(1, &dst);
        return;
    }

    /* --- NOT/NEG --- */
    if (strcasecmp(mnemonic, "not") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 2);
        return;
    }
    if (strcasecmp(mnemonic, "neg") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 3);
        return;
    }

    /* --- MUL/IMUL/DIV/IDIV --- */
    if (strcasecmp(mnemonic, "mul") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 4);
        return;
    }
    if (strcasecmp(mnemonic, "imul") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 5);
        return;
    }
    if (strcasecmp(mnemonic, "div") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 6);
        return;
    }
    if (strcasecmp(mnemonic, "idiv") == 0) {
        operand_t dst = parse_operand();
        int w = (dst.size == 2) ? 1 : 0;
        emit(0xF6 | w);
        emit_modrm(&dst, 7);
        return;
    }

    /* --- PUSH/POP --- */
    if (strcasecmp(mnemonic, "push") == 0) {
        operand_t op = parse_operand();
        asm_push_pop(0, &op);
        return;
    }
    if (strcasecmp(mnemonic, "pop") == 0) {
        operand_t op = parse_operand();
        asm_push_pop(1, &op);
        return;
    }

    /* --- JMP/CALL --- */
    if (strcasecmp(mnemonic, "jmp") == 0 || strcasecmp(mnemonic, "call") == 0) {
        operand_t op = parse_operand();
        asm_jmp_call(mnemonic, &op);
        return;
    }

    /* --- Conditional jumps --- */
    struct { const char *name; int cc; } jccs[] = {
        {"jo",  0x0}, {"jno", 0x1}, {"jb",   0x2}, {"jnae", 0x2},
        {"jc",  0x2}, {"jnb", 0x3}, {"jae",  0x3}, {"jnc",  0x3},
        {"jz",  0x4}, {"je",  0x4}, {"jnz",  0x5}, {"jne",  0x5},
        {"jbe", 0x6}, {"jna", 0x6}, {"jnbe", 0x7}, {"ja",   0x7},
        {"js",  0x8}, {"jns", 0x9}, {"jp",   0xA}, {"jpe",  0xA},
        {"jnp", 0xB}, {"jpo", 0xB}, {"jl",   0xC}, {"jnge", 0xC},
        {"jnl", 0xD}, {"jge", 0xD}, {"jle",  0xE}, {"jng",  0xE},
        {"jnle",0xF}, {"jg",  0xF},
        {"jcxz", -1},
        {NULL, 0}
    };
    for (int i = 0; jccs[i].name; i++) {
        if (strcasecmp(mnemonic, jccs[i].name) == 0) {
            if (jccs[i].cc == -1) {
                /* JCXZ */
                asm_loop(0xE3);
            } else {
                asm_jcc(jccs[i].cc);
            }
            return;
        }
    }

    /* --- LOOP variants --- */
    if (strcasecmp(mnemonic, "loop") == 0) { asm_loop(0xE2); return; }
    if (strcasecmp(mnemonic, "loope") == 0 || strcasecmp(mnemonic, "loopz") == 0) {
        asm_loop(0xE1); return;
    }
    if (strcasecmp(mnemonic, "loopne") == 0 || strcasecmp(mnemonic, "loopnz") == 0) {
        asm_loop(0xE0); return;
    }

    /* --- RET --- */
    if (strcasecmp(mnemonic, "ret") == 0 || strcasecmp(mnemonic, "retf") == 0) {
        asm_ret(mnemonic);
        return;
    }

    /* --- INT --- */
    if (strcasecmp(mnemonic, "int") == 0) { asm_int(); return; }

    /* --- IN/OUT --- */
    if (strcasecmp(mnemonic, "in") == 0 || strcasecmp(mnemonic, "out") == 0) {
        asm_in_out(mnemonic);
        return;
    }

    /* --- ENTER --- */
    if (strcasecmp(mnemonic, "enter") == 0) {
        operand_t size = parse_operand();
        expect(T_COMMA, "','");
        operand_t level = parse_operand();
        emit(0xC8);
        emit16(size.imm);
        emit(level.imm & 0xFF);
        return;
    }

    /* --- BOUND --- */
    if (strcasecmp(mnemonic, "bound") == 0) {
        operand_t dst = parse_operand();
        expect(T_COMMA, "','");
        operand_t src = parse_operand();
        emit(0x62);
        emit_modrm(&src, dst.reg);
        return;
    }

    /* --- V20 extensions --- */
    if (strcasecmp(mnemonic, "test1") == 0 || strcasecmp(mnemonic, "not1") == 0 ||
        strcasecmp(mnemonic, "clr1") == 0 || strcasecmp(mnemonic, "set1") == 0) {
        asm_v20_bitop(mnemonic);
        return;
    }
    if (strcasecmp(mnemonic, "add4s") == 0 || strcasecmp(mnemonic, "sub4s") == 0 ||
        strcasecmp(mnemonic, "cmp4s") == 0) {
        asm_v20_bcd_string(mnemonic);
        return;
    }
    if (strcasecmp(mnemonic, "rol4") == 0 || strcasecmp(mnemonic, "ror4") == 0) {
        asm_v20_nibble(mnemonic);
        return;
    }
    if (strcasecmp(mnemonic, "bext") == 0 || strcasecmp(mnemonic, "bins") == 0) {
        asm_v20_bext_bins(mnemonic);
        return;
    }
    if (strcasecmp(mnemonic, "brkem") == 0) {
        asm_brkem();
        return;
    }

    err("unknown instruction '%s'", mnemonic);
    errors++;
}

/* ---- Line processing ---- */

/* Scan for label at start of line before tokenizing */
static bool scan_label(const char *line, char *label_out, const char **rest) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!isalpha(*p) && *p != '_' && *p != '.') return false;
    const char *start = p;
    while (isalnum(*p) || *p == '_' || *p == '.') p++;
    int len = p - start;
    if (len == 0 || len >= 64) return false;
    (void)0; /* ident ends here */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ':') {
        memcpy(label_out, start, len);
        label_out[len] = '\0';
        *rest = p + 1;
        return true;
    }
    /* Check for "ident equ/db/dw" — label without colon before a directive */
    if ((strncasecmp(p, "equ", 3) == 0 && !isalnum(p[3]) && p[3] != '_') ||
        (strncasecmp(p, "db", 2) == 0 && !isalnum(p[2]) && p[2] != '_') ||
        (strncasecmp(p, "dw", 2) == 0 && !isalnum(p[2]) && p[2] != '_')) {
        memcpy(label_out, start, len);
        label_out[len] = '\0';
        *rest = p; /* point at directive */
        return true;
    }
    return false;
}

static void process_line(char *line) {
    char label_name[64];
    const char *rest = line;

    /* Capture ; @ debug comments */
    const char *lp = line;
    while (*lp == ' ' || *lp == '\t') lp++;
    if (strncmp(lp, "; @", 3) == 0) {
        const char *colon = strchr(lp + 3, ':');
        if (colon) {
            int flen = (int)(colon - (lp + 3));
            if (flen > 63) flen = 63;
            memcpy(pending_dbg_file, lp + 3, flen);
            pending_dbg_file[flen] = '\0';
            pending_dbg_line = atoi(colon + 1);
        }
        return;
    }
    /* On pass 2, record debug entry at the current address
     * when the next real instruction is encountered */
    if (pass == 2 && pending_dbg_line > 0 && *lp && *lp != ';') {
        if (ndbg_entries < MAX_DBG) {
            dbg_entry_t *de = &dbg_entries[ndbg_entries++];
            de->addr = org_base + out_pos;
            strncpy(de->file, pending_dbg_file, 63);
            de->line = pending_dbg_line;
        }
        /* Don't reset — keep for error messages until next ; @ comment */
    }

    /* Check for label before tokenizing */
    if (scan_label(line, label_name, &rest)) {
        /* Check if it's EQU */
        const char *p = rest;
        while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "equ", 3) == 0 && !isalnum(p[3]) && p[3] != '_') {
            p += 3;
            line_ptr = p;
            has_peek = false;
            int val = parse_expr();
            add_label_typed(label_name, val, LBL_EQU);
            return;
        }
        /* Check if this label precedes db/dw (data) */
        if ((strncasecmp(p, "db", 2) == 0 && !isalnum(p[2]) && p[2] != '_') ||
            (strncasecmp(p, "dw", 2) == 0 && !isalnum(p[2]) && p[2] != '_')) {
            add_label_typed(label_name, out_pos + org_base, LBL_DATA);
        } else {
            add_label(label_name, out_pos + org_base);
        }
        line = (char *)rest;
    }

    line_ptr = line;
    has_peek = false;

    token_t t = peek_token();
    if (t.type == T_EOL) return;

    t = consume();
    if (t.type == T_EOL) return;
    if (t.type != T_IDENT) {
        err("expected instruction or directive");
        errors++;
        return;
    }

    /* --- Directives --- */
    if (strcasecmp(t.sval, "org") == 0) {
        operand_t op = parse_operand();
        /* Push current position before changing */
        if (org_sp < MAX_ORG_STACK) {
            org_stack[org_sp].org_base = org_base;
            org_stack[org_sp].out_pos = out_pos;
            org_stack[org_sp].seg_base = seg_base;
            org_sp++;
        }
        org_base = op.imm;
        out_pos = 0;
        return;
    }

    if (strcasecmp(t.sval, "endorg") == 0) {
        /* Pop previous position */
        if (org_sp > 0) {
            org_sp--;
            org_base = org_stack[org_sp].org_base;
            out_pos = org_stack[org_sp].out_pos;
            seg_base = org_stack[org_sp].seg_base;
        }
        return;
    }

    if (strcasecmp(t.sval, "db") == 0) {
        for (;;) {
            token_t dt = peek_token();
            if (dt.type == T_STRING) {
                consume();
                for (int i = 0; dt.sval[i]; i++)
                    emit(dt.sval[i]);
            } else if (dt.type == T_NUMBER || dt.type == T_IDENT ||
                       dt.type == T_MINUS) {
                int val = parse_expr();
                emit(val & 0xFF);
            } else {
                break;
            }
            dt = peek_token();
            if (dt.type == T_COMMA) { consume(); continue; }
            break;
        }
        return;
    }

    if (strcasecmp(t.sval, "dw") == 0) {
        for (;;) {
            token_t dt = peek_token();
            if (dt.type == T_NUMBER || dt.type == T_IDENT ||
                dt.type == T_MINUS) {
                int val = parse_expr();
                emit16(val & 0xFFFF);
            } else {
                break;
            }
            dt = peek_token();
            if (dt.type == T_COMMA) { consume(); continue; }
            break;
        }
        return;
    }

    if (strcasecmp(t.sval, "seg") == 0) {
        /* SEG directive — sets current segment base for label tracking */
        operand_t op = parse_operand();
        seg_base = op.imm;
        return;
    }

    /* Must be an instruction */
    assemble_instruction(t.sval);
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    const char *infile = NULL;
    const char *outfile = "a.out";
    const char *mapfile = NULL;
    const char *dbgfile = NULL;
    bool ihex = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mapfile = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dbgfile = argv[++i];
        } else if (strcmp(argv[i], "--ihex") == 0) {
            ihex = true;
        } else {
            infile = argv[i];
        }
    }

    FILE *fp = stdin;
    if (infile) {
        fp = fopen(infile, "r");
        if (!fp) { perror(infile); return 1; }
        current_file = infile;
    }

    /* Read all lines into memory */
    char **lines = NULL;
    int nlines = 0;
    int lines_cap = 0;
    char buf[MAX_LINE];

    while (fgets(buf, sizeof(buf), fp)) {
        if (nlines >= lines_cap) {
            lines_cap = lines_cap ? lines_cap * 2 : 256;
            lines = realloc(lines, lines_cap * sizeof(char *));
        }
        lines[nlines++] = strdup(buf);
    }
    if (fp != stdin) fclose(fp);

    /* Allocate output buffers */
    output = malloc(MAX_OUTPUT);
    written = calloc(MAX_OUTPUT, sizeof(bool));
    if (!output || !written) { fprintf(stderr, "out of memory\n"); return 1; }
    memset(output, 0xFF, MAX_OUTPUT);

    /* Pass 1: collect labels and sizes, with iterative Jcc relaxation.
     * First run establishes labels assuming all Jcc are short.
     * If any are out of range, mark them, re-run to get correct sizes. */
    nrelaxed = 0;
    pass = 1;
    out_pos = 0;
    org_base = 0;
    seg_base = 0;
    org_sp = 0;
    errors = 0;
    jcc_relaxed = false;
    for (int i = 0; i < nlines; i++) {
        current_line = i + 1;
        process_line(lines[i]);
    }
    if (jcc_relaxed) {
        /* Relaxation changed sizes — re-run pass 1 to update labels.
         * Keep labels from prior iteration so forward refs resolve. */
        for (int iter = 0; iter < 4; iter++) {
            pass = 1;
            out_pos = 0;
            org_base = 0;
            seg_base = 0;
            org_sp = 0;
            errors = 0;
            jcc_relaxed = false;
            for (int i = 0; i < nlines; i++) {
                current_line = i + 1;
                process_line(lines[i]);
            }
            if (!jcc_relaxed) break;
        }
    }

    /* Pass 2: emit code */
    pass = 2;
    out_pos = 0;
    org_base = 0;
    seg_base = 0;
    org_sp = 0;
    pending_dbg_line = 0;
    for (int i = 0; i < nlines; i++) {
        current_line = i + 1;
        process_line(lines[i]);
    }

    if (errors > 0) {
        fprintf(stderr, "%d error(s)\n", errors);
        return 1;
    }

    /* Find extent of written data */
    int first_addr = -1, last_addr = -1;
    int total_written = 0;
    for (int i = 0; i < MAX_OUTPUT; i++) {
        if (written[i]) {
            if (first_addr < 0) first_addr = i;
            last_addr = i;
            total_written++;
        }
    }
    if (first_addr < 0) first_addr = last_addr = 0;

    /* Write output */
    FILE *out = fopen(outfile, ihex ? "w" : "wb");
    if (!out) { perror(outfile); return 1; }

    if (ihex) {
        /* Intel HEX format — emit only written regions */
        int cur_ext_addr = -1; /* current extended address, -1 = none */
        int addr = 0;
        while (addr < MAX_OUTPUT) {
            /* Skip unwritten regions */
            while (addr < MAX_OUTPUT && !written[addr]) addr++;
            if (addr >= MAX_OUTPUT) break;

            /* Emit extended segment address record (type 02) if needed.
             * Segment base = addr >> 4, rounded down to paragraph.
             * Data record offset = addr - (segment << 4). */
            int seg_base = addr >> 4;
            if (seg_base != cur_ext_addr) {
                uint8_t cksum = 0;
                cksum += 0x02;
                cksum += 0x02; /* record type */
                cksum += (seg_base >> 8) & 0xFF;
                cksum += seg_base & 0xFF;
                fprintf(out, ":02000002%04X%02X\n", seg_base,
                        (-cksum) & 0xFF);
                cur_ext_addr = seg_base;
            }

            /* Find contiguous run */
            int run_start = addr;
            int page_end = (seg_base << 4) + 0x10000; /* stay within segment reach */
            if (page_end > MAX_OUTPUT) page_end = MAX_OUTPUT;
            while (addr < MAX_OUTPUT && addr < page_end && written[addr])
                addr++;

            /* Emit data records — offset relative to segment base */
            int pos = run_start;
            int base_linear = cur_ext_addr << 4;
            while (pos < addr) {
                int chunk = addr - pos;
                if (chunk > 16) chunk = 16;
                int offset = pos - base_linear;
                uint8_t cksum = 0;
                cksum += chunk;
                cksum += (offset >> 8) & 0xFF;
                cksum += offset & 0xFF;
                cksum += 0x00; /* data record */
                fprintf(out, ":%02X%04X00", chunk, offset & 0xFFFF);
                for (int i = 0; i < chunk; i++) {
                    fprintf(out, "%02X", output[pos + i]);
                    cksum += output[pos + i];
                }
                fprintf(out, "%02X\n", (-cksum) & 0xFF);
                pos += chunk;
            }
        }
        /* EOF record */
        fprintf(out, ":00000001FF\n");
    } else {
        /* Flat binary — from first to last written byte */
        int size = last_addr - first_addr + 1;
        fwrite(output + first_addr, 1, size, out);
    }
    fclose(out);

    fprintf(stderr, "%s: %d bytes%s\n", outfile, total_written,
            ihex ? " (ihex)" : "");

    /* Write map file if requested */
    if (mapfile) {
        FILE *mf = fopen(mapfile, "w");
        if (!mf) { perror(mapfile); return 1; }
        fprintf(mf, "# nib map file\n");
        for (int i = 0; i < nlabels; i++) {
            const char *typestr;
            switch (labels[i].ltype) {
            case LBL_CODE: typestr = "code"; break;
            case LBL_DATA: typestr = "data"; break;
            case LBL_EQU:  typestr = "equ"; break;
            default:       typestr = "?"; break;
            }
            if (labels[i].ltype == LBL_EQU)
                fprintf(mf, "%04X %s %s = %04X\n",
                        labels[i].value, typestr, labels[i].name, labels[i].value);
            else
                fprintf(mf, "%04X %s %s\n",
                        labels[i].value, typestr, labels[i].name);
        }
        fclose(mf);
        fprintf(stderr, "%s: %d labels\n", mapfile, nlabels);
    }

    /* Write debug info file */
    if (dbgfile && ndbg_entries > 0) {
        FILE *df = fopen(dbgfile, "w");
        if (!df) { perror(dbgfile); return 1; }
        fprintf(df, "# nib debug info\n");
        for (int i = 0; i < ndbg_entries; i++) {
            fprintf(df, "%05X %s:%d\n",
                    dbg_entries[i].addr,
                    dbg_entries[i].file,
                    dbg_entries[i].line);
        }
        fclose(df);
        fprintf(stderr, "%s: %d entries\n", dbgfile, ndbg_entries);
    }

    /* Cleanup */
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);

    return 0;
}
