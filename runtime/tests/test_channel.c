/* test_channel.c — Event queue tests (stubs — need full runtime) */
#include "test_harness.h"

TEST(channel_stub) {
    /* Full channel tests need the runtime with g_adb etc.
       Just verify the test infrastructure works. */
    ASSERT(1);
}

/* ===== Suite runner ===== */

void run_all_channel_tests(void) {
    RUN_TEST(channel_stub);
}
