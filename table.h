#ifndef NIB_TABLE_H
#define NIB_TABLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *nib_xmalloc(size_t size, const char *what) {
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory allocating %s\n",
                what ? what : "storage");
        exit(1);
    }
    return p;
}

static inline void *nib_xcalloc(size_t count, size_t size, const char *what) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory allocating %s\n",
                what ? what : "storage");
        exit(1);
    }
    return p;
}

static inline void *nib_xrealloc_array(void *ptr, size_t count, size_t size,
                                       const char *what) {
    if (size != 0 && count > (size_t)-1 / size) {
        fprintf(stderr, "fatal: allocation overflow growing %s\n",
                what ? what : "storage");
        exit(1);
    }
    void *p = realloc(ptr, count * size);
    if (!p && count != 0) {
        fprintf(stderr, "fatal: out of memory growing %s\n",
                what ? what : "storage");
        exit(1);
    }
    return p;
}

static inline size_t nib_grow_capacity(size_t cap, size_t need) {
    size_t ncap = cap ? cap : 8;
    while (ncap < need) {
        if (ncap > (size_t)-1 / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    return ncap;
}

#define NIB_VEC(type) struct { type *items; int len; int cap; }

#define NIB_VEC_INIT(v) \
    do { (v)->items = NULL; (v)->len = 0; (v)->cap = 0; } while (0)

#define NIB_VEC_FREE(v) \
    do { free((v)->items); (v)->items = NULL; (v)->len = 0; (v)->cap = 0; } while (0)

#define NIB_VEC_RESERVE(v, need, what) do { \
    if ((need) > (v)->cap) { \
        size_t _nib_cap = nib_grow_capacity((size_t)(v)->cap, \
                                            (size_t)(need)); \
        (v)->items = nib_xrealloc_array((v)->items, _nib_cap, \
                                        sizeof(*(v)->items), (what)); \
        (v)->cap = (int)_nib_cap; \
    } \
} while (0)

#define NIB_VEC_PUSH(v, what) ({ \
    NIB_VEC_RESERVE((v), (v)->len + 1, (what)); \
    &((v)->items[(v)->len++]); \
})

#define NIB_VEC_AT(v, idx) (&((v)->items[(idx)]))

#endif /* NIB_TABLE_H */
