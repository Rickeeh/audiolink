// Testes de host do AudioLink v2: verificacao minima, sem framework.
#pragma once

#include <stdio.h>
#include <stdlib.h>

static int g_checks, g_fails;

#define CHECK(cond)                                                                \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            g_fails++;                                                             \
            fprintf(stderr, "%s:%d: FALHOU: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                          \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                                                 \
    do {                                                                                                   \
        g_checks++;                                                                                        \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);                   \
        if (va_ != vb_) {                                                                                  \
            g_fails++;                                                                                     \
            fprintf(stderr, "%s:%d: FALHOU: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                                  \
    } while (0)

static int check_done(const char *name) {
    printf("%s: %d verificacoes, %d falhas\n", name, g_checks, g_fails);
    return g_fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
