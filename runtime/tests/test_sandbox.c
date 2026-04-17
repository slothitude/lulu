/* test_sandbox.c — Path traversal and boundary tests */
#include "test_harness.h"
#include "sandbox.h"

static const char *g_workspace = "C:\\Users\\test\\workspace";

/* ===== Tests ===== */

TEST(block_empty_path) {
    ASSERT_EQ(0, sandbox_is_path_safe("", g_workspace));
}

TEST(block_absolute_windows) {
    ASSERT_EQ(0, sandbox_is_path_safe("C:\\Windows\\System32\\cmd.exe", g_workspace));
}

TEST(block_absolute_unix) {
    ASSERT_EQ(0, sandbox_is_path_safe("/etc/passwd", g_workspace));
}

TEST(block_path_traversal_dotdot) {
    ASSERT_EQ(0, sandbox_is_path_safe("../../etc/passwd", g_workspace));
}

TEST(block_path_traversal_embedded) {
    ASSERT_EQ(0, sandbox_is_path_safe("subdir/../../etc/passwd", g_workspace));
}

TEST(block_system32) {
    ASSERT_EQ(0, sandbox_is_path_safe("system32/drivers", g_workspace));
}

TEST(block_windows_dir) {
    ASSERT_EQ(0, sandbox_is_path_safe("Windows/System32", g_workspace));
}

TEST(block_unix_etc) {
    ASSERT_EQ(0, sandbox_is_path_safe("/etc/shadow", g_workspace));
}

TEST(block_unix_proc) {
    ASSERT_EQ(0, sandbox_is_path_safe("/proc/self/environ", g_workspace));
}

TEST(block_null_path) {
    ASSERT_EQ(0, sandbox_is_path_safe(NULL, g_workspace));
}

TEST(block_null_workspace) {
    ASSERT_EQ(0, sandbox_is_path_safe("file.txt", NULL));
}

TEST(allow_normal_file) {
    ASSERT_EQ(1, sandbox_is_path_safe("file.txt", g_workspace));
}

TEST(allow_subdirectory) {
    ASSERT_EQ(1, sandbox_is_path_safe("subdir/file.txt", g_workspace));
}

TEST(allow_nested_path) {
    ASSERT_EQ(1, sandbox_is_path_safe("src/include/header.h", g_workspace));
}

TEST(resolve_normal_file) {
    char *resolved = sandbox_resolve_path("test.txt", g_workspace);
    ASSERT_NOT_NULL(resolved);
    /* Should contain workspace prefix */
    ASSERT(strstr(resolved, "workspace") != NULL);
    free(resolved);
}

TEST(resolve_blocks_traversal) {
    char *resolved = sandbox_resolve_path("../../../etc/passwd", g_workspace);
    ASSERT_NULL(resolved);
}

TEST(resolve_blocks_absolute) {
    char *resolved = sandbox_resolve_path("C:\\Windows\\System32", g_workspace);
    ASSERT_NULL(resolved);
}

/* ===== Suite runner ===== */

void run_all_sandbox_tests(void) {
    RUN_TEST(block_empty_path);
    RUN_TEST(block_absolute_windows);
    RUN_TEST(block_absolute_unix);
    RUN_TEST(block_path_traversal_dotdot);
    RUN_TEST(block_path_traversal_embedded);
    RUN_TEST(block_system32);
    RUN_TEST(block_windows_dir);
    RUN_TEST(block_unix_etc);
    RUN_TEST(block_unix_proc);
    RUN_TEST(block_null_path);
    RUN_TEST(block_null_workspace);
    RUN_TEST(allow_normal_file);
    RUN_TEST(allow_subdirectory);
    RUN_TEST(allow_nested_path);
    RUN_TEST(resolve_normal_file);
    RUN_TEST(resolve_blocks_traversal);
    RUN_TEST(resolve_blocks_absolute);
}
