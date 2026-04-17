#pragma once
/* Minimal test framework for Lulu runtime tests */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared counters — defined in test_runner.c */
extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern int g_current_failed;

#define TEST(name) static void test_##name(void); \
    static void run_##name(void) { \
        g_tests_run++; g_current_failed = 0; \
        printf("  %-50s", #name); \
        test_##name(); \
        if (g_current_failed) { g_tests_failed++; printf("FAIL\n"); } \
        else { g_tests_passed++; printf("PASS\n"); } \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
        g_current_failed = 1; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("\n    ASSERT_EQ failed: %d != %d (%s:%d)", (int)(a), (int)(b), __FILE__, __LINE__); \
        g_current_failed = 1; return; \
    } \
} while(0)

#define ASSERT_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("\n    ASSERT_STR failed: \"%s\" != \"%s\" (%s:%d)", (a), (b), __FILE__, __LINE__); \
        g_current_failed = 1; return; \
    } \
} while(0)

#define ASSERT_NULL(p) do { \
    if ((p) != NULL) { \
        printf("\n    ASSERT_NULL failed: %p is not NULL (%s:%d)", (void*)(p), __FILE__, __LINE__); \
        g_current_failed = 1; return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) { \
        printf("\n    ASSERT_NOT_NULL failed: pointer is NULL (%s:%d)", __FILE__, __LINE__); \
        g_current_failed = 1; return; \
    } \
} while(0)

#define RUN_TEST(name) run_##name()

static void test_summary(void) {
    printf("\n========================================\n");
    printf("Tests: %d run, %d passed, %d failed\n", g_tests_run, g_tests_passed, g_tests_failed);
    printf("========================================\n");
}
