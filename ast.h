#ifndef NIB_AST_H
#define NIB_AST_H

/*
 * Minimal AST stub types for grammar validation.
 * These exist only so bison semantic actions compile.
 * No real AST implementation yet.
 */

typedef struct ast_node {
    int kind;
} ast_node_t;

/* Placeholder — all semantic values are either a node pointer or a string */
typedef ast_node_t *node_ptr;

#endif /* NIB_AST_H */
