/* test_runner.c — Main test harness for Lulu runtime */
#include "test_harness.h"

/* Define shared counters (declared extern in test_harness.h) */
int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_current_failed = 0;

/* Declare all test suite runners */
extern void run_all_sandbox_tests(void);
extern void run_all_tools_tests(void);
extern void run_all_agent_db_tests(void);
extern void run_all_channel_tests(void);
extern void run_all_tasks_tests(void);

int main(void) {
    printf("Lulu Runtime Test Suite\n");
    printf("========================\n\n");

    printf("[Sandbox Tests]\n");
    run_all_sandbox_tests();

    printf("\n[Tool Tests]\n");
    run_all_tools_tests();

    printf("\n[AgentDB Tests]\n");
    run_all_agent_db_tests();

    printf("\n[Channel Tests]\n");
    run_all_channel_tests();

    printf("\n[Tasks Tests]\n");
    run_all_tasks_tests();

    test_summary();
    return g_tests_failed > 0 ? 1 : 0;
}
