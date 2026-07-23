#include <stdio.h>
#include <string.h>

#include "code_table.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(                                                         \
                stderr,                                                      \
                "check failed at %s:%d: %s\n",                              \
                __FILE__,                                                    \
                __LINE__,                                                    \
                #condition                                                   \
            );                                                               \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int test_flypy_table(const char *path) {
    struct code_table_error error;
    struct code_table *table = code_table_load(path, &error);
    const struct code_table_entry *entries;
    size_t entry_count;
    size_t anqi_index = 0;
    const struct code_table_entry *matches[2];

    if (table == NULL) {
        fprintf(
            stderr,
            "failed to load %s at line %zu: %s\n",
            path,
            error.line,
            code_table_error_message(error.code)
        );
        return 1;
    }

    entries = code_table_entries(table);
    entry_count = code_table_entry_count(table);
    CHECK(strcmp(code_table_key_codes(table), "abcdefghijklmnopqrstuvwxyz;") == 0);
    CHECK(code_table_max_code_length(table) == 4);
    CHECK(entry_count == 59830);
    CHECK(strcmp(entries[0].code, "a") == 0);
    CHECK(strcmp(entries[0].text, "啊") == 0);
    CHECK(strcmp(entries[1].code, "a") == 0);
    CHECK(strcmp(entries[1].text, "按") == 0);
    CHECK(strcmp(entries[entry_count - 1].code, "oz") == 0);
    CHECK(strcmp(entries[entry_count - 1].text, "“") == 0);

    while (anqi_index < entry_count) {
        if (strcmp(entries[anqi_index].code, "anqi") == 0) {
            break;
        }
        anqi_index++;
    }
    CHECK(anqi_index + 1 < entry_count);
    CHECK(strcmp(entries[anqi_index].text, "按期") == 0);
    CHECK(strcmp(entries[anqi_index + 1].code, "anqi") == 0);
    CHECK(strcmp(entries[anqi_index + 1].text, "暗器") == 0);

    CHECK(code_table_lookup(table, "bce", matches, 2) == 2);
    CHECK(strcmp(matches[0]->text, "报恩") == 0);
    CHECK(strcmp(matches[1]->text, "剥") == 0);

    code_table_destroy(table);
    return 0;
}

static int test_invalid_code(const char *path) {
    struct code_table_error error;
    struct code_table *table = code_table_load(path, &error);

    CHECK(table == NULL);
    CHECK(error.code == CODE_TABLE_ERROR_INVALID_CODE);
    CHECK(error.line == 5);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    CHECK(test_flypy_table(argv[1]) == 0);
    CHECK(test_invalid_code(argv[2]) == 0);
    return 0;
}
