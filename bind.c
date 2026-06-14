/*
 * bind.c — Nib binder
 *
 * Reads .nir files, performs register allocation, and emits .asm.
 *
 * Pipeline:
 *   1. Parse IR into internal representation
 *   2. Build basic blocks and control flow graph
 *   3. Liveness analysis (backward dataflow)
 *   4. Build interference graph
 *   5. Graph coloring with register classes, pre-coloring, aliases
 *   6. Spill if needed (reserve BP, re-allocate)
 *   7. Emit .asm with physical registers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

/* ================================================================
 * Physical register model
 * ================================================================ */

/* Physical register IDs — unified numbering */
enum {
    /* Word registers */
    PREG_AX = 0, PREG_CX, PREG_DX, PREG_BX,
    PREG_SP, PREG_BP, PREG_SI, PREG_DI,
    /* Byte registers */
    PREG_AL = 8, PREG_CL, PREG_DL, PREG_BL,
    PREG_AH, PREG_CH, PREG_DH, PREG_BH,
    /* Segment registers */
    PREG_ES = 16, PREG_CS, PREG_SS, PREG_DS,
    /* Special */
    PREG_NONE = -1,
    NUM_PREGS = 20
};

static const char *preg_name[] = {
    "AX","CX","DX","BX","SP","BP","SI","DI",
    "AL","CL","DL","BL","AH","CH","DH","BH",
    "ES","CS","SS","DS"
};

/* Alias table: word register <-> byte halves */
static int preg_alias_lo[] = { PREG_AL, PREG_CL, PREG_DL, PREG_BL }; /* AX->AL etc */
static int preg_alias_hi[] = { PREG_AH, PREG_CH, PREG_DH, PREG_BH }; /* AX->AH etc */
static int preg_alias_parent[] = {
    [PREG_AL]=PREG_AX, [PREG_CL]=PREG_CX, [PREG_DL]=PREG_DX, [PREG_BL]=PREG_BX,
    [PREG_AH]=PREG_AX, [PREG_CH]=PREG_CX, [PREG_DH]=PREG_DX, [PREG_BH]=PREG_BX,
};

/* Check if two physical registers alias */
static bool pregs_alias(int a, int b) {
    if (a == b) return true;
    /* Word vs its byte halves */
    if (a < 4 && (b == preg_alias_lo[a] || b == preg_alias_hi[a])) return true;
    if (b < 4 && (a == preg_alias_lo[b] || a == preg_alias_hi[b])) return true;
    return false;
}

/* Parse a register name to PREG_* */
static int parse_preg(const char *s) {
    for (int i = 0; i < NUM_PREGS; i++)
        if (strcasecmp(s, preg_name[i]) == 0)
            return i;
    return PREG_NONE;
}


/* ================================================================
 * IR representation
 * ================================================================ */

#define MAX_VREGS    256
#define MAX_INSNS    4096
#define MAX_BLOCKS   256
#define MAX_FNS      128
#define MAX_LABELS   512

/* IR instruction opcodes */
typedef enum {
    IR_MOV,         /* mov %d, %s  or  mov %d, imm */
    IR_ALU,         /* add/sub/etc %d, %l, %r */
    IR_UNARY,       /* neg/not/etc %d, %s */
    IR_CMP,         /* cmp.xx %d, %l, %r */
    IR_JZ,          /* jz %cond, label */
    IR_JMP,         /* jmp label */
    IR_CALL,        /* call %d, name, args... */
    IR_TAILCALL,    /* tailcall name, args... */
    IR_GOTO_FN,     /* goto.fn name — raw jump to function, no cleanup */
    IR_CJMP,        /* conditional jump: jcc label (flag-check blocks) */
    IR_RET,         /* ret */
    IR_RETVAL,      /* retval %s */
    IR_LOAD,        /* load %d, %base[%idx] */
    IR_STORE,       /* store %base[%idx], %val */
    IR_LOADMEM,     /* loadmem %d, [addr] */
    IR_STOREMEM,    /* storemem [addr], %val */
    IR_FIELD,       /* field %d, %obj, name */
    IR_STOREFIELD,  /* storefield %obj, name, %val */
    IR_BOUND,       /* bound %idx, %arr */
    IR_SETFLAG,     /* setflag FLAG, %val */
    IR_GETFLAG,     /* getflag %d, FLAG */
    IR_TOGGLEFLAG,  /* toggleflag FLAG */
    IR_LOOP,        /* loop label */
    IR_BREAK,       /* break */
    IR_CONTINUE,    /* continue */
    IR_ASM,         /* asm block (opaque) */
    IR_PREFER,      /* .prefer %v, REG (directive, not real insn) */
    IR_LABEL,       /* label: */
    IR_NOP,         /* placeholder */
} ir_op_t;

typedef struct {
    ir_op_t op;
    int     dst;            /* vreg written, or -1 */
    int     src1, src2;     /* vregs read, or -1 */
    int     extra_args[8];  /* for calls with >2 args */
    int     nargs;          /* total args for calls */
    int     imm;            /* immediate value */
    bool    has_imm;
    char    name[64];       /* label target, function name, alu op name */
    char    asm_body[512];  /* for IR_ASM */
    char    asm_ann[128];   /* clobbers/preserves text */
    int     label_id;       /* resolved label index for jumps */
    int     line;           /* source line */
    char    src_file[64];   /* source filename for debug */
} ir_insn_t;

/* Virtual register info */
typedef struct {
    int     prefer;         /* preferred physical reg (PREG_*) or PREG_NONE */
    bool    is_byte;        /* true if this vreg is 8-bit */
    bool    is_seg;         /* true if segment register */
    bool    needs_addressable; /* true if used in memory operand (any of base/index) */
    bool    needs_base;        /* true if used as base: must be BX or BP */
    bool    needs_index;       /* true if used as index: must be SI or DI */
    int     assigned;       /* physical reg after coloring, or PREG_NONE */
    int     spill_slot;     /* stack offset if spilled, or -1 */
    /* Liveness */
    int     def_pos;        /* first def position */
    int     last_use;       /* last use position */
    bool    live;           /* currently live in analysis */
} vreg_info_t;

/* Basic block */
typedef struct {
    int     start, end;     /* insn index range [start, end) */
    int     succs[4];       /* successor block indices */
    int     nsuccs;
    int     preds[16];      /* predecessor block indices */
    int     npreds;
    uint64_t live_in;       /* bitset of live vregs at block entry */
    uint64_t live_out;      /* bitset of live vregs at block exit */
} bblock_t;

/* Function */
typedef struct {
    char        name[64];
    char        module[64];     /* source module name (from .nir filename) */
    bool        is_pub;
    bool        is_far;
    bool        is_interrupt;
    int         int_vector;
    bool        is_reentrant;
    bool        has_chain;
    char        chain_name[64];
    bool        has_at;
    int         at_seg;
    int         at_off;

    ir_insn_t   insns[MAX_INSNS];
    int         ninsns;

    vreg_info_t vregs[MAX_VREGS];
    int         nvregs;

    int         nparams;
    char        param_names[16][64];
    int         param_vregs[16];
    struct { int preg; } param_pins[16]; /* pinned register for params */

    bool        has_return;
    char        return_type[32];
    int         ret_pin;            /* PREG_NONE or pinned return register */

    bblock_t    blocks[MAX_BLOCKS];
    int         nblocks;

    /* Labels: name -> insn index */
    struct { char name[64]; int insn_idx; } labels[MAX_LABELS];
    int         nlabels;

    /* Allocation state */
    bool        needs_frame;    /* BP reserved for frame pointer */
    int         nspill_slots;
    int         frame_size;     /* total bytes for spills */

    /* Callee-saved registers */
    int         fn_preserves[NUM_PREGS]; /* list of PREG_* to save/restore */
    int         nfn_preserves;
} func_t;

/* Extern function declarations */
typedef struct {
    char name[64];
    bool is_far;
    bool is_interrupt;
    int  int_vector;
    bool has_address;
    int  addr_seg;
    int  addr_off;
    struct { int preg; } param_pins[16];
    int  nparams;
} extern_fn_t;

#define MAX_EXTERNS 64
static extern_fn_t externs[MAX_EXTERNS];
static int nexterns = 0;

/* Constant pool */
#define MAX_CONSTS 256
typedef struct {
    char label[32];
    char data[256];     /* raw text to emit as db/dw */
    bool is_far_ref;    /* true if this is a far.ref to a function */
    char ref_name[64];  /* function name for far.ref resolution */
} const_entry_t;

static const_entry_t consts[MAX_CONSTS];
static int nconsts = 0;

/* Data blocks (initialized globals with placement) */
#define MAX_DATA_BLOCKS 64
#define MAX_DATA_ENTRIES 1024
typedef struct {
    char label[64];
    bool has_at;
    int  at_seg;
    int  at_off;
    /* Entries: raw assembly lines to emit */
    char entries[MAX_DATA_ENTRIES][512];
    int  nentries;
} data_block_t;

static data_block_t data_blocks[MAX_DATA_BLOCKS];
static int ndata_blocks = 0;

/* Global variable declarations */
#define MAX_GLOBALS 256
typedef struct {
    char name[64];
    char type[32];
    int  size;          /* bytes of storage */
} global_var_t;

static global_var_t globals[MAX_GLOBALS];
static int nglobals = 0;

/* Global binder state */
static func_t functions[MAX_FNS];
static int nfunctions = 0;
static FILE *out_asm = NULL;

/* ================================================================
 * IR parser
 * ================================================================ */

static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *read_word(char *p, char *buf, int bufsz) {
    p = skip_ws(p);
    int i = 0;
    while (*p && !isspace(*p) && *p != ',' && *p != ')' && *p != ']' && i < bufsz-1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return p;
}

static int parse_vreg(char *p, char **endp) {
    p = skip_ws(p);
    if (*p != '%') { *endp = p; return -1; }
    p++;
    int v = 0;
    while (isdigit(*p)) { v = v * 10 + (*p - '0'); p++; }
    *endp = p;
    return v;
}

static void skip_comma(char **p) {
    *p = skip_ws(*p);
    if (**p == ',') (*p)++;
}

static void parse_function(FILE *fp, func_t *fn, char *first_line) {
    /* Parse .fn header: .fn name[, modifiers] */
    char *p = first_line + 3; /* skip ".fn" */
    p = skip_ws(p);
    read_word(p, fn->name, sizeof(fn->name));
    p += strlen(fn->name);

    /* Parse optional modifiers */
    while (*p) {
        p = skip_ws(p);
        if (*p == ',') p++;
        p = skip_ws(p);
        char word[64];
        read_word(p, word, sizeof(word));
        if (!word[0]) break;
        p += strlen(word);

        if (strcmp(word, "pub") == 0) fn->is_pub = true;
        else if (strcmp(word, "far") == 0) fn->is_far = true;
        else if (strcmp(word, "reentrant") == 0) fn->is_reentrant = true;
        else if (strncmp(word, "interrupt(", 10) == 0) {
            fn->is_interrupt = true;
            fn->int_vector = (int)strtol(word + 10, NULL, 0);
            /* Skip the ) that read_word left behind */
            p = skip_ws(p);
            if (*p == ')') p++;
        }
        else if (strncmp(word, "at(", 3) == 0) {
            fn->has_at = true;
            /* word is "at(0xSEG:0xOFF" — colon inside the word */
            char *colon = strchr(word + 3, ':');
            if (colon) {
                fn->at_seg = (int)strtol(word + 3, NULL, 0);
                fn->at_off = (int)strtol(colon + 1, NULL, 0);
            }
            p = skip_ws(p);
            if (*p == ')') p++;
        }
        else if (strncmp(word, "chain(", 6) == 0) {
            fn->has_chain = true;
            /* read_word stops at ), so the name is everything after "chain(" */
            int len = strlen(word + 6);
            memcpy(fn->chain_name, word + 6, len);
            fn->chain_name[len] = '\0';
            /* Skip the ) that read_word left behind */
            p = skip_ws(p);
            if (*p == ')') p++;
        }
    }

    /* Read body lines until .endfn */
    char cur_dbg_file[64] = "";
    int cur_dbg_line = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        p = skip_ws(line);
        if (!*p) continue;
        /* Capture ; @file:line debug comments */
        if (strncmp(p, "; @", 3) == 0) {
            /* Parse file:line — store for next instruction */
            char *colon = strchr(p + 3, ':');
            if (colon) {
                int flen = (int)(colon - (p + 3));
                if (flen > 63) flen = 63;
                memcpy(cur_dbg_file, p + 3, flen);
                cur_dbg_file[flen] = '\0';
                cur_dbg_line = atoi(colon + 1);
            }
            continue;
        }
        if (*p == ';') continue; /* other comments */

        /* Directives */
        if (strncmp(p, ".endfn", 6) == 0) break;

        if (strncmp(p, ".param", 6) == 0) {
            p += 6;
            int v = parse_vreg(p, &p);
            skip_comma(&p);
            char type[32];
            p = read_word(p, type, sizeof(type));
            skip_comma(&p);
            /* Parse quoted name */
            p = skip_ws(p);
            int pidx = fn->nparams;
            if (*p == '"') {
                p++;
                char *e = strchr(p, '"');
                int nlen = e ? (int)(e - p) : 0;
                if (pidx < 16) {
                    memcpy(fn->param_names[pidx], p, nlen);
                    fn->param_names[pidx][nlen] = '\0';
                    fn->param_vregs[pidx] = v;
                    fn->nparams++;
                }
                if (e) p = e + 1;
            }
            /* Check for ", in REG" */
            char *in_ptr = strstr(p, " in ");
            if (in_ptr && pidx < 16) {
                char reg[16];
                read_word(in_ptr + 4, reg, sizeof(reg));
                fn->param_pins[pidx].preg = parse_preg(reg);
                /* Set as hard preference on the vreg */
                if (v < MAX_VREGS && fn->param_pins[pidx].preg != PREG_NONE)
                    fn->vregs[v].prefer = fn->param_pins[pidx].preg;
            }
            if (v >= fn->nvregs) fn->nvregs = v + 1;
            /* Set type info */
            if (v < MAX_VREGS) {
                fn->vregs[v].is_byte = (strcmp(type, "u8") == 0);
                fn->vregs[v].is_seg = (strcmp(type, "seg") == 0);
            }
            continue;
        }

        if (strncmp(p, ".returns", 8) == 0) {
            p += 8;
            p = skip_ws(p);
            p = read_word(p, fn->return_type, sizeof(fn->return_type));
            fn->has_return = true;
            /* Check for ", in REG" */
            char *in_ptr = strstr(p, "in ");
            if (in_ptr) {
                char reg[16];
                read_word(in_ptr + 3, reg, sizeof(reg));
                fn->ret_pin = parse_preg(reg);
            }
            continue;
        }

        if (strncmp(p, ".const ", 7) == 0) {
            /* .const _C0, "Hello" or .const _C1, far 0xF000:0x0100 */
            p += 7;
            if (nconsts < MAX_CONSTS) {
                const_entry_t *c = &consts[nconsts++];
                p = read_word(p, c->label, sizeof(c->label));
                /* skip comma */
                p = skip_ws(p);
                if (*p == ',') p++;
                p = skip_ws(p);
                /* Copy label locally to avoid restrict overlap in snprintf */
                char lbl[32];
                memcpy(lbl, c->label, sizeof(lbl));
                /* Parse the data */
                if (*p == '"') {
                    /* String constant */
                    p++;
                    char *end = strrchr(p, '"');
                    int dlen = end ? (int)(end - p) : (int)strlen(p);
                    /* Build db line */
                    int pos = 0;
                    pos += snprintf(c->data + pos, sizeof(c->data) - pos,
                                    "%s db ", lbl);
                    for (int i = 0; i < dlen && pos < (int)sizeof(c->data) - 10; i++) {
                        if (i > 0) pos += snprintf(c->data + pos, sizeof(c->data) - pos, ", ");
                        if (p[i] == '\\' && i + 3 < dlen && p[i+1] == 'x') {
                            /* hex escape */
                            char hex[3] = { p[i+2], p[i+3], 0 };
                            pos += snprintf(c->data + pos, sizeof(c->data) - pos,
                                            "0x%s", hex);
                            i += 3;
                        } else {
                            pos += snprintf(c->data + pos, sizeof(c->data) - pos,
                                            "'%c'", p[i]);
                        }
                    }
                } else if (strncmp(p, "far.ref ", 8) == 0) {
                    /* Far function reference: far.ref function_name
                     * Deferred to assembler — emit label + SEG operator */
                    p += 8;
                    char fname[64];
                    p = read_word(p, fname, sizeof(fname));
                    c->is_far_ref = true;
                    strncpy(c->ref_name, fname, 63);
                    c->ref_name[63] = '\0';
                    snprintf(c->data, sizeof(c->data),
                             "%s dw %s, SEG %s", lbl, fname, fname);
                } else if (strncmp(p, "far ", 4) == 0) {
                    /* Far constant: far 0xSEG:0xOFF */
                    p += 4;
                    int seg = (int)strtol(p, (char **)&p, 0);
                    if (*p == ':') p++;
                    int off = (int)strtol(p, (char **)&p, 0);
                    snprintf(c->data, sizeof(c->data),
                             "%s dw 0x%04X, 0x%04X", lbl, off, seg);
                }
            }
            continue;
        }

        if (strncmp(p, ".prefer", 7) == 0) {
            p += 7;
            int v = parse_vreg(p, &p);
            skip_comma(&p);
            char reg[16];
            read_word(p, reg, sizeof(reg));
            int preg = parse_preg(reg);
            if (v >= 0 && v < MAX_VREGS) {
                fn->vregs[v].prefer = preg;
            }
            if (v >= fn->nvregs) fn->nvregs = v + 1;
            /* Also add as IR_PREFER so we track it */
            ir_insn_t *ins = &fn->insns[fn->ninsns++];
            memset(ins, 0, sizeof(*ins));
            ins->op = IR_PREFER;
            ins->dst = v;
            ins->src1 = ins->src2 = -1;
            ins->imm = preg;
            continue;
        }

        if (strncmp(p, ".preserves", 10) == 0) {
            p += 10;
            /* Parse comma-separated register list */
            while (*p) {
                char reg[16];
                p = skip_ws(p);
                if (!*p || *p == '\n') break;
                p = read_word(p, reg, sizeof(reg));
                if (!reg[0]) break;
                int preg = parse_preg(reg);
                if (preg != PREG_NONE && fn->nfn_preserves < NUM_PREGS) {
                    fn->fn_preserves[fn->nfn_preserves++] = preg;
                }
                p = skip_ws(p);
                if (*p == ',') p++;
            }
            continue;
        }

        /* Labels */
        if (*p == '.') {
            /* Could be .L0: or similar */
            char *colon = strchr(p, ':');
            if (colon && colon[1] == '\0') {
                /* It's a label */
                *colon = '\0';
                ir_insn_t *ins = &fn->insns[fn->ninsns];
                memset(ins, 0, sizeof(*ins));
                ins->op = IR_LABEL;
                ins->dst = ins->src1 = ins->src2 = -1;
                strncpy(ins->name, p, 63);
                /* Record label position */
                if (fn->nlabels < MAX_LABELS) {
                    strncpy(fn->labels[fn->nlabels].name, p, 63);
                    fn->labels[fn->nlabels].insn_idx = fn->ninsns;
                    fn->nlabels++;
                }
                fn->ninsns++;
                continue;
            }
        }

        /* User labels (no dot prefix) */
        {
            char *colon = strchr(p, ':');
            if (colon && colon[1] == '\0' && isalpha(*p)) {
                *colon = '\0';
                ir_insn_t *ins = &fn->insns[fn->ninsns];
                memset(ins, 0, sizeof(*ins));
                ins->op = IR_LABEL;
                ins->dst = ins->src1 = ins->src2 = -1;
                strncpy(ins->name, p, 63);
                if (fn->nlabels < MAX_LABELS) {
                    strncpy(fn->labels[fn->nlabels].name, p, 63);
                    fn->labels[fn->nlabels].insn_idx = fn->ninsns;
                    fn->nlabels++;
                }
                fn->ninsns++;
                continue;
            }
        }

        /* Instructions */
        ir_insn_t *ins = &fn->insns[fn->ninsns];
        memset(ins, 0, sizeof(*ins));
        ins->dst = ins->src1 = ins->src2 = -1;
        for (int i = 0; i < 8; i++) ins->extra_args[i] = -1;

        char opname[64];
        p = read_word(p, opname, sizeof(opname));

        if (strcmp(opname, "mov") == 0) {
            ins->op = IR_MOV;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            /* Could be vreg, immediate, or label */
            p = skip_ws(p);
            if (*p == '%') {
                ins->src1 = parse_vreg(p, &p);
            } else if (*p == '_' || isalpha(*p)) {
                /* Label reference (e.g., _C0 for constant pool) */
                ins->has_imm = false;
                p = read_word(p, ins->name, sizeof(ins->name));
            } else {
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
            }
        }
        else if (strcmp(opname, "ret") == 0) {
            ins->op = IR_RET;
        }
        else if (strcmp(opname, "retval") == 0) {
            ins->op = IR_RETVAL;
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "jmp") == 0) {
            ins->op = IR_JMP;
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "jz") == 0) {
            ins->op = IR_JZ;
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "call") == 0) {
            ins->op = IR_CALL;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            p = read_word(p, ins->name, sizeof(ins->name));
            /* Parse args */
            ins->nargs = 0;
            while (*p) {
                skip_comma(&p);
                p = skip_ws(p);
                if (*p != '%') break;
                int a = parse_vreg(p, &p);
                if (ins->nargs == 0) ins->src1 = a;
                else if (ins->nargs == 1) ins->src2 = a;
                else if (ins->nargs - 2 < 8) ins->extra_args[ins->nargs - 2] = a;
                ins->nargs++;
            }
        }
        else if (strcmp(opname, "goto.fn") == 0) {
            ins->op = IR_GOTO_FN;
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "tailcall") == 0) {
            ins->op = IR_TAILCALL;
            p = read_word(p, ins->name, sizeof(ins->name));
            ins->nargs = 0;
            while (*p) {
                skip_comma(&p);
                p = skip_ws(p);
                if (*p != '%') break;
                int a = parse_vreg(p, &p);
                if (ins->nargs == 0) ins->src1 = a;
                else if (ins->nargs == 1) ins->src2 = a;
                else if (ins->nargs - 2 < 8) ins->extra_args[ins->nargs - 2] = a;
                ins->nargs++;
            }
        }
        else if (strcmp(opname, "load") == 0) {
            ins->op = IR_LOAD;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p); /* array */
            p = skip_ws(p);
            if (*p == '[') { p++; ins->src2 = parse_vreg(p, &p); } /* index */
        }
        else if (strcmp(opname, "store") == 0) {
            ins->op = IR_STORE;
            ins->src1 = parse_vreg(p, &p); /* array */
            p = skip_ws(p);
            if (*p == '[') { p++; ins->src2 = parse_vreg(p, &p); } /* index */
            skip_comma(&p);
            /* skip ], */
            p = skip_ws(p);
            if (*p == ']') p++;
            skip_comma(&p);
            ins->dst = parse_vreg(p, &p); /* value — stored as "dst" but really src */
        }
        else if (strcmp(opname, "loadmem") == 0) {
            ins->op = IR_LOADMEM;
            ins->dst = parse_vreg(p, &p);
            /* Rest is address text — store as name */
            skip_comma(&p);
            p = skip_ws(p);
            strncpy(ins->name, p, 63);
        }
        else if (strcmp(opname, "storemem") == 0) {
            ins->op = IR_STOREMEM;
            /* Address text then vreg */
            p = skip_ws(p);
            /* Read until ], */
            char *bracket = strrchr(p, ']');
            if (bracket) {
                int alen = (int)(bracket - p + 1);
                memcpy(ins->name, p, alen);
                ins->name[alen] = '\0';
                p = bracket + 1;
                skip_comma(&p);
                ins->src1 = parse_vreg(p, &p);
            }
        }
        else if (strcmp(opname, "field") == 0) {
            ins->op = IR_FIELD;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "storefield") == 0) {
            ins->op = IR_STOREFIELD;
            ins->src1 = parse_vreg(p, &p); /* obj */
            skip_comma(&p);
            p = read_word(p, ins->name, sizeof(ins->name));
            skip_comma(&p);
            ins->src2 = parse_vreg(p, &p); /* val */
        }
        else if (strcmp(opname, "bound") == 0) {
            ins->op = IR_BOUND;
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src2 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "setflag") == 0) {
            ins->op = IR_SETFLAG;
            p = read_word(p, ins->name, sizeof(ins->name));
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "getflag") == 0) {
            ins->op = IR_GETFLAG;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "toggleflag") == 0) {
            ins->op = IR_TOGGLEFLAG;
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "loop") == 0) {
            ins->op = IR_LOOP;
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "break") == 0) {
            ins->op = IR_BREAK;
        }
        else if (strcmp(opname, "continue") == 0) {
            ins->op = IR_CONTINUE;
        }
        else if (strncmp(opname, "far.", 4) == 0) {
            /* far.off, far.seg, far.lit — far pointer operations */
            strncpy(ins->name, opname, 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            if (strcmp(opname, "far.lit") == 0) {
                /* far.lit %d, seg, off — store 4 bytes on stack */
                ins->op = IR_ASM;
                p = skip_ws(p);
                int seg_val = (int)strtol(p, (char **)&p, 0);
                skip_comma(&p);
                int off_val = (int)strtol(p, (char **)&p, 0);
                snprintf(ins->asm_body, sizeof(ins->asm_body),
                    "    sub sp, 4\n"
                    "    mov word [sp], 0x%04X\n"
                    "    mov word [sp+2], 0x%04X\n"
                    "    mov %%_%d, sp",
                    off_val, seg_val, ins->dst);
                ins->asm_ann[0] = '\0';
            } else {
                /* far.off / far.seg — load word from [ptr] or [ptr+2] */
                ins->op = IR_ALU;
                ins->src1 = parse_vreg(p, &p);
                /* Mark src1 as needing addressable register */
                if (ins->src1 >= 0 && ins->src1 < MAX_VREGS)
                    fn->vregs[ins->src1].needs_addressable = true;
            }
        }
        else if (strcmp(opname, "hlt") == 0 || strcmp(opname, "nop") == 0 ||
                 strcmp(opname, "salc") == 0 || strcmp(opname, "into") == 0 ||
                 strcmp(opname, "rep") == 0 || strcmp(opname, "repe") == 0 ||
                 strcmp(opname, "repne") == 0 ||
                 /* Conditional jumps from flag-check blocks */
                 strcmp(opname, "int") == 0) {
            /* Pass-through: emit as literal assembly */
            ins->op = IR_ASM;
            p = skip_ws(p);
            snprintf(ins->asm_body, sizeof(ins->asm_body), "    %s %s", opname, p);
            ins->asm_ann[0] = '\0';
        }
        else if (strcmp(opname, "jc") == 0 || strcmp(opname, "jnc") == 0 ||
                 strcmp(opname, "jo") == 0 || strcmp(opname, "jno") == 0 ||
                 strcmp(opname, "jz") == 0 || strcmp(opname, "jnz") == 0 ||
                 strcmp(opname, "js") == 0 || strcmp(opname, "jns") == 0 ||
                 strcmp(opname, "jp") == 0 || strcmp(opname, "jnp") == 0) {
            /* Flag-check conditional jump — store mnemonic and label separately */
            ins->op = IR_CJMP;
            strncpy(ins->asm_ann, opname, sizeof(ins->asm_ann) - 1);
            p = skip_ws(p);
            read_word(p, ins->name, sizeof(ins->name));
        }
        else if (strcmp(opname, "in") == 0) {
            ins->op = IR_ALU;
            strncpy(ins->name, "in", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%') {
                ins->src1 = parse_vreg(p, &p);
            } else {
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
            }
        }
        else if (strcmp(opname, "out") == 0) {
            ins->op = IR_ALU;
            strncpy(ins->name, "out", 63);
            p = skip_ws(p);
            if (*p == '%') {
                ins->dst = parse_vreg(p, &p);
                skip_comma(&p);
                ins->src1 = parse_vreg(p, &p);
            } else {
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
                skip_comma(&p);
                ins->src1 = parse_vreg(p, &p);
            }
        }
        else if (strcmp(opname, "cbw") == 0) {
            ins->op = IR_UNARY;
            strncpy(ins->name, "cbw", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "zext") == 0) {
            ins->op = IR_UNARY;
            strncpy(ins->name, "zext", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "xlat") == 0) {
            ins->op = IR_ALU;
            strncpy(ins->name, "xlat", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src2 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "lods") == 0) {
            /* LODSB/LODSW: load from [SI], advance SI */
            ins->op = IR_UNARY;
            strncpy(ins->name, "lods", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "stos") == 0) {
            /* STOSB/STOSW: store to [DI], advance DI */
            ins->op = IR_ALU;
            strncpy(ins->name, "stos", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "bext") == 0) {
            /* V20 EXT: extract bit field */
            ins->op = IR_ALU;
            strncpy(ins->name, "bext", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src2 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->extra_args[0] = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "bins") == 0) {
            /* V20 INS: insert bit field */
            ins->op = IR_ALU;
            strncpy(ins->name, "bins", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src2 = parse_vreg(p, &p);
            skip_comma(&p);
            ins->extra_args[0] = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "rol4") == 0 || strcmp(opname, "ror4") == 0) {
            /* V20 ROL4/ROR4: nibble rotation */
            ins->op = IR_UNARY;
            strncpy(ins->name, opname, 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "swap_flags") == 0) {
            /* LAHF/SAHF: exchange AH with flags */
            ins->op = IR_UNARY;
            strncpy(ins->name, "swap_flags", 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "brkem") == 0) {
            ins->op = IR_ASM;
            snprintf(ins->asm_body, sizeof(ins->asm_body), "    brkem %s", p);
            ins->asm_ann[0] = '\0';
        }
        else if (strcmp(opname, "asm") == 0) {
            ins->op = IR_ASM;
            p = skip_ws(p);
            /* Find the { */
            char *brace = strchr(p, '{');
            if (brace) {
                /* Annotation is everything before { */
                int alen = (int)(brace - p);
                memcpy(ins->asm_ann, p, alen);
                ins->asm_ann[alen] = '\0';
                /* Body: check if } is on same line */
                char *end = strrchr(brace, '}');
                if (end) {
                    int blen = (int)(end - brace - 1);
                    memcpy(ins->asm_body, brace + 1, blen);
                    ins->asm_body[blen] = '\0';
                } else {
                    /* Multi-line asm: read until } */
                    ins->asm_body[0] = '\0';
                    /* Copy remainder of current line after { */
                    strcat(ins->asm_body, brace + 1);
                    char asmline[512];
                    while (fgets(asmline, sizeof(asmline), fp)) {
                        int alen2 = strlen(asmline);
                        while (alen2 > 0 && (asmline[alen2-1] == '\n' || asmline[alen2-1] == '\r'))
                            asmline[--alen2] = '\0';
                        char *closing = strchr(asmline, '}');
                        if (closing) {
                            *closing = '\0';
                            if (strlen(ins->asm_body) + strlen(asmline) < sizeof(ins->asm_body) - 2) {
                                strcat(ins->asm_body, "\n");
                                strcat(ins->asm_body, asmline);
                            }
                            break;
                        }
                        if (strlen(ins->asm_body) + strlen(asmline) < sizeof(ins->asm_body) - 2) {
                            strcat(ins->asm_body, "\n");
                            strcat(ins->asm_body, asmline);
                        }
                    }
                }
            }
        }
        else {
            /* ALU / CMP / unary — three-operand: op %d, %l, %r
               or two-operand: op %d, %s */
            strncpy(ins->name, opname, 63);
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%') {
                ins->src1 = parse_vreg(p, &p);
                skip_comma(&p);
                p = skip_ws(p);
                if (*p == '%') {
                    ins->src2 = parse_vreg(p, &p);
                    if (strncmp(opname, "cmp.", 4) == 0)
                        ins->op = IR_CMP;
                    else
                        ins->op = IR_ALU;
                } else if (*p == '-' || isdigit(*p)) {
                    /* Third operand is immediate */
                    ins->has_imm = true;
                    ins->imm = (int)strtol(p, (char **)&p, 0);
                    ins->src2 = -1;
                    if (strncmp(opname, "cmp.", 4) == 0)
                        ins->op = IR_CMP;
                    else
                        ins->op = IR_ALU;
                } else {
                    ins->op = IR_UNARY;
                }
            } else {
                ins->op = IR_MOV;
                ins->has_imm = true;
                ins->imm = (int)strtol(p, &p, 0);
            }
        }

        /* Track max vreg */
        if (ins->dst >= fn->nvregs) fn->nvregs = ins->dst + 1;
        if (ins->src1 >= fn->nvregs) fn->nvregs = ins->src1 + 1;
        if (ins->src2 >= fn->nvregs) fn->nvregs = ins->src2 + 1;
        for (int i = 0; i < 8; i++)
            if (ins->extra_args[i] >= fn->nvregs)
                fn->nvregs = ins->extra_args[i] + 1;

        /* Attach debug info from last ; @ comment */
        if (cur_dbg_line > 0) {
            ins->line = cur_dbg_line;
            strncpy(ins->src_file, cur_dbg_file, 63);
        }

        fn->ninsns++;
        if (fn->ninsns >= MAX_INSNS) break;
    }
}

static void parse_nir(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }

    /* Extract module name from filename (strip path and .nir extension) */
    char cur_module[64] = "";
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(cur_module, base, 63);
    char *dot = strrchr(cur_module, '.');
    if (dot) *dot = '\0';

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char *p = skip_ws(line);
        if (!*p || *p == ';') continue;

        if (strncmp(p, ".fn ", 4) == 0) {
            if (nfunctions >= MAX_FNS) break;
            func_t *fn = &functions[nfunctions++];
            memset(fn, 0, sizeof(*fn));
            fn->ret_pin = PREG_NONE;
            strncpy(fn->module, cur_module, 63);
            for (int i = 0; i < 16; i++)
                fn->param_pins[i].preg = PREG_NONE;
            for (int i = 0; i < MAX_VREGS; i++) {
                fn->vregs[i].prefer = PREG_NONE;
                fn->vregs[i].assigned = PREG_NONE;
                fn->vregs[i].spill_slot = -1;
            }
            parse_function(fp, fn, p);
        }
        if (strncmp(p, ".extern ", 8) == 0) {
            if (nexterns >= MAX_EXTERNS) continue;
            extern_fn_t *ext = &externs[nexterns++];
            memset(ext, 0, sizeof(*ext));
            p += 8;
            char word[64];
            p = read_word(p, ext->name, sizeof(ext->name));
            /* Parse modifiers */
            while (*p) {
                p = skip_ws(p);
                if (*p == ',') p++;
                p = skip_ws(p);
                read_word(p, word, sizeof(word));
                if (!word[0]) break;
                p += strlen(word);
                if (strcmp(word, "far") == 0) ext->is_far = true;
                else if (strncmp(word, "interrupt(", 10) == 0) {
                    ext->is_interrupt = true;
                    ext->int_vector = (int)strtol(word + 10, NULL, 0);
                    p = skip_ws(p); if (*p == ')') p++;
                }
                else if (strncmp(word, "addr(", 5) == 0) {
                    ext->has_address = true;
                    char *colon = strchr(word + 5, ':');
                    if (colon) {
                        ext->addr_seg = (int)strtol(word + 5, NULL, 0);
                        ext->addr_off = (int)strtol(colon + 1, NULL, 0);
                    }
                    p = skip_ws(p); if (*p == ')') p++;
                }
            }
            /* Skip .eparam lines until .endextern */
            char eline[512];
            int pi = 0;
            while (fgets(eline, sizeof(eline), fp)) {
                char *ep = eline;
                while (*ep == ' ' || *ep == '\t') ep++;
                if (strncmp(ep, ".endextern", 10) == 0) break;
                if (strncmp(ep, ".eparam", 7) == 0) {
                    /* Parse "in REG" if present */
                    char *in_ptr = strstr(ep, " in ");
                    if (in_ptr && pi < 16) {
                        char reg[16];
                        read_word(in_ptr + 4, reg, sizeof(reg));
                        ext->param_pins[pi].preg = parse_preg(reg);
                    }
                    pi++;
                }
            }
            ext->nparams = pi;
        }
        if (strncmp(p, ".data ", 6) == 0) {
            if (ndata_blocks >= MAX_DATA_BLOCKS) continue;
            data_block_t *db = &data_blocks[ndata_blocks++];
            memset(db, 0, sizeof(*db));
            p += 6;
            /* Parse: name, type[, at(seg:off)] */
            p = read_word(p, db->label, sizeof(db->label));
            /* strip trailing comma from label */
            int llen = strlen(db->label);
            if (llen > 0 && db->label[llen-1] == ',') db->label[llen-1] = '\0';
            /* scan for at() anywhere on the rest of the line */
            char *at_ptr = strstr(p, "at(");
            if (at_ptr) {
                db->has_at = true;
                char *colon = strchr(at_ptr + 3, ':');
                if (colon) {
                    db->at_seg = (int)strtol(at_ptr + 3, NULL, 0);
                    db->at_off = (int)strtol(colon + 1, NULL, 0);
                }
            }
            /* Read data entries until .enddata */
            char dline[256];
            while (fgets(dline, sizeof(dline), fp)) {
                char *dp = dline;
                while (*dp == ' ' || *dp == '\t') dp++;
                /* strip newline */
                char *nl = strchr(dp, '\n');
                if (nl) *nl = '\0';
                nl = strchr(dp, '\r');
                if (nl) *nl = '\0';
                if (strncmp(dp, ".enddata", 8) == 0) break;
                if (!*dp || *dp == ';') continue;
                if (db->nentries < MAX_DATA_ENTRIES) {
                    /* Convert IR data entries to assembly */
                    if (strncmp(dp, "far.ref ", 8) == 0) {
                        char fname[64];
                        read_word(dp + 8, fname, sizeof(fname));
                        /* Mark with \x01 prefix for deferred resolution */
                        snprintf(db->entries[db->nentries++], 512,
                                 "\x01%s", fname);
                    } else if (strncmp(dp, "far ", 4) == 0) {
                        int seg = 0, off = 0;
                        char *fp2 = dp + 4;
                        seg = (int)strtol(fp2, &fp2, 0);
                        if (*fp2 == ':') fp2++;
                        off = (int)strtol(fp2, NULL, 0);
                        snprintf(db->entries[db->nentries++], 512,
                                 "    dw 0x%04X, 0x%04X", off, seg);
                    } else if (strncmp(dp, "dw ", 3) == 0) {
                        snprintf(db->entries[db->nentries++], 512,
                                 "    %s", dp);
                    } else if (strncmp(dp, "db ", 3) == 0) {
                        snprintf(db->entries[db->nentries++], 512,
                                 "    %s", dp);
                    } else if (strncmp(dp, "dd ", 3) == 0) {
                        snprintf(db->entries[db->nentries++], 512,
                                 "    %s", dp);
                    }
                }
            }
            continue;
        }
        if (strncmp(p, ".global ", 8) == 0 && nglobals < MAX_GLOBALS) {
            p += 8;
            global_var_t *g = &globals[nglobals];
            p = read_word(p, g->name, sizeof(g->name));
            /* strip trailing comma */
            int nlen = strlen(g->name);
            if (nlen > 0 && g->name[nlen-1] == ',') g->name[nlen-1] = '\0';
            p = skip_ws(p);
            if (*p == ',') p++;
            p = skip_ws(p);
            read_word(p, g->type, sizeof(g->type));
            /* Compute size from type */
            g->size = 2; /* default u16 */
            if (strcmp(g->type, "u8") == 0) g->size = 1;
            else if (strcmp(g->type, "u16") == 0) g->size = 2;
            else if (strcmp(g->type, "u32") == 0) g->size = 4;
            else if (strcmp(g->type, "seg") == 0) g->size = 2;
            else if (strcmp(g->type, "far") == 0) g->size = 4;
            else {
                /* Array type like u8[80] or far[256] */
                char *bracket = strchr(g->type, '[');
                if (bracket) {
                    int count = atoi(bracket + 1);
                    int elem = 1;
                    if (strncmp(g->type, "u16", 3) == 0) elem = 2;
                    else if (strncmp(g->type, "u32", 3) == 0) elem = 4;
                    else if (strncmp(g->type, "far", 3) == 0) elem = 4;
                    g->size = elem * count;
                }
            }
            nglobals++;
            continue;
        }
        /* Skip other top-level directives */
    }

    fclose(fp);
}

/* ================================================================
 * Liveness analysis
 * ================================================================ */

/* Simple linear liveness — no CFG, just scan backward */
static void compute_liveness(func_t *fn) {
    /* Initialize */
    for (int i = 0; i < fn->nvregs; i++) {
        fn->vregs[i].def_pos = -1;
        fn->vregs[i].last_use = -1;
    }

    /* Forward pass: find first def */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->dst >= 0 && fn->vregs[ins->dst].def_pos < 0)
            fn->vregs[ins->dst].def_pos = i;
    }

    /* Backward pass: find last use */
    for (int i = fn->ninsns - 1; i >= 0; i--) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->src1 >= 0 && fn->vregs[ins->src1].last_use < 0)
            fn->vregs[ins->src1].last_use = i;
        if (ins->src2 >= 0 && fn->vregs[ins->src2].last_use < 0)
            fn->vregs[ins->src2].last_use = i;
        /* Call args */
        for (int j = 0; j < 8; j++)
            if (ins->extra_args[j] >= 0 && fn->vregs[ins->extra_args[j]].last_use < 0)
                fn->vregs[ins->extra_args[j]].last_use = i;
        /* dst is also a "use" for stores */
        if ((ins->op == IR_STORE || ins->op == IR_STOREMEM || ins->op == IR_STOREFIELD) &&
            ins->dst >= 0 && fn->vregs[ins->dst].last_use < 0)
            fn->vregs[ins->dst].last_use = i;
    }

    /* Parameters are defined at position -1 (before first insn) */
    for (int i = 0; i < fn->nparams; i++) {
        int v = fn->param_vregs[i];
        if (v >= 0) fn->vregs[v].def_pos = -1;
    }
}

/* Check if two vregs have overlapping live ranges */
static bool vregs_interfere(func_t *fn, int a, int b) {
    int a_start = fn->vregs[a].def_pos;
    int a_end   = fn->vregs[a].last_use;
    int b_start = fn->vregs[b].def_pos;
    int b_end   = fn->vregs[b].last_use;

    if (a_start < 0 || a_end < 0 || b_start < 0 || b_end < 0)
        return false;

    /* Ranges overlap if one starts before the other ends */
    return !(a_end < b_start || b_end < a_start);
}

/* ================================================================
 * Register allocation — graph coloring
 * ================================================================ */

/* Scan IR for vregs used in memory operands and mark them as needing
 * addressable registers (BX, BP, SI, DI — not AX, CX, DX) */
static void scan_addressing_constraints(func_t *fn) {
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        /* LOAD: %dst = %base[%idx] — base needs addressable reg */
        if (ins->op == IR_LOAD || ins->op == IR_STORE) {
            if (ins->src1 >= 0 && ins->src1 < MAX_VREGS) {
                fn->vregs[ins->src1].needs_addressable = true;
                fn->vregs[ins->src1].needs_base = true;
            }
            if (ins->src2 >= 0 && ins->src2 < MAX_VREGS) {
                fn->vregs[ins->src2].needs_addressable = true;
                fn->vregs[ins->src2].needs_index = true;
            }
        }
    }
}

static void allocate_registers(func_t *fn, bool bp_available) {
    /* Build list of allocatable word registers */
    int word_pool[8];
    int nword = 0;
    word_pool[nword++] = PREG_AX;
    word_pool[nword++] = PREG_BX;
    word_pool[nword++] = PREG_CX;
    word_pool[nword++] = PREG_DX;
    word_pool[nword++] = PREG_SI;
    word_pool[nword++] = PREG_DI;
    if (bp_available) word_pool[nword++] = PREG_BP;

    int byte_pool[] = { PREG_AL, PREG_AH, PREG_BL, PREG_BH,
                        PREG_CL, PREG_CH, PREG_DL, PREG_DH };
    int nbyte = 8;

    /* First pass: assign pre-colored (preferred) vregs */
    for (int i = 0; i < fn->nvregs; i++) {
        if (fn->vregs[i].prefer != PREG_NONE) {
            int preg = fn->vregs[i].prefer;
            /* Don't honor preference if it violates addressing constraints */
            if (fn->vregs[i].needs_base &&
                preg != PREG_BX && preg != PREG_BP) {
                continue;
            }
            if (fn->vregs[i].needs_index &&
                preg != PREG_SI && preg != PREG_DI) {
                continue;
            }
            if (fn->vregs[i].needs_addressable &&
                (preg == PREG_AX || preg == PREG_CX || preg == PREG_DX)) {
                continue;
            }
            fn->vregs[i].assigned = preg;
        }
    }

    /* Second pass: assign remaining vregs */
    for (int i = 0; i < fn->nvregs; i++) {
        if (fn->vregs[i].assigned != PREG_NONE) continue;
        if (fn->vregs[i].def_pos < 0 && fn->vregs[i].last_use < 0) continue;

        int *pool;
        int poolsz;
        if (fn->vregs[i].is_seg) {
            /* Segment regs handled separately */
            continue;
        } else if (fn->vregs[i].is_byte) {
            pool = byte_pool;
            poolsz = nbyte;
        } else if (fn->vregs[i].needs_base) {
            /* Base register: must be BX or BP */
            static int base_pool[2];
            int nbase = 0;
            base_pool[nbase++] = PREG_BX;
            if (bp_available) base_pool[nbase++] = PREG_BP;
            pool = base_pool;
            poolsz = nbase;
        } else if (fn->vregs[i].needs_index) {
            /* Index register: must be SI or DI */
            static int idx_pool[] = { PREG_SI, PREG_DI };
            pool = idx_pool;
            poolsz = 2;
        } else if (fn->vregs[i].needs_addressable) {
            /* General addressable (single-reg memory operand) */
            static int addr_pool[4];
            int naddr = 0;
            addr_pool[naddr++] = PREG_BX;
            if (bp_available) addr_pool[naddr++] = PREG_BP;
            addr_pool[naddr++] = PREG_SI;
            addr_pool[naddr++] = PREG_DI;
            pool = addr_pool;
            poolsz = naddr;
        } else {
            pool = word_pool;
            poolsz = nword;
        }

        /* Try each register in the pool */
        bool assigned = false;
        for (int r = 0; r < poolsz; r++) {
            int preg = pool[r];
            bool conflict = false;

            /* Check against all already-assigned vregs */
            for (int j = 0; j < fn->nvregs; j++) {
                if (j == i) continue;
                if (fn->vregs[j].assigned == PREG_NONE) continue;
                if (!vregs_interfere(fn, i, j)) continue;

                /* Direct conflict */
                if (fn->vregs[j].assigned == preg) {
                    conflict = true;
                    break;
                }
                /* Alias conflict */
                if (pregs_alias(fn->vregs[j].assigned, preg)) {
                    conflict = true;
                    break;
                }
            }

            if (!conflict) {
                fn->vregs[i].assigned = preg;
                assigned = true;
                break;
            }
        }

        if (!assigned) {
            /* Spill this vreg */
            fn->vregs[i].spill_slot = fn->nspill_slots++;
        }
    }
}

/* ================================================================
 * Assembly emission
 * ================================================================ */

static const char *vreg_asm(func_t *fn, int v) {
    static char buf[4][32];
    static int idx = 0;
    char *b = buf[idx++ & 3];

    if (v < 0) { strcpy(b, "???"); return b; }

    if (fn->vregs[v].assigned != PREG_NONE) {
        return preg_name[fn->vregs[v].assigned];
    }
    if (fn->vregs[v].spill_slot >= 0) {
        int off = -(fn->vregs[v].spill_slot + 1) * 2;
        snprintf(b, 32, "[BP%+d]", off);
        return b;
    }
    snprintf(b, 32, "%%_%d", v); /* shouldn't happen */
    return b;
}

/* Get the assembly label for a function — pub uses bare name, private gets module prefix */
static const char *fn_asm_name(func_t *fn) {
    static char buf[128];
    if (fn->is_pub || !fn->module[0])
        return fn->name;
    snprintf(buf, sizeof(buf), "%s_%s", fn->module, fn->name);
    return buf;
}

/* Look up a function's assembly name by its Nib name */
static const char *resolve_fn_name(const char *name) {
    for (int i = 0; i < nfunctions; i++)
        if (strcmp(functions[i].name, name) == 0)
            return fn_asm_name(&functions[i]);
    return name; /* not found — return as-is (extern, etc.) */
}

/* Scope a local label to the current function to avoid collisions */
static const char *scoped_label(func_t *fn, const char *label) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s_%s", fn_asm_name(fn), label);
    return buf;
}

static void emit_function(func_t *fn) {
    const char *asm_name = fn_asm_name(fn);
    fprintf(out_asm, "\n; === %s ===\n", asm_name);

    if (fn->has_at) {
        int linear = fn->at_seg * 16 + fn->at_off;
        fprintf(out_asm, "    seg 0x%04X\n", fn->at_seg);
        fprintf(out_asm, "    org 0x%05X ; %04X:%04X\n",
                linear, fn->at_seg, fn->at_off);
    }

    /* Determine which callee-saved registers need saving.
     * A register needs saving if it's in the preserves list AND
     * it's assigned to a vreg that the function body uses. */
    int save_regs[NUM_PREGS];
    int nsave = 0;

    if (fn->nfn_preserves > 0) {
        bool used[NUM_PREGS] = {0};
        for (int v = 0; v < fn->nvregs; v++) {
            if (fn->vregs[v].assigned != PREG_NONE &&
                fn->vregs[v].def_pos >= 0)
                used[fn->vregs[v].assigned] = true;
        }
        for (int i = 0; i < fn->nfn_preserves; i++) {
            int preg = fn->fn_preserves[i];
            if (used[preg])
                save_regs[nsave++] = preg;
        }
    }

    /* Prologue */
    if (fn->is_interrupt) {
        fprintf(out_asm, "; interrupt handler vector 0x%02X\n", fn->int_vector);
        fprintf(out_asm, "    pusha\n");
        if (fn->is_reentrant)
            fprintf(out_asm, "    sti\n");
    }

    fprintf(out_asm, "%s:\n", asm_name);

    /* Callee-save pushes */
    for (int i = 0; i < nsave; i++)
        fprintf(out_asm, "    push %s\n", preg_name[save_regs[i]]);

    if (fn->needs_frame) {
        fprintf(out_asm, "    push bp\n");
        fprintf(out_asm, "    mov bp, sp\n");
        if (fn->frame_size > 0)
            fprintf(out_asm, "    sub sp, %d\n", fn->frame_size);
    }

    /* Body */
    int last_dbg_line = 0;
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];

        /* Emit debug line comment when source line changes */
        if (ins->line > 0 && ins->line != last_dbg_line && ins->src_file[0]) {
            fprintf(out_asm, "; @%s:%d\n", ins->src_file, ins->line);
            last_dbg_line = ins->line;
        }

            switch (ins->op) {
        case IR_PREFER:
        case IR_NOP:
            break;

        case IR_LABEL:
            fprintf(out_asm, "%s:\n", scoped_label(fn, ins->name));
            break;

        case IR_MOV:
            if (ins->has_imm) {
                fprintf(out_asm, "    mov %s, %d\n",
                        vreg_asm(fn, ins->dst), ins->imm);
            } else if (ins->name[0]) {
                /* Label reference — load address of constant */
                fprintf(out_asm, "    mov %s, %s\n",
                        vreg_asm(fn, ins->dst), ins->name);
            } else {
                const char *d = vreg_asm(fn, ins->dst);
                const char *s = vreg_asm(fn, ins->src1);
                if (strcmp(d, s) != 0) /* skip self-moves */
                    fprintf(out_asm, "    mov %s, %s\n", d, s);
            }
            break;

        case IR_ALU: {
            const char *op = ins->name;

            /* Special two-operand forms */
            if (strcmp(op, "in") == 0) {
                /* IN: port is in src1 vreg or was an immediate */
                if (ins->has_imm)
                    fprintf(out_asm, "    in %s, 0x%02X\n",
                            vreg_asm(fn, ins->dst), ins->imm);
                else
                    fprintf(out_asm, "    in %s, %s\n",
                            vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
                break;
            }
            if (strcmp(op, "out") == 0) {
                if (ins->has_imm)
                    fprintf(out_asm, "    out 0x%02X, %s\n",
                            ins->imm, vreg_asm(fn, ins->src1));
                else
                    fprintf(out_asm, "    out %s, %s\n",
                            vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
                break;
            }
            if (strcmp(op, "far.off") == 0) {
                /* Load offset word from [ptr+0] */
                fprintf(out_asm, "    mov %s, [%s]\n",
                        vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
                break;
            }
            if (strcmp(op, "far.seg") == 0) {
                /* Load segment word from [ptr+2] */
                fprintf(out_asm, "    mov %s, [%s+2]\n",
                        vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
                break;
            }
            if (strcmp(op, "xlat") == 0) {
                /* xlat %d, %table, %idx — needs BX=table, AL=idx */
                fprintf(out_asm, "    ; xlat %s[%s] -> %s\n",
                        vreg_asm(fn, ins->src1), vreg_asm(fn, ins->src2),
                        vreg_asm(fn, ins->dst));
                fprintf(out_asm, "    xlat\n");
                break;
            }

            /* Special ops that need specific lowering */
            if (strcmp(op, "mul") == 0 || strcmp(op, "imul") == 0) {
                /* MUL/IMUL: AX * src -> DX:AX */
                fprintf(out_asm, "    %s %s\n", op, vreg_asm(fn, ins->src2));
                break;
            }
            if (strcmp(op, "div") == 0 || strcmp(op, "mod") == 0) {
                /* DIV: DX:AX / src -> AX=quot, DX=rem */
                fprintf(out_asm, "    div %s\n", vreg_asm(fn, ins->src2));
                break;
            }
            if (strcmp(op, "idiv") == 0 || strcmp(op, "imod") == 0) {
                fprintf(out_asm, "    idiv %s\n", vreg_asm(fn, ins->src2));
                break;
            }
            if (strcmp(op, "xchg") == 0) {
                fprintf(out_asm, "    xchg %s, %s\n",
                        vreg_asm(fn, ins->src1), vreg_asm(fn, ins->src2));
                break;
            }
            if (strcmp(op, "stos") == 0) {
                /* STOSB/STOSW: value in AL/AX, dest in DI */
                fprintf(out_asm, "    stosb\n");
                break;
            }
            if (strcmp(op, "bext") == 0) {
                /* V20 EXT: extract bit field from [src1] at CL:src2 */
                fprintf(out_asm, "    bext %s, %s\n",
                        vreg_asm(fn, ins->src1), vreg_asm(fn, ins->dst));
                break;
            }
            if (strcmp(op, "bins") == 0) {
                /* V20 INS: insert bit field into [dst] at CL:src2 */
                fprintf(out_asm, "    bins %s, %s\n",
                        vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
                break;
            }

            /* Map IR op names to asm mnemonics */
            const char *mnem = op;
            if (strcmp(op, "add") == 0) mnem = "add";
            else if (strcmp(op, "sub") == 0) mnem = "sub";
            else if (strcmp(op, "and") == 0) mnem = "and";
            else if (strcmp(op, "or") == 0) mnem = "or";
            else if (strcmp(op, "xor") == 0) mnem = "xor";
            else if (strcmp(op, "shl") == 0) mnem = "shl";
            else if (strcmp(op, "shr") == 0) mnem = "shr";
            else if (strcmp(op, "sar") == 0) mnem = "sar";
            else if (strcmp(op, "rol") == 0) mnem = "rol";
            else if (strcmp(op, "ror") == 0) mnem = "ror";
            else if (strcmp(op, "rcl") == 0) mnem = "rcl";
            else if (strcmp(op, "rcr") == 0) mnem = "rcr";

            /* Three-address -> two-address:
               op %d, %l, %r  becomes  mov %d, %l; op %d, %r
               op %d, %l, imm becomes  mov %d, %l; op %d, imm */
            const char *d = vreg_asm(fn, ins->dst);
            const char *l = vreg_asm(fn, ins->src1);
            if (strcmp(d, l) != 0)
                fprintf(out_asm, "    mov %s, %s\n", d, l);
            if (ins->has_imm)
                fprintf(out_asm, "    %s %s, %d\n", mnem, d, ins->imm);
            else
                fprintf(out_asm, "    %s %s, %s\n", mnem, d, vreg_asm(fn, ins->src2));
            break;
        }

        case IR_UNARY: {
            const char *d = vreg_asm(fn, ins->dst);
            const char *s = vreg_asm(fn, ins->src1);
            const char *mnem = ins->name;
            /* Map IR unary names to V20 instructions */
            if (strcmp(mnem, "lnot") == 0) mnem = "not";
            if (strcmp(mnem, "cbw") == 0) {
                /* CBW: sign-extend AL -> AX */
                if (strcmp(d, s) != 0)
                    fprintf(out_asm, "    mov %s, %s\n", d, s);
                fprintf(out_asm, "    cbw\n");
                break;
            }
            if (strcmp(mnem, "lods") == 0) {
                /* LODSB/LODSW: load from [SI], result in AL/AX */
                fprintf(out_asm, "    lodsb\n");
                break;
            }
            if (strcmp(mnem, "rol4") == 0) {
                /* V20 ROL4: nibble rotate left through AL on [mem] */
                fprintf(out_asm, "    rol4 [%s]\n", s);
                break;
            }
            if (strcmp(mnem, "ror4") == 0) {
                /* V20 ROR4: nibble rotate right through AL on [mem] */
                fprintf(out_asm, "    ror4 [%s]\n", s);
                break;
            }
            if (strcmp(mnem, "swap_flags") == 0) {
                /* LAHF: load flags into AH, then SAHF to restore from val */
                if (strcmp(d, s) != 0)
                    fprintf(out_asm, "    mov %s, %s\n", d, s);
                fprintf(out_asm, "    lahf\n");
                fprintf(out_asm, "    mov %s, AH\n", d);
                fprintf(out_asm, "    mov AH, %s\n", s);
                fprintf(out_asm, "    sahf\n");
                break;
            }
            if (strcmp(mnem, "zext") == 0) {
                /* Zero-extend: clear upper byte */
                if (strcmp(d, s) != 0)
                    fprintf(out_asm, "    mov %s, %s\n", d, s);
                fprintf(out_asm, "    xor %s, %s\n", d, d);
                fprintf(out_asm, "    mov %s, %s\n", d, s);
                break;
            }
            if (strcmp(d, s) != 0)
                fprintf(out_asm, "    mov %s, %s\n", d, s);
            fprintf(out_asm, "    %s %s\n", mnem, d);
            break;
        }

        case IR_CMP:
            /* Handled together with the following JZ */
            break;

        case IR_JZ: {
            /* Find the preceding CMP that defined this condition vreg */
            const char *jcc = "jz"; /* fallback */
            for (int j = i - 1; j >= 0; j--) {
                ir_insn_t *cmp = &fn->insns[j];
                if (cmp->op == IR_ALU || cmp->op == IR_CMP) {
                    if (cmp->dst == ins->src1) {
                        /* Emit the CMP */
                        if (cmp->has_imm)
                            fprintf(out_asm, "    cmp %s, %d\n",
                                    vreg_asm(fn, cmp->src1), cmp->imm);
                        else
                            fprintf(out_asm, "    cmp %s, %s\n",
                                    vreg_asm(fn, cmp->src1),
                                    vreg_asm(fn, cmp->src2));
                        /* Map comparison kind to Jcc — note: jz means
                           "jump if condition NOT met" (the condition
                           vreg is zero), so we invert */
                        const char *op = cmp->name;
                        if (strcmp(op, "cmp.eq") == 0)       jcc = "jne";
                        else if (strcmp(op, "cmp.ne") == 0)  jcc = "je";
                        else if (strcmp(op, "cmp.b") == 0)   jcc = "jnb";
                        else if (strcmp(op, "cmp.a") == 0)   jcc = "jbe";
                        else if (strcmp(op, "cmp.be") == 0)  jcc = "ja";
                        else if (strcmp(op, "cmp.ae") == 0)  jcc = "jb";
                        else if (strcmp(op, "cmp.l") == 0)   jcc = "jnl";
                        else if (strcmp(op, "cmp.g") == 0)   jcc = "jle";
                        else if (strcmp(op, "cmp.le") == 0)  jcc = "jg";
                        else if (strcmp(op, "cmp.ge") == 0)  jcc = "jl";
                        break;
                    }
                }
                if (cmp->op == IR_LABEL || cmp->op == IR_JMP) break;
            }
            fprintf(out_asm, "    %s %s\n", jcc, scoped_label(fn, ins->name));
            break;
        }

        case IR_GOTO_FN: {
            /* Raw jump to function — no cleanup, no args */
            bool gf_far = false;
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0) {
                    gf_far = functions[fi2].is_far;
                    break;
                }
            }
            if (!gf_far) {
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0) {
                        gf_far = externs[e].is_far;
                        if (externs[e].has_address) {
                            fprintf(out_asm, "    jmp far 0x%04X:0x%04X\n",
                                    externs[e].addr_seg, externs[e].addr_off);
                            gf_far = false;
                        }
                        break;
                    }
                }
            }
            if (gf_far)
                fprintf(out_asm, "    jmp far %s\n", resolve_fn_name(ins->name));
            else
                fprintf(out_asm, "    jmp %s\n", resolve_fn_name(ins->name));
            break;
        }

        case IR_JMP:
            fprintf(out_asm, "    jmp %s\n", scoped_label(fn, ins->name));
            break;

        case IR_CALL: {
            /* Check if this is a chain call */
            if (fn->has_chain && strcmp(ins->name, fn->chain_name) == 0) {
                fprintf(out_asm, "    pushf\n");
                fprintf(out_asm, "    call far [%s_vec]\n", fn->chain_name);
                break;
            }
            /* Check if calling an extern */
            bool found_extern = false;
            for (int e = 0; e < nexterns; e++) {
                if (strcmp(externs[e].name, ins->name) == 0) {
                    if (externs[e].is_interrupt) {
                        fprintf(out_asm, "    int 0x%02X\n", externs[e].int_vector);
                    } else if (externs[e].has_address) {
                        fprintf(out_asm, "    call far 0x%04X:0x%04X\n",
                                externs[e].addr_seg, externs[e].addr_off);
                    } else {
                        fprintf(out_asm, "    ; ERROR: extern '%s' has no address\n",
                                ins->name);
                    }
                    found_extern = true;
                    break;
                }
            }
            if (!found_extern)
                fprintf(out_asm, "    call %s\n", resolve_fn_name(ins->name));
            break;
        }

        case IR_TAILCALL: {
            /* Tear down frame */
            if (fn->needs_frame) {
                fprintf(out_asm, "    mov sp, bp\n");
                fprintf(out_asm, "    pop bp\n");
            }
            /* Check if target is far */
            bool tc_far = false;
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0) {
                    tc_far = functions[fi2].is_far;
                    break;
                }
            }
            if (!tc_far) {
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0) {
                        tc_far = externs[e].is_far;
                        if (externs[e].has_address) {
                            fprintf(out_asm, "    jmp far 0x%04X:0x%04X\n",
                                    externs[e].addr_seg, externs[e].addr_off);
                            tc_far = false; /* already emitted */
                        }
                        break;
                    }
                }
            }
            if (tc_far)
                fprintf(out_asm, "    jmp far %s\n", resolve_fn_name(ins->name));
            else
                fprintf(out_asm, "    jmp %s\n", resolve_fn_name(ins->name));
            break;
        }

        case IR_RETVAL:
            /* Return value is in the assigned register — if it's not
               already in the return register, the binder would insert
               a mov. For now the allocator handles this via preferences. */
            break;

        case IR_RET:
            if (fn->needs_frame) {
                fprintf(out_asm, "    mov sp, bp\n");
                fprintf(out_asm, "    pop bp\n");
            }
            /* Callee-save pops (reverse order) */
            for (int j = nsave - 1; j >= 0; j--)
                fprintf(out_asm, "    pop %s\n", preg_name[save_regs[j]]);
            if (fn->is_interrupt) {
                fprintf(out_asm, "    popa\n");
                fprintf(out_asm, "    iret\n");
            } else {
                fprintf(out_asm, "    ret\n");
            }
            break;

        case IR_LOAD:
            if (ins->src2 >= 0)
                fprintf(out_asm, "    mov %s, [%s+%s]\n",
                        vreg_asm(fn, ins->dst),
                        vreg_asm(fn, ins->src1),
                        vreg_asm(fn, ins->src2));
            else
                fprintf(out_asm, "    mov %s, [%s]\n",
                        vreg_asm(fn, ins->dst),
                        vreg_asm(fn, ins->src1));
            break;

        case IR_STORE:
            if (ins->src2 >= 0)
                fprintf(out_asm, "    mov [%s+%s], %s\n",
                        vreg_asm(fn, ins->src1),
                        vreg_asm(fn, ins->src2),
                        vreg_asm(fn, ins->dst));
            else
                fprintf(out_asm, "    mov [%s], %s\n",
                        vreg_asm(fn, ins->src1),
                        vreg_asm(fn, ins->dst));
            break;

        case IR_LOADMEM:
            fprintf(out_asm, "    mov %s, %s\n",
                    vreg_asm(fn, ins->dst), ins->name);
            break;

        case IR_STOREMEM:
            fprintf(out_asm, "    mov %s, %s\n",
                    ins->name, vreg_asm(fn, ins->src1));
            break;

        case IR_FIELD:
            fprintf(out_asm, "    ; field %s.%s -> %s\n",
                    vreg_asm(fn, ins->src1), ins->name,
                    vreg_asm(fn, ins->dst));
            break;

        case IR_SETFLAG: {
            /* Determine value: look back for the mov that defined src1 */
            int val = 1; /* default assume set */
            if (ins->src1 >= 0) {
                for (int j = i - 1; j >= 0; j--) {
                    if (fn->insns[j].dst == ins->src1 &&
                        fn->insns[j].op == IR_MOV && fn->insns[j].has_imm) {
                        val = fn->insns[j].imm;
                        break;
                    }
                }
            }
            if (strcmp(ins->name, "CF") == 0)
                fprintf(out_asm, "    %s\n", val ? "stc" : "clc");
            else if (strcmp(ins->name, "DF") == 0)
                fprintf(out_asm, "    %s\n", val ? "std" : "cld");
            else if (strcmp(ins->name, "IF") == 0)
                fprintf(out_asm, "    %s\n", val ? "sti" : "cli");
            else if (strcmp(ins->name, "TF") == 0)
                fprintf(out_asm, "    ; TF := %d (no direct instruction)\n", val);
            break;
        }

        case IR_GETFLAG:
            fprintf(out_asm, "    ; getflag %s -> %s\n",
                    ins->name, vreg_asm(fn, ins->dst));
            break;

        case IR_TOGGLEFLAG:
            if (strcmp(ins->name, "CF") == 0)
                fprintf(out_asm, "    cmc\n");
            break;

        case IR_LOOP: {
            /* LOOP decrements CX and jumps if CX != 0 */
            fprintf(out_asm, "    loop %s\n", scoped_label(fn, ins->name));
            break;
        }

        case IR_CJMP:
            /* Flag-check conditional jump with scoped label */
            fprintf(out_asm, "    %s %s\n", ins->asm_ann,
                    scoped_label(fn, ins->name));
            break;

        case IR_ASM:
            if (ins->asm_ann[0])
                fprintf(out_asm, "    ; asm %s\n", ins->asm_ann);
            fprintf(out_asm, "%s\n", ins->asm_body);
            break;

        case IR_BOUND:
            fprintf(out_asm, "    ; bound check\n");
            break;

        case IR_BREAK:
        case IR_CONTINUE:
            /* Dead code — compiler resolves these to JMPs */
            break;

        default:
            fprintf(out_asm, "    ; unhandled IR op %d\n", ins->op);
            break;
        }
    }

    /* Final ret if not already emitted */
    if (fn->ninsns == 0 || fn->insns[fn->ninsns-1].op != IR_RET) {
        if (fn->needs_frame) {
            fprintf(out_asm, "    mov sp, bp\n");
            fprintf(out_asm, "    pop bp\n");
        }
        for (int j = nsave - 1; j >= 0; j--)
            fprintf(out_asm, "    pop %s\n", preg_name[save_regs[j]]);
        if (fn->is_interrupt) {
            fprintf(out_asm, "    popa\n");
            fprintf(out_asm, "    iret\n");
        } else {
            fprintf(out_asm, "    ret\n");
        }
    }

    /* Emit chain vector storage for interrupt handlers */
    if (fn->has_chain) {
        fprintf(out_asm, "%s_vec dw 0, 0 ; saved vector for chaining\n",
                fn->chain_name);
    }
}

/* ================================================================
 * Inter-procedural register propagation
 * ================================================================ */

/* Call graph */
typedef struct {
    int caller_fn;          /* index into functions[] */
    int callee_fn;          /* index into functions[], or -1 if external */
    int insn_idx;           /* call instruction index in caller */
    int arg_vregs[16];      /* caller's vregs for each argument */
    int nargs;
    int ret_vreg;           /* caller's vreg receiving return value */
    char callee_name[64];
} call_edge_t;

#define MAX_EDGES 1024
static call_edge_t call_edges[MAX_EDGES];
static int nedges = 0;

/* Resolved parameter register assignments per function */
typedef struct {
    int param_regs[16];     /* PREG_* for each parameter, or PREG_NONE */
    int return_reg;         /* PREG_* for return value, or PREG_NONE */
    bool resolved;
} fn_assignment_t;

static fn_assignment_t fn_assigns[MAX_FNS];

static int find_fn(const char *name) {
    for (int i = 0; i < nfunctions; i++)
        if (strcmp(functions[i].name, name) == 0)
            return i;
    return -1;
}

/* Build call graph by scanning all functions for call instructions */
static void build_call_graph(void) {
    nedges = 0;
    for (int fi = 0; fi < nfunctions; fi++) {
        func_t *fn = &functions[fi];
        for (int i = 0; i < fn->ninsns; i++) {
            ir_insn_t *ins = &fn->insns[i];
            if (ins->op != IR_CALL && ins->op != IR_TAILCALL) continue;

            if (nedges >= MAX_EDGES) break;
            call_edge_t *e = &call_edges[nedges++];
            e->caller_fn = fi;
            e->callee_fn = find_fn(ins->name);
            e->insn_idx = i;
            strncpy(e->callee_name, ins->name, 63);
            e->ret_vreg = ins->dst;
            e->nargs = 0;

            /* Collect argument vregs */
            if (ins->src1 >= 0) e->arg_vregs[e->nargs++] = ins->src1;
            if (ins->src2 >= 0) e->arg_vregs[e->nargs++] = ins->src2;
            for (int j = 0; j < 8 && ins->extra_args[j] >= 0; j++)
                e->arg_vregs[e->nargs++] = ins->extra_args[j];
        }
    }
    fprintf(stderr, "Call graph: %d edges\n", nedges);
}

/* Topological sort of functions — leaf functions first.
 * A leaf is a function that makes no calls to other Nib functions. */
static int topo_order[MAX_FNS];
static int ntopo;

static void topo_sort(void) {
    /* Compute in-degree (how many Nib functions call this one) */
    bool visited[MAX_FNS] = {0};
    bool has_callee[MAX_FNS] = {0}; /* does this fn call other Nib fns? */

    for (int i = 0; i < nedges; i++) {
        if (call_edges[i].callee_fn >= 0)
            has_callee[call_edges[i].caller_fn] = true;
    }

    ntopo = 0;
    /* First pass: all leaf functions (no outgoing Nib calls) */
    for (int i = 0; i < nfunctions; i++) {
        if (!has_callee[i]) {
            topo_order[ntopo++] = i;
            visited[i] = true;
        }
    }

    /* Iterative: add functions whose callees are all visited */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < nfunctions; i++) {
            if (visited[i]) continue;
            bool all_callees_visited = true;
            for (int e = 0; e < nedges; e++) {
                if (call_edges[e].caller_fn != i) continue;
                if (call_edges[e].callee_fn < 0) continue; /* external */
                if (!visited[call_edges[e].callee_fn]) {
                    all_callees_visited = false;
                    break;
                }
            }
            if (all_callees_visited) {
                topo_order[ntopo++] = i;
                visited[i] = true;
                changed = true;
            }
        }
    }

    /* Any remaining (recursive) — add them anyway */
    for (int i = 0; i < nfunctions; i++) {
        if (!visited[i])
            topo_order[ntopo++] = i;
    }
}

/* Propagate register preferences bottom-up through the call graph.
 *
 * Starting from leaf functions, for each function:
 * 1. Look at its .prefer directives — these are the function's own
 *    desires (e.g., "I use rep movsb so I want param 0 in SI")
 * 2. Assign parameter registers based on preferences
 * 3. For each caller, propagate: "at this call site, arg vreg N
 *    should be in register R" — this becomes a preference on the
 *    caller's vreg
 */
static void propagate_preferences(void) {
    /* Initialize */
    for (int i = 0; i < nfunctions; i++) {
        fn_assigns[i].resolved = false;
        fn_assigns[i].return_reg = PREG_NONE;
        for (int j = 0; j < 16; j++)
            fn_assigns[i].param_regs[j] = PREG_NONE;
    }

    /* Pre-propagate: extern functions have fixed param registers.
     * Set preferences on caller vregs for calls to externs. */
    for (int e = 0; e < nedges; e++) {
        if (call_edges[e].callee_fn >= 0) continue; /* not an extern */
        /* Find the extern declaration */
        for (int x = 0; x < nexterns; x++) {
            if (strcmp(externs[x].name, call_edges[e].callee_name) != 0) continue;
            int caller_fi = call_edges[e].caller_fn;
            func_t *caller = &functions[caller_fi];
            for (int a = 0; a < call_edges[e].nargs && a < externs[x].nparams; a++) {
                int caller_vreg = call_edges[e].arg_vregs[a];
                int callee_reg = externs[x].param_pins[a].preg;
                if (caller_vreg >= 0 && callee_reg != PREG_NONE) {
                    if (caller->vregs[caller_vreg].prefer == PREG_NONE)
                        caller->vregs[caller_vreg].prefer = callee_reg;
                }
            }
            break;
        }
    }

    /* Process in topological order (leaves first) */
    for (int ti = 0; ti < ntopo; ti++) {
        int fi = topo_order[ti];
        func_t *fn = &functions[fi];
        fn_assignment_t *fa = &fn_assigns[fi];

        /* Step 1: Assign parameter registers from this function's preferences.
         * If a parameter's vreg has a .prefer, use that.
         * Otherwise, pick a free register. */
        bool reg_used[NUM_PREGS] = {0};

        /* First pass: honor explicit preferences */
        for (int p = 0; p < fn->nparams; p++) {
            int v = fn->param_vregs[p];
            if (v >= 0 && fn->vregs[v].prefer != PREG_NONE) {
                int preg = fn->vregs[v].prefer;
                if (!reg_used[preg]) {
                    fa->param_regs[p] = preg;
                    reg_used[preg] = true;
                    /* Also mark aliases */
                    if (preg < 4) {
                        reg_used[preg_alias_lo[preg]] = true;
                        reg_used[preg_alias_hi[preg]] = true;
                    }
                }
            }
        }

        /* Check if any vreg used in the function body has a preference
         * that traces back to a parameter (via mov chains).
         * Simplified: scan for mov %pinned, %param patterns */
        for (int i = 0; i < fn->ninsns; i++) {
            ir_insn_t *ins = &fn->insns[i];
            if (ins->op != IR_MOV || ins->has_imm) continue;
            if (ins->dst < 0 || ins->src1 < 0) continue;

            /* If dst has a preference and src is a parameter, propagate */
            int dst_pref = fn->vregs[ins->dst].prefer;
            if (dst_pref == PREG_NONE) continue;

            for (int p = 0; p < fn->nparams; p++) {
                if (fn->param_vregs[p] == ins->src1 &&
                    fa->param_regs[p] == PREG_NONE) {
                    if (!reg_used[dst_pref]) {
                            fa->param_regs[p] = dst_pref;
                        reg_used[dst_pref] = true;
                        if (dst_pref < 4) {
                            reg_used[preg_alias_lo[dst_pref]] = true;
                            reg_used[preg_alias_hi[dst_pref]] = true;
                        }
                        /* The mov becomes a no-op since param arrives
                         * in the right register already */
                    }
                }
            }
        }

        /* Second pass: assign remaining parameters to free registers */
        int word_order[] = { PREG_AX, PREG_BX, PREG_CX, PREG_DX,
                             PREG_SI, PREG_DI, PREG_BP };
        int byte_order[] = { PREG_AL, PREG_BL, PREG_CL, PREG_DL,
                             PREG_AH, PREG_BH, PREG_CH, PREG_DH };

        for (int p = 0; p < fn->nparams; p++) {
            if (fa->param_regs[p] != PREG_NONE) continue;
            int v = fn->param_vregs[p];
            if (v < 0) continue;

            bool is_byte = fn->vregs[v].is_byte;
            int *order = is_byte ? byte_order : word_order;
            int norder = is_byte ? 8 : 7;

            for (int r = 0; r < norder; r++) {
                int preg = order[r];
                if (reg_used[preg]) continue;
                /* Check alias */
                bool alias_conflict = false;
                if (preg < 4) {
                    if (reg_used[preg_alias_lo[preg]] || reg_used[preg_alias_hi[preg]])
                        alias_conflict = true;
                } else if (preg >= PREG_AL && preg <= PREG_BH) {
                    if (reg_used[preg_alias_parent[preg]])
                        alias_conflict = true;
                }
                if (alias_conflict) continue;

                fa->param_regs[p] = preg;
                reg_used[preg] = true;
                if (preg < 4) {
                    reg_used[preg_alias_lo[preg]] = true;
                    reg_used[preg_alias_hi[preg]] = true;
                }
                break;
            }
        }

        /* Assign return register */
        if (fn->has_return) {
            if (fn->ret_pin != PREG_NONE) {
                fa->return_reg = fn->ret_pin;
            } else {
                bool is_byte = (strcmp(fn->return_type, "u8") == 0);
                fa->return_reg = is_byte ? PREG_AL : PREG_AX;
            }
        }

        fa->resolved = true;

        /* Step 2: Propagate to callers.
         * For each call edge where we are the callee, set preferences
         * on the caller's argument vregs. */
        for (int e = 0; e < nedges; e++) {
            if (call_edges[e].callee_fn != fi) continue;

            int caller_fi = call_edges[e].caller_fn;
            func_t *caller = &functions[caller_fi];

            for (int a = 0; a < call_edges[e].nargs && a < fn->nparams; a++) {
                int caller_vreg = call_edges[e].arg_vregs[a];
                int callee_reg = fa->param_regs[a];

                if (caller_vreg < 0 || callee_reg == PREG_NONE) continue;

                /* Set preference on the caller's vreg — but don't
                 * override an existing preference */
                if (caller->vregs[caller_vreg].prefer == PREG_NONE) {
                    caller->vregs[caller_vreg].prefer = callee_reg;
                }
            }

            /* Return value: set preference on caller's dst vreg */
            if (call_edges[e].ret_vreg >= 0 && fa->return_reg != PREG_NONE) {
                int rv = call_edges[e].ret_vreg;
                if (caller->vregs[rv].prefer == PREG_NONE)
                    caller->vregs[rv].prefer = fa->return_reg;
            }
        }

        fprintf(stderr, "  resolved %s: params=[", fn->name);
        for (int p = 0; p < fn->nparams; p++) {
            if (p > 0) fprintf(stderr, ", ");
            if (fa->param_regs[p] != PREG_NONE)
                fprintf(stderr, "%s", preg_name[fa->param_regs[p]]);
            else
                fprintf(stderr, "?");
        }
        fprintf(stderr, "]");
        if (fa->return_reg != PREG_NONE)
            fprintf(stderr, " -> %s", preg_name[fa->return_reg]);
        fprintf(stderr, "\n");
    }

    /* Final step: for each function, set parameter vreg preferences
     * to match the resolved assignments */
    for (int fi = 0; fi < nfunctions; fi++) {
        func_t *fn = &functions[fi];
        fn_assignment_t *fa = &fn_assigns[fi];

        for (int p = 0; p < fn->nparams; p++) {
            int v = fn->param_vregs[p];
            if (v >= 0 && fa->param_regs[p] != PREG_NONE) {
                fn->vregs[v].prefer = fa->param_regs[p];
            }
        }
    }
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char **argv) {
    const char *outpath = "out.asm";
    int ninputs = 0;
    const char *inputs[64];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        } else {
            inputs[ninputs++] = argv[i];
        }
    }

    if (ninputs == 0) {
        fprintf(stderr, "usage: nibbind [-o out.asm] file.nir ...\n");
        return 1;
    }

    /* Parse all .nir files */
    for (int i = 0; i < ninputs; i++)
        parse_nir(inputs[i]);

    fprintf(stderr, "Loaded %d functions\n", nfunctions);

    /* Inter-procedural register propagation */
    build_call_graph();
    topo_sort();
    fprintf(stderr, "Topo order:");
    for (int i = 0; i < ntopo; i++)
        fprintf(stderr, " %s", functions[topo_order[i]].name);
    fprintf(stderr, "\n");
    propagate_preferences();

    /* For each function: liveness, allocation, emit */
    out_asm = fopen(outpath, "w");
    if (!out_asm) { perror(outpath); return 1; }

    fprintf(out_asm, "; Nib assembly — generated by nib bind\n");

    for (int i = 0; i < nfunctions; i++) {
        func_t *fn = &functions[i];

        /* Phase 1: scan constraints and liveness */
        scan_addressing_constraints(fn);
        compute_liveness(fn);

        /* Phase 2: try allocation with BP available */
        allocate_registers(fn, true);

        /* Phase 3: check if we need spills */
        if (fn->nspill_slots > 0) {
            /* Need BP for frame pointer — re-allocate */
            fn->needs_frame = true;
            fn->frame_size = fn->nspill_slots * 2;
            /* Reset assignments */
            for (int v = 0; v < fn->nvregs; v++) {
                fn->vregs[v].assigned = PREG_NONE;
                fn->vregs[v].spill_slot = -1;
            }
            fn->nspill_slots = 0;
            allocate_registers(fn, false);
            fn->frame_size = fn->nspill_slots * 2;
        }

        /* Debug: show assignments */
        fprintf(stderr, "  %s: %d vregs, %d spills [", fn->name, fn->nvregs, fn->nspill_slots);
        for (int v = 0; v < fn->nvregs; v++) {
            if (fn->vregs[v].assigned != PREG_NONE)
                fprintf(stderr, " %%%d=%s", v, preg_name[fn->vregs[v].assigned]);
            else if (fn->vregs[v].spill_slot >= 0)
                fprintf(stderr, " %%%d=spill%d", v, fn->vregs[v].spill_slot);
        }
        fprintf(stderr, " ]\n");

        /* Phase 4: emit all functions in topo order */
        emit_function(fn);
    }

    /* Emit constant pool, non-placed data blocks, and globals
     * BEFORE any at()-placed items so they don't end up after
     * a high-address org directive */
    if (nconsts > 0) {
        fprintf(out_asm, "\n; === constant pool ===\n");
        for (int i = 0; i < nconsts; i++) {
            if (consts[i].is_far_ref) {
                const char *resolved = resolve_fn_name(consts[i].ref_name);
                fprintf(out_asm, "%s dw %s, SEG %s\n",
                        consts[i].label, resolved, resolved);
            } else {
                fprintf(out_asm, "%s\n", consts[i].data);
            }
        }
    }

    for (int i = 0; i < ndata_blocks; i++) {
        data_block_t *db = &data_blocks[i];
        if (db->has_at) continue;
        fprintf(out_asm, "\n; === data: %s ===\n", db->label);
        fprintf(out_asm, "%s:\n", db->label);
        for (int j = 0; j < db->nentries; j++) {
            if (db->entries[j][0] == '\x01') {
                const char *resolved = resolve_fn_name(db->entries[j] + 1);
                fprintf(out_asm, "    dw %s, SEG %s\n", resolved, resolved);
            } else {
                fprintf(out_asm, "%s\n", db->entries[j]);
            }
        }
    }

    if (nglobals > 0) {
        fprintf(out_asm, "\n; === globals ===\n");
        for (int i = 0; i < nglobals; i++) {
            fprintf(out_asm, "%s:", globals[i].name);
            if (globals[i].size <= 2)
                fprintf(out_asm, " dw 0\n");
            else {
                fprintf(out_asm, "\n");
                for (int b = 0; b < globals[i].size; b += 2)
                    fprintf(out_asm, "    dw 0\n");
            }
        }
    }

    /* at()-placed data blocks go after globals */
    for (int i = 0; i < ndata_blocks; i++) {
        data_block_t *db = &data_blocks[i];
        if (!db->has_at) continue;
        fprintf(out_asm, "\n; === data: %s ===\n", db->label);
        int linear = db->at_seg * 16 + db->at_off;
        fprintf(out_asm, "    seg 0x%04X\n", db->at_seg);
        fprintf(out_asm, "    org 0x%05X ; %04X:%04X\n",
                linear, db->at_seg, db->at_off);
        fprintf(out_asm, "%s:\n", db->label);
        for (int j = 0; j < db->nentries; j++) {
            if (db->entries[j][0] == '\x01') {
                const char *resolved = resolve_fn_name(db->entries[j] + 1);
                fprintf(out_asm, "    dw %s, SEG %s\n", resolved, resolved);
            } else {
                fprintf(out_asm, "%s\n", db->entries[j]);
            }
        }
    }

    fclose(out_asm);
    fprintf(stderr, "Wrote %s\n", outpath);
    return 0;
}
