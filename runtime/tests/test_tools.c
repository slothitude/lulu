/* test_tools.c — Tool loading and cJSON tests */
#include "test_harness.h"
#include "cJSON.h"
#include <string.h>

/* ===== Tests ===== */

TEST(cjson_parse_valid) {
    cJSON *j = cJSON_Parse("{\"tool\":\"create_file\",\"arguments\":{\"path\":\"test.txt\"}}");
    ASSERT_NOT_NULL(j);
    cJSON *tool = cJSON_GetObjectItem(j, "tool");
    ASSERT_NOT_NULL(tool);
    ASSERT_STR("create_file", tool->valuestring);
    cJSON_Delete(j);
}

TEST(cjson_parse_invalid) {
    cJSON *j = cJSON_Parse("not valid json {{{");
    ASSERT_NULL(j);
}

TEST(cjson_parse_empty_string) {
    cJSON *j = cJSON_Parse("");
    ASSERT_NULL(j);
}

TEST(cjson_parse_null) {
    cJSON *j = cJSON_Parse(NULL);
    ASSERT_NULL(j);
}

TEST(cjson_nested_object) {
    const char *json = "{\"tool\":\"replay_script\",\"arguments\":{\"script_name\":\"calc_workflow\"}}";
    cJSON *j = cJSON_Parse(json);
    ASSERT_NOT_NULL(j);
    cJSON *args = cJSON_GetObjectItem(j, "arguments");
    ASSERT_NOT_NULL(args);
    cJSON *sn = cJSON_GetObjectItem(args, "script_name");
    ASSERT_NOT_NULL(sn);
    ASSERT_STR("calc_workflow", sn->valuestring);
    cJSON_Delete(j);
}

TEST(cjson_array_operations) {
    const char *json = "[1,2,3,4,5]";
    cJSON *arr = cJSON_Parse(json);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(5, cJSON_GetArraySize(arr));
    ASSERT_EQ(3, cJSON_GetArrayItem(arr, 2)->valueint);
    cJSON_Delete(arr);
}

/* ===== Suite runner ===== */

void run_all_tools_tests(void) {
    RUN_TEST(cjson_parse_valid);
    RUN_TEST(cjson_parse_invalid);
    RUN_TEST(cjson_parse_empty_string);
    RUN_TEST(cjson_parse_null);
    RUN_TEST(cjson_nested_object);
    RUN_TEST(cjson_array_operations);
}
