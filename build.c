/*
 * build.c — nib build driver
 *
 * Follows use declarations from a root .nib file, compiles changed
 * files, binds all .nir files, and assembles the result.
 *
 * Usage: nibbuild main.nib [-o output.bin]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <libgen.h>

#define MAX_MODULES 128

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

/* ---- Path helpers ---- */

static void replace_ext(const char *path, const char *ext, char *out, int outsz) {
    const char *dot = strrchr(path, '.');
    int baselen = dot ? (int)(dot - path) : (int)strlen(path);
    snprintf(out, outsz, "%.*s%s", baselen, path, ext);
}

static void get_dir(const char *path, char *out, int outsz) {
    /* Copy path and use dirname */
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

/* ---- Module discovery ---- */

static int find_module(const char *nib_path) {
    for (int i = 0; i < nmodules; i++)
        if (strcmp(modules[i].nib_path, nib_path) == 0)
            return i;
    return -1;
}

/* Scan a .nib file for use declarations and add dependencies recursively */
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
        /* Look for: use "path"; */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "use ", 4) != 0) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') continue;
        p++;
        char *end = strchr(p, '"');
        if (!end) continue;

        /* Extract the .nif path */
        int len = (int)(end - p);
        char nif_path[256];
        memcpy(nif_path, p, len);
        nif_path[len] = '\0';

        /* Derive the .nib path from the .nif path */
        char nib_dep[256];
        replace_ext(nif_path, ".nib", nib_dep, sizeof(nib_dep));

        /* Resolve relative to the current module's directory */
        char resolved[256];
        resolve_path(m->dir, nib_dep, resolved, sizeof(resolved));

        /* Check if the .nib file exists */
        struct stat st;
        if (stat(resolved, &st) < 0) {
            /* Maybe the use path is already absolute or relative to cwd */
            strncpy(resolved, nib_dep, sizeof(resolved));
            if (stat(resolved, &st) < 0) {
                fprintf(stderr, "warning: cannot find source for '%s'\n", nif_path);
                continue;
            }
        }

        /* Add as dependency */
        int dep = add_module(resolved);
        if (dep >= 0 && m->ndeps < MAX_MODULES) {
            m->deps[m->ndeps++] = dep;
        }
    }

    fclose(fp);
}

static int add_module(const char *nib_path) {
    /* Already known? */
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

    /* Scan for dependencies */
    scan_uses(idx);

    return idx;
}

/* ---- Topological sort ---- */

static int build_order[MAX_MODULES];
static int nbuild;

static void topo_sort(void) {
    bool visited[MAX_MODULES] = {0};
    nbuild = 0;

    /* DFS post-order */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < nmodules; i++) {
            if (visited[i]) continue;
            /* Check if all deps are visited */
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

    /* Anything left is a cycle */
    for (int i = 0; i < nmodules; i++) {
        if (!visited[i]) {
            fprintf(stderr, "error: dependency cycle involving '%s'\n",
                    modules[i].nib_path);
            build_order[nbuild++] = i;
        }
    }
}

/* ---- Build execution ---- */

static int run_cmd(const char *cmd) {
    fprintf(stderr, "  %s\n", cmd);
    return system(cmd);
}

/* Resolve tool path relative to nibbuild's directory */
static char tool_dir[256] = ".";

static void resolve_tool_dir(const char *argv0) {
    strncpy(tool_dir, argv0, sizeof(tool_dir) - 1);
    char *slash = strrchr(tool_dir, '/');
    if (slash)
        *slash = '\0';
    else
        strcpy(tool_dir, ".");
}

int main(int argc, char **argv) {
    const char *root = NULL;
    const char *outbin = NULL;

    resolve_tool_dir(argv[0]);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outbin = argv[++i];
        else
            root = argv[i];
    }

    if (!root) {
        fprintf(stderr, "usage: nibbuild [-o output.bin] main.nib\n");
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

        /* Recompile if source is newer than outputs, or outputs don't exist */
        m->needs_compile = (nir_time == 0 || nif_time == 0 ||
                            src_time > nir_time || src_time > nif_time);

        /* Also recompile if any dependency's .nif is newer */
        for (int d = 0; d < m->ndeps; d++) {
            time_t dep_nif = file_mtime(modules[m->deps[d]].nif_path);
            if (dep_nif > nir_time)
                m->needs_compile = true;
        }
    }

    /* Phase 1: Compile */
    int errors = 0;
    for (int i = 0; i < nbuild; i++) {
        module_t *m = &modules[build_order[i]];
        if (!m->needs_compile) {
            fprintf(stderr, "  [skip] %s (up to date)\n", m->nib_path);
            continue;
        }

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s/nib %s", tool_dir, m->nib_path);
        if (run_cmd(cmd) != 0) {
            fprintf(stderr, "error: compilation failed for '%s'\n", m->nib_path);
            errors++;
        }
    }

    if (errors > 0) return 1;

    /* Phase 2: Bind all .nir files */
    char bind_cmd[4096];
    snprintf(bind_cmd, sizeof(bind_cmd), "%s/nibbind", tool_dir);
    char asm_path[256];
    replace_ext(root, ".asm", asm_path, sizeof(asm_path));

    for (int i = 0; i < nbuild; i++) {
        strcat(bind_cmd, " ");
        strcat(bind_cmd, modules[build_order[i]].nir_path);
    }
    strcat(bind_cmd, " -o ");
    strcat(bind_cmd, asm_path);

    if (run_cmd(bind_cmd) != 0) {
        fprintf(stderr, "error: binding failed\n");
        return 1;
    }

    /* Phase 3: Assemble */
    char asm_cmd[1024];
    snprintf(asm_cmd, sizeof(asm_cmd), "%s/nibasm %s -o %s", tool_dir, asm_path, outbin);
    if (run_cmd(asm_cmd) != 0) {
        fprintf(stderr, "error: assembly failed\n");
        return 1;
    }

    fprintf(stderr, "Build complete: %s\n", outbin);
    return 0;
}
