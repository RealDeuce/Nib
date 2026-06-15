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
    IR_ICALL,       /* icall %d, %addr, name, args... */
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
    IR_FAR_LIT,     /* far.lit %d, seg, off — store far literal on stack */
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
    bool    needs_cl;          /* true if used as shift/rotate count: must be CL */
    bool    is_const;          /* true if immutable — prefer to spill over mutable */
    int     use_count;         /* number of instructions referencing this vreg */
    bool    in_loop;           /* true if live range spans a loop back-edge */
    bool    is_cs_ref;         /* true if this vreg points to constant pool (CS segment) */
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

/* Per-function constant pool entry */
#define MAX_FN_CONSTS 64
typedef struct {
    char label[32];
    char data[256];
    bool is_far_ref;
    char ref_name[64];
} fn_const_t;

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
    int         emit_seg;       /* segment at emission time (-1 = unknown) */

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
    int         ncl_fixups;     /* CL routing fixups (push/pop CX) */
    int         frame_size;     /* total bytes for spills */

    /* Callee-saved registers */
    int         fn_preserves[NUM_PREGS]; /* list of PREG_* to save/restore */
    int         nfn_preserves;

    /* Per-function constant pool */
    fn_const_t consts[MAX_FN_CONSTS];
    int nconsts;
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

/* Forward declarations */
static const char *fn_asm_name(func_t *fn);

/* Resolved parameter register assignments per function */
typedef struct {
    int param_regs[16];     /* PREG_* for each parameter, or PREG_NONE */
    int return_reg;         /* PREG_* for return value, or PREG_NONE */
    bool resolved;
} fn_assignment_t;

static fn_assignment_t fn_assigns[MAX_FNS];


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
    char module[64];    /* source module this data block belongs to */
} data_block_t;

static data_block_t data_blocks[MAX_DATA_BLOCKS];
static int ndata_blocks = 0;

/* Global variable declarations */
#define MAX_GLOBALS 256
typedef struct {
    char name[64];
    char type[32];
    int  size;          /* bytes of storage */
    char module[64];    /* source module this global belongs to */
    bool has_at;
    int  at_seg;
    int  at_off;
} global_var_t;

static global_var_t globals[MAX_GLOBALS];
static int nglobals = 0;

/* Emission order — tracks the source order of all top-level items */
#define EMIT_FN    0
#define EMIT_DATA  1
#define EMIT_GLOB  2
#define EMIT_AT    3
#define EMIT_USE   4
#define EMIT_ENDAT 5
#define MAX_AT_DIRECTIVES 64
static struct { int seg; int off; } at_directives[MAX_AT_DIRECTIVES];
static int nat_directives = 0;

#define MAX_USE_DIRECTIVES 64
static char use_modules[MAX_USE_DIRECTIVES][64]; /* module name from .use */
static int nuse_directives = 0;

#define MAX_EMIT_ORDER 1024
static struct { int kind; int index; char module[64]; } emit_order[MAX_EMIT_ORDER];
static int nemit_order = 0;

static char _cur_parse_module[64]; /* set during parse_nir */

static void record_emit(int kind, int index) {
    if (nemit_order < MAX_EMIT_ORDER) {
        emit_order[nemit_order].kind = kind;
        emit_order[nemit_order].index = index;
        strncpy(emit_order[nemit_order].module, _cur_parse_module, 63);
        nemit_order++;
    }
}

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
            if (fn->nconsts < MAX_FN_CONSTS) {
                fn_const_t *c = &fn->consts[fn->nconsts++];
                memset(c, 0, sizeof(*c));
                char raw_label[32];
                p = read_word(p, raw_label, sizeof(raw_label));
                /* Prefix with function's asm name to avoid cross-module collisions */
                snprintf(c->label, sizeof(c->label), "%s_%s",
                         fn_asm_name(fn), raw_label);
                /* skip comma */
                p = skip_ws(p);
                if (*p == ',') p++;
                p = skip_ws(p);
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
                /* Mark segment register vregs */
                if (preg >= PREG_ES && preg <= PREG_DS)
                    fn->vregs[v].is_seg = true;
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

        if (strncmp(p, ".byte", 5) == 0) {
            p += 5;
            int v = parse_vreg(p, &p);
            if (v >= 0 && v < MAX_VREGS)
                fn->vregs[v].is_byte = true;
            if (v >= fn->nvregs) fn->nvregs = v + 1;
            continue;
        }

        if (strncmp(p, ".immutable", 10) == 0) {
            p += 10;
            int v = parse_vreg(p, &p);
            if (v >= 0 && v < MAX_VREGS)
                fn->vregs[v].is_const = true;
            if (v >= fn->nvregs) fn->nvregs = v + 1;
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
                /* Constant pool refs live in CS (code segment) */
                if (ins->name[0] == '_' && ins->name[1] == 'C' &&
                    ins->dst >= 0 && ins->dst < MAX_VREGS)
                    fn->vregs[ins->dst].is_cs_ref = true;
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
        else if (strcmp(opname, "icall") == 0) {
            ins->op = IR_ICALL;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);  /* addr vreg */
            skip_comma(&p);
            p = read_word(p, ins->name, sizeof(ins->name)); /* extern name */
            /* Mark addr vreg as needing an addressable register */
            if (ins->src1 >= 0 && ins->src1 < MAX_VREGS)
                fn->vregs[ins->src1].needs_addressable = true;
            /* Parse args */
            ins->nargs = 0;
            while (*p) {
                skip_comma(&p);
                p = skip_ws(p);
                if (*p != '%') break;
                int a = parse_vreg(p, &p);
                if (ins->nargs < 8) ins->extra_args[ins->nargs] = a;
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
        else if (strcmp(opname, "load") == 0 || strcmp(opname, "loadb") == 0) {
            ins->op = IR_LOAD;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p); /* array */
            p = skip_ws(p);
            if (*p == '[') { p++; ins->src2 = parse_vreg(p, &p); } /* index */
            if (strcmp(opname, "loadb") == 0 && ins->dst >= 0 && ins->dst < MAX_VREGS)
                fn->vregs[ins->dst].is_byte = true;
        }
        else if (strcmp(opname, "store") == 0 || strcmp(opname, "storeb") == 0) {
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
            if (strcmp(opname, "storeb") == 0 && ins->dst >= 0 && ins->dst < MAX_VREGS)
                fn->vregs[ins->dst].is_byte = true;
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
            p = skip_ws(p);
            if (*p == '%') {
                ins->src1 = parse_vreg(p, &p);
            } else {
                /* Immediate value (e.g., setflag DF, 0) */
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
            }
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
                /* far.lit %d, seg, off — store far literal on stack */
                ins->op = IR_FAR_LIT;
                p = skip_ws(p);
                int seg_val = (int)strtol(p, (char **)&p, 0);
                skip_comma(&p);
                int off_val = (int)strtol(p, (char **)&p, 0);
                ins->imm = seg_val;
                ins->extra_args[0] = off_val;
                ins->has_imm = true;
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
                 strcmp(opname, "push") == 0 || strcmp(opname, "pop") == 0 ||
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
        else if (strcmp(opname, "in") == 0 || strcmp(opname, "inb") == 0) {
            ins->op = IR_ALU;
            strncpy(ins->name, opname, 63);
            ins->dst = parse_vreg(p, &p);
            if (strcmp(opname, "inb") == 0 && ins->dst >= 0 && ins->dst < MAX_VREGS)
                fn->vregs[ins->dst].is_byte = true;
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%') {
                ins->src1 = parse_vreg(p, &p);
            } else {
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
            }
        }
        else if (strcmp(opname, "out") == 0 || strcmp(opname, "outb") == 0) {
            ins->op = IR_ALU;
            strncpy(ins->name, opname, 63);
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
            if (strcmp(opname, "outb") == 0 && ins->src1 >= 0 && ins->src1 < MAX_VREGS)
                fn->vregs[ins->src1].is_byte = true;
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
    strncpy(_cur_parse_module, cur_module, 63);

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
            record_emit(EMIT_FN, nfunctions - 1);
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
            strncpy(db->module, cur_module, 63);
            record_emit(EMIT_DATA, ndata_blocks - 1);
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
            /* Check for at() placement */
            char *at_ptr = strstr(p, "at(");
            if (at_ptr) {
                g->has_at = true;
                char *colon = strchr(at_ptr + 3, ':');
                if (colon) {
                    g->at_seg = (int)strtol(at_ptr + 3, NULL, 0);
                    g->at_off = (int)strtol(colon + 1, NULL, 0);
                }
            }
            strncpy(g->module, cur_module, 63);
            nglobals++;
            record_emit(EMIT_GLOB, nglobals - 1);
            continue;
        }
        if (strncmp(p, ".use ", 5) == 0) {
            p += 5;
            if (*p == '"') p++;
            if (nuse_directives < MAX_USE_DIRECTIVES) {
                char *m = use_modules[nuse_directives];
                int mi = 0;
                while (*p && *p != '"' && *p != '.' && mi < 63)
                    m[mi++] = *p++;
                m[mi] = '\0';
                record_emit(EMIT_USE, nuse_directives);
                nuse_directives++;
            }
            continue;
        }
        if (strncmp(p, ".endat", 6) == 0) {
            record_emit(EMIT_ENDAT, 0);
            continue;
        }
        if (strncmp(p, ".at ", 4) == 0) {
            p += 4;
            if (nat_directives < MAX_AT_DIRECTIVES) {
                int seg = (int)strtol(p, (char **)&p, 0);
                if (*p == ':') p++;
                int off = (int)strtol(p, (char **)&p, 0);
                at_directives[nat_directives].seg = seg;
                at_directives[nat_directives].off = off;
                record_emit(EMIT_AT, nat_directives);
                nat_directives++;
            }
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
        fn->vregs[i].use_count = 0;
        fn->vregs[i].in_loop = false;
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

    /* Count accesses: every reference as src or dst.
     * Excludes the first def (every vreg has one). */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->src1 >= 0 && ins->src1 < fn->nvregs)
            fn->vregs[ins->src1].use_count++;
        if (ins->src2 >= 0 && ins->src2 < fn->nvregs)
            fn->vregs[ins->src2].use_count++;
        for (int j = 0; j < 8; j++)
            if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs)
                fn->vregs[ins->extra_args[j]].use_count++;
        /* Count re-definitions (assignments after the initial def) */
        if (ins->dst >= 0 && ins->dst < fn->nvregs &&
            fn->vregs[ins->dst].def_pos >= 0 &&
            fn->vregs[ins->dst].def_pos < i)
            fn->vregs[ins->dst].use_count++;
    }

    /* Parameters are defined at position -1 (before first insn) */
    for (int i = 0; i < fn->nparams; i++) {
        int v = fn->param_vregs[i];
        if (v >= 0) fn->vregs[v].def_pos = -1;
    }

    /* Vregs that are defined but never read still occupy a register
     * at the def point (e.g. unused call return values clobber AX).
     * Give them a minimal range so they participate in interference. */
    for (int i = 0; i < fn->nvregs; i++) {
        if (fn->vregs[i].def_pos >= 0 && fn->vregs[i].last_use < 0)
            fn->vregs[i].last_use = fn->vregs[i].def_pos;
    }

    /* Loop-aware liveness: find backward jumps (loops) and extend
     * liveness of vregs used inside the loop body.
     * A vreg defined before the loop and used inside it must be live
     * for the entire loop (since the back-edge re-enters). */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->op != IR_JMP) continue;
        /* Find the label this jumps to */
        int target = -1;
        for (int j = 0; j < fn->nlabels; j++) {
            if (strcmp(fn->labels[j].name, ins->name) == 0) {
                target = fn->labels[j].insn_idx;
                break;
            }
        }
        if (target < 0 || target >= i) continue; /* not a back-edge */
        /* Loop from target..i — extend liveness of vregs used in this range */
        int loop_end = i;
        for (int v = 0; v < fn->nvregs; v++) {
            int def = fn->vregs[v].def_pos;
            int use = fn->vregs[v].last_use;
            if (def < 0 || use < 0) continue;
            /* Only extend vregs defined BEFORE the loop and used inside it.
             * Vregs defined inside the loop are re-created each iteration. */
            if (def < target && use >= target && use < loop_end) {
                fn->vregs[v].last_use = loop_end;
                fn->vregs[v].in_loop = true;
            }
            /* Also mark vregs defined AND used inside the loop body */
            if (def >= target && def < loop_end &&
                use >= target && use <= loop_end) {
                fn->vregs[v].in_loop = true;
            }
        }
    }
}

/* Check if two vregs have overlapping live ranges */
static bool vregs_interfere(func_t *fn, int a, int b) {
    int a_start = fn->vregs[a].def_pos;
    int a_end   = fn->vregs[a].last_use;
    int b_start = fn->vregs[b].def_pos;
    int b_end   = fn->vregs[b].last_use;

    /* Unused vregs don't interfere */
    if ((a_start < 0 && a_end < 0) || (b_start < 0 && b_end < 0))
        return false;

    /* Parameters have def_pos=-1 (before first insn); treat as valid */
    if (a_end < 0 || b_end < 0)
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
        /* Shift/rotate: count operand must be CL */
        if (ins->op == IR_ALU && !ins->has_imm) {
            const char *op = ins->name;
            if (strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0 ||
                strcmp(op, "sar") == 0 || strcmp(op, "rol") == 0 ||
                strcmp(op, "ror") == 0 || strcmp(op, "rcl") == 0 ||
                strcmp(op, "rcr") == 0) {
                if (ins->src2 >= 0 && ins->src2 < MAX_VREGS) {
                    fn->vregs[ins->src2].needs_cl = true;
                    fn->vregs[ins->src2].is_byte = true;
                }
            }
        }
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
            if (fn->vregs[i].needs_cl && preg != PREG_CL) {
                continue;
            }
            /* Check for conflicts with already-assigned vregs */
            bool conflict = false;
            for (int j = 0; j < i; j++) {
                if (fn->vregs[j].assigned == PREG_NONE) continue;
                if (!vregs_interfere(fn, i, j)) continue;
                if (fn->vregs[j].assigned == preg ||
                    pregs_alias(fn->vregs[j].assigned, preg)) {
                    conflict = true;
                    break;
                }
            }
            if (conflict) continue;
            fn->vregs[i].assigned = preg;
        }
    }

    /* Pre-spill: const vregs with few accesses outside loops don't
     * justify occupying a register for their entire live range.
     * Only applies on the second allocation pass (bp_available=false),
     * when we already know there's register pressure. */
    if (!bp_available)
    for (int i = 0; i < fn->nvregs; i++) {
        if (fn->vregs[i].assigned != PREG_NONE) continue;
        if (!fn->vregs[i].is_const) continue;
        if (fn->vregs[i].in_loop) continue;
        if (fn->vregs[i].def_pos < 0 && fn->vregs[i].last_use < 0) continue;
        /* Don't pre-spill vregs with register constraints */
        if (fn->vregs[i].prefer != PREG_NONE) continue;
        if (fn->vregs[i].needs_cl) continue;
        if (fn->vregs[i].needs_addressable) continue;
        if (fn->vregs[i].is_seg) continue;
        if (fn->vregs[i].use_count <= 2) {
            fn->vregs[i].spill_slot = fn->nspill_slots++;
        }
    }

    /* Second pass: assign remaining vregs */
    for (int i = 0; i < fn->nvregs; i++) {
        if (fn->vregs[i].assigned != PREG_NONE) continue;
        if (fn->vregs[i].spill_slot >= 0) continue;  /* already pre-spilled */
        if (fn->vregs[i].def_pos < 0 && fn->vregs[i].last_use < 0) continue;

        int *pool;
        int poolsz;
        if (fn->vregs[i].is_seg) {
            /* Segment registers: only ES is freely allocatable.
             * DS and SS are critical (data segment, stack segment). */
            static int seg_pool[] = { PREG_ES };
            pool = seg_pool;
            poolsz = 1;
        } else if (fn->vregs[i].needs_cl) {
            /* Shift/rotate count: must be CL */
            static int cl_pool[] = { PREG_CL };
            pool = cl_pool;
            poolsz = 1;
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
            /* Before spilling this vreg, check if we can evict a
             * const vreg from a register instead.  Const vregs are
             * cheaper to spill because they don't need write-back. */
            if (!fn->vregs[i].is_const) {
                for (int r = 0; r < poolsz && !assigned; r++) {
                    int preg = pool[r];
                    /* Find a const vreg in this register that conflicts */
                    for (int j = 0; j < fn->nvregs; j++) {
                        if (j == i) continue;
                        if (fn->vregs[j].assigned != preg) continue;
                        if (!fn->vregs[j].is_const) continue;
                        if (!vregs_interfere(fn, i, j)) continue;
                        /* Check no OTHER non-const conflict blocks us */
                        bool other_conflict = false;
                        for (int k = 0; k < fn->nvregs; k++) {
                            if (k == i || k == j) continue;
                            if (fn->vregs[k].assigned == PREG_NONE) continue;
                            if (!vregs_interfere(fn, i, k)) continue;
                            if (fn->vregs[k].assigned == preg ||
                                pregs_alias(fn->vregs[k].assigned, preg)) {
                                other_conflict = true;
                                break;
                            }
                        }
                        if (!other_conflict) {
                            /* Evict the const vreg */
                            fn->vregs[j].assigned = PREG_NONE;
                            fn->vregs[j].spill_slot = fn->nspill_slots++;
                            fn->vregs[i].assigned = preg;
                            assigned = true;
                            break;
                        }
                    }
                }
            }
            if (!assigned) {
                /* Spill this vreg */
                fn->vregs[i].spill_slot = fn->nspill_slots++;
            }
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

/* Look up a constant pool label in a function */
static const char *resolve_const_label(func_t *fn, const char *name) {
    if (name[0] == '_' && name[1] == 'C') {
        static char buf[64];
        snprintf(buf, sizeof(buf), "%s_%s", fn_asm_name(fn), name);
        return buf;
    }
    return NULL;
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

/* Check if a vreg is spilled to memory */
static bool is_spilled(func_t *fn, int v) {
    if (v < 0 || v >= MAX_VREGS) return false;
    return fn->vregs[v].spill_slot >= 0;
}

/* Emit a mov that handles memory-to-memory via AX scratch */
static void emit_mov(func_t *fn, int dst, int src) {
    const char *d = vreg_asm(fn, dst);
    const char *s = vreg_asm(fn, src);
    if (strcmp(d, s) == 0) return; /* skip self-moves */
    bool dst_byte = (dst >= 0 && dst < MAX_VREGS && fn->vregs[dst].is_byte);
    bool src_byte = (src >= 0 && src < MAX_VREGS && fn->vregs[src].is_byte);
    if (is_spilled(fn, dst) && is_spilled(fn, src)) {
        if (dst_byte || src_byte) {
            /* Byte spill-to-spill: route through AL to avoid
             * word push/pop copying garbage from adjacent bytes. */
            fprintf(out_asm, "    push AX\n");
            fprintf(out_asm, "    mov AL, %s\n", s);
            fprintf(out_asm, "    mov %s, AL\n", d);
            fprintf(out_asm, "    pop AX\n");
        } else {
            /* Word spill-to-spill: use push/pop */
            fprintf(out_asm, "    push word %s\n", s);
            fprintf(out_asm, "    pop word %s\n", d);
        }
    } else if (fn->vregs[dst].is_seg && fn->vregs[src].is_seg) {
        /* seg-to-seg: go through AX, saving it first */
        fprintf(out_asm, "    push AX\n");
        fprintf(out_asm, "    mov AX, %s\n", s);
        fprintf(out_asm, "    mov %s, AX\n", d);
        fprintf(out_asm, "    pop AX\n");
    } else if (dst_byte && !src_byte && !is_spilled(fn, src)) {
        /* byte dst, word src: extract low byte through AX */
        int src_preg = fn->vregs[src].assigned;
        if (src_preg >= PREG_AX && src_preg <= PREG_BX) {
            /* AX..BX have accessible low bytes */
            fprintf(out_asm, "    mov %s, %s\n", d, preg_name[preg_alias_lo[src_preg]]);
        } else {
            /* SI, DI, BP — no low byte; use xchg AX to access via AL */
            int dst_preg2 = fn->vregs[dst].assigned;
            if (dst_preg2 == PREG_AL) {
                /* dst is AL — mov AX,src puts low byte in AL */
                fprintf(out_asm, "    mov AX, %s\n", s);
            } else {
                /* Any other byte reg — xchg AX with src, copy AL, restore */
                fprintf(out_asm, "    xchg AX, %s\n", s);
                fprintf(out_asm, "    mov %s, AL\n", d);
                fprintf(out_asm, "    xchg AX, %s\n", s);
            }
        }
    } else if (!dst_byte && src_byte && !is_spilled(fn, dst)) {
        /* word dst, byte src: zero-extend or just move low byte */
        int dst_preg = fn->vregs[dst].assigned;
        if (dst_preg >= PREG_AX && dst_preg <= PREG_BX) {
            fprintf(out_asm, "    mov %s, %s\n", preg_name[preg_alias_lo[dst_preg]], s);
        } else {
            fprintf(out_asm, "    push AX\n");
            fprintf(out_asm, "    xor AX, AX\n");
            fprintf(out_asm, "    mov AL, %s\n", s);
            fprintf(out_asm, "    mov %s, AX\n", d);
            fprintf(out_asm, "    pop AX\n");
        }
    } else {
        fprintf(out_asm, "    mov %s, %s\n", d, s);
    }
}

static void emit_function(func_t *fn) {
    const char *asm_name = fn_asm_name(fn);
    fprintf(out_asm, "\n; === %s ===\n", asm_name);

    if (fn->has_at) {
        int linear = fn->at_seg * 16 + fn->at_off;
        fprintf(out_asm, "    org 0x%05X ; %04X:%04X\n",
                linear, fn->at_seg, fn->at_off);
        fprintf(out_asm, "    seg 0x%04X\n", fn->at_seg);
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
                const char *d = vreg_asm(fn, ins->dst);
                /* Segment registers can't take immediates — use AX as intermediate */
                if (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                    fn->vregs[ins->dst].is_seg) {
                    fprintf(out_asm, "    mov AX, %d\n", ins->imm);
                    fprintf(out_asm, "    mov %s, AX\n", d);
                } else {
                    fprintf(out_asm, "    mov %s, %d\n", d, ins->imm);
                }
            } else if (ins->name[0]) {
                /* Label reference — load address of constant or function */
                const char *d = vreg_asm(fn, ins->dst);
                const char *clbl = resolve_const_label(fn, ins->name);
                const char *label = clbl ? clbl : resolve_fn_name(ins->name);
                /* Segment registers can't take label refs either */
                if (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                    fn->vregs[ins->dst].is_seg) {
                    fprintf(out_asm, "    mov AX, %s\n", label);
                    fprintf(out_asm, "    mov %s, AX\n", d);
                } else {
                    fprintf(out_asm, "    mov %s, %s\n", d, label);
                }
            } else {
                emit_mov(fn, ins->dst, ins->src1);
            }
            break;

        case IR_ALU: {
            const char *op = ins->name;

            /* Special two-operand forms */
            if (strcmp(op, "in") == 0 || strcmp(op, "inb") == 0) {
                /* IN always reads into AL (byte) or AX (word) */
                bool byte_in = (strcmp(op, "inb") == 0);
                const char *acc = byte_in ? "AL" : "AX";
                if (ins->has_imm)
                    fprintf(out_asm, "    in %s, 0x%02X\n", acc, ins->imm);
                else
                    fprintf(out_asm, "    in %s, %s\n", acc, vreg_asm(fn, ins->src1));
                fprintf(out_asm, "    mov %s, %s\n", vreg_asm(fn, ins->dst), acc);
                break;
            }
            if (strcmp(op, "out") == 0 || strcmp(op, "outb") == 0) {
                /* OUT always uses AL (byte) or AX (word) as source */
                bool byte_out = (strcmp(op, "outb") == 0);
                const char *acc = byte_out ? "AL" : "AX";
                fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src1));
                if (ins->has_imm)
                    fprintf(out_asm, "    out 0x%02X, %s\n", ins->imm, acc);
                else
                    fprintf(out_asm, "    out %s, %s\n",
                            vreg_asm(fn, ins->dst), acc);
                break;
            }
            if (strcmp(op, "far.off") == 0) {
                /* Load offset word from [ptr+0] */
                bool cs = (ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
                           fn->vregs[ins->src1].is_cs_ref);
                const char *seg = cs ? "CS:" : "";
                if (is_spilled(fn, ins->src1)) {
                    fprintf(out_asm, "    push BX\n");
                    fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                    if (is_spilled(fn, ins->dst)) {
                        fprintf(out_asm, "    mov AX, [%sBX]\n", seg);
                        fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
                    } else {
                        fprintf(out_asm, "    mov %s, [%sBX]\n",
                                vreg_asm(fn, ins->dst), seg);
                    }
                    fprintf(out_asm, "    pop BX\n");
                } else if (is_spilled(fn, ins->dst)) {
                    fprintf(out_asm, "    mov AX, [%s%s]\n",
                            seg, vreg_asm(fn, ins->src1));
                    fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
                } else {
                    fprintf(out_asm, "    mov %s, [%s%s]\n",
                            vreg_asm(fn, ins->dst), seg, vreg_asm(fn, ins->src1));
                }
                break;
            }
            if (strcmp(op, "far.seg") == 0) {
                /* Load segment word from [ptr+2] */
                bool cs = (ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
                           fn->vregs[ins->src1].is_cs_ref);
                const char *seg = cs ? "CS:" : "";
                const char *d = vreg_asm(fn, ins->dst);
                bool dst_seg = (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                                fn->vregs[ins->dst].is_seg);
                if (is_spilled(fn, ins->src1)) {
                    fprintf(out_asm, "    push BX\n");
                    fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                    if (dst_seg || is_spilled(fn, ins->dst)) {
                        fprintf(out_asm, "    mov AX, [%sBX+2]\n", seg);
                        fprintf(out_asm, "    mov %s, AX\n", d);
                    } else {
                        fprintf(out_asm, "    mov %s, [%sBX+2]\n", d, seg);
                    }
                    fprintf(out_asm, "    pop BX\n");
                } else {
                    const char *s = vreg_asm(fn, ins->src1);
                    if (dst_seg || is_spilled(fn, ins->dst)) {
                        fprintf(out_asm, "    mov AX, [%s%s+2]\n", seg, s);
                        fprintf(out_asm, "    mov %s, AX\n", d);
                    } else {
                        fprintf(out_asm, "    mov %s, [%s%s+2]\n", d, seg, s);
                    }
                }
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
                if (ins->has_imm) {
                    /* IMUL reg, imm (186+ three-operand form).
                     * Only works with word registers. For byte regs,
                     * zero-extend to word, multiply, result in word. */
                    bool src_is_byte = (ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
                                        fn->vregs[ins->src1].is_byte);
                    if (src_is_byte) {
                        /* Zero-extend byte to AX, multiply as word.
                         * Use AX as scratch — push/pop to preserve. */
                        const char *s = vreg_asm(fn, ins->src1);
                        int src_preg = fn->vregs[ins->src1].assigned;
                        int parent = (src_preg >= PREG_AL && src_preg <= PREG_BH)
                                     ? preg_alias_parent[src_preg] : -1;
                        /* Use a word register for the zero-extend + multiply.
                         * Pick the parent of the source byte reg.
                         * Save/restore the other byte half if it's live. */
                        {
                            int wd_preg = (parent >= 0) ? parent : PREG_AX;
                            const char *wd = preg_name[wd_preg];
                            const char *lo = preg_name[preg_alias_lo[wd_preg]];
                            const char *hi = preg_name[preg_alias_hi[wd_preg]];
                            int other_half = (src_preg == preg_alias_lo[wd_preg])
                                             ? preg_alias_hi[wd_preg]
                                             : preg_alias_lo[wd_preg];
                            /* Check if the other half is live */
                            bool save_other = false;
                            for (int v2 = 0; v2 < fn->nvregs; v2++) {
                                if (fn->vregs[v2].assigned == other_half &&
                                    fn->vregs[v2].def_pos <= (int)i &&
                                    fn->vregs[v2].last_use > (int)i) {
                                    save_other = true;
                                    break;
                                }
                            }
                            if (save_other)
                                fprintf(out_asm, "    push %s\n", wd);
                            if (src_preg == preg_alias_hi[wd_preg]) {
                                fprintf(out_asm, "    mov %s, %s\n", lo, s);
                                fprintf(out_asm, "    xor %s, %s\n", hi, hi);
                            } else {
                                fprintf(out_asm, "    xor %s, %s\n", hi, hi);
                            }
                            fprintf(out_asm, "    imul %s, %d\n", wd, ins->imm);
                            if (save_other) {
                                /* Result is in word reg. Move to dst first,
                                 * then restore. */
                                emit_mov(fn, ins->dst, ins->src1);
                                fprintf(out_asm, "    pop %s\n", wd);
                                /* Skip the emit_mov after the if block */
                                goto imul_done;
                            }
                        }
                    } else {
                        fprintf(out_asm, "    imul %s, %d\n",
                                vreg_asm(fn, ins->src1), ins->imm);
                    }
                    emit_mov(fn, ins->dst, ins->src1);
                    imul_done:;
                } else {
                    fprintf(out_asm, "    %s %s\n", op, vreg_asm(fn, ins->src2));
                }
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
               op %d, %l, imm becomes  mov %d, %l; op %d, imm
               If both operands are spilled, use AX as scratch. */
            emit_mov(fn, ins->dst, ins->src1);
            if (ins->has_imm) {
                fprintf(out_asm, "    %s %s, %d\n", mnem,
                        vreg_asm(fn, ins->dst), ins->imm);
            } else if (strcmp(mnem, "shl") == 0 || strcmp(mnem, "shr") == 0 ||
                       strcmp(mnem, "sar") == 0 || strcmp(mnem, "rol") == 0 ||
                       strcmp(mnem, "ror") == 0 || strcmp(mnem, "rcl") == 0 ||
                       strcmp(mnem, "rcr") == 0) {
                /* Shift/rotate: count must be CL on x86 */
                if (fn->vregs[ins->src2].assigned == PREG_CL) {
                    fprintf(out_asm, "    %s %s, CL\n", mnem,
                            vreg_asm(fn, ins->dst));
                } else {
                    /* Route count through CL, saving/restoring CX */
                    fn->ncl_fixups++;
                    int dst_preg = fn->vregs[ins->dst].assigned;
                    bool dst_is_cx = (dst_preg == PREG_CX ||
                                      dst_preg == PREG_CL ||
                                      dst_preg == PREG_CH);
                    if (dst_is_cx) {
                        /* dst aliases CX — shift in AL, save/restore
                         * CX so that CL (which may hold a live vreg)
                         * is preserved across the count load. */
                        const char *d = vreg_asm(fn, ins->dst);
                        bool byte_dst = fn->vregs[ins->dst].is_byte;
                        fprintf(out_asm, "    push AX\n");
                        fprintf(out_asm, "    mov %s, %s\n",
                                byte_dst ? "AL" : "AX", d);
                        fprintf(out_asm, "    push CX\n");
                        fprintf(out_asm, "    mov CL, %s\n",
                                vreg_asm(fn, ins->src2));
                        fprintf(out_asm, "    %s %s, CL\n", mnem,
                                byte_dst ? "AL" : "AX");
                        fprintf(out_asm, "    pop CX\n");
                        fprintf(out_asm, "    mov %s, %s\n", d,
                                byte_dst ? "AL" : "AX");
                        fprintf(out_asm, "    pop AX\n");
                    } else {
                        fprintf(out_asm, "    push CX\n");
                        fprintf(out_asm, "    mov CL, %s\n",
                                vreg_asm(fn, ins->src2));
                        fprintf(out_asm, "    %s %s, CL\n", mnem,
                                vreg_asm(fn, ins->dst));
                        fprintf(out_asm, "    pop CX\n");
                    }
                }
            } else if (is_spilled(fn, ins->dst) && is_spilled(fn, ins->src2)) {
                /* op [mem], [mem] — load src2 into scratch, then op */
                bool alu_byte = fn->vregs[ins->dst].is_byte;
                const char *acc = alu_byte ? "AL" : "AX";
                fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src2));
                fprintf(out_asm, "    %s %s, %s\n", mnem, vreg_asm(fn, ins->dst), acc);
            } else {
                fprintf(out_asm, "    %s %s, %s\n", mnem,
                        vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src2));
            }
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
                emit_mov(fn, ins->dst, ins->src1);
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
                emit_mov(fn, ins->dst, ins->src1);
                fprintf(out_asm, "    lahf\n");
                fprintf(out_asm, "    mov %s, AH\n", d);
                fprintf(out_asm, "    mov AH, %s\n", s);
                fprintf(out_asm, "    sahf\n");
                break;
            }
            if (strcmp(mnem, "zext") == 0) {
                /* Zero-extend u8 -> u16: clear word, then mov low byte.
                 * dst is a word reg or spill slot, src is byte.
                 * Must handle case where src is AL — xor AX clobbers it. */
                int dst_preg = fn->vregs[ins->dst].assigned;
                int src_preg = fn->vregs[ins->src1].assigned;
                if (dst_preg >= PREG_AX && dst_preg <= PREG_BX &&
                    !is_spilled(fn, ins->dst)) {
                    /* dst is AX..BX — zero the word, then copy byte in */
                    if (preg_alias_lo[dst_preg] == src_preg) {
                        /* src is already the low byte of dst — just clear high */
                        fprintf(out_asm, "    xor %s, %s\n",
                                preg_name[preg_alias_hi[dst_preg]],
                                preg_name[preg_alias_hi[dst_preg]]);
                    } else {
                        fprintf(out_asm, "    xor %s, %s\n", d, d);
                        fprintf(out_asm, "    mov %s, %s\n",
                                preg_name[preg_alias_lo[dst_preg]], s);
                    }
                } else {
                    /* Spilled or SI/DI/BP — use AX as scratch.
                     * If src is AL, save it first since xor AX clobbers AL. */
                    bool src_is_al = (src_preg == PREG_AL);
                    if (src_is_al) {
                        fprintf(out_asm, "    xor AH, AH\n");
                        fprintf(out_asm, "    mov %s, AX\n", d);
                    } else {
                        fprintf(out_asm, "    push AX\n");
                        fprintf(out_asm, "    xor AX, AX\n");
                        fprintf(out_asm, "    mov AL, %s\n", s);
                        fprintf(out_asm, "    mov %s, AX\n", d);
                        fprintf(out_asm, "    pop AX\n");
                    }
                }
                break;
            }
            emit_mov(fn, ins->dst, ins->src1);
            if (is_spilled(fn, ins->dst) && fn->vregs[ins->dst].is_byte) {
                /* Unary on spilled byte: route through AL to
                 * avoid word-sized not/neg on the spill slot. */
                fprintf(out_asm, "    push AX\n");
                fprintf(out_asm, "    mov AL, %s\n", vreg_asm(fn, ins->dst));
                fprintf(out_asm, "    %s AL\n", mnem);
                fprintf(out_asm, "    mov %s, AL\n", vreg_asm(fn, ins->dst));
                fprintf(out_asm, "    pop AX\n");
            } else {
                fprintf(out_asm, "    %s %s\n", mnem, vreg_asm(fn, ins->dst));
            }
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
            bool gf_emitted = false;
            /* Check regular functions */
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0) {
                    if (functions[fi2].has_at) {
                        fprintf(out_asm, "    jmp far 0x%04X:0x%04X\n",
                                functions[fi2].at_seg, functions[fi2].at_off);
                        gf_emitted = true;
                    } else if (functions[fi2].is_far ||
                               (fn->emit_seg >= 0 && functions[fi2].emit_seg >= 0 &&
                                fn->emit_seg != functions[fi2].emit_seg)) {
                        fprintf(out_asm, "    jmp far %s\n", resolve_fn_name(ins->name));
                        gf_emitted = true;
                    } else {
                        fprintf(out_asm, "    jmp %s\n", resolve_fn_name(ins->name));
                        gf_emitted = true;
                    }
                    break;
                }
            }
            /* Check externs */
            if (!gf_emitted) {
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0) {
                        if (externs[e].has_address) {
                            fprintf(out_asm, "    jmp far 0x%04X:0x%04X\n",
                                    externs[e].addr_seg, externs[e].addr_off);
                        } else if (externs[e].is_far) {
                            fprintf(out_asm, "    jmp far %s\n", ins->name);
                        } else {
                            fprintf(out_asm, "    jmp %s\n", ins->name);
                        }
                        gf_emitted = true;
                        break;
                    }
                }
            }
            if (!gf_emitted)
                fprintf(out_asm, "    jmp %s\n", resolve_fn_name(ins->name));
            break;
        }

        case IR_JMP:
            fprintf(out_asm, "    jmp %s\n", scoped_label(fn, ins->name));
            break;

        case IR_CALL: {
            /* Caller-save: push live registers that the callee may clobber */
            bool callee_preserves[NUM_PREGS] = {0};
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0) {
                    for (int pp = 0; pp < functions[fi2].nfn_preserves; pp++)
                        callee_preserves[functions[fi2].fn_preserves[pp]] = true;
                    break;
                }
            }
            int call_saved[16];
            int call_nsaved = 0;

            /* If the caller uses BP as a frame pointer and the callee
             * doesn't preserve it, BP must be saved/restored.  The
             * vreg scan below won't catch it because BP isn't assigned
             * to any vreg — it's the implicit frame pointer. */
            if (fn->needs_frame && !callee_preserves[PREG_BP]) {
                call_saved[call_nsaved++] = PREG_BP;
                fprintf(out_asm, "    push BP\n");
            }

            for (int v = 0; v < fn->nvregs; v++) {
                int preg = fn->vregs[v].assigned;
                if (preg == PREG_NONE || preg == PREG_SP) continue;
                if (fn->vregs[v].is_seg) continue;
                if (fn->vregs[v].last_use <= (int)i) continue;
                if (fn->vregs[v].def_pos > (int)i) continue;
                int push_reg = preg;
                if (preg >= PREG_AL && preg <= PREG_BH)
                    push_reg = preg_alias_parent[preg];
                if (callee_preserves[push_reg]) continue;
                bool dup = false;
                for (int s = 0; s < call_nsaved; s++)
                    if (call_saved[s] == push_reg) { dup = true; break; }
                if (dup) continue;
                if (call_nsaved < 16)
                    call_saved[call_nsaved++] = push_reg;
                fprintf(out_asm, "    push %s\n", preg_name[push_reg]);
            }

            /* Argument fixup: if a call argument vreg isn't in the
             * register the callee expects, emit a mov to place it.
             * This handles spilled args and preference mismatches. */
            {
                /* Find callee's expected parameter registers */
                int callee_fi = -1;
                for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                    if (strcmp(functions[fi2].name, ins->name) == 0) {
                        callee_fi = fi2;
                        break;
                    }
                }
                if (callee_fi >= 0) {
                    fn_assignment_t *callee_fa = &fn_assigns[callee_fi];
                    /* Collect fixups: args not in the expected register */
                    int fix_src[16], fix_dst[16];
                    int nfix = 0;
                    for (int a = 0; a < ins->nargs; a++) {
                        int arg_vreg;
                        if (a == 0) arg_vreg = ins->src1;
                        else if (a == 1) arg_vreg = ins->src2;
                        else arg_vreg = ins->extra_args[a - 2];
                        if (arg_vreg < 0) continue;
                        int expected = callee_fa->param_regs[a];
                        if (expected == PREG_NONE) continue;
                        int actual = fn->vregs[arg_vreg].assigned;
                        /* Check if already in the right register */
                        if (actual == expected) continue;
                        /* Check if actual is a byte alias of expected
                         * (e.g., AL when AX expected — close enough) */
                        if (pregs_alias(actual, expected)) continue;
                        fix_src[nfix] = arg_vreg;
                        fix_dst[nfix] = expected;
                        nfix++;
                    }
                    /* Emit fixups: spilled args first, then reg-to-reg */
                    for (int f = 0; f < nfix; f++) {
                        int dst_preg = fix_dst[f];
                        const char *dst_name = preg_name[dst_preg];
                        if (is_spilled(fn, fix_src[f])) {
                            fprintf(out_asm, "    mov %s, %s\n",
                                    dst_name, vreg_asm(fn, fix_src[f]));
                        } else {
                            int src_preg = fn->vregs[fix_src[f]].assigned;
                            fprintf(out_asm, "    mov %s, %s\n",
                                    dst_name, preg_name[src_preg]);
                        }
                    }
                }
            }

            /* Emit the actual call instruction */
            if (fn->has_chain && strcmp(ins->name, fn->chain_name) == 0) {
                fprintf(out_asm, "    pushf\n");
                fprintf(out_asm, "    call far [%s_vec]\n", fn->chain_name);
            } else {
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
                if (!found_extern) {
                    bool callee_far = false;
                    for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                        if (strcmp(functions[fi2].name, ins->name) == 0) {
                            callee_far = functions[fi2].is_far;
                            break;
                        }
                    }
                    if (callee_far)
                        fprintf(out_asm, "    call far %s\n", resolve_fn_name(ins->name));
                    else
                        fprintf(out_asm, "    call %s\n", resolve_fn_name(ins->name));
                }
            }
            /* Caller-restore: pop saved registers (reverse order) */
            for (int s = call_nsaved - 1; s >= 0; s--)
                fprintf(out_asm, "    pop %s\n", preg_name[call_saved[s]]);
            break;
        }

        case IR_ICALL: {
            /* Indirect far call through a memory-resident far pointer.
             * src1 = vreg holding address of the far pointer in memory. */
            const char *addr = vreg_asm(fn, ins->src1);
            if (is_spilled(fn, ins->src1)) {
                /* Spilled: the spill slot IS the memory address */
                fprintf(out_asm, "    call far %s\n", addr);
            } else {
                /* In a register: dereference as [reg] */
                fprintf(out_asm, "    call far [%s]\n", addr);
            }
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
            } else if (fn->is_far) {
                fprintf(out_asm, "    retf\n");
            } else {
                fprintf(out_asm, "    ret\n");
            }
            break;

        case IR_LOAD: {
            const char *base_str;
            const char *idx_str = NULL;
            bool pushed_bx = false, pushed_si = false;
            /* Load spilled base/index into scratch regs */
            if (is_spilled(fn, ins->src1)) {
                fprintf(out_asm, "    push BX\n");
                fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                base_str = "BX";
                pushed_bx = true;
            } else {
                base_str = vreg_asm(fn, ins->src1);
            }
            if (ins->src2 >= 0 && is_spilled(fn, ins->src2)) {
                fprintf(out_asm, "    push SI\n");
                fprintf(out_asm, "    mov SI, %s\n", vreg_asm(fn, ins->src2));
                idx_str = "SI";
                pushed_si = true;
            } else if (ins->src2 >= 0) {
                idx_str = vreg_asm(fn, ins->src2);
            }
            if (is_spilled(fn, ins->dst)) {
                bool ld_byte = fn->vregs[ins->dst].is_byte;
                const char *acc = ld_byte ? "AL" : "AX";
                if (idx_str)
                    fprintf(out_asm, "    mov %s, [%s+%s]\n", acc, base_str, idx_str);
                else
                    fprintf(out_asm, "    mov %s, [%s]\n", acc, base_str);
                if (pushed_si) fprintf(out_asm, "    pop SI\n");
                if (pushed_bx) fprintf(out_asm, "    pop BX\n");
                fprintf(out_asm, "    mov %s, %s\n", vreg_asm(fn, ins->dst), acc);
            } else {
                if (idx_str)
                    fprintf(out_asm, "    mov %s, [%s+%s]\n",
                            vreg_asm(fn, ins->dst), base_str, idx_str);
                else
                    fprintf(out_asm, "    mov %s, [%s]\n",
                            vreg_asm(fn, ins->dst), base_str);
                if (pushed_si) fprintf(out_asm, "    pop SI\n");
                if (pushed_bx) fprintf(out_asm, "    pop BX\n");
            }
            break;
        }

        case IR_STORE: {
            const char *val_str;
            const char *base_str;
            const char *idx_str = NULL;
            bool pushed_bx = false, pushed_si = false;
            /* Value to store */
            if (is_spilled(fn, ins->dst)) {
                bool st_byte = fn->vregs[ins->dst].is_byte;
                const char *acc = st_byte ? "AL" : "AX";
                fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->dst));
                val_str = acc;
            } else {
                val_str = vreg_asm(fn, ins->dst);
            }
            /* Load spilled base/index into scratch regs */
            if (is_spilled(fn, ins->src1)) {
                fprintf(out_asm, "    push BX\n");
                fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                base_str = "BX";
                pushed_bx = true;
            } else {
                base_str = vreg_asm(fn, ins->src1);
            }
            if (ins->src2 >= 0 && is_spilled(fn, ins->src2)) {
                fprintf(out_asm, "    push SI\n");
                fprintf(out_asm, "    mov SI, %s\n", vreg_asm(fn, ins->src2));
                idx_str = "SI";
                pushed_si = true;
            } else if (ins->src2 >= 0) {
                idx_str = vreg_asm(fn, ins->src2);
            }
            /* Emit the store */
            if (idx_str)
                fprintf(out_asm, "    mov [%s+%s], %s\n", base_str, idx_str, val_str);
            else
                fprintf(out_asm, "    mov [%s], %s\n", base_str, val_str);
            if (pushed_si) fprintf(out_asm, "    pop SI\n");
            if (pushed_bx) fprintf(out_asm, "    pop BX\n");
            break;
        }

        case IR_LOADMEM:
        case IR_STOREMEM: {
            /* Fix up address registers: if the address expression
             * references a physical register (e.g., [ES:SI]) and a
             * vreg with .prefer for that register ended up elsewhere,
             * emit a mov to place the value in the expected register. */
            for (int preg = 0; preg < NUM_PREGS; preg++) {
                const char *rn = preg_name[preg];
                /* Check if this register name appears in the address */
                const char *p2 = ins->name;
                bool found = false;
                while ((p2 = strstr(p2, rn)) != NULL) {
                    /* Verify it's a whole word (not a substring) */
                    char before = (p2 > ins->name) ? p2[-1] : '[';
                    char after = p2[strlen(rn)];
                    if (!isalpha(before) && !isalpha(after)) {
                        found = true;
                        break;
                    }
                    p2++;
                }
                if (!found) continue;
                /* Find the most recently defined vreg that prefers
                 * this register but isn't assigned to it.  The vreg
                 * may appear dead (last_use < i) because the loadmem/
                 * storemem address uses the physical register without
                 * referencing the vreg, so we search by def_pos. */
                int best_v = -1, best_def = -1;
                for (int v = 0; v < fn->nvregs; v++) {
                    if (fn->vregs[v].prefer != preg) continue;
                    if (fn->vregs[v].assigned == preg) continue;
                    if (fn->vregs[v].assigned == PREG_NONE) continue;
                    if (fn->vregs[v].def_pos > (int)i) continue;
                    if (fn->vregs[v].def_pos > best_def) {
                        best_def = fn->vregs[v].def_pos;
                        best_v = v;
                    }
                }
                if (best_v >= 0) {
                    fprintf(out_asm, "    mov %s, %s\n",
                            rn, preg_name[fn->vregs[best_v].assigned]);
                }
            }

            if (ins->op == IR_LOADMEM) {
                bool dst_byte = (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                                 fn->vregs[ins->dst].is_byte);
                if (is_spilled(fn, ins->dst)) {
                    const char *acc = dst_byte ? "AL" : "AX";
                    fprintf(out_asm, "    mov %s, %s\n", acc, ins->name);
                    fprintf(out_asm, "    mov %s, %s\n", vreg_asm(fn, ins->dst), acc);
                } else {
                    fprintf(out_asm, "    mov %s, %s\n",
                            vreg_asm(fn, ins->dst), ins->name);
                }
            } else {
                bool src_byte = (ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
                                 fn->vregs[ins->src1].is_byte);
                if (is_spilled(fn, ins->src1)) {
                    const char *acc = src_byte ? "AL" : "AX";
                    fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src1));
                    fprintf(out_asm, "    mov %s, %s\n", ins->name, acc);
                } else {
                    fprintf(out_asm, "    mov %s, %s\n",
                            ins->name, vreg_asm(fn, ins->src1));
                }
            }
            break;
        }
            break;

        case IR_FIELD:
            fprintf(out_asm, "    ; field %s.%s -> %s\n",
                    vreg_asm(fn, ins->src1), ins->name,
                    vreg_asm(fn, ins->dst));
            break;

        case IR_SETFLAG: {
            int val = 1; /* default assume set */
            if (ins->has_imm) {
                val = ins->imm;
            } else if (ins->src1 >= 0) {
                /* Look back for the mov that defined src1 */
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

        case IR_FAR_LIT: {
            /* Store far literal (seg:off) on stack, point dst at it */
            int seg_val = ins->imm;
            int off_val = ins->extra_args[0];
            const char *d = vreg_asm(fn, ins->dst);
            fprintf(out_asm, "    sub sp, 4\n");
            fprintf(out_asm, "    mov word [sp], 0x%04X\n", off_val);
            fprintf(out_asm, "    mov word [sp+2], 0x%04X\n", seg_val);
            fprintf(out_asm, "    mov %s, sp\n", d);
            break;
        }

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
        } else if (fn->is_far) {
            fprintf(out_asm, "    retf\n");
        } else {
            fprintf(out_asm, "    ret\n");
        }
    }

    /* Emit chain vector storage for interrupt handlers */
    if (fn->has_chain) {
        fprintf(out_asm, "%s_vec dw 0, 0 ; saved vector for chaining\n",
                fn->chain_name);
    }

    /* Emit per-function constant pool (strings, far refs) */
    for (int i = 0; i < fn->nconsts; i++) {
        fn_const_t *c = &fn->consts[i];
        if (c->is_far_ref) {
            const char *resolved = resolve_fn_name(c->ref_name);
            fprintf(out_asm, "%s dw %s, SEG %s\n", c->label, resolved, resolved);
        } else {
            fprintf(out_asm, "%s\n", c->data);
        }
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
            if (ins->op != IR_CALL && ins->op != IR_TAILCALL &&
                ins->op != IR_ICALL) continue;

            if (nedges >= MAX_EDGES) break;
            call_edge_t *e = &call_edges[nedges++];
            e->caller_fn = fi;
            e->callee_fn = (ins->op == IR_ICALL) ? -1 : find_fn(ins->name);
            e->insn_idx = i;
            strncpy(e->callee_name, ins->name, 63);
            e->ret_vreg = ins->dst;
            e->nargs = 0;

            if (ins->op == IR_ICALL) {
                /* icall: args are in extra_args (src1 is addr vreg) */
                for (int j = 0; j < 8 && ins->extra_args[j] >= 0; j++)
                    e->arg_vregs[e->nargs++] = ins->extra_args[j];
            } else {
                /* call/tailcall: args in src1, src2, extra_args */
                if (ins->src1 >= 0) e->arg_vregs[e->nargs++] = ins->src1;
                if (ins->src2 >= 0) e->arg_vregs[e->nargs++] = ins->src2;
                for (int j = 0; j < 8 && ins->extra_args[j] >= 0; j++)
                    e->arg_vregs[e->nargs++] = ins->extra_args[j];
            }
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
                if (caller_vreg >= 0 && callee_reg != PREG_NONE &&
                    callee_reg != PREG_SP) {
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
                if (caller->vregs[caller_vreg].prefer == PREG_NONE &&
                    callee_reg != PREG_SP) {
                    caller->vregs[caller_vreg].prefer = callee_reg;
                }
                /* Propagate is_seg from callee parameter */
                if (a < MAX_VREGS && fn->vregs[a].is_seg) {
                    caller->vregs[caller_vreg].is_seg = true;
                }
            }

            /* Return value: set preference on caller's dst vreg */
            if (call_edges[e].ret_vreg >= 0 && fa->return_reg != PREG_NONE &&
                fa->return_reg != PREG_SP) {
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
            if (v >= 0 && fa->param_regs[p] != PREG_NONE &&
                fa->param_regs[p] != PREG_SP) {
                fn->vregs[v].prefer = fa->param_regs[p];
            }
        }
    }
}

/* ================================================================
 * Main
 * ================================================================ */

/* ================================================================
 * Source-order emission with depth-first use expansion
 * ================================================================ */

static int at_depth = 0;
static int current_seg = -1;    /* segment from last at() directive */

static void emit_item(int ei) {
    int kind = emit_order[ei].kind;
    int idx = emit_order[ei].index;

    if (kind == EMIT_FN) {
        functions[idx].emit_seg = functions[idx].has_at
            ? functions[idx].at_seg : current_seg;
        emit_function(&functions[idx]);
    } else if (kind == EMIT_DATA) {
        data_block_t *db = &data_blocks[idx];
        fprintf(out_asm, "\n; === data: %s ===\n", db->label);
        if (db->has_at) {
            int lin = db->at_seg * 16 + db->at_off;
            fprintf(out_asm, "    org 0x%05X ; %04X:%04X\n",
                    lin, db->at_seg, db->at_off);
            fprintf(out_asm, "    seg 0x%04X\n", db->at_seg);
        }
        fprintf(out_asm, "%s:\n", db->label);
        for (int j = 0; j < db->nentries; j++) {
            if (db->entries[j][0] == '\x01') {
                const char *r = resolve_fn_name(db->entries[j] + 1);
                fprintf(out_asm, "    dw %s, SEG %s\n", r, r);
            } else {
                fprintf(out_asm, "%s\n", db->entries[j]);
            }
        }
    } else if (kind == EMIT_GLOB) {
        global_var_t *g = &globals[idx];
        if (g->has_at) {
            int lin = g->at_seg * 16 + g->at_off;
            fprintf(out_asm, "\n    org 0x%05X ; %04X:%04X\n",
                    lin, g->at_seg, g->at_off);
            fprintf(out_asm, "    seg 0x%04X\n", g->at_seg);
            at_depth++;
        }
        fprintf(out_asm, "%s:", g->name);
        if (g->size <= 2)
            fprintf(out_asm, " dw 0\n");
        else {
            fprintf(out_asm, "\n");
            for (int b = 0; b < g->size; b += 2)
                fprintf(out_asm, "    dw 0\n");
        }
    } else if (kind == EMIT_AT) {
        int s = at_directives[idx].seg;
        int o = at_directives[idx].off;
        int lin = s * 16 + o;
        fprintf(out_asm, "\n    org 0x%05X ; %04X:%04X\n", lin, s, o);
        fprintf(out_asm, "    seg 0x%04X\n", s);
        current_seg = s;
        at_depth++;
    } else if (kind == EMIT_ENDAT) {
        fprintf(out_asm, "    endorg\n");
        if (at_depth > 0) at_depth--;
    }
}

static char done_modules[128][64];
static int ndone_modules = 0;

static void emit_module(const char *path) {
    /* Extract module name from path */
    char mod[64];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(mod, base, 63);
    char *dot = strrchr(mod, '.');
    if (dot) *dot = '\0';

    /* Skip if already emitted */
    for (int i = 0; i < ndone_modules; i++)
        if (strcmp(done_modules[i], mod) == 0) return;
    strncpy(done_modules[ndone_modules++], mod, 63);

    /* Walk this module's items in emit_order, expanding .use recursively */
    for (int ei = 0; ei < nemit_order; ei++) {
        if (strcmp(emit_order[ei].module, mod) != 0) continue;
        if (emit_order[ei].kind == EMIT_USE) {
            /* Expand: emit the referenced module depth-first */
            char use_path[128];
            const char *use_mod = use_modules[emit_order[ei].index];
            /* Reconstruct path relative to current module's path */
            const char *slash = strrchr(path, '/');
            if (slash) {
                int dirlen = (int)(slash - path) + 1;
                snprintf(use_path, sizeof(use_path), "%.*s%s.nir",
                         dirlen, path, use_mod);
            } else {
                snprintf(use_path, sizeof(use_path), "%s.nir", use_mod);
            }
            int saved_depth = at_depth;
            emit_module(use_path);
            /* Unwind any at() the used module left on the stack */
            while (at_depth > saved_depth) {
                fprintf(out_asm, "    endorg ; unwind use\n");
                at_depth--;
            }
        } else {
            emit_item(ei);
        }
    }
}

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

        /* Count CL fixups: shift/rotate ops where count didn't get CL */
        fn->ncl_fixups = 0;
        for (int i = 0; i < fn->ninsns; i++) {
            ir_insn_t *ins = &fn->insns[i];
            if (ins->op == IR_ALU && !ins->has_imm && ins->src2 >= 0) {
                const char *op = ins->name;
                if (strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0 ||
                    strcmp(op, "sar") == 0 || strcmp(op, "rol") == 0 ||
                    strcmp(op, "ror") == 0 || strcmp(op, "rcl") == 0 ||
                    strcmp(op, "rcr") == 0) {
                    if (fn->vregs[ins->src2].assigned != PREG_CL)
                        fn->ncl_fixups++;
                }
            }
        }

        /* Debug: show assignments */
        fprintf(stderr, "  %s: %d vregs, %d spills", fn->name, fn->nvregs, fn->nspill_slots);
        if (fn->ncl_fixups > 0)
            fprintf(stderr, ", %d cl fixups", fn->ncl_fixups);
        fprintf(stderr, " [");
        for (int v = 0; v < fn->nvregs; v++) {
            if (fn->vregs[v].assigned != PREG_NONE)
                fprintf(stderr, " %%%d=%s", v, preg_name[fn->vregs[v].assigned]);
            else if (fn->vregs[v].spill_slot >= 0)
                fprintf(stderr, " %%%d=spill%d", v, fn->vregs[v].spill_slot);
        }
        fprintf(stderr, " ]\n");

        /* Phase 4: deferred — emitted below in source order */
    }

    /* Emit in source order via depth-first walk of the use tree */
    ndone_modules = 0;
    emit_module(inputs[ninputs - 1]);

    fclose(out_asm);
    fprintf(stderr, "Wrote %s\n", outpath);
    return 0;
}
