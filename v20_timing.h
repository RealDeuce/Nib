#ifndef V20_TIMING_H
#define V20_TIMING_H

#include <stdbool.h>

typedef enum {
    V20_MIX_OTHER,
    V20_MIX_MEMORY,
    V20_MIX_PUSH_POP,
    V20_MIX_CALL,
    V20_MIX_BRANCH,
    V20_MIX_SEGMENT,
    V20_MIX_IO,
    V20_MIX_SHIFT,
    V20_MIX_STRING,
} v20_mix_t;

typedef struct {
    bool known;
    bool variable;
    int clocks_min;
    int clocks_max;
    int transfers_min;
    int transfers_max;
    int bytes_min;
    int bytes_max;
    v20_mix_t mix;
    char mnemonic[16];
    char form[96];
    char clocks[32];
    char transfers[32];
    char note[64];
} v20_timing_t;

v20_timing_t v20_timing_estimate(const char *asm_line);
const char *v20_mix_name(v20_mix_t mix);

#endif
