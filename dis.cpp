/*
 * nib dis — standalone V20 disassembler
 *
 * Wraps the dis86 disassembler from dreamulator as a command-line tool.
 * Reads a flat binary and outputs Intel-syntax assembly.
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include "table.h"

/* Pull in the disassembler directly */
#include "dis86.cpp"

/* ---- Map file support ---- */

struct map_entry {
    uint32_t addr;
    char     type[8];   /* "code", "data", "equ" */
    char     name[64];
};

typedef NIB_VEC(map_entry) map_entry_vec_t;
static map_entry_vec_t map_vec;
#define map (map_vec.items)
#define nmap (map_vec.len)

static map_entry *push_map_entry(void) {
    if (map_vec.len >= map_vec.cap) {
        size_t new_cap = nib_grow_capacity((size_t)map_vec.cap,
                                           (size_t)map_vec.len + 1);
        map_vec.items = static_cast<map_entry *>(
            nib_xrealloc_array(map_vec.items, new_cap,
                               sizeof(map_vec.items[0]),
                               "disassembler map entries"));
        map_vec.cap = (int)new_cap;
    }
    return &map_vec.items[map_vec.len++];
}

static void load_map(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        map_entry *e = push_map_entry();
        unsigned int addr;
        if (sscanf(line, "%x %7s %63s", &addr, e->type, e->name) >= 3) {
            e->addr = addr;
        } else {
            map_vec.len--;
        }
    }
    fclose(fp);
}

static const map_entry *find_map(uint32_t addr) {
    for (int i = 0; i < nmap; i++)
        if (map[i].addr == addr)
            return &map[i];
    return NULL;
}

static const map_entry *find_map_by_name(const char *name) {
    for (int i = 0; i < nmap; i++)
        if (strcmp(map[i].name, name) == 0)
            return &map[i];
    return NULL;
}

/* Find the next map entry at or after addr */
static const map_entry *next_map_any(uint32_t addr) {
    const map_entry *best = NULL;
    for (int i = 0; i < nmap; i++) {
        if (map[i].addr >= addr && strcmp(map[i].type, "equ") != 0) {
            if (!best || map[i].addr < best->addr)
                best = &map[i];
        }
    }
    return best;
}

/* Check if addr falls inside a data region */
static bool in_data_region(uint32_t addr) {
    const map_entry *covering = NULL;
    for (int i = 0; i < nmap; i++) {
        if (map[i].addr <= addr && strcmp(map[i].type, "equ") != 0) {
            if (!covering || map[i].addr > covering->addr)
                covering = &map[i];
        }
    }
    return covering && strcmp(covering->type, "data") == 0;
}

/* ---- Debug info support ---- */

struct dbg_entry {
    uint32_t addr;
    char     file[64];
    int      line;
};

typedef NIB_VEC(dbg_entry) dbg_entry_vec_t;
static dbg_entry_vec_t dbg_vec;
#define dbg (dbg_vec.items)
#define ndbg (dbg_vec.len)

static dbg_entry *push_dbg_entry(void) {
    if (dbg_vec.len >= dbg_vec.cap) {
        size_t new_cap = nib_grow_capacity((size_t)dbg_vec.cap,
                                           (size_t)dbg_vec.len + 1);
        dbg_vec.items = static_cast<dbg_entry *>(
            nib_xrealloc_array(dbg_vec.items, new_cap,
                               sizeof(dbg_vec.items[0]),
                               "disassembler debug entries"));
        dbg_vec.cap = (int)new_cap;
    }
    return &dbg_vec.items[dbg_vec.len++];
}

static void load_dbg(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        dbg_entry *e = push_dbg_entry();
        unsigned int addr;
        if (sscanf(line, "%x %63[^:]:%d", &addr, e->file, &e->line) >= 3) {
            e->addr = addr;
        } else {
            dbg_vec.len--;
        }
    }
    fclose(fp);
}

static const dbg_entry *find_dbg(uint32_t addr) {
    for (int i = 0; i < ndbg; i++)
        if (dbg[i].addr == addr)
            return &dbg[i];
    return NULL;
}

/* ---- Main ---- */

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [options] file.bin\n", argv0);
    fprintf(stderr, "  -o org      set origin address (default 0x0000)\n");
    fprintf(stderr, "  -b bytes    disassemble N bytes\n");
    fprintf(stderr, "  -s offset   start at file offset\n");
    fprintf(stderr, "  -a addr     start at linear address\n");
    fprintf(stderr, "  -l label    disassemble function/label (requires -m)\n");
    fprintf(stderr, "  -m mapfile  load label/data map\n");
    fprintf(stderr, "  -d dbgfile  load source debug info\n");
}

int main(int argc, char **argv) {
    const char *infile = NULL;
    const char *mappath = NULL;
    const char *dbgpath = NULL;
    const char *label = NULL;
    uint32_t org = 0;
    int max_bytes = -1;
    long start_offset = -1;
    long start_addr = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            org = (uint32_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            max_bytes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            start_offset = strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            start_addr = strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mappath = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dbgpath = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            infile = argv[i];
        }
    }

    if (!infile) {
        usage(argv[0]);
        return 1;
    }

    if (mappath) load_map(mappath);
    if (dbgpath) load_dbg(dbgpath);

    /* Resolve -l label to start address and byte count */
    if (label) {
        const map_entry *me = find_map_by_name(label);
        if (!me) {
            fprintf(stderr, "error: label '%s' not found in map\n", label);
            return 1;
        }
        start_addr = me->addr;
        /* Find the next label to determine range */
        const map_entry *next = next_map_any(me->addr + 1);
        if (next && max_bytes < 0)
            max_bytes = next->addr - me->addr;
        else if (max_bytes < 0)
            max_bytes = 256;  /* default if no next label */
    }

    FILE *fp = fopen(infile, "rb");
    if (!fp) { perror(infile); return 1; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *code = (uint8_t *)malloc(fsize);
    fread(code, 1, fsize, fp);
    fclose(fp);

    /* Determine starting position */
    long pos;
    uint32_t ip;

    if (start_addr >= 0) {
        /* -a addr: start at linear address (file offset = addr for flat binary) */
        pos = start_addr;
        ip = (uint32_t)start_addr;
    } else if (start_offset >= 0) {
        /* -s offset: start at file offset, address = org + offset */
        pos = start_offset;
        ip = org + (uint32_t)start_offset;
    } else {
        pos = 0;
        ip = org;
    }

    long end = (max_bytes >= 0) ? pos + max_bytes : fsize;
    if (end > fsize) end = fsize;

    while (pos < end) {
        /* Check for label at this address */
        const map_entry *me = find_map(ip);
        if (me) {
            printf("\n%s:\n", me->name);
        }

        /* Show source location if debug info available */
        const dbg_entry *de = find_dbg(ip);
        if (de) {
            printf("                    ; %s:%d\n", de->file, de->line);
        }

        /* If we're in a data region, emit DB/DW instead of disassembling */
        if (nmap > 0 && in_data_region(ip)) {
            const map_entry *next = next_map_any(ip + 1);
            long data_end = next ? (long)next->addr - (long)(ip - pos) : end;
            /* Convert address-based end to pos-based end */
            data_end = next ? (pos + (next->addr - ip)) : end;
            if (data_end > end) data_end = end;

            printf("%05X  ", ip);
            int count = 0;
            while (pos < data_end) {
                uint8_t b = code[pos];
                if (count == 0)
                    printf("DB ");
                else
                    printf(", ");
                if (b >= 0x20 && b < 0x7F)
                    printf("'%c'", b);
                else
                    printf("%02Xh", b);
                pos++;
                ip++;
                count++;
                if (count >= 8 && pos < data_end) {
                    printf("\n%05X  ", ip);
                    count = 0;
                }
            }
            printf("\n");
            continue;
        }

        char buf[256];
        int nbytes = dis86(code + pos, (int)(end - pos), (uint16_t)(ip & 0xFFFF), buf, sizeof(buf));
        if (nbytes <= 0) {
            printf("%05X  %02X            DB %02Xh\n", ip, code[pos], code[pos]);
            pos++;
            ip++;
            continue;
        }

        printf("%05X  ", ip);
        for (int i = 0; i < nbytes && i < 6; i++)
            printf("%02X", code[pos + i]);
        for (int i = nbytes; i < 6; i++)
            printf("  ");
        printf("  %s\n", buf);

        pos += nbytes;
        ip += nbytes;
    }

    /* Print EQU labels */
    if (nmap > 0) {
        bool first = true;
        for (int i = 0; i < nmap; i++) {
            if (strcmp(map[i].type, "equ") == 0) {
                if (first) { printf("\n; Constants\n"); first = false; }
                printf("%-20s EQU %05Xh\n", map[i].name, map[i].addr);
            }
        }
    }

    free(code);
    return 0;
}
