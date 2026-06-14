/*
 * build.c — nib build driver
 *
 * Follows use declarations from a root .nib file, compiles changed
 * files, binds all .nir files, and assembles the result.
 *
 * Usage: nibbuild [-f] [-o output.bin] main.nib
 *                 [--nib ARGS...] [--asm ARGS...] [--bind ARGS...]
 *
 * Or with a nib.build file in the current directory:
 *        nibbuild
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <spawn.h>

extern char **environ;

#define MAX_MODULES 128
#define MAX_EXTRA_ARGS 32
#define MAX_ARGV 256

typedef struct {
    char nib_path[256];     /* source file */
    char nir_path[256];     /* IR output */
    char nif_path[256];     /* interface output */
    char dir[256];          /* directory of the .nib file */
    bool needs_compile;
    int  deps[MAX_MODULES]; /* indices of modules this one depends on */
    int  ndeps;
} module_t;

static module_t modules[MAX_MODULES];
static int nmodules = 0;

/* Per-stage extra arguments */
static char *nib_extra[MAX_EXTRA_ARGS];
static int   nib_nextra = 0;
static char *bind_extra[MAX_EXTRA_ARGS];
static int   bind_nextra = 0;
static char *asm_extra[MAX_EXTRA_ARGS];
static int   asm_nextra = 0;

/* ---- Path helpers ---- */

static void replace_ext(const char *path, const char *ext, char *out, int outsz) {
    const char *dot = strrchr(path, '.');
    int baselen = dot ? (int)(dot - path) : (int)strlen(path);
    snprintf(out, outsz, "%.*s%s", baselen, path, ext);
}

static void get_dir(const char *path, char *out, int outsz) {
    char tmp[256];
    strncpy(tmp, path, 255);
    tmp[255] = '\0';
    char *d = dirname(tmp);
    strncpy(out, d, outsz - 1);
    out[outsz - 1] = '\0';
}

static void resolve_path(const char *base_dir, const char *rel_path, char *out, int outsz) {
    if (rel_path[0] == '/') {
        strncpy(out, rel_path, outsz - 1);
    } else {
        snprintf(out, outsz, "%s/%s", base_dir, rel_path);
    }
    out[outsz - 1] = '\0';
}

static time_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return st.st_mtime;
}

/* ---- Process execution ---- */

static char tool_dir[256] = ".";

static void resolve_tool_dir(const char *argv0) {
    strncpy(tool_dir, argv0, sizeof(tool_dir) - 1);
    char *slash = strrchr(tool_dir, '/');
    if (slash)
        *slash = '\0';
    else
        strcpy(tool_dir, ".");
}

static void tool_path(const char *name, char *out, int outsz) {
    snprintf(out, outsz, "%s/%s", tool_dir, name);
}

static int run_argv(char **argv) {
    /* Print command for visibility */
    fprintf(stderr, " ");
    for (int i = 0; argv[i]; i++)
        fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");

    pid_t pid;
    int status = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (status != 0) {
        fprintf(stderr, "error: failed to spawn %s: %s\n", argv[0], strerror(status));
        return 1;
    }
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

/* ---- Module discovery ---- */

static int find_module(const char *nib_path) {
    for (int i = 0; i < nmodules; i++)
        if (strcmp(modules[i].nib_path, nib_path) == 0)
            return i;
    return -1;
}

static int add_module(const char *nib_path);

static void scan_uses(int mod_idx) {
    module_t *m = &modules[mod_idx];

    FILE *fp = fopen(m->nib_path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", m->nib_path);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "use ", 4) != 0) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') continue;
        p++;
        char *end = strchr(p, '"');
        if (!end) continue;

        int len = (int)(end - p);
        char nif_path[256];
        memcpy(nif_path, p, len);
        nif_path[len] = '\0';

        char nib_dep[256];
        replace_ext(nif_path, ".nib", nib_dep, sizeof(nib_dep));

        char resolved[256];
        resolve_path(m->dir, nib_dep, resolved, sizeof(resolved));

        struct stat st;
        if (stat(resolved, &st) < 0) {
            strncpy(resolved, nib_dep, sizeof(resolved));
            if (stat(resolved, &st) < 0) {
                fprintf(stderr, "warning: cannot find source for '%s'\n", nif_path);
                continue;
            }
        }

        int dep = add_module(resolved);
        if (dep >= 0 && m->ndeps < MAX_MODULES) {
            m->deps[m->ndeps++] = dep;
        }
    }

    fclose(fp);
}

static int add_module(const char *nib_path) {
    int idx = find_module(nib_path);
    if (idx >= 0) return idx;

    if (nmodules >= MAX_MODULES) {
        fprintf(stderr, "error: too many modules\n");
        return -1;
    }

    idx = nmodules++;
    module_t *m = &modules[idx];
    strncpy(m->nib_path, nib_path, 255);
    replace_ext(nib_path, ".nir", m->nir_path, sizeof(m->nir_path));
    replace_ext(nib_path, ".nif", m->nif_path, sizeof(m->nif_path));
    get_dir(nib_path, m->dir, sizeof(m->dir));
    m->ndeps = 0;

    scan_uses(idx);

    return idx;
}

/* ---- Topological sort ---- */

static int build_order[MAX_MODULES];
static int nbuild;

static void topo_sort(void) {
    bool visited[MAX_MODULES] = {0};
    nbuild = 0;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < nmodules; i++) {
            if (visited[i]) continue;
            bool ready = true;
            for (int d = 0; d < modules[i].ndeps; d++) {
                if (!visited[modules[i].deps[d]]) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                build_order[nbuild++] = i;
                visited[i] = true;
                changed = true;
            }
        }
    }

    for (int i = 0; i < nmodules; i++) {
        if (!visited[i]) {
            fprintf(stderr, "error: dependency cycle involving '%s'\n",
                    modules[i].nib_path);
            build_order[nbuild++] = i;
        }
    }
}

/* ---- Build file (nib.build) ---- */

static void add_extra(char **arr, int *cnt, const char *arg) {
    if (*cnt < MAX_EXTRA_ARGS)
        arr[(*cnt)++] = strdup(arg);
}

/* Tokenize a value string and append tokens to an extra array.
 * Splits on whitespace, respects double quotes. */
static void tokenize_into(char **arr, int *cnt, const char *value) {
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char token[256];
        int ti = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && ti < 255)
                token[ti++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && ti < 255)
                token[ti++] = *p++;
        }
        token[ti] = '\0';
        if (ti > 0)
            add_extra(arr, cnt, token);
    }
}

static bool load_build_file(const char *path, const char **root_out, const char **output_out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    fprintf(stderr, "Using build file: %s\n", path);

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Skip comments and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        /* Parse key */
        char key[64];
        int ki = 0;
        while (*p && *p != ' ' && *p != '\t' && ki < 63)
            key[ki++] = *p++;
        key[ki] = '\0';

        /* Skip whitespace to value */
        while (*p == ' ' || *p == '\t') p++;

        if (strcmp(key, "root") == 0) {
            *root_out = strdup(p);
        } else if (strcmp(key, "output") == 0) {
            *output_out = strdup(p);
        } else if (strcmp(key, "nib") == 0) {
            tokenize_into(nib_extra, &nib_nextra, p);
        } else if (strcmp(key, "asm") == 0) {
            tokenize_into(asm_extra, &asm_nextra, p);
        } else if (strcmp(key, "bind") == 0) {
            tokenize_into(bind_extra, &bind_nextra, p);
        } else {
            fprintf(stderr, "warning: unknown build file key '%s'\n", key);
        }
    }

    fclose(fp);
    return true;
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    const char *root = NULL;
    const char *outbin = NULL;
    bool force_rebuild = false;

    resolve_tool_dir(argv[0]);

    /* Load build file (if present) before CLI parsing */
    load_build_file("nib.build", &root, &outbin);

    /* Parse CLI arguments with section markers */
    int section = 0;  /* 0=nibbuild, 1=nib, 2=bind, 3=asm */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nib") == 0) {
            section = 1;
        } else if (strcmp(argv[i], "--bind") == 0) {
            section = 2;
        } else if (strcmp(argv[i], "--asm") == 0) {
            section = 3;
        } else if (section == 0) {
            /* nibbuild's own flags */
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                outbin = argv[++i];
            else if (strcmp(argv[i], "-f") == 0)
                force_rebuild = true;
            else
                root = argv[i];
        } else if (section == 1) {
            add_extra(nib_extra, &nib_nextra, argv[i]);
        } else if (section == 2) {
            add_extra(bind_extra, &bind_nextra, argv[i]);
        } else if (section == 3) {
            add_extra(asm_extra, &asm_nextra, argv[i]);
        }
    }

    if (!root) {
        fprintf(stderr,
            "usage: nibbuild [-f] [-o output.bin] main.nib\n"
            "                [--nib ARGS...] [--asm ARGS...] [--bind ARGS...]\n"
            "\n"
            "Or create a nib.build file in the current directory.\n");
        return 1;
    }

    /* Derive default output name from root */
    char default_out[256];
    if (!outbin) {
        replace_ext(root, ".bin", default_out, sizeof(default_out));
        outbin = default_out;
    }

    /* Discover all modules */
    fprintf(stderr, "Scanning dependencies...\n");
    add_module(root);

    /* Sort */
    topo_sort();

    fprintf(stderr, "Build order (%d modules):\n", nbuild);
    for (int i = 0; i < nbuild; i++)
        fprintf(stderr, "  %s\n", modules[build_order[i]].nib_path);

    /* Determine what needs recompilation */
    for (int i = 0; i < nmodules; i++) {
        module_t *m = &modules[i];
        time_t src_time = file_mtime(m->nib_path);
        time_t nir_time = file_mtime(m->nir_path);
        time_t nif_time = file_mtime(m->nif_path);

        if (src_time == 0) {
            fprintf(stderr, "error: cannot stat '%s'\n", m->nib_path);
            return 1;
        }

        m->needs_compile = force_rebuild ||
                            (nir_time == 0 || nif_time == 0 ||
                            src_time > nir_time || src_time > nif_time);

        for (int d = 0; d < m->ndeps; d++) {
            time_t dep_nif = file_mtime(modules[m->deps[d]].nif_path);
            if (dep_nif > nir_time)
                m->needs_compile = true;
        }
    }

    /* Phase 1: Compile */
    int errors = 0;
    char nib_tool[256];
    tool_path("nib", nib_tool, sizeof(nib_tool));

    for (int i = 0; i < nbuild; i++) {
        module_t *m = &modules[build_order[i]];
        if (!m->needs_compile) {
            fprintf(stderr, "  [skip] %s (up to date)\n", m->nib_path);
            continue;
        }

        char *av[MAX_ARGV];
        int ac = 0;
        av[ac++] = nib_tool;
        av[ac++] = m->nib_path;
        for (int j = 0; j < nib_nextra; j++)
            av[ac++] = nib_extra[j];
        av[ac] = NULL;

        if (run_argv(av) != 0) {
            fprintf(stderr, "error: compilation failed for '%s'\n", m->nib_path);
            errors++;
        }
    }

    if (errors > 0) return 1;

    /* Phase 2: Bind all .nir files */
    char bind_tool[256];
    tool_path("nibbind", bind_tool, sizeof(bind_tool));
    char asm_path[256];
    replace_ext(root, ".asm", asm_path, sizeof(asm_path));

    char *bav[MAX_ARGV];
    int bac = 0;
    bav[bac++] = bind_tool;
    for (int i = 0; i < nbuild; i++)
        bav[bac++] = modules[build_order[i]].nir_path;
    for (int j = 0; j < bind_nextra; j++)
        bav[bac++] = bind_extra[j];
    bav[bac++] = "-o";
    bav[bac++] = asm_path;
    bav[bac] = NULL;

    if (run_argv(bav) != 0) {
        fprintf(stderr, "error: binding failed\n");
        return 1;
    }

    /* Phase 3: Assemble */
    char asm_tool[256];
    tool_path("nibasm", asm_tool, sizeof(asm_tool));

    char *aav[MAX_ARGV];
    int aac = 0;
    aav[aac++] = asm_tool;
    aav[aac++] = asm_path;
    aav[aac++] = "-o";
    aav[aac++] = (char *)outbin;
    for (int j = 0; j < asm_nextra; j++)
        aav[aac++] = asm_extra[j];
    aav[aac] = NULL;

    if (run_argv(aav) != 0) {
        fprintf(stderr, "error: assembly failed\n");
        return 1;
    }

    fprintf(stderr, "Build complete: %s\n", outbin);
    return 0;
}
