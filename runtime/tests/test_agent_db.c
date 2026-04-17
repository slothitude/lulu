/* test_agent_db.c — Graph database tests (stubs — need RyuGraph runtime) */
#include "test_harness.h"

TEST(agent_db_stub) {
    /* Full DB tests need RyuGraph linked. Just verify infrastructure works. */
    ASSERT(1);
}

/* ===== Suite runner ===== */

void run_all_agent_db_tests(void) {
    RUN_TEST(agent_db_stub);
}
