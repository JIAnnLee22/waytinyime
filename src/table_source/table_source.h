#ifndef WAYTINYIME_TABLE_SOURCE_H
#define WAYTINYIME_TABLE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

struct code_table;
struct table_config;
struct table_source;

enum table_source_error_code {
    TABLE_SOURCE_ERROR_NONE = 0,
    TABLE_SOURCE_ERROR_INVALID_ARGUMENT,
    TABLE_SOURCE_ERROR_NOT_FOUND,
    TABLE_SOURCE_ERROR_CONFIG,
    TABLE_SOURCE_ERROR_TABLE,
    TABLE_SOURCE_ERROR_LIBIME,
    TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
    TABLE_SOURCE_ERROR_UNSUPPORTED,
};

struct table_source_error {
    enum table_source_error_code code;
    char path[4096];
    size_t line;
    char detail[512];
};

struct table_source *table_source_load_config(
    const char *config_path,
    struct table_source_error *error
);

struct table_source *table_source_load_table(
    const char *table_path,
    struct table_source_error *error
);

/*
 * Look for <name>.conf in the fcitx5 XDG data directories.  If no config is
 * found, fall back to the built-in flypy.txt.  WAYTINYIME_CONF overrides the
 * search.
 */
struct table_source *table_source_load_default(
    const char *input_method,
    struct table_source_error *error
);

void table_source_destroy(struct table_source *source);

const struct table_config *table_source_config(
    const struct table_source *source
);
const struct code_table *table_source_table(
    const struct table_source *source
);
const char *table_source_config_path(const struct table_source *source);
const char *table_source_table_path(const struct table_source *source);
bool table_source_table_is_binary(const struct table_source *source);
size_t table_source_emoji_entries(const struct table_source *source);

const char *table_source_error_message(enum table_source_error_code code);

#endif
