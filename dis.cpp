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

/* Pull in the disassembler directly */
#include "../dreamulator/src/dis86.cpp"

/* ---- Map file support ---- */

#define MAX_MAP_ENTRIES 4096

struct map_entry {
    uint16_t addr;
    char     type[8];   /* "code", "data", "equ" */
    char     name[64];
};

static map_entry map[MAX_MAP_ENTRIES];
static int nmap = 0;

static void load_map(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (nmap >= MAX_MAP_ENTRIES) break;
        map_entry *e = &map[nmap];
        if (sscanf(line, "%hx %7s %63s", &e->addr, e->type, e->name) >= 3) {
            nmap++;
        }
    }
    fclose(fp);
}

static const map_entry *find_map(uint16_t addr) {
    for (int i = 0; i < nmap; i++)
        if (map[i].addr == addr)
            return &map[i];
    return NULL;
}

/* Find the next map entry at or after addr, optionally filtered by type */
static const map_entry *next_map_any(uint16_t addr) {
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
static bool in_data_region(uint16_t addr) {
    /* Find the map entry at or before this address */
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

#define MAX_DBG_ENTRIES 8192

struct dbg_entry {
    uint32_t addr;
    char     file[64];
    int      line;
};

static dbg_entry dbg[MAX_DBG_ENTRIES];
static int ndbg = 0;

static void load_dbg(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (ndbg >= MAX_DBG_ENTRIES) break;
        dbg_entry *e = &dbg[ndbg];
        unsigned int addr;
        if (sscanf(line, "%x %63[^:]:%d", &addr, e->file, &e->line) >= 3) {
            e->addr = addr;
            ndbg++;
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
    fprintf(stderr, "usage: %s [-o org] [-b bytes] [-m mapfile] [-d dbgfile] file.bin\n", argv0);
    fprintf(stderr, "  -o org      set origin address (default 0x0000)\n");
    fprintf(stderr, "  -b bytes    disassemble only first N bytes\n");
    fprintf(stderr, "  -m mapfile  load label/data map\n");
    fprintf(stderr, "  -d dbgfile  load source debug info\n");
}

int main(int argc, char **argv) {
    const char *infile = NULL;
    const char *mappath = NULL;
    const char *dbgpath = NULL;
    uint16_t org = 0;
    int max_bytes = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            org = (uint16_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            max_bytes = atoi(argv[++i]);
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

    FILE *fp = fopen(infile, "rb");
    if (!fp) { perror(infile); return 1; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *code = (uint8_t *)malloc(fsize);
    fread(code, 1, fsize, fp);
    fclose(fp);

    int len = (max_bytes >= 0 && max_bytes < fsize) ? max_bytes : (int)fsize;
    int pos = 0;
    uint16_t ip = org;

    while (pos < len) {
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
            /* Find end of this data region */
            const map_entry *next = next_map_any(ip + 1);
            int data_end = next ? (next->addr - org) : len;
            if (data_end > len) data_end = len;

            /* Emit as DB */
            printf("%04X  ", ip);
            int count = 0;
            while (pos < data_end) {
                uint8_t b = code[pos];
                if (count == 0)
                    printf("DB ");
                else
                    printf(", ");
                /* Check if printable ASCII */
                if (b >= 0x20 && b < 0x7F)
                    printf("'%c'", b);
                else
                    printf("%02Xh", b);
                pos++;
                ip++;
                count++;
                if (count >= 8 && pos < data_end) {
                    printf("\n%04X  ", ip);
                    count = 0;
                }
            }
            printf("\n");
            continue;
        }

        char buf[256];
        int nbytes = dis86(code + pos, len - pos, ip, buf, sizeof(buf));
        if (nbytes <= 0) {
            printf("%04X  %02X            DB %02Xh\n", ip, code[pos], code[pos]);
            pos++;
            ip++;
            continue;
        }

        /* Print address and hex bytes */
        printf("%04X  ", ip);
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
                printf("%-20s EQU %04Xh\n", map[i].name, map[i].addr);
            }
        }
    }

    free(code);
    return 0;
}
