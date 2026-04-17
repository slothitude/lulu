/* test_tasks.c — Task lifecycle tests (stubs — need full runtime) */
#include "test_harness.h"

TEST(tasks_stub) {
    /* Full task tests need the graph database runtime.
       Just verify the test infrastructure works. */
    ASSERT(1);
}

/* ===== Suite runner ===== */

void run_all_tasks_tests(void) {
    RUN_TEST(tasks_stub);
}
