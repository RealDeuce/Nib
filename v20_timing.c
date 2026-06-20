#include "v20_timing.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OP_NONE,
    OP_REG8,
    OP_REG16,
    OP_SREG,
    OP_IMM,
    OP_MEM8,
    OP_MEM16,
    OP_MEM,
    OP_FAR,
} op_class_t;

typedef struct {
    op_class_t cls;
    bool mem;
    bool seg_override;
    bool imm8;
    char text[96];
} cost_operand_t;

static void lower_copy(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < dstsz; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static bool streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static bool is_reg8(const char *s) {
    static const char *regs[] = {
        "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"
    };
    for (int i = 0; i < 8; i++)
        if (streq(s, regs[i]))
            return true;
    return false;
}

static bool is_reg16(const char *s) {
    static const char *regs[] = {
        "ax", "cx", "dx", "bx", "sp", "bp", "si", "di"
    };
    for (int i = 0; i < 8; i++)
        if (streq(s, regs[i]))
            return true;
    return false;
}

static bool is_sreg(const char *s) {
    return streq(s, "es") || streq(s, "cs") ||
           streq(s, "ss") || streq(s, "ds");
}

static bool parse_int_literal(const char *s, long *out) {
    char *end = NULL;
    int base = 10;
    char buf[64];
    size_t len = strlen(s);
    if (len == 0 || len >= sizeof(buf))
        return false;
    strcpy(buf, s);
    if (len > 1 && (buf[len - 1] == 'h' || buf[len - 1] == 'H')) {
        buf[len - 1] = '\0';
        base = 16;
    }
    long v = strtol(buf, &end, base);
    if (!end || *trim(end) != '\0')
        return false;
    if (out)
        *out = v;
    return true;
}

static void split_operands(const char *src, char ops[3][96], int *nops) {
    *nops = 0;
    int depth = 0;
    char cur[96];
    int ncur = 0;
    for (const char *p = src; ; p++) {
        char ch = *p;
        if (ch == '[')
            depth++;
        else if (ch == ']' && depth > 0)
            depth--;
        if ((ch == ',' && depth == 0) || ch == '\0') {
            cur[ncur] = '\0';
            if (*nops < 3) {
                char *t = trim(cur);
                strncpy(ops[*nops], t, 95);
                ops[*nops][95] = '\0';
                (*nops)++;
            }
            ncur = 0;
            if (ch == '\0')
                break;
            continue;
        }
        if (ncur + 1 < (int)sizeof(cur))
            cur[ncur++] = ch;
    }
}

static cost_operand_t classify_operand(const char *raw) {
    cost_operand_t op;
    memset(&op, 0, sizeof(op));
    op.cls = OP_NONE;
    lower_copy(op.text, sizeof(op.text), raw);
    char *s = trim(op.text);

    bool force_byte = false;
    bool force_word = false;
    bool force_far = false;
    if (strncmp(s, "byte ", 5) == 0) {
        force_byte = true;
        s = trim(s + 5);
    } else if (strncmp(s, "word ", 5) == 0) {
        force_word = true;
        s = trim(s + 5);
    } else if (strncmp(s, "far ", 4) == 0) {
        force_far = true;
        s = trim(s + 4);
    }

    memmove(op.text, s, strlen(s) + 1);
    s = op.text;

    if (is_reg8(s)) {
        op.cls = OP_REG8;
        return op;
    }
    if (is_reg16(s)) {
        op.cls = OP_REG16;
        return op;
    }
    if (is_sreg(s)) {
        op.cls = OP_SREG;
        return op;
    }
    if (strchr(s, '[')) {
        op.mem = true;
        op.seg_override =
            strstr(s, "[es:") || strstr(s, "[cs:") ||
            strstr(s, "[ss:") || strstr(s, "[ds:");
        if (force_far)
            op.cls = OP_FAR;
        else if (force_byte)
            op.cls = OP_MEM8;
        else if (force_word)
            op.cls = OP_MEM16;
        else
            op.cls = OP_MEM;
        return op;
    }
    if (force_far || strchr(s, ':')) {
        op.cls = OP_FAR;
        return op;
    }
    long v;
    op.cls = OP_IMM;
    op.imm8 = parse_int_literal(s, &v) && v >= -128 && v <= 255;
    return op;
}

static bool op_is_reg(op_class_t c) {
    return c == OP_REG8 || c == OP_REG16;
}

static bool op_is_mem(op_class_t c) {
    return c == OP_MEM || c == OP_MEM8 || c == OP_MEM16;
}

static int op_width(op_class_t c) {
    if (c == OP_REG8 || c == OP_MEM8)
        return 1;
    if (c == OP_REG16 || c == OP_MEM16 || c == OP_SREG)
        return 2;
    return 0;
}

static void set_known(v20_timing_t *t, const char *form, int bmin, int bmax,
                      int cmin, int cmax, int xmin, int xmax,
                      v20_mix_t mix) {
    t->known = true;
    t->bytes_min = bmin;
    t->bytes_max = bmax;
    t->clocks_min = cmin;
    t->clocks_max = cmax;
    t->transfers_min = xmin;
    t->transfers_max = xmax;
    t->mix = mix;
    snprintf(t->form, sizeof(t->form), "%s", form);
    if (cmin == cmax)
        snprintf(t->clocks, sizeof(t->clocks), "%d", cmin);
    else
        snprintf(t->clocks, sizeof(t->clocks), "%d-%d", cmin, cmax);
    if (xmin == xmax)
        snprintf(t->transfers, sizeof(t->transfers), "%d", xmin);
    else
        snprintf(t->transfers, sizeof(t->transfers), "%d-%d", xmin, xmax);
}

static void add_seg_override_cost(v20_timing_t *t, cost_operand_t *a,
                                  int nops) {
    if (!t->known)
        return;
    for (int i = 0; i < nops; i++) {
        if (!a[i].seg_override)
            continue;
        t->bytes_min++;
        t->bytes_max++;
        t->clocks_min += 2;
        t->clocks_max += 2;
        snprintf(t->note, sizeof(t->note), "includes segment override");
        if (t->clocks_min == t->clocks_max)
            snprintf(t->clocks, sizeof(t->clocks), "%d", t->clocks_min);
        else
            snprintf(t->clocks, sizeof(t->clocks), "%d-%d",
                     t->clocks_min, t->clocks_max);
        break;
    }
}

static bool is_jcc(const char *m) {
    static const char *cc[] = {
        "jo","jno","jb","jc","jnae","jnb","jnc","jae","jz","je",
        "jnz","jne","jbe","jna","ja","jnbe","js","jns","jp","jpe",
        "jnp","jpo","jl","jnge","jge","jnl","jle","jng","jg","jnle",
        "jcxz"
    };
    for (int i = 0; i < (int)(sizeof(cc) / sizeof(cc[0])); i++)
        if (streq(m, cc[i]))
            return true;
    return false;
}

static bool is_alu(const char *m) {
    static const char *ops[] = {
        "add","adc","sub","sbb","cmp","and","or","xor"
    };
    for (int i = 0; i < 8; i++)
        if (streq(m, ops[i]))
            return true;
    return false;
}

static bool is_shift(const char *m) {
    static const char *ops[] = {
        "shl","sal","shr","sar","rol","ror","rcl","rcr"
    };
    for (int i = 0; i < 8; i++)
        if (streq(m, ops[i]))
            return true;
    return false;
}

static bool is_flag_simple(const char *m) {
    return streq(m, "clc") || streq(m, "cld") || streq(m, "cli") ||
           streq(m, "cmc") || streq(m, "stc") || streq(m, "std") ||
           streq(m, "sti");
}

static bool is_prefix(const char *m) {
    return streq(m, "lock") || streq(m, "rep") || streq(m, "repe") ||
           streq(m, "repz") || streq(m, "repne") || streq(m, "repnz") ||
           streq(m, "repc") || streq(m, "repnc");
}

static bool is_asm_directive(const char *s) {
    return strncmp(s, "org ", 4) == 0 || strncmp(s, "seg ", 4) == 0 ||
           strncmp(s, "db ", 3) == 0 || strncmp(s, "dw ", 3) == 0 ||
           strncmp(s, "equ ", 4) == 0 || strncmp(s, "endorg", 6) == 0;
}

static void set_variable(v20_timing_t *t, const char *form, int bmin,
                         int bmax, const char *clocks,
                         const char *transfers, v20_mix_t mix) {
    t->known = true;
    t->variable = true;
    t->bytes_min = bmin;
    t->bytes_max = bmax;
    t->mix = mix;
    snprintf(t->form, sizeof(t->form), "%s", form);
    snprintf(t->clocks, sizeof(t->clocks), "%s", clocks);
    snprintf(t->transfers, sizeof(t->transfers), "%s", transfers);
}

v20_timing_t v20_timing_estimate(const char *asm_line) {
    v20_timing_t t;
    memset(&t, 0, sizeof(t));
    t.mix = V20_MIX_OTHER;

    char line[256];
    lower_copy(line, sizeof(line), asm_line);
    char *comment = strchr(line, ';');
    if (comment)
        *comment = '\0';
    char *s = trim(line);
    char *label_colon = strchr(s, ':');
    char *first_ws = s;
    while (*first_ws && !isspace((unsigned char)*first_ws))
        first_ws++;
    if (label_colon && label_colon < first_ws) {
        char *after_label = trim(label_colon + 1);
        if (*after_label == '\0' || is_asm_directive(after_label))
            return t;
        s = after_label;
    }
    if (*s == '\0' || *s == '.' ||
        is_asm_directive(s))
        return t;

    char mnem[32];
    int mi = 0;
    while (s[mi] && !isspace((unsigned char)s[mi]) && mi + 1 < (int)sizeof(mnem)) {
        mnem[mi] = s[mi];
        mi++;
    }
    mnem[mi] = '\0';
    s = trim(s + mi);

    bool rep_prefix = false;
    if (is_prefix(mnem) && *s) {
        rep_prefix = true;
        char next[32];
        int ni = 0;
        while (s[ni] && !isspace((unsigned char)s[ni]) &&
               ni + 1 < (int)sizeof(next)) {
            next[ni] = s[ni];
            ni++;
        }
        next[ni] = '\0';
        snprintf(mnem, sizeof(mnem), "%s", next);
        s = trim(s + ni);
    }
    snprintf(t.mnemonic, sizeof(t.mnemonic), "%s", mnem);

    char op_text[3][96];
    int nops = 0;
    if (*s)
        split_operands(s, op_text, &nops);
    cost_operand_t ops[3];
    memset(ops, 0, sizeof(ops));
    for (int i = 0; i < nops; i++)
        ops[i] = classify_operand(op_text[i]);

    if (rep_prefix) {
        if (streq(mnem, "movsb"))
            set_variable(&t, "rep movsb", 2, 2, "11+8*n", "2*n",
                         V20_MIX_STRING);
        else if (streq(mnem, "movsw"))
            set_variable(&t, "rep movsw", 2, 2, "11+16*n", "2*n",
                         V20_MIX_STRING);
        else if (streq(mnem, "stosb"))
            set_variable(&t, "rep stosb", 2, 2, "7+4*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "stosw"))
            set_variable(&t, "rep stosw", 2, 2, "7+8*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "lodsb"))
            set_variable(&t, "rep lodsb", 2, 2, "7+9*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "lodsw"))
            set_variable(&t, "rep lodsw", 2, 2, "7+13*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "cmpsb"))
            set_variable(&t, "rep cmpsb", 2, 2, "7+14*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "cmpsw"))
            set_variable(&t, "rep cmpsw", 2, 2, "7+22*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "scasb"))
            set_variable(&t, "rep scasb", 2, 2, "7+10*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "scasw"))
            set_variable(&t, "rep scasw", 2, 2, "7+14*n", "n",
                         V20_MIX_STRING);
        else if (streq(mnem, "insb") || streq(mnem, "outsb"))
            set_variable(&t, mnem, 2, 2, "9+8*n", "2*n",
                         V20_MIX_STRING);
        else if (streq(mnem, "insw") || streq(mnem, "outsw"))
            set_variable(&t, mnem, 2, 2, "9+16*n", "2*n",
                         V20_MIX_STRING);
        else
            set_variable(&t, "repeat prefix", 1, 1, "2+body", "body",
                         V20_MIX_STRING);
        return t;
    }

    if (is_prefix(mnem)) {
        set_known(&t, "prefix", 1, 1, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (is_flag_simple(mnem)) {
        set_known(&t, mnem, 1, 1, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "nop")) {
        set_known(&t, "nop", 1, 1, 3, 3, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "hlt")) {
        set_known(&t, "hlt", 1, 1, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "poll")) {
        set_variable(&t, "poll", 1, 1, "2+5*n", "0", V20_MIX_OTHER);
    } else if (streq(mnem, "brkem")) {
        set_known(&t, "brkem imm8", 3, 3, 50, 50, 5, 5, V20_MIX_CALL);
    } else if (streq(mnem, "calln")) {
        set_known(&t, "calln imm8", 3, 3, 58, 58, 5, 5, V20_MIX_CALL);
    } else if (streq(mnem, "cbw") || streq(mnem, "cwd")) {
        set_known(&t, mnem, 1, 1, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "lahf") || streq(mnem, "sahf")) {
        set_known(&t, mnem, 1, 1, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "mov") && nops == 2) {
        op_class_t d = ops[0].cls, scls = ops[1].cls;
        if (op_is_reg(d) && op_is_reg(scls))
            set_known(&t, op_width(d) == 1 ? "mov reg8, reg8" :
                      "mov reg16, reg16", 2, 2, 2, 2, 0, 0,
                      V20_MIX_OTHER);
        else if (op_is_mem(d) && op_is_reg(scls))
            set_known(&t, op_width(scls) == 1 ? "mov mem8, reg8" :
                      "mov mem16, reg16", 2, 4,
                      op_width(scls) == 1 ? 9 : 13,
                      op_width(scls) == 1 ? 9 : 13, 1, 1,
                      V20_MIX_MEMORY);
        else if (op_is_reg(d) && op_is_mem(scls))
            set_known(&t, op_width(d) == 1 ? "mov reg8, mem8" :
                      "mov reg16, mem16", 2, 4,
                      op_width(d) == 1 ? 11 : 15,
                      op_width(d) == 1 ? 11 : 15, 1, 1,
                      V20_MIX_MEMORY);
        else if (op_is_reg(d) && scls == OP_IMM)
            set_known(&t, op_width(d) == 1 ? "mov reg8, imm8" :
                      "mov reg16, imm16", op_width(d) == 1 ? 2 : 3,
                      op_width(d) == 1 ? 2 : 3, 4, 4, 0, 0,
                      V20_MIX_OTHER);
        else if (op_is_mem(d) && scls == OP_IMM)
            set_known(&t, ops[0].cls == OP_MEM8 ? "mov mem8, imm8" :
                      "mov mem16, imm16", ops[0].cls == OP_MEM8 ? 3 : 4,
                      ops[0].cls == OP_MEM8 ? 5 : 6,
                      ops[0].cls == OP_MEM8 ? 11 : 15,
                      ops[0].cls == OP_MEM8 ? 11 : 15, 1, 1,
                      V20_MIX_MEMORY);
        else if (d == OP_SREG && (scls == OP_REG16 || op_is_mem(scls)))
            set_known(&t, scls == OP_REG16 ? "mov sreg, reg16" :
                      "mov sreg, mem16", scls == OP_REG16 ? 2 : 2,
                      scls == OP_REG16 ? 2 : 4,
                      scls == OP_REG16 ? 2 : 15,
                      scls == OP_REG16 ? 2 : 15,
                      scls == OP_REG16 ? 0 : 1,
                      scls == OP_REG16 ? 0 : 1, V20_MIX_SEGMENT);
        else if ((d == OP_REG16 || op_is_mem(d)) && scls == OP_SREG)
            set_known(&t, d == OP_REG16 ? "mov reg16, sreg" :
                      "mov mem16, sreg", d == OP_REG16 ? 2 : 2,
                      d == OP_REG16 ? 2 : 4,
                      d == OP_REG16 ? 2 : 14,
                      d == OP_REG16 ? 2 : 14, d == OP_REG16 ? 0 : 1,
                      d == OP_REG16 ? 0 : 1, V20_MIX_SEGMENT);
    } else if (is_alu(mnem) && nops == 2) {
        op_class_t d = ops[0].cls, scls = ops[1].cls;
        v20_mix_t mix = (op_is_mem(d) || op_is_mem(scls)) ?
                        V20_MIX_MEMORY : V20_MIX_OTHER;
        if (op_is_reg(d) && op_is_reg(scls))
            set_known(&t, "alu reg, reg", 2, 2, 2, 2, 0, 0, mix);
        else if (op_is_mem(d) && op_is_reg(scls))
            set_known(&t, op_width(scls) == 1 ? "alu mem8, reg8" :
                      "alu mem16, reg16", 2, 4,
                      op_width(scls) == 1 ? 16 : 24,
                      op_width(scls) == 1 ? 16 : 24, 2, 2, mix);
        else if (op_is_reg(d) && op_is_mem(scls))
            set_known(&t, op_width(d) == 1 ? "alu reg8, mem8" :
                      "alu reg16, mem16", 2, 4,
                      op_width(d) == 1 ? 11 : 15,
                      op_width(d) == 1 ? 11 : 15, 1, 1, mix);
        else if (op_is_reg(d) && scls == OP_IMM)
            set_known(&t, op_width(d) == 1 ? "alu reg8, imm8" :
                      "alu reg16, imm", op_width(d) == 1 ? 3 : 3,
                      op_width(d) == 1 ? 3 : 4, 4, 4, 0, 0, mix);
        else if (op_is_mem(d) && scls == OP_IMM)
            set_known(&t, ops[0].cls == OP_MEM8 ? "alu mem8, imm8" :
                      "alu mem16, imm", 3, 6,
                      ops[0].cls == OP_MEM8 ? 18 : 26,
                      ops[0].cls == OP_MEM8 ? 18 : 26, 2, 2, mix);
    } else if ((streq(mnem, "inc") || streq(mnem, "dec")) && nops == 1) {
        if (op_is_reg(ops[0].cls))
            set_known(&t, ops[0].cls == OP_REG8 ? "inc/dec reg8" :
                      "inc/dec reg16", ops[0].cls == OP_REG8 ? 2 : 1,
                      ops[0].cls == OP_REG8 ? 2 : 1, 2, 2, 0, 0,
                      V20_MIX_OTHER);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, ops[0].cls == OP_MEM8 ? "inc/dec mem8" :
                      "inc/dec mem16", 2, 4,
                      ops[0].cls == OP_MEM8 ? 16 : 24,
                      ops[0].cls == OP_MEM8 ? 16 : 24, 2, 2,
                      V20_MIX_MEMORY);
    } else if ((streq(mnem, "neg") || streq(mnem, "not")) && nops == 1) {
        if (op_is_reg(ops[0].cls))
            set_known(&t, ops[0].cls == OP_REG8 ? "unary reg8" :
                      "unary reg16", 2, 2, 2, 2, 0, 0, V20_MIX_OTHER);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, ops[0].cls == OP_MEM8 ? "unary mem8" :
                      "unary mem16", 2, 4,
                      ops[0].cls == OP_MEM8 ? 16 : 24,
                      ops[0].cls == OP_MEM8 ? 16 : 24, 2, 2,
                      V20_MIX_MEMORY);
    } else if (is_shift(mnem) && nops == 2) {
        bool one = ops[1].cls == OP_IMM && streq(ops[1].text, "1");
        if (op_is_reg(ops[0].cls)) {
            if (one)
                set_known(&t, "shift/rotate reg, 1", 2, 2, 2, 2, 0, 0,
                          V20_MIX_SHIFT);
            else
                set_variable(&t, "shift/rotate reg, n", 2,
                             ops[1].cls == OP_IMM ? 3 : 2, "7+n",
                             "0", V20_MIX_SHIFT);
        } else if (op_is_mem(ops[0].cls)) {
            if (one)
                set_known(&t, ops[0].cls == OP_MEM8 ?
                          "shift/rotate mem8, 1" :
                          "shift/rotate mem16, 1", 2, 4,
                          ops[0].cls == OP_MEM8 ? 16 : 24,
                          ops[0].cls == OP_MEM8 ? 16 : 24, 2, 2,
                          V20_MIX_SHIFT);
            else
                set_variable(&t, ops[0].cls == OP_MEM8 ?
                             "shift/rotate mem8, n" :
                             "shift/rotate mem16, n", 2,
                             ops[1].cls == OP_IMM ? 5 : 4,
                             ops[0].cls == OP_MEM8 ? "19+n" : "27+n",
                             "2", V20_MIX_SHIFT);
        }
    } else if (streq(mnem, "push") && nops == 1) {
        if (ops[0].cls == OP_REG16 || ops[0].cls == OP_SREG)
            set_known(&t, "push reg16/sreg", 1, 1, 12, 12, 1, 1,
                      V20_MIX_PUSH_POP);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, "push mem16", 2, 4, 26, 26, 2, 2,
                      V20_MIX_PUSH_POP);
        else if (ops[0].cls == OP_IMM)
            set_known(&t, ops[0].imm8 ? "push imm8" : "push imm16",
                      ops[0].imm8 ? 2 : 3, ops[0].imm8 ? 2 : 3,
                      ops[0].imm8 ? 11 : 12, ops[0].imm8 ? 11 : 12,
                      1, 1, V20_MIX_PUSH_POP);
    } else if (streq(mnem, "pop") && nops == 1) {
        if (ops[0].cls == OP_REG16 || ops[0].cls == OP_SREG)
            set_known(&t, "pop reg16/sreg", 1, 1, 12, 12, 1, 1,
                      V20_MIX_PUSH_POP);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, "pop mem16", 2, 4, 25, 25, 2, 2,
                      V20_MIX_PUSH_POP);
    } else if (streq(mnem, "pusha")) {
        set_known(&t, "pusha", 1, 1, 67, 67, 8, 8, V20_MIX_PUSH_POP);
    } else if (streq(mnem, "popa")) {
        set_known(&t, "popa", 1, 1, 75, 75, 7, 7, V20_MIX_PUSH_POP);
    } else if (streq(mnem, "pushf") || streq(mnem, "popf")) {
        set_known(&t, mnem, 1, 1, 12, 12, 1, 1, V20_MIX_PUSH_POP);
    } else if (streq(mnem, "call") && nops == 1) {
        if (ops[0].cls == OP_REG16)
            set_known(&t, "call reg16", 2, 2, 18, 18, 1, 1,
                      V20_MIX_CALL);
        else if (ops[0].cls == OP_FAR && ops[0].mem)
            set_known(&t, "call far mem32", 2, 4, 47, 47, 4, 4,
                      V20_MIX_CALL);
        else if (ops[0].cls == OP_FAR)
            set_known(&t, "call far seg:off", 5, 5, 29, 29, 2, 2,
                      V20_MIX_CALL);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, "call mem16", 2, 4, 31, 31, 2, 2,
                      V20_MIX_CALL);
        else
            set_known(&t, "call rel16", 3, 3, 20, 20, 1, 1,
                      V20_MIX_CALL);
    } else if (streq(mnem, "jmp") && nops == 1) {
        if (ops[0].cls == OP_REG16)
            set_known(&t, "jmp reg16", 2, 2, 11, 11, 0, 0,
                      V20_MIX_BRANCH);
        else if (ops[0].cls == OP_FAR && ops[0].mem)
            set_known(&t, "jmp far mem32", 2, 4, 35, 35, 2, 2,
                      V20_MIX_BRANCH);
        else if (ops[0].cls == OP_FAR)
            set_known(&t, "jmp far seg:off", 5, 5, 15, 15, 0, 0,
                      V20_MIX_BRANCH);
        else if (op_is_mem(ops[0].cls))
            set_known(&t, "jmp mem16", 2, 4, 24, 24, 1, 1,
                      V20_MIX_BRANCH);
        else
            set_known(&t, "jmp rel", 2, 3, 12, 12, 0, 0,
                      V20_MIX_BRANCH);
    } else if (streq(mnem, "call") == false && streq(mnem, "jmp") == false &&
               nops >= 2 && streq(op_text[0], "far")) {
        /* Not reached by the normal splitter, kept for clarity. */
    } else if (streq(mnem, "enter")) {
        if (nops >= 2 && strcmp(ops[1].text, "0") != 0)
            set_variable(&t, "enter imm16, imm8", 4, 4,
                         "23+16*(n-1)", "1+2*(n-1)", V20_MIX_PUSH_POP);
        else
            set_known(&t, "enter imm16, 0", 4, 4, 16, 16, 0, 0,
                      V20_MIX_PUSH_POP);
    } else if (streq(mnem, "leave")) {
        set_known(&t, "leave", 1, 1, 10, 10, 1, 1, V20_MIX_PUSH_POP);
    } else if (is_jcc(mnem) && nops == 1) {
        if (streq(mnem, "jcxz"))
            set_known(&t, "jcxz rel8", 2, 2, 5, 13, 0, 0,
                      V20_MIX_BRANCH);
        else
            set_known(&t, "jcc rel8", 2, 2, 4, 14, 0, 0,
                      V20_MIX_BRANCH);
    } else if (strncmp(mnem, "ret", 3) == 0) {
        if (streq(mnem, "ret"))
            set_known(&t, nops ? "ret imm16" : "ret", nops ? 3 : 1,
                      nops ? 3 : 1, nops ? 24 : 19, nops ? 24 : 19,
                      1, 1, V20_MIX_BRANCH);
        else if (streq(mnem, "retf"))
            set_known(&t, nops ? "retf imm16" : "retf", nops ? 3 : 1,
                      nops ? 3 : 1, nops ? 32 : 29, nops ? 32 : 29,
                      2, 2, V20_MIX_BRANCH);
    } else if (streq(mnem, "iret")) {
        set_known(&t, "iret", 1, 1, 32, 32, 3, 3, V20_MIX_BRANCH);
    } else if (streq(mnem, "loop") || streq(mnem, "loope") ||
               streq(mnem, "loopz") || streq(mnem, "loopne") ||
               streq(mnem, "loopnz")) {
        set_known(&t, "loop rel8", 2, 2, 5,
                  streq(mnem, "loop") ? 13 : 14, 0, 0,
                  V20_MIX_BRANCH);
    } else if (streq(mnem, "in") && nops == 2) {
        int word = ops[0].cls == OP_REG16;
        set_known(&t, word ? "in AX, port" : "in AL, port", 1, 2,
                  word ? (ops[1].cls == OP_IMM ? 13 : 12) :
                         (ops[1].cls == OP_IMM ? 9 : 8),
                  word ? (ops[1].cls == OP_IMM ? 13 : 12) :
                         (ops[1].cls == OP_IMM ? 9 : 8),
                  1, 1, V20_MIX_IO);
    } else if (streq(mnem, "out") && nops == 2) {
        int word = ops[1].cls == OP_REG16;
        set_known(&t, word ? "out port, AX" : "out port, AL", 1, 2,
                  word ? 12 : 8, word ? 12 : 8, 1, 1, V20_MIX_IO);
    } else if (streq(mnem, "lea") && nops == 2) {
        set_known(&t, "lea reg16, mem", 2, 4, 4, 4, 0, 0, V20_MIX_MEMORY);
    } else if ((streq(mnem, "lds") || streq(mnem, "les")) && nops == 2) {
        set_known(&t, streq(mnem, "lds") ? "lds reg16, mem32" :
                  "les reg16, mem32", 2, 4, 26, 26, 2, 2,
                  V20_MIX_SEGMENT);
    } else if (streq(mnem, "xchg") && nops == 2) {
        if (op_is_reg(ops[0].cls) && op_is_reg(ops[1].cls))
            set_known(&t, "xchg reg, reg", 1, 2, 3, 3, 0, 0,
                      V20_MIX_OTHER);
        else
            set_known(&t, "xchg reg, mem", 2, 4, 17, 25, 2, 2,
                      V20_MIX_MEMORY);
    } else if (streq(mnem, "test") && nops == 2) {
        if (ops[1].cls == OP_IMM)
            set_known(&t, "test r/m, imm", 2, 6,
                      op_is_mem(ops[0].cls) ? 11 : 4,
                      op_is_mem(ops[0].cls) ? 11 : 4,
                      op_is_mem(ops[0].cls) ? 1 : 0,
                      op_is_mem(ops[0].cls) ? 1 : 0,
                      op_is_mem(ops[0].cls) ? V20_MIX_MEMORY :
                                              V20_MIX_OTHER);
        else
            set_known(&t, "test r/m, reg", 2, 4,
                      op_is_mem(ops[0].cls) ? 11 : 2,
                      op_is_mem(ops[0].cls) ? 11 : 2,
                      op_is_mem(ops[0].cls) ? 1 : 0,
                      op_is_mem(ops[0].cls) ? 1 : 0,
                      op_is_mem(ops[0].cls) ? V20_MIX_MEMORY :
                                              V20_MIX_OTHER);
    } else if (streq(mnem, "imul") || streq(mnem, "mul") ||
               streq(mnem, "div") || streq(mnem, "idiv")) {
        if (streq(mnem, "mul"))
            set_known(&t, "mul r/m", 2, 4, 24, 41, op_is_mem(ops[0].cls),
                      op_is_mem(ops[0].cls), V20_MIX_OTHER);
        else if (streq(mnem, "imul"))
            set_known(&t, "imul r/m", 2, 4, 30, 47, op_is_mem(ops[0].cls),
                      op_is_mem(ops[0].cls), V20_MIX_OTHER);
        else
            set_known(&t, "div/idiv r/m", 2, 4, 80, 180,
                      op_is_mem(ops[0].cls), op_is_mem(ops[0].cls),
                      V20_MIX_OTHER);
    } else if (streq(mnem, "lodsb")) {
        set_known(&t, "lodsb", 1, 1, 7, 7, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "lodsw")) {
        set_known(&t, "lodsw", 1, 1, 11, 11, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "stosb")) {
        set_known(&t, "stosb", 1, 1, 7, 7, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "stosw")) {
        set_known(&t, "stosw", 1, 1, 11, 11, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "movsb")) {
        set_known(&t, "movsb", 1, 1, 11, 11, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "movsw")) {
        set_known(&t, "movsw", 1, 1, 19, 19, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "cmpsb")) {
        set_known(&t, "cmpsb", 1, 1, 13, 13, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "cmpsw")) {
        set_known(&t, "cmpsw", 1, 1, 21, 21, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "scasb")) {
        set_known(&t, "scasb", 1, 1, 7, 7, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "scasw")) {
        set_known(&t, "scasw", 1, 1, 11, 11, 1, 1, V20_MIX_STRING);
    } else if (streq(mnem, "insb") || streq(mnem, "outsb")) {
        set_known(&t, mnem, 1, 1, 10, 10, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "insw") || streq(mnem, "outsw")) {
        set_known(&t, mnem, 1, 1, 18, 18, 2, 2, V20_MIX_STRING);
    } else if (streq(mnem, "xlat")) {
        set_known(&t, "xlat", 1, 1, 11, 11, 1, 1, V20_MIX_MEMORY);
    } else if (streq(mnem, "aaa")) {
        set_known(&t, "aaa", 1, 1, 3, 3, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "aas") || streq(mnem, "aad")) {
        set_known(&t, mnem, streq(mnem, "aad") ? 2 : 1,
                  streq(mnem, "aad") ? 2 : 1, 7, 7, 0, 0,
                  V20_MIX_OTHER);
    } else if (streq(mnem, "aam")) {
        set_known(&t, "aam", 2, 2, 15, 15, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "daa") || streq(mnem, "das")) {
        set_known(&t, mnem, 1, 1, 3, 3, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "bext") || streq(mnem, "bins")) {
        set_known(&t, mnem, 2, 4, 34, 59, 1, 2, V20_MIX_OTHER);
    } else if (streq(mnem, "rol4") || streq(mnem, "ror4")) {
        set_known(&t, mnem, 2, 4, 29, 29, 2, 2, V20_MIX_MEMORY);
    } else if (streq(mnem, "add4s") || streq(mnem, "sub4s")) {
        set_variable(&t, mnem, 2, 2, "7+19*n", "3*n",
                     V20_MIX_STRING);
    } else if (streq(mnem, "cmp4s")) {
        set_variable(&t, "cmp4s", 2, 2, "7+19*n", "2",
                     V20_MIX_STRING);
    } else if (streq(mnem, "retem")) {
        set_known(&t, "retem", 2, 2, 39, 39, 3, 3, V20_MIX_BRANCH);
    } else if (streq(mnem, "fp01") || streq(mnem, "fp02")) {
        if (nops >= 2 && op_is_mem(ops[1].cls))
            set_known(&t, mnem, 2, 4, 15, 15, 1, 1, V20_MIX_MEMORY);
        else
            set_known(&t, mnem, 2, 2, 2, 2, 0, 0, V20_MIX_OTHER);
    } else if (streq(mnem, "test1") || streq(mnem, "clr1") ||
               streq(mnem, "set1") || streq(mnem, "not1")) {
        bool mem = nops > 0 && op_is_mem(ops[0].cls);
        bool word = nops > 0 && (ops[0].cls == OP_REG16 ||
                                 ops[0].cls == OP_MEM16);
        bool imm = nops > 1 && ops[1].cls == OP_IMM;
        int clocks = 0, transfers = 0;
        if (streq(mnem, "test1")) {
            clocks = mem ? (word ? 16 : 12) : (imm ? 4 : 3);
            transfers = mem ? 1 : (imm ? 0 : 1);
        } else if (streq(mnem, "clr1")) {
            clocks = mem ? (word ? 23 : 15) : (imm ? 6 : 5);
            transfers = mem ? 2 : 0;
        } else if (streq(mnem, "set1")) {
            clocks = mem ? (word ? 22 : 14) : (imm ? 5 : 4);
            transfers = mem ? 2 : 0;
        } else {
            clocks = mem ? (word ? 27 : 19) : (imm ? 5 : 4);
            transfers = mem ? 2 : 0;
        }
        set_known(&t, mnem, imm ? 4 : 3, mem ? (imm ? 6 : 5) :
                  (imm ? 4 : 3), clocks, clocks, transfers, transfers,
                  mem ? V20_MIX_MEMORY : V20_MIX_OTHER);
    } else if (streq(mnem, "bound")) {
        set_known(&t, "bound", 2, 4, 27, 35, 2, 2, V20_MIX_MEMORY);
    } else if (streq(mnem, "int")) {
        set_known(&t, "int imm8", 2, 2, 50, 50, 3, 3, V20_MIX_CALL);
    } else if (streq(mnem, "into")) {
        set_known(&t, "into", 1, 1, 4, 52, 0, 3, V20_MIX_CALL);
    }

    add_seg_override_cost(&t, ops, nops);
    if (!t.known)
        snprintf(t.form, sizeof(t.form), "unknown");
    return t;
}

const char *v20_mix_name(v20_mix_t mix) {
    switch (mix) {
    case V20_MIX_MEMORY: return "memory";
    case V20_MIX_PUSH_POP: return "push-pop";
    case V20_MIX_CALL: return "call";
    case V20_MIX_BRANCH: return "branch";
    case V20_MIX_SEGMENT: return "segment";
    case V20_MIX_IO: return "io";
    case V20_MIX_SHIFT: return "shift";
    case V20_MIX_STRING: return "string";
    case V20_MIX_OTHER:
    default:
        return "other";
    }
}
