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
#include <limits.h>
#include <stdarg.h>
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

typedef enum {
    DS_POLICY_UNSPEC,
    DS_POLICY_CALLER,
    DS_POLICY_NONE,
    DS_POLICY_SYMBOL,
    DS_POLICY_LITERAL
} ds_policy_t;

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
    if (a < 0 || b < 0) return false;
    if (a == b) return true;
    /* Word vs its byte halves */
    if (a < 4 && (b == preg_alias_lo[a] || b == preg_alias_hi[a])) return true;
    if (b < 4 && (a == preg_alias_lo[b] || a == preg_alias_hi[b])) return true;
    return false;
}

static bool preg_write_clobbers(int written, int live) {
    if (written < 0 || live < 0)
        return false;
    if (written == live)
        return true;
    if (written >= PREG_AX && written <= PREG_BX) {
        return live == preg_alias_lo[written] || live == preg_alias_hi[written];
    }
    if (written >= PREG_AL && written <= PREG_BH) {
        return live == preg_alias_parent[written];
    }
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
#define MAX_RETURNS  8

/* IR instruction opcodes */
typedef enum {
    IR_MOV,         /* mov %d, %s  or  mov %d, imm */
    IR_ALU,         /* add/sub/etc %d, %l, %r */
    IR_UNARY,       /* neg/not/etc %d, %s */
    IR_CMP,         /* cmp.xx %d, %l, %r */
    IR_JZ,          /* jz %cond, label */
    IR_JMP,         /* jmp label */
    IR_CALL,        /* call %d, name, args... */
    IR_MCALL,       /* mcall %d, %d, name, args... */
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
    IR_LEA,         /* lea %dst, %local — address of a stack local */
    IR_FRAME_ENTER, /* frame_enter — push bp, mov bp, sp, sub sp, N */
    IR_FRAME_LEAVE, /* frame_leave — mov sp, bp, pop bp */
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
    int     ret_vregs[MAX_RETURNS]; /* extra return destinations/sources */
    int     nrets;
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
    bool    fixed;          /* true if this vreg denotes an exact register */
    bool    is_byte;        /* true if this vreg is 8-bit */
    bool    is_seg;         /* true if segment register */
    bool    needs_addressable; /* true if used in memory operand (any of base/index) */
    bool    needs_base;        /* true if used as base: must be BX or BP */
    bool    needs_index;       /* true if used as index: must be SI or DI */
    bool    needs_ds_addr;     /* true if used as DS-relative address: BX, SI, DI (not BP) */
    bool    needs_cl;          /* true if used as shift/rotate count: must be CL */
    bool    is_const;          /* true if immutable — prefer to spill over mutable */
    int     use_count;         /* number of instructions referencing this vreg */
    bool    in_loop;           /* true if live range spans a loop back-edge */
    bool    is_cs_ref;         /* true if this vreg points to constant pool (CS segment) */
    char    data_label[64];    /* initialized data label this vreg is based on */
    bool    is_local_slot;     /* true if this vreg names a source stack local */
    int     local_size;        /* bytes reserved for the source local */
    int     local_offset;      /* positive offset within the local area */
    int     assigned;       /* physical reg after coloring, or PREG_NONE */
    int     spill_slot;     /* stack offset if spilled, or -1 */
    /* Liveness */
    int     def_pos;        /* first def position */
    int     last_use;       /* last use position */
    bool    live;           /* currently live in analysis */
} vreg_info_t;

/* Basic block — CFG node for dataflow liveness */
#define VREG_WORDS ((MAX_VREGS + 63) / 64)

typedef struct {
    int      start, end;            /* insn index range [start, end) */
    int      succs[4];              /* successor block indices */
    int      nsuccs;
    int      preds[16];             /* predecessor block indices */
    int      npreds;
    uint64_t live_in[VREG_WORDS];   /* vregs live at block entry */
    uint64_t live_out[VREG_WORDS];  /* vregs live at block exit */
    uint64_t defs[VREG_WORDS];     /* vregs defined in this block */
    uint64_t uses[VREG_WORDS];     /* vregs used before def in this block */
} bblock_t;

/* Bitset helpers for vreg sets */
static inline void vset_set(uint64_t *s, int v)   { s[v >> 6] |= (1ULL << (v & 63)); }
static inline void vset_clear(uint64_t *s, int v) { s[v >> 6] &= ~(1ULL << (v & 63)); }
static inline bool vset_test(const uint64_t *s, int v) { return (s[v >> 6] >> (v & 63)) & 1; }
static inline void vset_zero(uint64_t *s) { memset(s, 0, VREG_WORDS * sizeof(uint64_t)); }
static inline void vset_or(uint64_t *dst, const uint64_t *src) {
    for (int i = 0; i < VREG_WORDS; i++) dst[i] |= src[i];
}
static inline void vset_diff(uint64_t *dst, const uint64_t *a, const uint64_t *b) {
    /* dst = a & ~b */
    for (int i = 0; i < VREG_WORDS; i++) dst[i] = a[i] & ~b[i];
}
static inline bool vset_equal(const uint64_t *a, const uint64_t *b) {
    for (int i = 0; i < VREG_WORDS; i++) if (a[i] != b[i]) return false;
    return true;
}

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
    bool        is_bare;
    bool        has_at;
    int         at_seg;
    int         at_off;
    int         emit_seg;       /* segment at emission time (-1 = unknown) */
    ds_policy_t ds_policy;
    char        ds_symbol[64];
    int         ds_literal;

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
    int         nreturns;
    char        return_types[MAX_RETURNS][32];
    int         ret_pins[MAX_RETURNS];

    bblock_t    blocks[MAX_BLOCKS];
    int         nblocks;

    /* Interference graph: adj[v] is bitset of vregs that interfere with v */
    uint64_t    igraph[MAX_VREGS][VREG_WORDS];
    int         degree[MAX_VREGS]; /* number of neighbors */

    /* Resolved instruction stream (post-allocation, pre-emission) */
#define MAX_RESOLVED 8192
    struct {
        enum { RINS_IR, RINS_ASM } kind;
        union {
            int  ir_idx;        /* index into insns[] */
            char asm_text[128]; /* pre-formatted assembly line */
        };
    } resolved[MAX_RESOLVED];
    int nresolved;

    /* Labels: name -> insn index */
    struct { char name[64]; int insn_idx; } labels[MAX_LABELS];
    int         nlabels;

    /* Allocation state */
    bool        needs_frame;    /* BP reserved for frame pointer */
    int         nspill_slots;
    int         ncl_fixups;     /* CL routing fixups (push/pop CX) */
    int         local_size;     /* total bytes for source stack locals */
    int         frame_size;     /* total bytes for spills and source locals */

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
    ds_policy_t ds_policy;
    char ds_symbol[64];
    int  ds_literal;
    bool has_address;
    int  addr_seg;
    int  addr_off;
    struct { int preg; } param_pins[16];
    int  nparams;
    int  nreturns;
    char return_types[MAX_RETURNS][32];
    int  ret_pins[MAX_RETURNS];
    int  preserves[NUM_PREGS];  /* list of PREG_* preserved by extern */
    int  npreserves;
} extern_fn_t;

#define MAX_EXTERNS 64
static extern_fn_t externs[MAX_EXTERNS];
static int nexterns = 0;

/* Forward declarations */
static const char *fn_asm_name(func_t *fn);
static const char *vreg_asm(func_t *fn, int v);
static bool is_spilled(func_t *fn, int v);
static void add_call_saved_reg(int *call_saved, int *call_nsaved, int preg);

static bool insn_defines_dst(const ir_insn_t *ins) {
    if (!ins || ins->dst < 0)
        return false;
    if (ins->op == IR_STORE || ins->op == IR_STOREMEM ||
        ins->op == IR_STOREFIELD)
        return false;
    if (ins->op == IR_ALU &&
        (strcmp(ins->name, "out") == 0 ||
         strcmp(ins->name, "outb") == 0 ||
         strcmp(ins->name, "bins") == 0 ||
         strcmp(ins->name, "stos") == 0))
        return false;
    return true;
}

static bool insn_reads_dst(const ir_insn_t *ins) {
    return ins && ins->dst >= 0 && !insn_defines_dst(ins);
}

static void mark_preg_clobber(bool *clobbers, int preg) {
    if (!clobbers || preg < 0 || preg >= NUM_PREGS)
        return;
    clobbers[preg] = true;
    if (preg >= PREG_AL && preg <= PREG_BH)
        clobbers[preg_alias_parent[preg]] = true;
}

static void mark_vreg_clobber(func_t *fn, bool *clobbers, int vreg) {
    if (!fn || vreg < 0 || vreg >= fn->nvregs)
        return;
    mark_preg_clobber(clobbers, fn->vregs[vreg].assigned);
}

static void collect_insn_clobbers(func_t *fn, ir_insn_t *ins,
                                  bool *clobbers) {
    if (insn_defines_dst(ins))
        mark_vreg_clobber(fn, clobbers, ins->dst);

    if (ins->op == IR_MCALL) {
        for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++)
            mark_vreg_clobber(fn, clobbers, ins->ret_vregs[j]);
    }

    if (ins->op == IR_ALU &&
        (strcmp(ins->name, "in") == 0 ||
         strcmp(ins->name, "inb") == 0)) {
        mark_preg_clobber(clobbers,
                          strcmp(ins->name, "inb") == 0 ? PREG_AL
                                                          : PREG_AX);
    }

    if (ins->op == IR_ALU &&
        (strcmp(ins->name, "out") == 0 ||
         strcmp(ins->name, "outb") == 0)) {
        int acc = strcmp(ins->name, "outb") == 0 ? PREG_AL : PREG_AX;
        int val = (ins->src1 >= 0 && ins->src1 < fn->nvregs)
            ? fn->vregs[ins->src1].assigned : PREG_NONE;
        if (val != acc)
            mark_preg_clobber(clobbers, acc);
    }
}

/* Resolved parameter register assignments per function */
typedef struct {
    int param_regs[16];     /* PREG_* for each parameter, or PREG_NONE */
    int return_reg;         /* PREG_* for return value, or PREG_NONE */
    int return_regs[MAX_RETURNS];
    int nreturns;
    bool resolved;
    bool clobbers[NUM_PREGS]; /* true if function clobbers this register */
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
    bool has_emit_seg;
    int  emit_seg;
    /* Entries: raw assembly lines to emit */
    char entries[MAX_DATA_ENTRIES][512];
    int  nentries;
    char module[64];    /* source module this data block belongs to */
} data_block_t;

static data_block_t data_blocks[MAX_DATA_BLOCKS];
static int ndata_blocks = 0;
static int bind_errors = 0;

static data_block_t *find_data_block(const char *label) {
    for (int i = 0; i < ndata_blocks; i++)
        if (strcmp(data_blocks[i].label, label) == 0)
            return &data_blocks[i];
    return NULL;
}

static const char *vreg_data_label(func_t *fn, int v) {
    if (v < 0 || v >= MAX_VREGS || fn->vregs[v].data_label[0] == '\0')
        return NULL;
    return fn->vregs[v].data_label;
}

static void set_vreg_data_label(func_t *fn, int v, const char *label) {
    if (v < 0 || v >= MAX_VREGS || !label || !label[0])
        return;
    strncpy(fn->vregs[v].data_label, label, sizeof(fn->vregs[v].data_label) - 1);
    fn->vregs[v].data_label[sizeof(fn->vregs[v].data_label) - 1] = '\0';
}

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

static void parse_ds_policy_word(const char *word, ds_policy_t *policy,
                                 char *symbol, size_t symbol_sz,
                                 int *literal) {
    if (strncmp(word, "ds=", 3) != 0)
        return;

    const char *value = word + 3;
    if (strcmp(value, "caller") == 0) {
        *policy = DS_POLICY_CALLER;
    } else if (strcmp(value, "none") == 0) {
        *policy = DS_POLICY_NONE;
    } else if (isdigit((unsigned char)value[0])) {
        char *end = NULL;
        long seg = strtol(value, &end, 0);
        if (*end || seg < 0 || seg > 0xFFFF) {
            fprintf(stderr, "bind: invalid ds() segment literal '%s'\n", value);
            bind_errors++;
            seg = 0;
        }
        *policy = DS_POLICY_LITERAL;
        *literal = (int)seg;
    } else {
        *policy = DS_POLICY_SYMBOL;
        strncpy(symbol, value, symbol_sz - 1);
        symbol[symbol_sz - 1] = '\0';
    }
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
        /* reentrant removed */
        else if (strcmp(word, "interrupt") == 0) {
            fn->is_interrupt = true;
        }
        else if (strcmp(word, "bare") == 0) {
            fn->is_bare = true;
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
        else if (strncmp(word, "ds=", 3) == 0) {
            parse_ds_policy_word(word, &fn->ds_policy, fn->ds_symbol,
                                 sizeof(fn->ds_symbol), &fn->ds_literal);
        }
        /* chain removed */
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
            /* Check for "pin=REG" (new) or "in REG" (legacy) */
            char *pin_ptr = strstr(p, "pin=");
            char *in_ptr = strstr(p, " in ");
            if (pin_ptr && pidx < 16) {
                char reg[16];
                read_word(pin_ptr + 4, reg, sizeof(reg));
                fn->param_pins[pidx].preg = parse_preg(reg);
                if (v < MAX_VREGS && fn->param_pins[pidx].preg != PREG_NONE)
                    fn->vregs[v].prefer = fn->param_pins[pidx].preg;
            } else if (in_ptr && pidx < 16) {
                char reg[16];
                read_word(in_ptr + 4, reg, sizeof(reg));
                fn->param_pins[pidx].preg = parse_preg(reg);
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
            int ri = fn->nreturns;
            if (ri >= MAX_RETURNS)
                ri = MAX_RETURNS - 1;
            p = read_word(p, fn->return_types[ri], sizeof(fn->return_types[ri]));
            if (fn->nreturns == 0)
                strncpy(fn->return_type, fn->return_types[ri], sizeof(fn->return_type) - 1);
            fn->has_return = true;
            /* Check for "pin=REG" (new) or "in REG" (legacy) */
            char *pin_ptr = strstr(p, "pin=");
            char *in_ptr = strstr(p, "in ");
            if (pin_ptr) {
                char reg[16];
                read_word(pin_ptr + 4, reg, sizeof(reg));
                fn->ret_pins[ri] = parse_preg(reg);
            } else if (in_ptr) {
                char reg[16];
                read_word(in_ptr + 3, reg, sizeof(reg));
                fn->ret_pins[ri] = parse_preg(reg);
            }
            fn->ret_pin = fn->ret_pins[0];
            if (fn->nreturns < MAX_RETURNS)
                fn->nreturns++;
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

        if (strncmp(p, ".vreg", 5) == 0) {
            p += 5;
            int v = parse_vreg(p, &p);
            if (v >= fn->nvregs) fn->nvregs = v + 1;
            if (v >= 0 && v < MAX_VREGS) {
                /* Parse type and flags: .vreg %N, type [, flag...] */
                skip_comma(&p);
                char tok[16];
                read_word(p, tok, sizeof(tok));
                p += strlen(tok);
                if (strcmp(tok, "u8") == 0)
                    fn->vregs[v].is_byte = true;
                else if (strcmp(tok, "seg") == 0)
                    fn->vregs[v].is_seg = true;
                /* Parse optional flags */
                while (*p) {
                    skip_comma(&p);
                    p = skip_ws(p);
                    if (!*p || *p == '\n' || *p == ';') break;
                    read_word(p, tok, sizeof(tok));
                    p += strlen(tok);
                    if (strcmp(tok, "csref") == 0)
                        fn->vregs[v].is_cs_ref = true;
                    else if (strcmp(tok, "const") == 0)
                        fn->vregs[v].is_const = true;
                    else if (strncmp(tok, "pin=", 4) == 0) {
                        int preg = parse_preg(tok + 4);
                        fn->vregs[v].prefer = preg;
                        fn->vregs[v].fixed = (preg != PREG_NONE);
                        if (preg >= PREG_ES && preg <= PREG_DS)
                            fn->vregs[v].is_seg = true;
                    }
                }
            }
            continue;
        }

        if (strncmp(p, ".local", 6) == 0) {
            p += 6;
            int v = parse_vreg(p, &p);
            skip_comma(&p);
            p = skip_ws(p);
            int sz = (int)strtol(p, (char **)&p, 0);
            if (v >= fn->nvregs) fn->nvregs = v + 1;
            if (v >= 0 && v < MAX_VREGS) {
                sz = (sz + 1) & ~1; /* keep SP word-aligned */
                fn->local_size += sz;
                fn->vregs[v].is_local_slot = true;
                fn->vregs[v].local_size = sz;
                fn->vregs[v].local_offset = fn->local_size;
            }
            continue;
        }

        /* Legacy annotations — kept for backward compatibility */
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

        if (strncmp(p, ".csref", 6) == 0) {
            p += 6;
            int v = parse_vreg(p, &p);
            if (v >= 0 && v < MAX_VREGS)
                fn->vregs[v].is_cs_ref = true;
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
        for (int i = 0; i < MAX_RETURNS; i++) ins->ret_vregs[i] = -1;

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
                char word[64];
                read_word(p, word, sizeof(word));
                if (strcmp(word, "SEG") == 0) {
                    /* SEG label — segment of a label, passed through to asm */
                    p += 3;
                    p = skip_ws(p);
                    snprintf(ins->name, sizeof(ins->name), "SEG ");
                    p = read_word(p, ins->name + 4, sizeof(ins->name) - 4);
                } else {
                    ins->has_imm = false;
                    p = read_word(p, ins->name, sizeof(ins->name));
                    /* Constant pool refs live in CS (code segment) */
                    if (ins->name[0] == '_' && ins->name[1] == 'C' &&
                        ins->dst >= 0 && ins->dst < MAX_VREGS)
                        fn->vregs[ins->dst].is_cs_ref = true;
                }
            } else {
                ins->has_imm = true;
                ins->imm = (int)strtol(p, (char **)&p, 0);
            }
        }
        else if (strcmp(opname, "lea") == 0) {
            ins->op = IR_LEA;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);
        }
        else if (strcmp(opname, "ret") == 0) {
            ins->op = IR_RET;
        }
        else if (strcmp(opname, "retval") == 0) {
            ins->op = IR_RETVAL;
            ins->nrets = 0;
            while (*p) {
                skip_comma(&p);
                p = skip_ws(p);
                if (*p != '%') break;
                int rv = parse_vreg(p, &p);
                if (ins->nrets == 0) ins->src1 = rv;
                else if (ins->nrets - 1 < MAX_RETURNS) ins->ret_vregs[ins->nrets - 1] = rv;
                ins->nrets++;
            }
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
        else if (strcmp(opname, "mcall") == 0) {
            ins->op = IR_MCALL;
            ins->nrets = 0;
            while (*p) {
                skip_comma(&p);
                p = skip_ws(p);
                if (*p != '%') break;
                int rv = parse_vreg(p, &p);
                if (ins->nrets == 0) ins->dst = rv;
                else if (ins->nrets - 1 < MAX_RETURNS) ins->ret_vregs[ins->nrets - 1] = rv;
                ins->nrets++;
            }
            skip_comma(&p);
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
        else if (strcmp(opname, "icall") == 0) {
            ins->op = IR_ICALL;
            ins->dst = parse_vreg(p, &p);
            skip_comma(&p);
            ins->src1 = parse_vreg(p, &p);  /* addr/off vreg */
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%') {
                /* Three-vreg form: icall %dst, %off, %seg, name */
                ins->src2 = parse_vreg(p, &p);
                skip_comma(&p);
            }
            p = read_word(p, ins->name, sizeof(ins->name)); /* extern name */
            /* Mark addr vreg as needing an addressable register
             * (only for pointer-based form, not register pair) */
            if (ins->src2 < 0 && ins->src1 >= 0 && ins->src1 < MAX_VREGS)
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
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%') {
                /* Vreg-based form: loadmem %dst, %off [, %seg] */
                ins->src1 = parse_vreg(p, &p);
                skip_comma(&p);
                p = skip_ws(p);
                if (*p == '%')
                    ins->src2 = parse_vreg(p, &p);
                ins->name[0] = '\0'; /* no address text */
            } else {
                /* Label/address form: loadmem %dst, [addr] */
                strncpy(ins->name, p, 63);
            }
        }
        else if (strcmp(opname, "storemem") == 0) {
            ins->op = IR_STOREMEM;
            p = skip_ws(p);
            if (*p == '%') {
                /* Vreg-based form: storemem %off [, %seg], %val */
                ins->dst = parse_vreg(p, &p);
                skip_comma(&p);
                p = skip_ws(p);
                if (*p == '%') {
                    int v1 = parse_vreg(p, &p);
                    skip_comma(&p);
                    p = skip_ws(p);
                    if (*p == '%') {
                        /* Three vregs: off, seg, val */
                        ins->src2 = v1; /* seg */
                        ins->src1 = parse_vreg(p, &p); /* val */
                    } else {
                        /* Two vregs: off, val */
                        ins->src1 = v1; /* val */
                    }
                }
                ins->name[0] = '\0';
            } else {
                /* Label/address form: storemem [addr], %val */
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
            p = skip_ws(p);
            ins->has_imm = true;
            ins->imm = (int)strtol(p, (char **)&p, 0);
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
            /* Parse optional CX vreg: loop .label, %cx_vreg */
            skip_comma(&p);
            p = skip_ws(p);
            if (*p == '%')
                ins->src1 = parse_vreg(p, &p);
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
                 strcmp(opname, "pushf") == 0 || strcmp(opname, "popf") == 0 ||
                 strcmp(opname, "cli") == 0 || strcmp(opname, "sti") == 0) {
            ins->op = IR_ASM;
            p = skip_ws(p);
            snprintf(ins->asm_body, sizeof(ins->asm_body), "    %s %s", opname, p);
            ins->asm_ann[0] = '\0';
        }
        else if (strcmp(opname, "frame_enter") == 0) {
            ins->op = IR_FRAME_ENTER;
        }
        else if (strcmp(opname, "frame_leave") == 0) {
            ins->op = IR_FRAME_LEAVE;
        }
        else if (
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
        for (int i = 0; i < MAX_RETURNS; i++)
            if (ins->ret_vregs[i] >= fn->nvregs)
                fn->nvregs = ins->ret_vregs[i] + 1;

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
            for (int i = 0; i < MAX_RETURNS; i++)
                fn->ret_pins[i] = PREG_NONE;
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
            for (int i = 0; i < MAX_RETURNS; i++)
                ext->ret_pins[i] = PREG_NONE;
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
                else if (strncmp(word, "addr_seg=", 9) == 0) {
                    ext->has_address = true;
                    ext->addr_seg = (int)strtol(word + 9, NULL, 0);
                }
                else if (strncmp(word, "addr_off=", 9) == 0) {
                    ext->has_address = true;
                    ext->addr_off = (int)strtol(word + 9, NULL, 0);
                }
                else if (strncmp(word, "addr(", 5) == 0) {
                    /* Legacy format: addr(seg:off) */
                    ext->has_address = true;
                    char *colon = strchr(word + 5, ':');
                    if (colon) {
                        ext->addr_seg = (int)strtol(word + 5, NULL, 0);
                        ext->addr_off = (int)strtol(colon + 1, NULL, 0);
                    }
                    p = skip_ws(p); if (*p == ')') p++;
                }
                else if (strncmp(word, "ds=", 3) == 0) {
                    parse_ds_policy_word(word, &ext->ds_policy,
                                         ext->ds_symbol,
                                         sizeof(ext->ds_symbol),
                                         &ext->ds_literal);
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
                    /* Parse "pin=REG" (new) or "in REG" (legacy) */
                    char *pin_ptr = strstr(ep, "pin=");
                    char *in_ptr = strstr(ep, " in ");
                    if (pin_ptr && pi < 16) {
                        char reg[16];
                        read_word(pin_ptr + 4, reg, sizeof(reg));
                        ext->param_pins[pi].preg = parse_preg(reg);
                    } else if (in_ptr && pi < 16) {
                        char reg[16];
                        read_word(in_ptr + 4, reg, sizeof(reg));
                        ext->param_pins[pi].preg = parse_preg(reg);
                    }
                    pi++;
                }
                if (strncmp(ep, ".returns", 8) == 0) {
                    int ri = ext->nreturns;
                    if (ri >= MAX_RETURNS)
                        ri = MAX_RETURNS - 1;
                    ep += 8;
                    ep = skip_ws(ep);
                    ep = read_word(ep, ext->return_types[ri],
                                   sizeof(ext->return_types[ri]));
                    char *pin_ptr = strstr(ep, "pin=");
                    char *in_ptr = strstr(ep, "in ");
                    if (pin_ptr) {
                        char reg[16];
                        read_word(pin_ptr + 4, reg, sizeof(reg));
                        ext->ret_pins[ri] = parse_preg(reg);
                    } else if (in_ptr) {
                        char reg[16];
                        read_word(in_ptr + 3, reg, sizeof(reg));
                        ext->ret_pins[ri] = parse_preg(reg);
                    }
                    if (ext->nreturns < MAX_RETURNS)
                        ext->nreturns++;
                }
                if (strncmp(ep, ".preserves", 10) == 0) {
                    /* Parse comma-separated register list */
                    ep += 10;
                    while (*ep) {
                        ep = skip_ws(ep);
                        if (!*ep || *ep == '\n') break;
                        char reg[16];
                        read_word(ep, reg, sizeof(reg));
                        if (!reg[0]) break;
                        ep += strlen(reg);
                        if (*ep == ',') ep++;
                        if (strcmp(reg, "FLAGS") == 0) continue;
                        int preg = parse_preg(reg);
                        if (preg != PREG_NONE && ext->npreserves < NUM_PREGS)
                            ext->preserves[ext->npreserves++] = preg;
                    }
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
            char *opts = p;
            /* scan for at() anywhere on the rest of the line */
            char *at_ptr = strstr(opts, "at(");
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

/* Derive vreg liveness intervals from CFG dataflow results */
static void compute_liveness(func_t *fn) {
    /* Derive vreg liveness info from CFG dataflow results.
     * The CFG liveness (live_in/live_out per block) handles loops,
     * parameters, and dead defs correctly without special cases. */

    for (int i = 0; i < fn->nvregs; i++) {
        fn->vregs[i].def_pos = -1;
        fn->vregs[i].last_use = -1;
        fn->vregs[i].use_count = 0;
        fn->vregs[i].in_loop = false;
    }

    /* def_pos: first instruction that defines each vreg */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (insn_defines_dst(ins) && ins->dst < fn->nvregs &&
            fn->vregs[ins->dst].def_pos < 0)
            fn->vregs[ins->dst].def_pos = i;
        if (ins->op == IR_MCALL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs && fn->vregs[rv].def_pos < 0)
                    fn->vregs[rv].def_pos = i;
            }
        }
    }

    /* Parameters: def before first instruction */
    for (int i = 0; i < fn->nparams; i++) {
        int v = fn->param_vregs[i];
        if (v >= 0 && v < fn->nvregs)
            fn->vregs[v].def_pos = -1;
    }

    /* last_use: derived from CFG liveness.
     * A vreg's last use is the latest instruction where it's still
     * live. Walk blocks and find the latest point. */
    for (int b = 0; b < fn->nblocks; b++) {
        bblock_t *bb = &fn->blocks[b];
        for (int v = 0; v < fn->nvregs; v++) {
            if (vset_test(bb->live_out, v)) {
                /* Live at block exit — last_use is at least end-1 */
                if (bb->end - 1 > fn->vregs[v].last_use)
                    fn->vregs[v].last_use = bb->end - 1;
            }
            if (vset_test(bb->live_in, v)) {
                /* Live at block entry — last_use is at least start */
                if (bb->start > fn->vregs[v].last_use)
                    fn->vregs[v].last_use = bb->start;
            }
        }
        /* Also check actual use points within the block */
        for (int i = bb->start; i < bb->end; i++) {
            ir_insn_t *ins = &fn->insns[i];
            if (ins->src1 >= 0 && ins->src1 < fn->nvregs &&
                ins->op != IR_LEA &&
                i > fn->vregs[ins->src1].last_use)
                fn->vregs[ins->src1].last_use = i;
            if (ins->src2 >= 0 && ins->src2 < fn->nvregs &&
                i > fn->vregs[ins->src2].last_use)
                fn->vregs[ins->src2].last_use = i;
            for (int j = 0; j < 8; j++)
                if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs &&
                    i > fn->vregs[ins->extra_args[j]].last_use)
                    fn->vregs[ins->extra_args[j]].last_use = i;
            if (ins->op == IR_RETVAL) {
                for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                    int rv = ins->ret_vregs[j];
                    if (rv >= 0 && rv < fn->nvregs &&
                        i > fn->vregs[rv].last_use)
                        fn->vregs[rv].last_use = i;
                }
            }
            if ((ins->op == IR_STORE || ins->op == IR_STOREMEM ||
                 ins->op == IR_STOREFIELD) &&
                ins->dst >= 0 && ins->dst < fn->nvregs &&
                i > fn->vregs[ins->dst].last_use)
                fn->vregs[ins->dst].last_use = i;
        }
    }

    /* Dead defs: defined but never live anywhere — minimal range */
    for (int v = 0; v < fn->nvregs; v++) {
        if (fn->vregs[v].def_pos >= 0 && fn->vregs[v].last_use < 0)
            fn->vregs[v].last_use = fn->vregs[v].def_pos;
    }

    /* use_count: count all references (reads + re-definitions) */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->src1 >= 0 && ins->src1 < fn->nvregs &&
            ins->op != IR_LEA)
            fn->vregs[ins->src1].use_count++;
        if (ins->src2 >= 0 && ins->src2 < fn->nvregs)
            fn->vregs[ins->src2].use_count++;
        for (int j = 0; j < 8; j++)
            if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs)
                fn->vregs[ins->extra_args[j]].use_count++;
        if (ins->op == IR_RETVAL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs)
                    fn->vregs[rv].use_count++;
            }
        }
        if (ins->op == IR_MCALL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs &&
                    fn->vregs[rv].def_pos >= 0 &&
                    fn->vregs[rv].def_pos < i)
                    fn->vregs[rv].use_count++;
            }
        }
        if (ins->dst >= 0 && ins->dst < fn->nvregs &&
            fn->vregs[ins->dst].def_pos >= 0 &&
            fn->vregs[ins->dst].def_pos < i)
            fn->vregs[ins->dst].use_count++;
    }

    /* in_loop: true if the vreg is live in any block that has a
     * predecessor with a higher block index (back-edge = loop) */
    for (int b = 0; b < fn->nblocks; b++) {
        bool block_in_loop = false;
        for (int p = 0; p < fn->blocks[b].npreds; p++)
            if (fn->blocks[b].preds[p] >= b) { block_in_loop = true; break; }
        if (!block_in_loop) continue;
        for (int v = 0; v < fn->nvregs; v++) {
            if (vset_test(fn->blocks[b].live_in, v) ||
                vset_test(fn->blocks[b].live_out, v))
                fn->vregs[v].in_loop = true;
        }
    }
}

/* ================================================================
 * CFG construction — split IR into basic blocks
 * ================================================================ */

static void build_cfg(func_t *fn) {
    fn->nblocks = 0;
    if (fn->ninsns == 0) return;

    /* Pass 1: identify block boundaries.
     * A new block starts at:
     *   - instruction 0
     *   - any label target
     *   - the instruction after any jump/branch/ret */
    bool is_leader[MAX_INSNS];
    memset(is_leader, 0, sizeof(is_leader));
    is_leader[0] = true;

    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->op == IR_LABEL) {
            is_leader[i] = true;
        }
        if (ins->op == IR_JMP || ins->op == IR_JZ || ins->op == IR_CJMP ||
            ins->op == IR_RET || ins->op == IR_TAILCALL ||
            ins->op == IR_GOTO_FN || ins->op == IR_LOOP) {
            /* Instruction after a branch starts a new block */
            if (i + 1 < fn->ninsns)
                is_leader[i + 1] = true;
        }
    }

    /* Pass 2: create blocks from leaders */
    for (int i = 0; i < fn->ninsns; i++) {
        if (!is_leader[i]) continue;
        if (fn->nblocks >= MAX_BLOCKS) break;
        bblock_t *bb = &fn->blocks[fn->nblocks++];
        memset(bb, 0, sizeof(*bb));
        bb->start = i;
        /* Block extends until the next leader */
        int end = i + 1;
        while (end < fn->ninsns && !is_leader[end]) end++;
        bb->end = end;
    }

    /* Pass 3: connect edges (successors and predecessors) */
    for (int b = 0; b < fn->nblocks; b++) {
        bblock_t *bb = &fn->blocks[b];
        int last = bb->end - 1;
        ir_insn_t *term = &fn->insns[last];

        /* Check if the block ends with a terminator */
        bool is_term = (term->op == IR_JMP || term->op == IR_RET ||
                        term->op == IR_TAILCALL || term->op == IR_GOTO_FN);

        /* Conditional branch: fall-through + jump target */
        if (term->op == IR_JZ || term->op == IR_CJMP || term->op == IR_LOOP) {
            /* Fall-through to next block */
            if (b + 1 < fn->nblocks && bb->nsuccs < 4)
                bb->succs[bb->nsuccs++] = b + 1;
            /* Jump target — find the block containing the label */
            const char *target = term->name;
            for (int t = 0; t < fn->nblocks; t++) {
                if (fn->insns[fn->blocks[t].start].op == IR_LABEL &&
                    strcmp(fn->insns[fn->blocks[t].start].name, target) == 0) {
                    if (bb->nsuccs < 4)
                        bb->succs[bb->nsuccs++] = t;
                    break;
                }
            }
        }
        /* Unconditional jump: just the target */
        else if (term->op == IR_JMP) {
            const char *target = term->name;
            for (int t = 0; t < fn->nblocks; t++) {
                if (fn->insns[fn->blocks[t].start].op == IR_LABEL &&
                    strcmp(fn->insns[fn->blocks[t].start].name, target) == 0) {
                    if (bb->nsuccs < 4)
                        bb->succs[bb->nsuccs++] = t;
                    break;
                }
            }
        }
        /* Non-terminator: fall through to next block */
        else if (!is_term && b + 1 < fn->nblocks) {
            bb->succs[bb->nsuccs++] = b + 1;
        }
        /* RET/TAILCALL/GOTO_FN: no successors */
    }

    /* Build predecessor lists from successors */
    for (int b = 0; b < fn->nblocks; b++) {
        bblock_t *bb = &fn->blocks[b];
        for (int s = 0; s < bb->nsuccs; s++) {
            bblock_t *succ = &fn->blocks[bb->succs[s]];
            if (succ->npreds < 16)
                succ->preds[succ->npreds++] = b;
        }
    }
}

/* ================================================================
 * Dataflow liveness on CFG
 * ================================================================ */

/* Collect def and use sets for a basic block */
static void compute_block_def_use(func_t *fn, bblock_t *bb) {
    vset_zero(bb->defs);
    vset_zero(bb->uses);

    for (int i = bb->start; i < bb->end; i++) {
        ir_insn_t *ins = &fn->insns[i];

        /* Uses: vregs read by this instruction.
         * Only counts as a use if not already defined in this block. */
        int use_vregs[32];
        int nuses = 0;
        if (ins->src1 >= 0 && ins->op != IR_LEA) use_vregs[nuses++] = ins->src1;
        if (ins->src2 >= 0) use_vregs[nuses++] = ins->src2;
        for (int j = 0; j < 8; j++)
            if (ins->extra_args[j] >= 0) use_vregs[nuses++] = ins->extra_args[j];
        if (ins->op == IR_RETVAL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++)
                if (ins->ret_vregs[j] >= 0) use_vregs[nuses++] = ins->ret_vregs[j];
        }
        if (insn_reads_dst(ins))
            use_vregs[nuses++] = ins->dst;

        for (int u = 0; u < nuses; u++) {
            int v = use_vregs[u];
            if (v >= 0 && v < fn->nvregs && !vset_test(bb->defs, v))
                vset_set(bb->uses, v);
        }

        /* Defs: vreg written by this instruction */
        if (insn_defines_dst(ins) && ins->dst < fn->nvregs)
            vset_set(bb->defs, ins->dst);
        if (ins->op == IR_MCALL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs)
                    vset_set(bb->defs, rv);
            }
        }
    }
}

/* Run backward dataflow liveness to fixpoint */
static void compute_cfg_liveness(func_t *fn) {
    if (fn->nblocks == 0) return;

    /* Compute local def/use sets */
    for (int b = 0; b < fn->nblocks; b++)
        compute_block_def_use(fn, &fn->blocks[b]);

    /* Initialize live_in and live_out to empty */
    for (int b = 0; b < fn->nblocks; b++) {
        vset_zero(fn->blocks[b].live_in);
        vset_zero(fn->blocks[b].live_out);
    }

    /* Parameters are live-in at entry block */
    for (int p = 0; p < fn->nparams; p++) {
        int v = fn->param_vregs[p];
        if (v >= 0 && v < fn->nvregs)
            vset_set(fn->blocks[0].live_in, v);
    }

    /* Iterate until stable */
    bool changed = true;
    int max_iter = fn->nblocks * 4 + 10; /* safety bound */
    while (changed && max_iter-- > 0) {
        changed = false;
        /* Process blocks in reverse order (approximate reverse postorder) */
        for (int b = fn->nblocks - 1; b >= 0; b--) {
            bblock_t *bb = &fn->blocks[b];
            uint64_t old_in[VREG_WORDS], old_out[VREG_WORDS];
            memcpy(old_in, bb->live_in, sizeof(old_in));
            memcpy(old_out, bb->live_out, sizeof(old_out));

            /* live_out = union of live_in of all successors */
            vset_zero(bb->live_out);
            for (int s = 0; s < bb->nsuccs; s++)
                vset_or(bb->live_out, fn->blocks[bb->succs[s]].live_in);

            /* live_in = uses | (live_out - defs) */
            uint64_t diff[VREG_WORDS];
            vset_diff(diff, bb->live_out, bb->defs);
            memcpy(bb->live_in, bb->uses, VREG_WORDS * sizeof(uint64_t));
            vset_or(bb->live_in, diff);

            /* Preserve parameter liveness at entry */
            if (b == 0) {
                for (int p = 0; p < fn->nparams; p++) {
                    int v = fn->param_vregs[p];
                    if (v >= 0 && v < fn->nvregs)
                        vset_set(bb->live_in, v);
                }
            }

            if (!vset_equal(bb->live_in, old_in) ||
                !vset_equal(bb->live_out, old_out))
                changed = true;
        }
    }
}

/* ================================================================
 * Interference graph construction
 * ================================================================ */

static void add_interference(func_t *fn, int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= fn->nvregs || b >= fn->nvregs)
        return;
    if (!vset_test(fn->igraph[a], b)) {
        vset_set(fn->igraph[a], b);
        vset_set(fn->igraph[b], a);
        fn->degree[a]++;
        fn->degree[b]++;
    }
}

static void build_igraph(func_t *fn) {
    /* Clear */
    for (int v = 0; v < fn->nvregs; v++) {
        vset_zero(fn->igraph[v]);
        fn->degree[v] = 0;
    }

    /* Walk each block backward, maintaining a live set.
     * At each def point, add edges between the defined vreg
     * and everything else currently live. */
    for (int b = 0; b < fn->nblocks; b++) {
        bblock_t *bb = &fn->blocks[b];
        uint64_t live[VREG_WORDS];
        memcpy(live, bb->live_out, sizeof(live));

        for (int i = bb->end - 1; i >= (int)bb->start; i--) {
            ir_insn_t *ins = &fn->insns[i];

            /* For three-operand ALU instructions (add %d, %s1, %s2),
             * the lowering to two-operand x86 does: mov dst, src1; op dst, src2.
             * This means src2 must survive past the mov, so it must interfere
             * with dst. Add uses to live BEFORE the def for non-MOV instructions
             * to create this interference. MOV keeps the coalescing exception
             * (uses added after def). */
            bool is_copy = (ins->op == IR_MOV && !ins->has_imm);
            if (!is_copy) {
                if (ins->src1 >= 0 && ins->src1 < fn->nvregs &&
                    ins->op != IR_LEA)
                    vset_set(live, ins->src1);
                if (ins->src2 >= 0 && ins->src2 < fn->nvregs)
                    vset_set(live, ins->src2);
                for (int j = 0; j < 8; j++)
                    if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs)
                        vset_set(live, ins->extra_args[j]);
                if (ins->op == IR_RETVAL) {
                    for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++)
                        if (ins->ret_vregs[j] >= 0 && ins->ret_vregs[j] < fn->nvregs)
                            vset_set(live, ins->ret_vregs[j]);
                }
                if (insn_reads_dst(ins) && ins->dst < fn->nvregs)
                    vset_set(live, ins->dst);
            }

            /* Def: the defined vreg interferes with everything
             * currently live (except itself — handled by add_interference).
             * For MOV, the src does NOT interfere with dst (they can
             * share a register to eliminate the mov). */
            int def = -1;
            if (insn_defines_dst(ins))
                def = ins->dst;

            if (def >= 0 && def < fn->nvregs) {
                for (int w = 0; w < VREG_WORDS; w++) {
                    uint64_t bits = live[w];
                    while (bits) {
                        int bit = __builtin_ctzll(bits);
                        int v = w * 64 + bit;
                        add_interference(fn, def, v);
                        bits &= bits - 1;
                    }
                }
                /* Def kills the vreg from the live set */
                vset_clear(live, def);
            }
            if (ins->op == IR_MCALL) {
                for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                    int rdef = ins->ret_vregs[j];
                    if (rdef < 0 || rdef >= fn->nvregs) continue;
                    for (int w = 0; w < VREG_WORDS; w++) {
                        uint64_t bits = live[w];
                        while (bits) {
                            int bit = __builtin_ctzll(bits);
                            int v = w * 64 + bit;
                            add_interference(fn, rdef, v);
                            bits &= bits - 1;
                        }
                    }
                    vset_clear(live, rdef);
                }
            }

            /* For MOV: add uses after def (coalescing exception) */
            if (is_copy) {
                if (ins->src1 >= 0 && ins->src1 < fn->nvregs)
                    vset_set(live, ins->src1);
            }
        }

        /* After walking to block top, add interference between all
         * remaining live vregs. This catches parameters and other
         * vregs that are live_in but not defined in this block. */
        for (int w1 = 0; w1 < VREG_WORDS; w1++) {
            uint64_t bits1 = live[w1];
            while (bits1) {
                int b1 = w1 * 64 + __builtin_ctzll(bits1);
                /* Add edges with all other live vregs */
                for (int w2 = w1; w2 < VREG_WORDS; w2++) {
                    uint64_t bits2 = (w2 == w1) ?
                        (live[w2] & ~(1ULL << (b1 & 63))) : live[w2];
                    /* Only process pairs where b2 > b1 to avoid double-counting */
                    if (w2 == w1)
                        bits2 &= ~((1ULL << (b1 & 63)) | ((1ULL << (b1 & 63)) - 1));
                    while (bits2) {
                        int b2 = w2 * 64 + __builtin_ctzll(bits2);
                        add_interference(fn, b1, b2);
                        bits2 &= bits2 - 1;
                    }
                }
                bits1 &= bits1 - 1;
            }
        }
    }
}

static void annotate_data_refs(void) {
    for (int fi = 0; fi < nfunctions; fi++) {
        func_t *fn = &functions[fi];
        bool changed;

        for (int i = 0; i < fn->ninsns; i++) {
            ir_insn_t *ins = &fn->insns[i];
            if (ins->op == IR_MOV && ins->name[0] &&
                strncmp(ins->name, "SEG ", 4) != 0 &&
                find_data_block(ins->name)) {
                set_vreg_data_label(fn, ins->dst, ins->name);
            }
        }

        do {
            changed = false;
            for (int i = 0; i < fn->ninsns; i++) {
                ir_insn_t *ins = &fn->insns[i];
                const char *label = NULL;
                if (ins->op == IR_MOV && !ins->has_imm && ins->src1 >= 0) {
                    label = vreg_data_label(fn, ins->src1);
                } else if (ins->op == IR_ALU && strcmp(ins->name, "add") == 0) {
                    label = vreg_data_label(fn, ins->src1);
                    if (!label)
                        label = vreg_data_label(fn, ins->src2);
                }
                if (label && ins->dst >= 0 &&
                    strcmp(fn->vregs[ins->dst].data_label, label) != 0) {
                    set_vreg_data_label(fn, ins->dst, label);
                    changed = true;
                }
            }
        } while (changed);
    }
}

static bool symbol_is_data_object(const char *name) {
    if (find_data_block(name))
        return true;
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].name, name) == 0)
            return true;
    return false;
}

static bool mem_text_has_explicit_segment(const char *name) {
    return strstr(name, "CS:") || strstr(name, "ES:") || strstr(name, "SS:");
}

static bool vreg_is_non_ds_addr(func_t *fn, int v, bool *local_addr) {
    if (v < 0 || v >= MAX_VREGS)
        return false;
    if (fn->vregs[v].is_local_slot || local_addr[v])
        return true;
    if (fn->vregs[v].is_cs_ref || vreg_data_label(fn, v))
        return true;
    return false;
}

static void validate_ds_none(func_t *fn) {
    bool local_addr[MAX_VREGS] = {0};
    bool changed;

    do {
        changed = false;
        for (int i = 0; i < fn->ninsns; i++) {
            ir_insn_t *ins = &fn->insns[i];
            bool mark = false;
            if (ins->op == IR_LEA) {
                mark = true;
            } else if (ins->op == IR_MOV && ins->src1 >= 0) {
                mark = local_addr[ins->src1];
            } else if (ins->op == IR_ALU &&
                       strcmp(ins->name, "add") == 0) {
                mark = (ins->src1 >= 0 && local_addr[ins->src1]) ||
                       (ins->src2 >= 0 && local_addr[ins->src2]);
            }
            if (mark && ins->dst >= 0 && ins->dst < MAX_VREGS &&
                !local_addr[ins->dst]) {
                local_addr[ins->dst] = true;
                changed = true;
            }
        }
    } while (changed);

    for (int v = 0; v < fn->nvregs; v++) {
        if (fn->vregs[v].prefer == PREG_DS) {
            fprintf(stderr, "%s: ds(none) body uses DS-pinned vreg %%%d\n",
                    fn->name, v);
            bind_errors++;
        }
    }

    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];

        if (ins->op == IR_LOAD || ins->op == IR_STORE) {
            int base = ins->src1;
            if (!vreg_is_non_ds_addr(fn, base, local_addr)) {
                fprintf(stderr, "%s: ds(none) body uses DS-default memory\n",
                        fn->name);
                bind_errors++;
                return;
            }
        } else if (ins->op == IR_LOADMEM) {
            if (ins->name[0]) {
                if (!mem_text_has_explicit_segment(ins->name)) {
                    fprintf(stderr,
                            "%s: ds(none) body uses DS-default memory\n",
                            fn->name);
                    bind_errors++;
                    return;
                }
            } else if (ins->src2 < 0 &&
                       !vreg_is_non_ds_addr(fn, ins->src1, local_addr)) {
                fprintf(stderr, "%s: ds(none) body uses DS-default memory\n",
                        fn->name);
                bind_errors++;
                return;
            }
        } else if (ins->op == IR_STOREMEM) {
            if (ins->name[0]) {
                if (!mem_text_has_explicit_segment(ins->name)) {
                    fprintf(stderr,
                            "%s: ds(none) body uses DS-default memory\n",
                            fn->name);
                    bind_errors++;
                    return;
                }
            } else if (ins->src2 < 0 &&
                       !vreg_is_non_ds_addr(fn, ins->dst, local_addr)) {
                fprintf(stderr, "%s: ds(none) body uses DS-default memory\n",
                        fn->name);
                bind_errors++;
                return;
            }
        } else if (ins->op == IR_ALU &&
                   (strcmp(ins->name, "lods") == 0 ||
                    strcmp(ins->name, "xlat") == 0)) {
            fprintf(stderr, "%s: ds(none) body uses DS-default memory\n",
                    fn->name);
            bind_errors++;
            return;
        } else if (ins->op == IR_ASM && ins->asm_body[0]) {
            fprintf(stderr, "%s: ds(none) body contains opaque asm\n",
                    fn->name);
            bind_errors++;
            return;
        }
    }
}

static void validate_ds_policies(void) {
    for (int fi = 0; fi < nfunctions; fi++) {
        func_t *fn = &functions[fi];
        if (fn->is_pub && fn->is_far &&
            fn->ds_policy == DS_POLICY_UNSPEC) {
            fprintf(stderr, "%s: public far functions must declare ds(...)\n",
                    fn->name);
            bind_errors++;
        }
        if (fn->ds_policy == DS_POLICY_SYMBOL &&
            !symbol_is_data_object(fn->ds_symbol)) {
            fprintf(stderr, "%s: ds(%s) must name a global or data object\n",
                    fn->name, fn->ds_symbol);
            bind_errors++;
        }
        if (fn->ds_policy == DS_POLICY_NONE)
            validate_ds_none(fn);
    }

    for (int e = 0; e < nexterns; e++) {
        extern_fn_t *ext = &externs[e];
        if (ext->ds_policy == DS_POLICY_SYMBOL &&
            !symbol_is_data_object(ext->ds_symbol)) {
            fprintf(stderr, "%s: ds(%s) must name a global or data object\n",
                    ext->name, ext->ds_symbol);
            bind_errors++;
        }
    }
}

/* Check if two vregs interfere (graph lookup) */
static bool vregs_interfere(func_t *fn, int a, int b) {
    if (a < 0 || b < 0 || a >= fn->nvregs || b >= fn->nvregs)
        return false;
    return vset_test(fn->igraph[a], b);
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
            } else if (strcmp(op, "bext") == 0) {
                if (ins->dst >= 0 && ins->dst < MAX_VREGS)
                    fn->vregs[ins->dst].prefer = PREG_AX;
                if (ins->src1 >= 0 && ins->src1 < MAX_VREGS) {
                    fn->vregs[ins->src1].needs_index = true;
                    fn->vregs[ins->src1].prefer = PREG_SI;
                }
                if (ins->src2 >= 0 && ins->src2 < MAX_VREGS) {
                    fn->vregs[ins->src2].is_byte = true;
                    fn->vregs[ins->src2].prefer = PREG_CL;
                }
                if (ins->extra_args[0] >= 0 && ins->extra_args[0] < MAX_VREGS) {
                    fn->vregs[ins->extra_args[0]].is_byte = true;
                    fn->vregs[ins->extra_args[0]].prefer = PREG_DL;
                }
            } else if (strcmp(op, "bins") == 0) {
                if (ins->dst >= 0 && ins->dst < MAX_VREGS) {
                    fn->vregs[ins->dst].needs_index = true;
                    fn->vregs[ins->dst].prefer = PREG_DI;
                }
                if (ins->src1 >= 0 && ins->src1 < MAX_VREGS) {
                    fn->vregs[ins->src1].is_byte = true;
                    fn->vregs[ins->src1].prefer = PREG_CL;
                }
                if (ins->src2 >= 0 && ins->src2 < MAX_VREGS) {
                    fn->vregs[ins->src2].is_byte = true;
                    fn->vregs[ins->src2].prefer = PREG_DL;
                }
                if (ins->extra_args[0] >= 0 && ins->extra_args[0] < MAX_VREGS)
                    fn->vregs[ins->extra_args[0]].prefer = PREG_AX;
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
        /* Vreg-based loadmem/storemem: offset vreg needs addressable */
        if ((ins->op == IR_LOADMEM || ins->op == IR_STOREMEM) &&
            ins->name[0] == '\0') {
            /* For loadmem: src1=off, src2=seg; for storemem: dst=off, src2=seg */
            int off_vreg = (ins->op == IR_LOADMEM) ? ins->src1 : ins->dst;
            bool has_seg = (ins->src2 >= 0);
            if (off_vreg >= 0 && off_vreg < MAX_VREGS) {
                fn->vregs[off_vreg].needs_addressable = true;
                if (!has_seg)
                    fn->vregs[off_vreg].needs_ds_addr = true;
            }
        }
    }
    /* Propagate is_cs_ref through mov chains */
    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];
        if (ins->op == IR_MOV && !ins->has_imm &&
            ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
            ins->dst >= 0 && ins->dst < MAX_VREGS &&
            fn->vregs[ins->src1].is_cs_ref)
            fn->vregs[ins->dst].is_cs_ref = true;
    }
}

/* Get the register pool for a vreg based on its constraints */
static int get_vreg_pool(func_t *fn, int v, bool bp_available, int *pool) {
    vreg_info_t *vr = &fn->vregs[v];
    int n = 0;
    if (vr->is_seg) {
        pool[n++] = PREG_ES;
    } else if (vr->needs_cl) {
        pool[n++] = PREG_CL;
    } else if (vr->is_byte) {
        pool[n++] = PREG_AL; pool[n++] = PREG_AH;
        pool[n++] = PREG_BL; pool[n++] = PREG_BH;
        pool[n++] = PREG_CL; pool[n++] = PREG_CH;
        pool[n++] = PREG_DL; pool[n++] = PREG_DH;
    } else if (vr->needs_base) {
        pool[n++] = PREG_BX;
        if (bp_available) pool[n++] = PREG_BP;
    } else if (vr->needs_index) {
        pool[n++] = PREG_SI; pool[n++] = PREG_DI;
    } else if (vr->needs_ds_addr) {
        pool[n++] = PREG_BX;
        pool[n++] = PREG_SI; pool[n++] = PREG_DI;
    } else if (vr->needs_addressable) {
        pool[n++] = PREG_BX;
        if (bp_available) pool[n++] = PREG_BP;
        pool[n++] = PREG_SI; pool[n++] = PREG_DI;
    } else {
        pool[n++] = PREG_AX; pool[n++] = PREG_BX;
        pool[n++] = PREG_CX; pool[n++] = PREG_DX;
        pool[n++] = PREG_SI; pool[n++] = PREG_DI;
        if (bp_available) pool[n++] = PREG_BP;
    }
    return n;
}

/* Chaitin-Briggs register allocation using the interference graph.
 *
 * 1. Pre-color: assign vregs with preferences (params, propagated)
 * 2. Simplify: push vregs with degree < pool_size onto stack
 * 3. Potential spill: when stuck, push lowest spill_cost vreg
 * 4. Select: pop stack, assign colors respecting interference
 */
static void allocate_registers(func_t *fn, bool bp_available) {
    int nv = fn->nvregs;

    /* Compute effective degree per vreg (only counting active neighbors
     * with the same register class — byte vregs don't compete with
     * word vregs for the same pool slots, but they DO alias) */

    /* Phase 1: Pre-color — assign fixed and preferred registers.
     * Fixed vregs come from explicit register expressions and denote the
     * hardware register itself, not an allocator preference.
     *
     * A byte preference (AH, DL, etc.) on a word vreg is valid —
     * the vreg adapts to the preference's register class. But
     * addressing constraints must still be respected. */
    for (int i = 0; i < nv; i++) {
        if (fn->vregs[i].fixed && fn->vregs[i].prefer != PREG_NONE)
            fn->vregs[i].assigned = fn->vregs[i].prefer;
    }
    for (int i = 0; i < nv; i++) {
        if (fn->vregs[i].is_local_slot) continue;
        if (fn->vregs[i].fixed) continue;
        if (fn->vregs[i].prefer == PREG_NONE) continue;
        int preg = fn->vregs[i].prefer;
        if (!bp_available && preg == PREG_BP)
            continue;
        /* Segment preference must match segment vreg */
        if (fn->vregs[i].is_seg && !(preg >= PREG_ES && preg <= PREG_DS))
            continue;
        if (!fn->vregs[i].is_seg && preg >= PREG_ES && preg <= PREG_DS)
            continue;
        /* Addressing constraints override preferences */
        if (fn->vregs[i].needs_base && preg != PREG_BX && preg != PREG_BP)
            continue;
        if (fn->vregs[i].needs_index && preg != PREG_SI && preg != PREG_DI)
            continue;
        if (fn->vregs[i].needs_addressable &&
            preg != PREG_BX && preg != PREG_BP &&
            preg != PREG_SI && preg != PREG_DI)
            continue;
        if (fn->vregs[i].needs_cl && preg != PREG_CL)
            continue;
        /* Check for conflicts with already-assigned vregs */
        bool conflict = false;
        for (int j = 0; j < nv; j++) {
            if (j == i || fn->vregs[j].assigned == PREG_NONE) continue;
            if (!vregs_interfere(fn, i, j)) continue;
            if (fn->vregs[j].assigned == preg || pregs_alias(fn->vregs[j].assigned, preg))
                { conflict = true; break; }
        }
        if (!conflict) fn->vregs[i].assigned = preg;
    }

    /* Phase 2: Simplify/Select (Chaitin-Briggs)
     *
     * Build a work stack by repeatedly removing nodes that can be
     * trivially colored (degree < pool_size). When stuck, push the
     * node with lowest spill cost as a potential spill. Then pop
     * and assign colors. */

    int stack[MAX_VREGS];
    bool potential_spill[MAX_VREGS];
    bool removed[MAX_VREGS];
    int sp = 0;

    memset(potential_spill, 0, sizeof(potential_spill));
    memset(removed, 0, sizeof(removed));

    /* Mark already-handled vregs */
    for (int i = 0; i < nv; i++) {
        if (fn->vregs[i].is_local_slot) removed[i] = true;
        if (fn->vregs[i].assigned != PREG_NONE) removed[i] = true;
        if (fn->vregs[i].def_pos < 0 && fn->vregs[i].last_use < 0)
            removed[i] = true;
    }

    /* Simplify loop */
    int remaining = 0;
    for (int i = 0; i < nv; i++) if (!removed[i]) remaining++;

    while (remaining > 0) {
        bool progress = false;

        /* Find a node with effective degree < pool_size */
        for (int i = 0; i < nv; i++) {
            if (removed[i]) continue;
            int pool[16]; int psz = get_vreg_pool(fn, i, bp_available, pool);

            /* Count active (non-removed) interfering neighbors */
            int active_deg = 0;
            for (int w = 0; w < VREG_WORDS; w++) {
                uint64_t bits = fn->igraph[i][w];
                while (bits) {
                    int nb = w * 64 + __builtin_ctzll(bits);
                    if (nb < nv && !removed[nb]) active_deg++;
                    bits &= bits - 1;
                }
            }

            if (active_deg < psz) {
                stack[sp++] = i;
                removed[i] = true;
                remaining--;
                progress = true;
            }
        }

        if (!progress) {
            /* Stuck — pick lowest spill cost node as potential spill */
            int best = -1;
            int best_cost = INT_MAX;
            for (int i = 0; i < nv; i++) {
                if (removed[i]) continue;
                int cost = fn->vregs[i].use_count;
                if (fn->vregs[i].in_loop) cost *= 10;
                if (fn->vregs[i].is_const) cost /= 2;
                if (cost < best_cost) { best_cost = cost; best = i; }
            }
            if (best >= 0) {
                stack[sp++] = best;
                potential_spill[best] = true;
                removed[best] = true;
                remaining--;
            }
        }
    }

    /* Phase 3: Select — pop stack and assign colors */
    while (sp > 0) {
        int v = stack[--sp];
        int pool[16]; int psz = get_vreg_pool(fn, v, bp_available, pool);

        /* Try to assign a color that doesn't conflict */
        bool colored = false;
        /* Prefer the preference register if available */
        if (fn->vregs[v].prefer != PREG_NONE) {
            int preg = fn->vregs[v].prefer;
            if (!bp_available && preg == PREG_BP)
                goto skip_preferred_color;
            bool ok = true;
            for (int w = 0; w < VREG_WORDS && ok; w++) {
                uint64_t bits = fn->igraph[v][w];
                while (bits) {
                    int nb = w * 64 + __builtin_ctzll(bits);
                    if (nb < nv && fn->vregs[nb].assigned != PREG_NONE &&
                        (fn->vregs[nb].assigned == preg ||
                         pregs_alias(fn->vregs[nb].assigned, preg)))
                        ok = false;
                    bits &= bits - 1;
                }
            }
            if (ok) {
                /* Verify it's in the pool */
                for (int r = 0; r < psz; r++) {
                    if (pool[r] == preg) {
                        fn->vregs[v].assigned = preg;
                        colored = true;
                        break;
                    }
                }
            }
        }
skip_preferred_color:

        if (!colored) {
            for (int r = 0; r < psz; r++) {
                int preg = pool[r];
                bool conflict = false;
                for (int w = 0; w < VREG_WORDS && !conflict; w++) {
                    uint64_t bits = fn->igraph[v][w];
                    while (bits) {
                        int nb = w * 64 + __builtin_ctzll(bits);
                        if (nb < nv && fn->vregs[nb].assigned != PREG_NONE &&
                            (fn->vregs[nb].assigned == preg ||
                             pregs_alias(fn->vregs[nb].assigned, preg)))
                            { conflict = true; break; }
                        bits &= bits - 1;
                    }
                }
                if (!conflict) {
                    fn->vregs[v].assigned = preg;
                    colored = true;
                    break;
                }
            }
        }

        if (!colored) {
            /* Actual spill */
            fn->vregs[v].spill_slot = fn->nspill_slots++;
        }
    }
}

/* ================================================================
 * Post-allocation move insertion
 * ================================================================ */

/* Compute which physical registers are free at instruction position `pos`
 * within block `b`. Returns a bitmask of free PREG_* values. */
static uint32_t free_regs_at(func_t *fn, int b_idx, int pos) {
    bblock_t *bb = &fn->blocks[b_idx];
    uint64_t live[VREG_WORDS];
    memcpy(live, bb->live_out, sizeof(live));

    /* Walk backward from block end to pos, maintaining live set */
    for (int i = bb->end - 1; i >= pos; i--) {
        ir_insn_t *ins = &fn->insns[i];

        /* Remove defs (vreg dies here going backward) */
        if (insn_defines_dst(ins) && ins->dst < fn->nvregs)
            vset_clear(live, ins->dst);
        if (ins->op == IR_MCALL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs)
                    vset_clear(live, rv);
            }
        }

        /* Add uses (vreg becomes live going backward) */
        if (ins->src1 >= 0 && ins->src1 < fn->nvregs)
            vset_set(live, ins->src1);
        if (ins->src2 >= 0 && ins->src2 < fn->nvregs)
            vset_set(live, ins->src2);
        for (int j = 0; j < 8; j++)
            if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs)
                vset_set(live, ins->extra_args[j]);
        if (ins->op == IR_RETVAL) {
            for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
                int rv = ins->ret_vregs[j];
                if (rv >= 0 && rv < fn->nvregs)
                    vset_set(live, rv);
            }
        }
        if (insn_reads_dst(ins) && ins->dst < fn->nvregs)
            vset_set(live, ins->dst);
    }

    /* Build set of occupied physical registers */
    uint32_t occupied = 0;
    for (int v = 0; v < fn->nvregs; v++) {
        if (!vset_test(live, v)) continue;
        int preg = fn->vregs[v].assigned;
        if (preg == PREG_NONE) continue;
        occupied |= (1u << preg);
        /* Also mark aliases as occupied */
        if (preg < 4) { /* word reg — mark byte halves */
            occupied |= (1u << preg_alias_lo[preg]);
            occupied |= (1u << preg_alias_hi[preg]);
        } else if (preg >= PREG_AL && preg <= PREG_BH) {
            occupied |= (1u << preg_alias_parent[preg]);
            /* Mark sibling byte */
            int parent = preg_alias_parent[preg];
            occupied |= (1u << preg_alias_lo[parent]);
            occupied |= (1u << preg_alias_hi[parent]);
        }
    }
    /* SP is never free; BP is never free if frame pointer */
    occupied |= (1u << PREG_SP);
    if (fn->needs_frame) occupied |= (1u << PREG_BP);

    return ~occupied;  /* free = complement of occupied */
}

static void add_insn_uses_to_live(func_t *fn, ir_insn_t *ins,
                                  uint64_t *live) {
    if (ins->src1 >= 0 && ins->src1 < fn->nvregs && ins->op != IR_LEA)
        vset_set(live, ins->src1);
    if (ins->src2 >= 0 && ins->src2 < fn->nvregs)
        vset_set(live, ins->src2);
    for (int j = 0; j < 8; j++)
        if (ins->extra_args[j] >= 0 && ins->extra_args[j] < fn->nvregs)
            vset_set(live, ins->extra_args[j]);
    if (ins->op == IR_RETVAL) {
        for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
            int rv = ins->ret_vregs[j];
            if (rv >= 0 && rv < fn->nvregs)
                vset_set(live, rv);
        }
    }
    if (insn_reads_dst(ins) && ins->dst < fn->nvregs)
        vset_set(live, ins->dst);
}

static void remove_insn_defs_from_live(func_t *fn, ir_insn_t *ins,
                                       uint64_t *live) {
    if (insn_defines_dst(ins) && ins->dst < fn->nvregs)
        vset_clear(live, ins->dst);
    if (ins->op == IR_MCALL) {
        for (int j = 0; j < ins->nrets - 1 && j < MAX_RETURNS; j++) {
            int rv = ins->ret_vregs[j];
            if (rv >= 0 && rv < fn->nvregs)
                vset_clear(live, rv);
        }
    }
}

static bool preg_live_after_insn(func_t *fn, int insn_idx, int written_preg) {
    bblock_t *bb = NULL;
    for (int b = 0; b < fn->nblocks; b++) {
        if ((int)fn->blocks[b].start <= insn_idx &&
            insn_idx < (int)fn->blocks[b].end) {
            bb = &fn->blocks[b];
            break;
        }
    }
    if (!bb)
        return false;

    uint64_t live[VREG_WORDS];
    memcpy(live, bb->live_out, sizeof(live));
    for (int i = (int)bb->end - 1; i > insn_idx; i--) {
        ir_insn_t *ins = &fn->insns[i];
        remove_insn_defs_from_live(fn, ins, live);
        add_insn_uses_to_live(fn, ins, live);
    }

    for (int v = 0; v < fn->nvregs; v++) {
        if (!vset_test(live, v))
            continue;
        int preg = fn->vregs[v].assigned;
        if (preg_write_clobbers(written_preg, preg))
            return true;
    }
    return false;
}

/* Find which block contains instruction index `pos` */
static int block_of(func_t *fn, int pos) {
    for (int b = 0; b < fn->nblocks; b++)
        if (pos >= fn->blocks[b].start && pos < fn->blocks[b].end)
            return b;
    return 0;
}

/* Add a resolved IR instruction */
static void rins_ir(func_t *fn, int ir_idx) {
    if (fn->nresolved >= MAX_RESOLVED) return;
    fn->resolved[fn->nresolved].kind = RINS_IR;
    fn->resolved[fn->nresolved].ir_idx = ir_idx;
    fn->nresolved++;
}

/* Add a resolved pre-formatted assembly line */
static void rins_asm(func_t *fn, const char *fmt, ...) {
    if (fn->nresolved >= MAX_RESOLVED) return;
    fn->resolved[fn->nresolved].kind = RINS_ASM;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fn->resolved[fn->nresolved].asm_text,
              sizeof(fn->resolved[fn->nresolved].asm_text), fmt, ap);
    va_end(ap);
    fn->nresolved++;
}

static bool is_shift_op(const char *op) {
    return strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0 ||
           strcmp(op, "sar") == 0 || strcmp(op, "rol") == 0 ||
           strcmp(op, "ror") == 0 || strcmp(op, "rcl") == 0 ||
           strcmp(op, "rcr") == 0;
}

static int default_return_reg(const char *type, int idx) {
    static const int byte_regs[] = {PREG_AL, PREG_DL, PREG_CL, PREG_BL};
    static const int word_regs[] = {PREG_AX, PREG_DX, PREG_CX, PREG_BX};
    bool is_byte = (strcmp(type, "u8") == 0);
    if (is_byte)
        return idx >= 0 && idx < 4 ? byte_regs[idx] : PREG_NONE;
    return idx >= 0 && idx < 4 ? word_regs[idx] : PREG_NONE;
}

static int function_return_reg(func_t *fn, int idx) {
    if (!fn->has_return)
        return PREG_NONE;
    if (idx < 0 || idx >= fn->nreturns || idx >= MAX_RETURNS)
        return PREG_NONE;
    if (fn->ret_pins[idx] != PREG_NONE)
        return fn->ret_pins[idx];
    return default_return_reg(fn->return_types[idx], idx);
}

static int extern_return_reg(extern_fn_t *ext, int idx) {
    if (idx < 0 || idx >= ext->nreturns || idx >= MAX_RETURNS)
        return PREG_NONE;
    if (ext->ret_pins[idx] != PREG_NONE)
        return ext->ret_pins[idx];
    return default_return_reg(ext->return_types[idx], idx);
}

/* Build the resolved instruction stream.
 * Inserts explicit moves for fixups that the emitter would
 * otherwise handle with push/pop sequences. */
static void insert_fixup_moves(func_t *fn) {
    fn->nresolved = 0;

    for (int i = 0; i < fn->ninsns; i++) {
        ir_insn_t *ins = &fn->insns[i];

        /* ---- CL routing for variable shifts ---- */
        if (ins->op == IR_ALU && !ins->has_imm &&
            is_shift_op(ins->name) &&
            ins->src2 >= 0 && ins->src2 < fn->nvregs &&
            fn->vregs[ins->src2].assigned != PREG_CL) {

            int src2_preg = fn->vregs[ins->src2].assigned;
            int dst_preg = fn->vregs[ins->dst].assigned;
            bool dst_is_cx = (dst_preg == PREG_CX ||
                              dst_preg == PREG_CL ||
                              dst_preg == PREG_CH);
            int blk = block_of(fn, i);
            uint32_t free = free_regs_at(fn, blk, i);

            /* Check if CX/CL is free (no live vreg using it) */
            bool cx_free = (free >> PREG_CX) & 1;

            if (cx_free && !dst_is_cx) {
                /* CX is free — just mov CL, src2. No save needed. */
                if (is_spilled(fn, ins->src2))
                    rins_asm(fn, "    mov CL, %s", vreg_asm(fn, ins->src2));
                else
                    rins_asm(fn, "    mov CL, %s", preg_name[src2_preg]);
                /* Mark that the emitter should use CL directly */
                fn->vregs[ins->src2].assigned = PREG_CL;
                rins_ir(fn, i);
                fn->vregs[ins->src2].assigned = src2_preg; /* restore */
            } else if (!dst_is_cx) {
                /* CX is occupied — push/pop CX around the route */
                rins_asm(fn, "    push CX");
                if (is_spilled(fn, ins->src2))
                    rins_asm(fn, "    mov CL, %s", vreg_asm(fn, ins->src2));
                else
                    rins_asm(fn, "    mov CL, %s", preg_name[src2_preg]);
                fn->vregs[ins->src2].assigned = PREG_CL;
                rins_ir(fn, i);
                fn->vregs[ins->src2].assigned = src2_preg;
                rins_asm(fn, "    pop CX");
            } else {
                /* dst aliases CX — need to route through AX */
                bool byte_dst = fn->vregs[ins->dst].is_byte;
                const char *acc = byte_dst ? "AL" : "AX";
                const char *d = vreg_asm(fn, ins->dst);
                bool ax_free = (free >> PREG_AX) & 1;
                if (!ax_free) rins_asm(fn, "    push AX");
                rins_asm(fn, "    mov %s, %s", acc, d);
                rins_asm(fn, "    push CX");
                if (is_spilled(fn, ins->src2))
                    rins_asm(fn, "    mov CL, %s", vreg_asm(fn, ins->src2));
                else
                    rins_asm(fn, "    mov CL, %s", preg_name[src2_preg]);
                rins_asm(fn, "    %s %s, CL", ins->name, acc);
                rins_asm(fn, "    pop CX");
                rins_asm(fn, "    mov %s, %s", d, acc);
                if (!ax_free) rins_asm(fn, "    pop AX");
                /* Don't emit the original IR instruction — we handled it */
            }
            continue;
        }

        /* ---- Loadmem/storemem address register fixup ---- */
        if (ins->op == IR_LOADMEM || ins->op == IR_STOREMEM) {
            /* If address references a physical register (e.g., [ES:SI])
             * and the vreg preferring that register got assigned elsewhere,
             * insert a mov to place the value in the expected register. */
            for (int preg = 0; preg < NUM_PREGS; preg++) {
                const char *rn = preg_name[preg];
                const char *p2 = ins->name;
                bool found = false;
                while ((p2 = strstr(p2, rn)) != NULL) {
                    char before = (p2 > ins->name) ? p2[-1] : '[';
                    char after = p2[strlen(rn)];
                    if (!isalpha(before) && !isalpha(after))
                        { found = true; break; }
                    p2++;
                }
                if (!found) continue;
                int best_v = -1, best_def = -1;
                for (int v = 0; v < fn->nvregs; v++) {
                    if (fn->vregs[v].prefer != preg) continue;
                    if (fn->vregs[v].assigned == preg) continue;
                    if (fn->vregs[v].assigned == PREG_NONE) continue;
                    if (fn->vregs[v].def_pos > (int)i) continue;
                    if (fn->vregs[v].def_pos > best_def)
                        { best_def = fn->vregs[v].def_pos; best_v = v; }
                }
                if (best_v >= 0)
                    rins_asm(fn, "    mov %s, %s",
                             rn, preg_name[fn->vregs[best_v].assigned]);
            }
            rins_ir(fn, i);
            continue;
        }

        /* ---- Call argument fixup + BP caller-save ---- */
        if (ins->op == IR_CALL || ins->op == IR_MCALL) {
            /* Build callee preserves set.
             * Explicit preserves/clobbers declaration takes priority.
             * For internal functions without a declaration, use the
             * computed clobber set: preserves = everything not clobbered. */
            bool callee_preserves[NUM_PREGS];
            memset(callee_preserves, 0, sizeof(callee_preserves));
            bool found_callee = false;
            for (int fi2 = 0; fi2 < nfunctions && !found_callee; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0) {
                    if (functions[fi2].nfn_preserves > 0) {
                        /* Explicit declaration — use it */
                        for (int pp = 0; pp < functions[fi2].nfn_preserves; pp++)
                            callee_preserves[functions[fi2].fn_preserves[pp]] = true;
                    } else if (fn_assigns[fi2].resolved) {
                        /* No declaration — derive from computed clobbers */
                        for (int r = 0; r < NUM_PREGS; r++)
                            callee_preserves[r] = !fn_assigns[fi2].clobbers[r];
                    }
                    if (functions[fi2].ds_policy != DS_POLICY_UNSPEC)
                        callee_preserves[PREG_DS] = true;
                    /* else: unresolved, no preserves — assume all clobbered */
                    found_callee = true;
                }
            }
            if (!found_callee) {
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0) {
                        for (int pp = 0; pp < externs[e].npreserves; pp++)
                            callee_preserves[externs[e].preserves[pp]] = true;
                        if (externs[e].ds_policy != DS_POLICY_UNSPEC)
                            callee_preserves[PREG_DS] = true;
                        break;
                    }
                }
            }

            int callee_fi = -1;
            int callee_ext = -1;
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, ins->name) == 0)
                    { callee_fi = fi2; break; }
            }
            if (callee_fi < 0) {
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0)
                        { callee_ext = e; break; }
                }
            }

            /* BP caller-save */
            /* Caller-save: collect live registers the callee may clobber */
            int call_saved[16];
            int call_nsaved = 0;
            bool call_use_pusha = false;
            bool outgoing_bp_param = false;
            bool outgoing_clobbers[NUM_PREGS];
            memset(outgoing_clobbers, 0, sizeof(outgoing_clobbers));
            bool caller_bp_live = fn->needs_frame;
            if (callee_fi >= 0) {
                fn_assignment_t *callee_fa = &fn_assigns[callee_fi];
                for (int a = 0; a < ins->nargs && a < 16; a++) {
                    int expected = callee_fa->param_regs[a];
                    if (expected == PREG_BP) {
                        outgoing_bp_param = true;
                    }
                    if (expected == PREG_NONE || expected == PREG_SP)
                        continue;
                    int arg_vreg = (a == 0) ? ins->src1 :
                                   (a == 1) ? ins->src2 :
                                              ins->extra_args[a - 2];
                    if (arg_vreg < 0 || arg_vreg >= fn->nvregs)
                        continue;
                    int actual = fn->vregs[arg_vreg].assigned;
                    if (actual == expected || pregs_alias(actual, expected))
                        continue;
                    int clob = (expected >= PREG_AL && expected <= PREG_BH)
                        ? preg_alias_parent[expected] : expected;
                    outgoing_clobbers[clob] = true;
                }
            } else if (callee_ext >= 0) {
                for (int a = 0; a < ins->nargs && a < externs[callee_ext].nparams; a++) {
                    int expected = externs[callee_ext].param_pins[a].preg;
                    if (expected == PREG_BP) {
                        outgoing_bp_param = true;
                    }
                    if (expected == PREG_NONE || expected == PREG_SP)
                        continue;
                    int arg_vreg = (a == 0) ? ins->src1 :
                                   (a == 1) ? ins->src2 :
                                              ins->extra_args[a - 2];
                    if (arg_vreg < 0 || arg_vreg >= fn->nvregs)
                        continue;
                    int actual = fn->vregs[arg_vreg].assigned;
                    if (actual == expected || pregs_alias(actual, expected))
                        continue;
                    int clob = (expected >= PREG_AL && expected <= PREG_BH)
                        ? preg_alias_parent[expected] : expected;
                    outgoing_clobbers[clob] = true;
                }
            }
            if (!caller_bp_live) {
                for (int v = 0; v < fn->nvregs; v++) {
                    if (v == ins->dst) continue;
                    bool is_ret_dst = false;
                    if (ins->op == IR_MCALL) {
                        for (int ri = 0; ri < ins->nrets - 1 && ri < MAX_RETURNS; ri++) {
                            if (v == ins->ret_vregs[ri]) {
                                is_ret_dst = true;
                                break;
                            }
                        }
                    }
                    if (is_ret_dst) continue;
                    if (fn->vregs[v].assigned != PREG_BP) continue;
                    if (fn->vregs[v].last_use <= (int)i) continue;
                    if (fn->vregs[v].def_pos > (int)i) continue;
                    caller_bp_live = true;
                    break;
                }
            }
            if ((fn->needs_frame && !callee_preserves[PREG_BP]) ||
                (outgoing_bp_param && caller_bp_live))
                add_call_saved_reg(call_saved, &call_nsaved, PREG_BP);
            for (int v = 0; v < fn->nvregs; v++) {
                if (v == ins->dst) continue;
                bool is_ret_dst = false;
                if (ins->op == IR_MCALL) {
                    for (int ri = 0; ri < ins->nrets - 1 && ri < MAX_RETURNS; ri++) {
                        if (v == ins->ret_vregs[ri]) { is_ret_dst = true; break; }
                    }
                }
                if (is_ret_dst) continue;
                int preg = fn->vregs[v].assigned;
                if (preg == PREG_NONE || preg == PREG_SP) continue;
                if (fn->vregs[v].is_seg) continue;
                if (fn->vregs[v].last_use <= (int)i) continue;
                if (fn->vregs[v].def_pos > (int)i) continue;
                int push_reg = preg;
                if (preg >= PREG_AL && preg <= PREG_BH)
                    push_reg = preg_alias_parent[preg];
                if (callee_preserves[push_reg] && !outgoing_clobbers[push_reg])
                    continue;
                add_call_saved_reg(call_saved, &call_nsaved, push_reg);
            }
            /* Emit saves: PUSHA if >= 6, individual pushes otherwise */
            if (call_nsaved >= 6) {
                rins_asm(fn, "    pusha");
                call_use_pusha = true;
            } else {
                for (int s = 0; s < call_nsaved; s++)
                    rins_asm(fn, "    push %s", preg_name[call_saved[s]]);
            }

            /* Argument fixup: place args in expected registers */
            if (callee_fi >= 0) {
                fn_assignment_t *callee_fa = &fn_assigns[callee_fi];
                for (int a = 0; a < ins->nargs; a++) {
                    int arg_vreg;
                    if (a == 0) arg_vreg = ins->src1;
                    else if (a == 1) arg_vreg = ins->src2;
                    else arg_vreg = ins->extra_args[a - 2];
                    if (arg_vreg < 0) continue;
                    int expected = callee_fa->param_regs[a];
                    if (expected == PREG_NONE) continue;
                    int actual = fn->vregs[arg_vreg].assigned;
                    if (actual == expected || pregs_alias(actual, expected))
                        continue;
                    /* Emit fixup mov */
                    if (is_spilled(fn, arg_vreg)) {
                        rins_asm(fn, "    mov %s, %s",
                                 preg_name[expected], vreg_asm(fn, arg_vreg));
                    } else {
                        rins_asm(fn, "    mov %s, %s",
                                 preg_name[expected], preg_name[actual]);
                    }
                }
            }

            /* Emit the call instruction itself (emitter handles encoding) */
            rins_ir(fn, i);

            /* Capture return registers before restoring caller-saves. */
            int nrets = ins->op == IR_MCALL ? ins->nrets : 1;
            for (int ri = 0; ri < nrets && ri < MAX_RETURNS; ri++) {
                int ret_v = (ri == 0) ? ins->dst : ins->ret_vregs[ri - 1];
                if (ret_v < 0 || ret_v >= fn->nvregs) continue;
                int expected = PREG_NONE;
                if (callee_fi >= 0)
                    expected = fn_assigns[callee_fi].return_regs[ri];
                else if (callee_ext >= 0)
                    expected = extern_return_reg(&externs[callee_ext], ri);
                if (expected == PREG_NONE) continue;
                int actual = fn->vregs[ret_v].assigned;
                if (actual != expected) {
                    rins_asm(fn, "    mov %s, %s",
                             vreg_asm(fn, ret_v), preg_name[expected]);
                }
            }

            /* Caller-restore */
            if (call_use_pusha) {
                rins_asm(fn, "    popa");
            } else {
                for (int s = call_nsaved - 1; s >= 0; s--)
                    rins_asm(fn, "    pop %s", preg_name[call_saved[s]]);
            }

            continue;
        }

        /* ---- Indirect far call caller-save ---- */
        if (ins->op == IR_ICALL) {
            bool callee_preserves[NUM_PREGS];
            memset(callee_preserves, 0, sizeof(callee_preserves));
            for (int e = 0; e < nexterns; e++) {
                if (strcmp(externs[e].name, ins->name) == 0) {
                    for (int pp = 0; pp < externs[e].npreserves; pp++)
                        callee_preserves[externs[e].preserves[pp]] = true;
                    if (externs[e].ds_policy != DS_POLICY_UNSPEC)
                        callee_preserves[PREG_DS] = true;
                    break;
                }
            }

            int call_saved[16];
            int call_nsaved = 0;
            bool call_use_pusha = false;
            if (fn->needs_frame && !callee_preserves[PREG_BP]) {
                call_saved[call_nsaved++] = PREG_BP;
            }
            for (int v = 0; v < fn->nvregs; v++) {
                if (v == ins->dst) continue;
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
            }

            if (call_nsaved >= 6) {
                rins_asm(fn, "    pusha");
                call_use_pusha = true;
            } else {
                for (int s = 0; s < call_nsaved; s++)
                    rins_asm(fn, "    push %s", preg_name[call_saved[s]]);
            }

            rins_ir(fn, i);

            if (call_use_pusha) {
                rins_asm(fn, "    popa");
            } else {
                for (int s = call_nsaved - 1; s >= 0; s--)
                    rins_asm(fn, "    pop %s", preg_name[call_saved[s]]);
            }

            continue;
        }

        /* ---- Return value materialization ---- */
        if (ins->op == IR_RETVAL) {
            int nrets = ins->nrets > 0 ? ins->nrets : 1;
            struct { int dst_reg; int src_vreg; int src_reg; bool done; } moves[MAX_RETURNS];
            int nmoves = 0;
            for (int ri = 0; ri < nrets && ri < MAX_RETURNS; ri++) {
                int ret_reg = function_return_reg(fn, ri);
                int src = (ri == 0) ? ins->src1 : ins->ret_vregs[ri - 1];
                if (ret_reg == PREG_NONE || src < 0 || src >= fn->nvregs)
                    continue;
                int src_reg = fn->vregs[src].assigned;
                if (src_reg == ret_reg)
                    continue;
                moves[nmoves].dst_reg = ret_reg;
                moves[nmoves].src_vreg = src;
                moves[nmoves].src_reg = src_reg;
                moves[nmoves].done = false;
                nmoves++;
            }
            for (int done = 0; done < nmoves; ) {
                int pick = -1;
                for (int mi = 0; mi < nmoves; mi++) {
                    if (moves[mi].done) continue;
                    bool clobbers_pending_src = false;
                    for (int mj = 0; mj < nmoves; mj++) {
                        if (mi == mj || moves[mj].done) continue;
                        if (moves[mj].src_reg != PREG_NONE &&
                            pregs_alias(moves[mi].dst_reg, moves[mj].src_reg)) {
                            clobbers_pending_src = true;
                            break;
                        }
                    }
                    if (!clobbers_pending_src) { pick = mi; break; }
                }
                if (pick < 0)
                    pick = 0; /* Cycles are rare; preserve progress. */
                while (pick < nmoves && moves[pick].done) pick++;
                if (pick >= nmoves) break;
                rins_asm(fn, "    mov %s, %s",
                         preg_name[moves[pick].dst_reg],
                         vreg_asm(fn, moves[pick].src_vreg));
                moves[pick].done = true;
                done++;
            }
            rins_ir(fn, i);
            continue;
        }
        /* Default: pass through unchanged */
        rins_ir(fn, i);
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

static int local_bp_offset(func_t *fn, int v) {
    if (v < 0 || v >= fn->nvregs || !fn->vregs[v].is_local_slot)
        return 0;
    return -(fn->nspill_slots * 2 + fn->vregs[v].local_offset);
}

/* Like vreg_asm but with size prefix for spilled memory operands.
 * Use when the instruction has no register operand to disambiguate size
 * (e.g., shl [BP-N], imm or cmp [BP-N], imm). */
static const char *vreg_asm_sized(func_t *fn, int v) {
    if (v < 0 || v >= fn->nvregs) return vreg_asm(fn, v);
    if (fn->vregs[v].spill_slot < 0) return vreg_asm(fn, v);
    static char buf[4][40];
    static int idx = 0;
    char *b = buf[idx++ & 3];
    int off = -(fn->vregs[v].spill_slot + 1) * 2;
    const char *sz = fn->vregs[v].is_byte ? "byte" : "word";
    snprintf(b, 40, "%s [BP%+d]", sz, off);
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
            fprintf(out_asm, "    push %s\n", s);
            fprintf(out_asm, "    pop %s\n", d);
        }
    } else if (fn->vregs[dst].is_seg && fn->vregs[src].is_seg) {
        /* seg-to-seg: go through AX, saving it first */
        fprintf(out_asm, "    push AX\n");
        fprintf(out_asm, "    mov AX, %s\n", s);
        fprintf(out_asm, "    mov %s, AX\n", d);
        fprintf(out_asm, "    pop AX\n");
    } else if (dst_byte && !src_byte && !is_spilled(fn, src)) {
        /* byte dst, word src: extract low byte */
        int src_preg = fn->vregs[src].assigned;
        int dst_preg = fn->vregs[dst].assigned;
        if (src_preg >= PREG_AX && src_preg <= PREG_BX) {
            /* AX..BX have accessible low bytes — skip if dst is already it */
            if (dst_preg == preg_alias_lo[src_preg]) return;
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

/* ---- Emission helpers for hardware workarounds ---- */

static bool emit_es_data_prefix(func_t *fn, int addr_vreg) {
    const char *label = vreg_data_label(fn, addr_vreg);
    if (!label)
        return false;
    data_block_t *db = find_data_block(label);
    if (!db || !db->has_emit_seg)
        return false;
    if (fn->emit_seg >= 0 && db->emit_seg == fn->emit_seg)
        return false;

    fprintf(out_asm, "    push ES\n");
    fprintf(out_asm, "    push AX\n");
    fprintf(out_asm, "    mov AX, SEG %s\n", label);
    fprintf(out_asm, "    mov ES, AX\n");
    fprintf(out_asm, "    pop AX\n");
    return true;
}

static const char *near_data_seg_prefix(func_t *fn, int addr_vreg) {
    const char *label = vreg_data_label(fn, addr_vreg);
    if (label) {
        data_block_t *db = find_data_block(label);
        if (db && db->has_emit_seg &&
            fn->emit_seg >= 0 && db->emit_seg == fn->emit_seg)
            return "CS:";
        return "";
    }
    if (addr_vreg >= 0 && addr_vreg < MAX_VREGS &&
        fn->vregs[addr_vreg].is_cs_ref)
        return "CS:";
    return "";
}

static void emit_es_data_suffix(bool used_es) {
    if (used_es)
        fprintf(out_asm, "    pop ES\n");
}

static bool ds_policy_sets_ds(func_t *fn) {
    return fn->ds_policy == DS_POLICY_SYMBOL ||
           fn->ds_policy == DS_POLICY_LITERAL;
}

static bool reg_list_contains(int *regs, int nregs, int preg) {
    for (int i = 0; i < nregs; i++)
        if (regs[i] == preg)
            return true;
    return false;
}

static void add_call_saved_reg(int *call_saved, int *call_nsaved, int preg) {
    for (int s = 0; s < *call_nsaved; s++)
        if (call_saved[s] == preg)
            return;
    if (*call_nsaved < 16)
        call_saved[(*call_nsaved)++] = preg;
}

static void emit_ds_setup(func_t *fn, bool explicit_save) {
    if (!ds_policy_sets_ds(fn))
        return;
    if (explicit_save)
        fprintf(out_asm, "    push DS\n");
    fprintf(out_asm, "    push AX\n");
    if (fn->ds_policy == DS_POLICY_SYMBOL)
        fprintf(out_asm, "    mov AX, SEG %s\n", fn->ds_symbol);
    else
        fprintf(out_asm, "    mov AX, 0x%04X\n", fn->ds_literal & 0xFFFF);
    fprintf(out_asm, "    mov DS, AX\n");
    fprintf(out_asm, "    pop AX\n");
}

static void emit_epilogue(func_t *fn, int *save_regs, int nsave,
                          int *isr_save, int isr_nsave,
                          bool ds_explicit_save) {
    if (fn->needs_frame) {
        fprintf(out_asm, "    mov sp, bp\n");
        fprintf(out_asm, "    pop bp\n");
    }
    if (ds_explicit_save)
        fprintf(out_asm, "    pop DS\n");
    /* Callee-save pops (reverse order) */
    for (int j = nsave - 1; j >= 0; j--)
        fprintf(out_asm, "    pop %s\n", preg_name[save_regs[j]]);
    if (fn->is_interrupt) {
        if (isr_nsave >= 6) {
            fprintf(out_asm, "    popa\n");
        } else {
            for (int j = isr_nsave - 1; j >= 0; j--)
                fprintf(out_asm, "    pop %s\n", preg_name[isr_save[j]]);
        }
        fprintf(out_asm, "    iret\n");
    } else if (fn->is_far) {
        fprintf(out_asm, "    retf\n");
    } else {
        fprintf(out_asm, "    ret\n");
    }
}

static void emit_param_entry_moves(func_t *fn, int fn_idx) {
    if (fn_idx < 0)
        return;
    fn_assignment_t *fa = &fn_assigns[fn_idx];

    for (int p = 0; p < fn->nparams; p++) {
        int v = fn->param_vregs[p];
        int expected = fa->param_regs[p];
        if (v < 0 || v >= MAX_VREGS || expected == PREG_NONE)
            continue;
        if (fn->vregs[v].is_local_slot)
            continue;

        if (is_spilled(fn, v)) {
            if (expected == PREG_BP && fn->needs_frame) {
                fprintf(out_asm, "    push AX\n");
                fprintf(out_asm, "    mov AX, [BP]\n");
                fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, v));
                fprintf(out_asm, "    pop AX\n");
                continue;
            }
            if (fn->vregs[v].is_byte)
                fprintf(out_asm, "    mov %s, %s\n",
                        vreg_asm_sized(fn, v), preg_name[expected]);
            else
                fprintf(out_asm, "    mov %s, %s\n",
                        vreg_asm(fn, v), preg_name[expected]);
            continue;
        }

        int assigned = fn->vregs[v].assigned;
        if (assigned == PREG_NONE || assigned == expected ||
            pregs_alias(assigned, expected))
            continue;

        if (fn->vregs[v].is_seg && assigned >= PREG_ES &&
            assigned <= PREG_DS) {
            if (expected >= PREG_ES && expected <= PREG_DS) {
                fprintf(out_asm, "    push AX\n");
                fprintf(out_asm, "    mov AX, %s\n", preg_name[expected]);
                fprintf(out_asm, "    mov %s, AX\n", preg_name[assigned]);
                fprintf(out_asm, "    pop AX\n");
            } else if (expected == PREG_BP && fn->needs_frame) {
                fprintf(out_asm, "    push AX\n");
                fprintf(out_asm, "    mov AX, [BP]\n");
                fprintf(out_asm, "    mov %s, AX\n", preg_name[assigned]);
                fprintf(out_asm, "    pop AX\n");
            } else if (expected == PREG_AX) {
                fprintf(out_asm, "    mov %s, AX\n", preg_name[assigned]);
            } else {
                fprintf(out_asm, "    push AX\n");
                fprintf(out_asm, "    mov AX, %s\n", preg_name[expected]);
                fprintf(out_asm, "    mov %s, AX\n", preg_name[assigned]);
                fprintf(out_asm, "    pop AX\n");
            }
        }
    }
}

/* Emit an ALU instruction (handles special ops, spill combinations,
 * three-address lowering, byte IMUL, and shift CL routing fallback) */
static void emit_alu(func_t *fn, ir_insn_t *ins, int i) {
    const char *op = ins->name;

    /* --- Special fixed-register ops --- */
    if (strcmp(op, "in") == 0 || strcmp(op, "inb") == 0) {
        bool byte_in = (strcmp(op, "inb") == 0);
        const char *acc = byte_in ? "AL" : "AX";
        if (ins->has_imm)
            fprintf(out_asm, "    in %s, 0x%02X\n", acc, ins->imm);
        else
            fprintf(out_asm, "    in %s, %s\n", acc, vreg_asm(fn, ins->src1));
        const char *d = vreg_asm(fn, ins->dst);
        if (strcmp(d, acc) != 0)
            fprintf(out_asm, "    mov %s, %s\n", d, acc);
        return;
    }
    if (strcmp(op, "out") == 0 || strcmp(op, "outb") == 0) {
        bool byte_out = (strcmp(op, "outb") == 0);
        const char *acc = byte_out ? "AL" : "AX";
        int acc_preg = byte_out ? PREG_AL : PREG_AX;
        const char *val = vreg_asm(fn, ins->src1);
        bool preserve_acc = strcmp(acc, val) != 0 &&
            preg_live_after_insn(fn, i, acc_preg);
        if (preserve_acc)
            fprintf(out_asm, "    push AX\n");
        if (strcmp(acc, val) != 0)
            fprintf(out_asm, "    mov %s, %s\n", acc, val);
        if (ins->has_imm)
            fprintf(out_asm, "    out 0x%02X, %s\n", ins->imm, acc);
        else
            fprintf(out_asm, "    out %s, %s\n", vreg_asm(fn, ins->dst), acc);
        if (preserve_acc)
            fprintf(out_asm, "    pop AX\n");
        return;
    }

    /* --- Far pointer ops (spill handling for BX base) --- */
    if (strcmp(op, "far.off") == 0) {
        bool use_es = emit_es_data_prefix(fn, ins->src1);
        const char *seg = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
        if (is_spilled(fn, ins->src1)) {
            fprintf(out_asm, "    push BX\n");
            fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
            if (is_spilled(fn, ins->dst)) {
                fprintf(out_asm, "    mov AX, [%sBX]\n", seg);
                fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
            } else {
                fprintf(out_asm, "    mov %s, [%sBX]\n", vreg_asm(fn, ins->dst), seg);
            }
            fprintf(out_asm, "    pop BX\n");
        } else if (is_spilled(fn, ins->dst)) {
            fprintf(out_asm, "    mov AX, [%s%s]\n", seg, vreg_asm(fn, ins->src1));
            fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
        } else {
            fprintf(out_asm, "    mov %s, [%s%s]\n",
                    vreg_asm(fn, ins->dst), seg, vreg_asm(fn, ins->src1));
        }
        emit_es_data_suffix(use_es);
        return;
    }
    if (strcmp(op, "far.seg") == 0) {
        bool use_es = emit_es_data_prefix(fn, ins->src1);
        const char *seg = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
        const char *d = vreg_asm(fn, ins->dst);
        bool dst_seg = (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                        fn->vregs[ins->dst].is_seg);
        bool dst_is_es = (use_es && ins->dst >= 0 && ins->dst < MAX_VREGS &&
                          fn->vregs[ins->dst].assigned == PREG_ES &&
                          !is_spilled(fn, ins->dst));
        if (dst_is_es) {
            fprintf(out_asm, "    push AX\n");
            if (is_spilled(fn, ins->src1)) {
                fprintf(out_asm, "    push BX\n");
                fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                fprintf(out_asm, "    mov AX, [%sBX+2]\n", seg);
                fprintf(out_asm, "    pop BX\n");
            } else {
                fprintf(out_asm, "    mov AX, [%s%s+2]\n", seg, vreg_asm(fn, ins->src1));
            }
            emit_es_data_suffix(use_es);
            fprintf(out_asm, "    mov ES, AX\n");
            fprintf(out_asm, "    pop AX\n");
            return;
        }
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
        emit_es_data_suffix(use_es);
        return;
    }

    /* --- Other special ops --- */
    if (strcmp(op, "xlat") == 0) {
        fprintf(out_asm, "    xlat\n");
        return;
    }
    if (strcmp(op, "mul") == 0 || strcmp(op, "imul") == 0) {
        if (ins->has_imm) {
            /* Word IMUL with immediate — compiler handles byte-to-word
             * promotion, so src is always word-sized here */
            fprintf(out_asm, "    imul %s, %d\n", vreg_asm(fn, ins->src1), ins->imm);
            emit_mov(fn, ins->dst, ins->src1);
        } else {
            fprintf(out_asm, "    %s %s\n", op, vreg_asm(fn, ins->src2));
        }
        return;
    }
    if (strcmp(op, "div") == 0 || strcmp(op, "mod") == 0) {
        fprintf(out_asm, "    div %s\n", vreg_asm(fn, ins->src2)); return;
    }
    if (strcmp(op, "idiv") == 0 || strcmp(op, "imod") == 0) {
        fprintf(out_asm, "    idiv %s\n", vreg_asm(fn, ins->src2)); return;
    }
    if (strcmp(op, "xchg") == 0) {
        fprintf(out_asm, "    xchg %s, %s\n",
                vreg_asm(fn, ins->src1), vreg_asm(fn, ins->src2)); return;
    }
    if (strcmp(op, "stos") == 0) { fprintf(out_asm, "    stosb\n"); return; }
    if (strcmp(op, "bext") == 0) {
        bool dst_is_ax = (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                          fn->vregs[ins->dst].assigned == PREG_AX);
        if (!dst_is_ax)
            fprintf(out_asm, "    push AX\n");
        fprintf(out_asm, "    push SI\n");
        if (!(ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
              fn->vregs[ins->src1].assigned == PREG_SI))
            fprintf(out_asm, "    mov SI, %s\n", vreg_asm(fn, ins->src1));
        fprintf(out_asm, "    bext %s, %s\n",
                vreg_asm(fn, ins->src2),
                vreg_asm(fn, ins->extra_args[0]));
        fprintf(out_asm, "    pop SI\n");
        if (!dst_is_ax) {
            fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
            fprintf(out_asm, "    pop AX\n");
        }
        return;
    }
    if (strcmp(op, "bins") == 0) {
        fprintf(out_asm, "    push AX\n");
        fprintf(out_asm, "    push DI\n");
        if (!(ins->extra_args[0] >= 0 && ins->extra_args[0] < MAX_VREGS &&
              fn->vregs[ins->extra_args[0]].assigned == PREG_AX))
            fprintf(out_asm, "    mov AX, %s\n", vreg_asm(fn, ins->extra_args[0]));
        if (!(ins->dst >= 0 && ins->dst < MAX_VREGS &&
              fn->vregs[ins->dst].assigned == PREG_DI))
            fprintf(out_asm, "    mov DI, %s\n", vreg_asm(fn, ins->dst));
        fprintf(out_asm, "    bins %s, %s\n",
                vreg_asm(fn, ins->src1), vreg_asm(fn, ins->src2));
        fprintf(out_asm, "    pop DI\n");
        fprintf(out_asm, "    pop AX\n");
        return;
    }

    /* --- General ALU: three-address → two-address lowering --- */
    /* Check for dst == src2 alias: if dst already holds src2,
     * doing mov dst, src1 would clobber it. Instead, op dst, src1
     * since dst already contains src2 and the op is commutative.
     * For non-commutative ops (sub), swap operands and use reverse. */
    bool dst_is_src2 = (!ins->has_imm && ins->src2 >= 0 &&
                        ins->dst >= 0 && ins->src2 < MAX_VREGS &&
                        ins->dst < MAX_VREGS &&
                        !is_spilled(fn, ins->dst) &&
                        !is_spilled(fn, ins->src2) &&
                        fn->vregs[ins->dst].assigned ==
                        fn->vregs[ins->src2].assigned);
    if (dst_is_src2 && !is_shift_op(op)) {
        /* dst already contains src2 — emit: op dst, src1 */
        if (is_spilled(fn, ins->src1)) {
            bool alu_byte = fn->vregs[ins->dst].is_byte;
            const char *acc = alu_byte ? "AL" : "AX";
            fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src1));
            fprintf(out_asm, "    %s %s, %s\n", op, vreg_asm(fn, ins->dst), acc);
        } else {
            fprintf(out_asm, "    %s %s, %s\n", op,
                    vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src1));
        }
    } else {
        emit_mov(fn, ins->dst, ins->src1);
        if (ins->has_imm) {
            fprintf(out_asm, "    %s %s, %d\n", op, vreg_asm_sized(fn, ins->dst), ins->imm);
        } else if (is_shift_op(op)) {
            /* Shift: count should be in CL (move insertion pass handles routing).
             * Fallback: if src2 is already CL, emit directly. */
            fprintf(out_asm, "    %s %s, CL\n", op, vreg_asm_sized(fn, ins->dst));
        } else if (is_spilled(fn, ins->dst) && is_spilled(fn, ins->src2)) {
            bool alu_byte = fn->vregs[ins->dst].is_byte;
            const char *acc = alu_byte ? "AL" : "AX";
            fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src2));
            fprintf(out_asm, "    %s %s, %s\n", op, vreg_asm(fn, ins->dst), acc);
        } else {
            fprintf(out_asm, "    %s %s, %s\n", op,
                    vreg_asm(fn, ins->dst), vreg_asm(fn, ins->src2));
        }
    }
}

/* Emit a LOAD instruction (handles spilled base/index with scratch regs) */
static void emit_load(func_t *fn, ir_insn_t *ins) {
    const char *base_str;
    const char *idx_str = NULL;
    bool pushed_bx = false, pushed_si = false;
    bool use_es = emit_es_data_prefix(fn, ins->src1);
    const char *seg_pfx = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
    bool base_spilled = is_spilled(fn, ins->src1);
    bool idx_spilled = (ins->src2 >= 0 && is_spilled(fn, ins->src2));
    bool dst_conflicts_with_scratch = false;

    if (!is_spilled(fn, ins->dst)) {
        int dst_preg = fn->vregs[ins->dst].assigned;
        if ((base_spilled && pregs_alias(dst_preg, PREG_BX)) ||
            (idx_spilled && dst_preg == PREG_SI))
            dst_conflicts_with_scratch = true;
    }

    if (dst_conflicts_with_scratch)
        fprintf(out_asm, "    push AX\n");

    if (base_spilled) {
        fprintf(out_asm, "    push BX\n");
        fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
        base_str = "BX"; pushed_bx = true;
    } else {
        base_str = vreg_asm(fn, ins->src1);
    }
    if (idx_spilled) {
        fprintf(out_asm, "    push SI\n");
        fprintf(out_asm, "    mov SI, %s\n", vreg_asm(fn, ins->src2));
        idx_str = "SI"; pushed_si = true;
    } else if (ins->src2 >= 0) {
        idx_str = vreg_asm(fn, ins->src2);
    }

    if (use_es && !is_spilled(fn, ins->dst) &&
        fn->vregs[ins->dst].assigned == PREG_ES) {
        fprintf(out_asm, "    push AX\n");
        if (idx_str)
            fprintf(out_asm, "    mov AX, [%s%s+%s]\n", seg_pfx, base_str, idx_str);
        else
            fprintf(out_asm, "    mov AX, [%s%s]\n", seg_pfx, base_str);
        if (pushed_si) fprintf(out_asm, "    pop SI\n");
        if (pushed_bx) fprintf(out_asm, "    pop BX\n");
        emit_es_data_suffix(use_es);
        fprintf(out_asm, "    mov ES, AX\n");
        fprintf(out_asm, "    pop AX\n");
        return;
    }

    if (is_spilled(fn, ins->dst) || dst_conflicts_with_scratch) {
        bool ld_byte = fn->vregs[ins->dst].is_byte;
        const char *acc = ld_byte ? "AL" : "AX";
        if (idx_str)
            fprintf(out_asm, "    mov %s, [%s%s+%s]\n", acc, seg_pfx, base_str, idx_str);
        else
            fprintf(out_asm, "    mov %s, [%s%s]\n", acc, seg_pfx, base_str);
        if (pushed_si) fprintf(out_asm, "    pop SI\n");
        if (pushed_bx) fprintf(out_asm, "    pop BX\n");
        fprintf(out_asm, "    mov %s, %s\n", vreg_asm(fn, ins->dst), acc);
        if (dst_conflicts_with_scratch)
            fprintf(out_asm, "    pop AX\n");
        emit_es_data_suffix(use_es);
    } else {
        if (idx_str)
            fprintf(out_asm, "    mov %s, [%s%s+%s]\n",
                    vreg_asm(fn, ins->dst), seg_pfx, base_str, idx_str);
        else
            fprintf(out_asm, "    mov %s, [%s%s]\n",
                    vreg_asm(fn, ins->dst), seg_pfx, base_str);
        if (pushed_si) fprintf(out_asm, "    pop SI\n");
        if (pushed_bx) fprintf(out_asm, "    pop BX\n");
        emit_es_data_suffix(use_es);
    }
}

/* Emit a STORE instruction (handles spilled value/base/index) */
static void emit_store(func_t *fn, ir_insn_t *ins) {
    const char *val_str;
    const char *base_str;
    const char *idx_str = NULL;
    bool pushed_bx = false, pushed_si = false;
    bool need_es = vreg_data_label(fn, ins->src1) &&
        strcmp(near_data_seg_prefix(fn, ins->src1), "CS:") != 0;
    bool pushed_val_ax = false;

    if (is_spilled(fn, ins->dst)) {
        bool st_byte = fn->vregs[ins->dst].is_byte;
        const char *acc = st_byte ? "AL" : "AX";
        fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->dst));
        val_str = acc;
    } else {
        val_str = vreg_asm(fn, ins->dst);
        if (need_es && fn->vregs[ins->dst].assigned == PREG_ES) {
            fprintf(out_asm, "    push AX\n");
            fprintf(out_asm, "    mov AX, ES\n");
            val_str = "AX";
            pushed_val_ax = true;
        }
    }
    if (is_spilled(fn, ins->src1)) {
        /* Check for conflict: if value is in BX or BL/BH,
         * loading the spilled base into BX would clobber it.
         * Save value to accumulator first. */
        if (!is_spilled(fn, ins->dst)) {
            int val_preg = fn->vregs[ins->dst].assigned;
            if (val_preg == PREG_BX || val_preg == PREG_BL || val_preg == PREG_BH) {
                bool st_byte = fn->vregs[ins->dst].is_byte;
                const char *acc = st_byte ? "AL" : "AX";
                fprintf(out_asm, "    mov %s, %s\n", acc, val_str);
                val_str = acc;
            }
        }
        fprintf(out_asm, "    push BX\n");
        fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
        base_str = "BX"; pushed_bx = true;
    } else {
        base_str = vreg_asm(fn, ins->src1);
    }
    if (ins->src2 >= 0 && is_spilled(fn, ins->src2)) {
        /* Check for conflict: if value is in SI, save to accumulator first */
        if (!is_spilled(fn, ins->dst)) {
            int val_preg = fn->vregs[ins->dst].assigned;
            if (val_preg == PREG_SI) {
                bool st_byte = fn->vregs[ins->dst].is_byte;
                const char *acc = st_byte ? "AL" : "AX";
                fprintf(out_asm, "    mov %s, %s\n", acc, val_str);
                val_str = acc;
            }
        }
        fprintf(out_asm, "    push SI\n");
        fprintf(out_asm, "    mov SI, %s\n", vreg_asm(fn, ins->src2));
        idx_str = "SI"; pushed_si = true;
    } else if (ins->src2 >= 0) {
        idx_str = vreg_asm(fn, ins->src2);
    }
    bool use_es = emit_es_data_prefix(fn, ins->src1);
    const char *seg_pfx = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
    if (idx_str)
        fprintf(out_asm, "    mov [%s%s+%s], %s\n", seg_pfx, base_str, idx_str, val_str);
    else
        fprintf(out_asm, "    mov [%s%s], %s\n", seg_pfx, base_str, val_str);
    if (pushed_si) fprintf(out_asm, "    pop SI\n");
    if (pushed_bx) fprintf(out_asm, "    pop BX\n");
    emit_es_data_suffix(use_es);
    if (pushed_val_ax)
        fprintf(out_asm, "    pop AX\n");
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
    int fn_idx = -1;
    for (int fi2 = 0; fi2 < nfunctions; fi2++)
        if (&functions[fi2] == fn) { fn_idx = fi2; break; }

    if (fn->nfn_preserves > 0) {
        /* Find this function's clobber set */
        for (int i = 0; i < fn->nfn_preserves; i++) {
            int preg = fn->fn_preserves[i];
            if (fn_idx >= 0 && fn_assigns[fn_idx].clobbers[preg])
                save_regs[nsave++] = preg;
        }
    }
    bool ds_explicit_save = ds_policy_sets_ds(fn) &&
        !reg_list_contains(save_regs, nsave, PREG_DS);

    /* For interrupt handlers: compute which word registers are clobbered */
    int isr_save[8]; /* word regs to save: AX,CX,DX,BX,SP,BP,SI,DI */
    int isr_nsave = 0;
    if (fn->is_interrupt && !fn->is_bare) {
        bool word_used[8] = {0}; /* AX=0,CX=1,DX=2,BX=3,SP=4,BP=5,SI=6,DI=7 */
        for (int v = 0; v < fn->nvregs; v++) {
            int preg = fn->vregs[v].assigned;
            if (preg == PREG_NONE || fn->vregs[v].def_pos < 0) continue;
            if (preg < 4) word_used[preg] = true;           /* AX=0,CX=1,DX=2,BX=3 */
            else if (preg == PREG_BP) word_used[5] = true;
            else if (preg == PREG_SI) word_used[6] = true;
            else if (preg == PREG_DI) word_used[7] = true;
            else if (preg >= PREG_AL && preg <= PREG_BH)
                word_used[preg_alias_parent[preg]] = true;   /* byte → parent word */
        }
        if (fn->needs_frame) word_used[5] = true; /* BP */
        if (fn_idx >= 0) {
            for (int r = 0; r < 8; r++) {
                if (r == PREG_SP) continue;
                if (fn_assigns[fn_idx].clobbers[r])
                    word_used[r] = true;
            }
        }
        for (int r = 0; r < 8; r++) {
            if (r == 4) continue; /* skip SP */
            if (word_used[r])
                isr_save[isr_nsave++] = r;
        }
    }

    fprintf(out_asm, "%s:\n", asm_name);

    /* Prologue (bare functions manage their own stack) */
    if (!fn->is_bare) {
        if (fn->is_interrupt) {
            if (isr_nsave >= 6) {
                fprintf(out_asm, "    pusha\n");
            } else {
                for (int i = 0; i < isr_nsave; i++)
                    fprintf(out_asm, "    push %s\n", preg_name[isr_save[i]]);
            }
        }

        /* Callee-save pushes */
        for (int i = 0; i < nsave; i++)
            fprintf(out_asm, "    push %s\n", preg_name[save_regs[i]]);

        emit_ds_setup(fn, ds_explicit_save);

        if (fn->needs_frame) {
            fprintf(out_asm, "    push bp\n");
            fprintf(out_asm, "    mov bp, sp\n");
            if (fn->frame_size > 0)
                fprintf(out_asm, "    sub sp, %d\n", fn->frame_size);
        }

        emit_param_entry_moves(fn, fn_idx);
    }

    /* Body — iterate resolved instruction stream */
    int last_dbg_line = 0;
    for (unsigned ri = 0; ri < (unsigned)fn->nresolved; ri++) {
        /* Pre-formatted assembly from move insertion pass */
        if (fn->resolved[ri].kind == RINS_ASM) {
            fprintf(out_asm, "%s\n", fn->resolved[ri].asm_text);
            continue;
        }

        int i = fn->resolved[ri].ir_idx;
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
                /* Segment registers can't take immediates directly */
                if (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                    fn->vregs[ins->dst].is_seg) {
                    /* The compiler emits an intermediate word vreg +
                     * a mov to the seg vreg, so this path shouldn't
                     * be reached. Fallback: use AX. */
                    fprintf(out_asm, "    mov AX, %d\n", ins->imm);
                    fprintf(out_asm, "    mov %s, AX\n", d);
                } else {
                    fprintf(out_asm, "    mov %s, %d\n", d, ins->imm);
                }
            } else if (ins->name[0]) {
                /* Label reference — load address of constant or function */
                const char *d = vreg_asm(fn, ins->dst);
                const char *resolved;
                bool is_seg_op = (strncmp(ins->name, "SEG ", 4) == 0);
                if (is_seg_op) {
                    /* SEG label — resolve the label part, keep SEG prefix */
                    static char segbuf[128];
                    const char *lbl = resolve_fn_name(ins->name + 4);
                    snprintf(segbuf, sizeof(segbuf), "SEG %s", lbl);
                    resolved = segbuf;
                } else {
                    const char *clbl = resolve_const_label(fn, ins->name);
                    resolved = clbl ? clbl : resolve_fn_name(ins->name);
                }
                /* Segment registers can't take label refs — use AX */
                if (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                    fn->vregs[ins->dst].is_seg) {
                    /* Same fallback — compiler should route through word vreg */
                    fprintf(out_asm, "    mov AX, %s\n", resolved);
                    fprintf(out_asm, "    mov %s, AX\n", d);
                } else {
                    fprintf(out_asm, "    mov %s, %s\n", d, resolved);
                }
            } else {
                emit_mov(fn, ins->dst, ins->src1);
            }
            break;

        case IR_LEA: {
            int off = local_bp_offset(fn, ins->src1);
            if (is_spilled(fn, ins->dst)) {
                fprintf(out_asm, "    lea AX, [BP%+d]\n", off);
                fprintf(out_asm, "    mov %s, AX\n", vreg_asm(fn, ins->dst));
            } else {
                fprintf(out_asm, "    lea %s, [BP%+d]\n",
                        vreg_asm(fn, ins->dst), off);
            }
            break;
        }

        case IR_ALU:
            emit_alu(fn, ins, i);
            break;

        case IR_LOAD:
            emit_load(fn, ins);
            break;

        case IR_STORE:
            emit_store(fn, ins);
            break;

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
                    /* dst is SI/DI/BP or spilled — can't access byte halves.
                     * Use mov + AND to zero-extend without clobbering any
                     * byte register's sibling. */
                    int src_parent = (src_preg >= PREG_AL && src_preg <= PREG_BH)
                                     ? preg_alias_parent[src_preg] : -1;
                    if (src_parent >= 0) {
                        /* src is a byte reg — copy parent word, mask to byte */
                        fprintf(out_asm, "    mov %s, %s\n", d, preg_name[src_parent]);
                        if (src_preg == preg_alias_lo[src_parent]) {
                            fprintf(out_asm, "    and %s, 0x00FF\n", d);
                        } else {
                            /* src is high byte — shift right 8, mask */
                            fprintf(out_asm, "    shr %s, 8\n", d);
                        }
                    } else if (is_spilled(fn, ins->src1)) {
                        /* Spilled byte: load word from spill, mask to byte */
                        fprintf(out_asm, "    mov %s, %s\n", d, s);
                        fprintf(out_asm, "    and %s, 0x00FF\n", d);
                    } else {
                        /* src is a non-byte reg (shouldn't happen) */
                        fprintf(out_asm, "    mov %s, %s\n", d, s);
                        fprintf(out_asm, "    and %s, 0x00FF\n", d);
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
                fprintf(out_asm, "    %s %s\n", mnem, vreg_asm_sized(fn, ins->dst));
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

        case IR_CALL:
        case IR_MCALL: {
            /* Caller-save, argument fixup, and caller-restore are now
             * handled by the move insertion pass (insert_fixup_moves).
             * The emitter just emits the call instruction itself. */

            /* Emit the actual call instruction */
            {
                bool found_extern = false;
                for (int e = 0; e < nexterns; e++) {
                    if (strcmp(externs[e].name, ins->name) == 0) {
                        if (externs[e].has_address) {
                            fprintf(out_asm, "    call far 0x%04X:0x%04X\n",
                                    externs[e].addr_seg, externs[e].addr_off);
                            found_extern = true;
                        }
                        /* No address: fall through to function lookup
                         * (pub extern fn with body defines both) */
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
            /* Caller-restore handled by move insertion pass */
            break;
        }

        case IR_ICALL: {
            /* Indirect far call. src1 = offset vreg, src2 = seg vreg (if pair).
             * If src2 is a seg vreg, build a far pointer on the stack and call
             * through it. Otherwise src1 points to a memory-resident far32. */
            bool has_seg = (ins->src2 >= 0 && ins->src2 < MAX_VREGS &&
                            fn->vregs[ins->src2].is_seg);
            if (has_seg) {
                /* Register pair: build far pointer on stack, use BX to address it.
                 * push seg; push off; push BX; mov BX,SP; add BX,2; call far [SS:BX]
                 * After call: pop BX (restore); add SP,4 (clean far ptr) */
                fprintf(out_asm, "    push %s\n", vreg_asm(fn, ins->src2));
                fprintf(out_asm, "    push %s\n", vreg_asm(fn, ins->src1));
                fprintf(out_asm, "    push BX\n");
                fprintf(out_asm, "    mov BX, SP\n");
                fprintf(out_asm, "    add BX, 2\n");
                fprintf(out_asm, "    call far [SS:BX]\n");
                fprintf(out_asm, "    pop BX\n");
                fprintf(out_asm, "    add SP, 4\n");
            } else {
                const char *addr = vreg_asm(fn, ins->src1);
                bool use_es = emit_es_data_prefix(fn, ins->src1);
                const char *ic_seg = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
                if (is_spilled(fn, ins->src1) && vreg_data_label(fn, ins->src1)) {
                    fprintf(out_asm, "    push BX\n");
                    fprintf(out_asm, "    mov BX, %s\n", addr);
                    fprintf(out_asm, "    call far [%sBX]\n", ic_seg);
                    fprintf(out_asm, "    pop BX\n");
                } else if (is_spilled(fn, ins->src1)) {
                    fprintf(out_asm, "    call far %s\n", addr);
                } else {
                    fprintf(out_asm, "    call far [%s%s]\n", ic_seg, addr);
                }
                emit_es_data_suffix(use_es);
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
            if (!fn->is_bare) {
                emit_epilogue(fn, save_regs, nsave, isr_save, isr_nsave,
                              ds_explicit_save);
            }
            break;


        case IR_LOADMEM:
        case IR_STOREMEM: {
            if (ins->op == IR_LOADMEM) {
                bool dst_byte = (ins->dst >= 0 && ins->dst < MAX_VREGS &&
                                 fn->vregs[ins->dst].is_byte);
                if (ins->name[0]) {
                    /* Label-based: loadmem %dst, [addr_text] */
                    if (is_spilled(fn, ins->dst)) {
                        const char *acc = dst_byte ? "AL" : "AX";
                        fprintf(out_asm, "    mov %s, %s\n", acc, ins->name);
                        fprintf(out_asm, "    mov %s, %s\n", vreg_asm(fn, ins->dst), acc);
                    } else {
                        fprintf(out_asm, "    mov %s, %s\n",
                                vreg_asm(fn, ins->dst), ins->name);
                    }
                } else {
                    /* Vreg-based: loadmem %dst, %off [, %seg] */
                    bool has_seg = (ins->src2 >= 0);
                    const char *d = vreg_asm(fn, ins->dst);
                    const char *acc = dst_byte ? "AL" : "AX";
                    /* Resolve offset register — if spilled, load into BX scratch */
                    const char *off_reg;
                    bool off_spilled = is_spilled(fn, ins->src1);
                    if (off_spilled) {
                        fprintf(out_asm, "    push BX\n");
                        fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->src1));
                        off_reg = "BX";
                    } else {
                        off_reg = vreg_asm(fn, ins->src1);
                    }
                    if (has_seg) {
                        /* Far: set up ES from seg vreg, then mov dst, [ES:off] */
                        if (is_spilled(fn, ins->src2)) {
                            fprintf(out_asm, "    push AX\n");
                            fprintf(out_asm, "    mov AX, %s\n", vreg_asm(fn, ins->src2));
                            fprintf(out_asm, "    mov ES, AX\n");
                            fprintf(out_asm, "    pop AX\n");
                        }
                        if (is_spilled(fn, ins->dst)) {
                            fprintf(out_asm, "    mov %s, [ES:%s]\n", acc, off_reg);
                            fprintf(out_asm, "    mov %s, %s\n", d, acc);
                        } else {
                            fprintf(out_asm, "    mov %s, [ES:%s]\n", d, off_reg);
                        }
                    } else {
                        /* Near: mov dst, [off] or [CS:off] */
                        bool use_es = emit_es_data_prefix(fn, ins->src1);
                        const char *seg = use_es ? "ES:" : near_data_seg_prefix(fn, ins->src1);
                        if (is_spilled(fn, ins->dst)) {
                            fprintf(out_asm, "    mov %s, [%s%s]\n", acc, seg, off_reg);
                            fprintf(out_asm, "    mov %s, %s\n", d, acc);
                        } else {
                            fprintf(out_asm, "    mov %s, [%s%s]\n", d, seg, off_reg);
                        }
                        emit_es_data_suffix(use_es);
                    }
                    if (off_spilled)
                        fprintf(out_asm, "    pop BX\n");
                }
            } else {
                if (ins->name[0]) {
                    /* Label-based: storemem [addr_text], %val */
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
                } else {
                    /* Vreg-based: storemem %off [, %seg], %val */
                    bool has_seg = (ins->src2 >= 0);
                    bool val_byte = (ins->src1 >= 0 && ins->src1 < MAX_VREGS &&
                                     fn->vregs[ins->src1].is_byte);
                    /* Resolve offset register — if spilled, load into BX scratch */
                    const char *off_reg;
                    bool off_spilled = is_spilled(fn, ins->dst);
                    if (off_spilled) {
                        fprintf(out_asm, "    push BX\n");
                        fprintf(out_asm, "    mov BX, %s\n", vreg_asm(fn, ins->dst));
                        off_reg = "BX";
                    } else {
                        off_reg = vreg_asm(fn, ins->dst);
                    }
                    /* Resolve value — if spilled, load into accumulator */
                    const char *val_reg;
                    bool val_spilled = is_spilled(fn, ins->src1);
                    if (val_spilled) {
                        const char *acc = val_byte ? "AL" : "AX";
                        fprintf(out_asm, "    mov %s, %s\n", acc, vreg_asm(fn, ins->src1));
                        val_reg = acc;
                    } else {
                        val_reg = vreg_asm(fn, ins->src1);
                    }
                    if (has_seg) {
                        if (is_spilled(fn, ins->src2)) {
                            fprintf(out_asm, "    push AX\n");
                            fprintf(out_asm, "    mov AX, %s\n", vreg_asm(fn, ins->src2));
                            fprintf(out_asm, "    mov ES, AX\n");
                            fprintf(out_asm, "    pop AX\n");
                        }
                        fprintf(out_asm, "    mov [ES:%s], %s\n", off_reg, val_reg);
                    } else {
                        /* Near store, check CS: override */
                        bool use_es = emit_es_data_prefix(fn, ins->dst);
                        const char *seg = use_es ? "ES:" : near_data_seg_prefix(fn, ins->dst);
                        fprintf(out_asm, "    mov [%s%s], %s\n", seg, off_reg, val_reg);
                        emit_es_data_suffix(use_es);
                    }
                    if (off_spilled)
                        fprintf(out_asm, "    pop BX\n");
                }
            }
            break;
        }

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

        case IR_FRAME_ENTER:
            fprintf(out_asm, "    push bp\n");
            fprintf(out_asm, "    mov bp, sp\n");
            if (fn->frame_size > 0)
                fprintf(out_asm, "    sub sp, %d\n", fn->frame_size);
            break;

        case IR_FRAME_LEAVE:
            fprintf(out_asm, "    mov sp, bp\n");
            fprintf(out_asm, "    pop bp\n");
            break;

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

        case IR_BOUND: {
            /* Emit bounds check: if index >= limit, trigger INT 5 */
            const char *idx_reg = vreg_asm(fn, ins->src1);
            int limit = ins->imm;
            if (limit > 0) {
                fprintf(out_asm, "    cmp %s, %d\n", idx_reg, limit);
                fprintf(out_asm, "    jb .bound_ok_%d\n", i);
                fprintf(out_asm, "    int 0x05\n");
                fprintf(out_asm, ".bound_ok_%d:\n", i);
            }
            break;
        }

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
    if (!fn->is_bare &&
        (fn->ninsns == 0 || fn->insns[fn->ninsns-1].op != IR_RET)) {
        emit_epilogue(fn, save_regs, nsave, isr_save, isr_nsave,
                      ds_explicit_save);
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
    int ret_vregs[MAX_RETURNS];
    int nrets;
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
            if (ins->op != IR_CALL && ins->op != IR_MCALL && ins->op != IR_TAILCALL &&
                ins->op != IR_ICALL) continue;

            if (nedges >= MAX_EDGES) break;
            call_edge_t *e = &call_edges[nedges++];
            e->caller_fn = fi;
            e->callee_fn = (ins->op == IR_ICALL) ? -1 : find_fn(ins->name);
            e->insn_idx = i;
            strncpy(e->callee_name, ins->name, 63);
            e->ret_vreg = ins->dst;
            e->nrets = ins->op == IR_MCALL ? ins->nrets :
                       (ins->dst >= 0 ? 1 : 0);
            for (int ri = 0; ri < MAX_RETURNS; ri++)
                e->ret_vregs[ri] = -1;
            if (e->nrets > 0)
                e->ret_vregs[0] = ins->dst;
            if (ins->op == IR_MCALL) {
                for (int ri = 1; ri < e->nrets && ri < MAX_RETURNS; ri++)
                    e->ret_vregs[ri] = ins->ret_vregs[ri - 1];
            }
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
        fn_assigns[i].nreturns = functions[i].nreturns;
        for (int j = 0; j < MAX_RETURNS; j++)
            fn_assigns[i].return_regs[j] = PREG_NONE;
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
            for (int ri = 0; ri < call_edges[e].nrets && ri < externs[x].nreturns; ri++) {
                int rv = call_edges[e].ret_vregs[ri];
                int rr = extern_return_reg(&externs[x], ri);
                if (rv >= 0 && rr != PREG_NONE && rr != PREG_SP &&
                    caller->vregs[rv].prefer == PREG_NONE)
                    caller->vregs[rv].prefer = rr;
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
            for (int ri = 0; ri < fn->nreturns && ri < MAX_RETURNS; ri++)
                fa->return_regs[ri] = function_return_reg(fn, ri);
            fa->return_reg = fa->return_regs[0];
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

            /* Return values: set preferences on caller's dst vregs */
            for (int ri = 0; ri < call_edges[e].nrets && ri < fa->nreturns; ri++) {
                int rv = call_edges[e].ret_vregs[ri];
                int rr = fa->return_regs[ri];
                if (rv >= 0 && rr != PREG_NONE && rr != PREG_SP &&
                    caller->vregs[rv].prefer == PREG_NONE)
                    caller->vregs[rv].prefer = rr;
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
        if (fa->return_reg != PREG_NONE) {
            fprintf(stderr, " -> ");
            for (int ri = 0; ri < fa->nreturns && ri < MAX_RETURNS; ri++) {
                if (ri > 0) fprintf(stderr, ",");
                fprintf(stderr, "%s", preg_name[fa->return_regs[ri]]);
            }
        }
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
static int seg_stack[MAX_AT_DIRECTIVES];
static int seg_sp = 0;

static int place_at_depth = 0;
static int place_current_seg = -1;
static int place_seg_stack[MAX_AT_DIRECTIVES];
static int place_seg_sp = 0;
static char placed_modules[128][64];
static int nplaced_modules = 0;

static void place_push_seg(int seg) {
    if (place_seg_sp < MAX_AT_DIRECTIVES)
        place_seg_stack[place_seg_sp++] = place_current_seg;
    place_current_seg = seg;
    place_at_depth++;
}

static void place_pop_seg(void) {
    if (place_at_depth > 0) place_at_depth--;
    if (place_seg_sp > 0)
        place_current_seg = place_seg_stack[--place_seg_sp];
}

static void place_item(int ei) {
    int kind = emit_order[ei].kind;
    int idx = emit_order[ei].index;

    if (kind == EMIT_FN) {
        functions[idx].emit_seg = functions[idx].has_at
            ? functions[idx].at_seg : place_current_seg;
    } else if (kind == EMIT_DATA) {
        data_block_t *db = &data_blocks[idx];
        if (db->has_at) {
            place_push_seg(db->at_seg);
            db->emit_seg = db->at_seg;
            db->has_emit_seg = true;
        } else if (place_current_seg >= 0) {
            db->emit_seg = place_current_seg;
            db->has_emit_seg = true;
        } else {
            fprintf(stderr, "%s: initialized data requires explicit at() placement\n",
                    db->label);
            bind_errors++;
        }
    } else if (kind == EMIT_GLOB) {
        global_var_t *g = &globals[idx];
        if (g->has_at)
            place_push_seg(g->at_seg);
    } else if (kind == EMIT_AT) {
        place_push_seg(at_directives[idx].seg);
    } else if (kind == EMIT_ENDAT) {
        place_pop_seg();
    }
}

static void place_module(const char *path) {
    char mod[64];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(mod, base, 63);
    char *dot = strrchr(mod, '.');
    if (dot) *dot = '\0';

    for (int i = 0; i < nplaced_modules; i++)
        if (strcmp(placed_modules[i], mod) == 0) return;
    strncpy(placed_modules[nplaced_modules++], mod, 63);

    for (int ei = 0; ei < nemit_order; ei++) {
        if (strcmp(emit_order[ei].module, mod) != 0) continue;
        if (emit_order[ei].kind == EMIT_USE) {
            char use_path[128];
            const char *use_mod = use_modules[emit_order[ei].index];
            const char *slash = strrchr(path, '/');
            if (slash) {
                int dirlen = (int)(slash - path) + 1;
                snprintf(use_path, sizeof(use_path), "%.*s%s.nir",
                         dirlen, path, use_mod);
            } else {
                snprintf(use_path, sizeof(use_path), "%s.nir", use_mod);
            }
            int saved_depth = place_at_depth;
            place_module(use_path);
            while (place_at_depth > saved_depth)
                place_pop_seg();
        } else {
            place_item(ei);
        }
    }
}

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
            if (seg_sp < MAX_AT_DIRECTIVES)
                seg_stack[seg_sp++] = current_seg;
            fprintf(out_asm, "    org 0x%05X ; %04X:%04X\n",
                    lin, db->at_seg, db->at_off);
            fprintf(out_asm, "    seg 0x%04X\n", db->at_seg);
            current_seg = db->at_seg;
            at_depth++;
        } else if (db->has_emit_seg) {
            fprintf(out_asm, "    seg 0x%04X\n", db->emit_seg);
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
            if (seg_sp < MAX_AT_DIRECTIVES)
                seg_stack[seg_sp++] = current_seg;
            fprintf(out_asm, "\n    org 0x%05X ; %04X:%04X\n",
                    lin, g->at_seg, g->at_off);
            fprintf(out_asm, "    seg 0x%04X\n", g->at_seg);
            current_seg = g->at_seg;
            at_depth++;
        }
        fprintf(out_asm, "%s:", g->name);
        if (g->size == 1)
            fprintf(out_asm, " db 0\n");
        else if (g->size == 2)
            fprintf(out_asm, " dw 0\n");
        else {
            fprintf(out_asm, "\n");
            int b = 0;
            for (; b + 1 < g->size; b += 2)
                fprintf(out_asm, "    dw 0\n");
            if (b < g->size)
                fprintf(out_asm, "    db 0\n");
        }
    } else if (kind == EMIT_AT) {
        int s = at_directives[idx].seg;
        int o = at_directives[idx].off;
        int lin = s * 16 + o;
        if (seg_sp < MAX_AT_DIRECTIVES)
            seg_stack[seg_sp++] = current_seg;
        fprintf(out_asm, "\n    org 0x%05X ; %04X:%04X\n", lin, s, o);
        fprintf(out_asm, "    seg 0x%04X\n", s);
        current_seg = s;
        at_depth++;
    } else if (kind == EMIT_ENDAT) {
        fprintf(out_asm, "    endorg\n");
        if (at_depth > 0) at_depth--;
        if (seg_sp > 0)
            current_seg = seg_stack[--seg_sp];
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
                if (seg_sp > 0)
                    current_seg = seg_stack[--seg_sp];
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

    nplaced_modules = 0;
    place_current_seg = -1;
    place_at_depth = 0;
    place_seg_sp = 0;
    place_module(inputs[ninputs - 1]);
    if (bind_errors > 0)
        return 1;

    annotate_data_refs();
    validate_ds_policies();
    if (bind_errors > 0)
        return 1;

    /* Inter-procedural register propagation */
    build_call_graph();
    topo_sort();
    fprintf(stderr, "Topo order:");
    for (int i = 0; i < ntopo; i++)
        fprintf(stderr, " %s", functions[topo_order[i]].name);
    fprintf(stderr, "\n");
    propagate_preferences();

    /* For each function: liveness and allocation. */
    for (int i = 0; i < nfunctions; i++) {
        func_t *fn = &functions[i];

        /* Phase 1: build CFG, compute liveness, build interference graph */
        build_cfg(fn);
        compute_cfg_liveness(fn);
        compute_liveness(fn);  /* derives def_pos/last_use from CFG */
        build_igraph(fn);
        scan_addressing_constraints(fn);

        /* Phase 2: estimate register pressure from interference graph.
         * If any vreg's degree exceeds its pool size, spills are likely
         * and we should reserve BP for the frame pointer upfront. */
        bool bp_available = true;
        if (fn->local_size > 0) {
            bp_available = false;
            fn->needs_frame = true;
        }
        for (int v = 0; v < fn->nvregs; v++) {
            if (fn->vregs[v].is_local_slot)
                continue;
            if (fn->vregs[v].def_pos < 0 && fn->vregs[v].last_use < 0)
                continue;
            int pool[16];
            int psz = get_vreg_pool(fn, v, true, pool);
            if (fn->degree[v] >= psz) {
                bp_available = false;
                fn->needs_frame = true;
                break;
            }
        }

        allocate_registers(fn, bp_available);

        /* If spills occurred despite no pressure estimate, reserve BP */
        if (fn->nspill_slots > 0 && bp_available) {
            fn->needs_frame = true;
            for (int v = 0; v < fn->nvregs; v++) {
                fn->vregs[v].assigned = PREG_NONE;
                fn->vregs[v].spill_slot = -1;
            }
            fn->nspill_slots = 0;
            allocate_registers(fn, false);
        }
        fn->frame_size = fn->nspill_slots * 2 + fn->local_size;

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
    }

    /* Compute clobber sets after every function has been allocated.
     * Process leaves first so caller clobbers include callee clobbers. */
    for (int ti = 0; ti < ntopo; ti++) {
        int fi = topo_order[ti];
        func_t *fn = &functions[fi];
        fn_assignment_t *fa = &fn_assigns[fi];

        memset(fa->clobbers, 0, sizeof(fa->clobbers));
        for (int ii = 0; ii < fn->ninsns; ii++)
            collect_insn_clobbers(fn, &fn->insns[ii], fa->clobbers);

        for (int ii = 0; ii < fn->ninsns; ii++) {
            if (fn->insns[ii].op != IR_CALL && fn->insns[ii].op != IR_MCALL) continue;
            for (int fi2 = 0; fi2 < nfunctions; fi2++) {
                if (strcmp(functions[fi2].name, fn->insns[ii].name) == 0 &&
                    fn_assigns[fi2].resolved) {
                    for (int r = 0; r < NUM_PREGS; r++)
                        if (fn_assigns[fi2].clobbers[r])
                            fa->clobbers[r] = true;
                    break;
                }
            }
            for (int e = 0; e < nexterns; e++) {
                if (strcmp(externs[e].name, fn->insns[ii].name) == 0) {
                    for (int r = 0; r < NUM_PREGS; r++)
                        fa->clobbers[r] = true;
                    for (int pp = 0; pp < externs[e].npreserves; pp++)
                        fa->clobbers[externs[e].preserves[pp]] = false;
                    if (externs[e].ds_policy != DS_POLICY_UNSPEC)
                        fa->clobbers[PREG_DS] = false;
                    break;
                }
            }
        }
        if (fn->ds_policy != DS_POLICY_UNSPEC)
            fa->clobbers[PREG_DS] = false;
    }

    /* Build resolved instruction streams now that call fixups can see
     * complete callee clobber sets. */
    for (int i = 0; i < nfunctions; i++) {
        func_t *fn = &functions[i];
        insert_fixup_moves(fn);

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
    out_asm = fopen(outpath, "w");
    if (!out_asm) { perror(outpath); return 1; }

    fprintf(out_asm, "; Nib assembly — generated by nib bind\n");

    ndone_modules = 0;
    emit_module(inputs[ninputs - 1]);

    fclose(out_asm);
    fprintf(stderr, "Wrote %s\n", outpath);
    return 0;
}
