#ifndef MB_TEST_CHECK_H
#define MB_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) test_fail(__FILE__, __LINE__, #expr); } while (0)

#define TEST_MAIN(suite, ...)                                      \
    int main(void) {                                               \
        void (*tests[])(void) = {__VA_ARGS__};                     \
        for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i += 1) { \
            tests[i]();                                            \
        }                                                          \
        puts(#suite " tests passed");                              \
        return 0;                                                  \
    }

static void test_fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(1);
}

#endif
