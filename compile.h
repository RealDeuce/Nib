#ifndef NIB_COMPILE_H
#define NIB_COMPILE_H

#include "ast.h"
#include <stdio.h>

/* Compile a parsed program, emitting .nir and .nif files.
 * Returns 0 on success, nonzero on error. */
int compile(program_t *prog, const char *nir_path, const char *nif_path);

#endif /* NIB_COMPILE_H */
