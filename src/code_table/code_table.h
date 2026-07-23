#ifndef WAYTINYIME_CODE_TABLE_H
#define WAYTINYIME_CODE_TABLE_H

#include <stddef.h>

struct code_table;

struct code_table_entry {
    const char *code;
    const char *text;
};

enum code_table_error_code {
    CODE_TABLE_ERROR_NONE = 0,
    CODE_TABLE_ERROR_INVALID_ARGUMENT,
    CODE_TABLE_ERROR_OPEN,
    CODE_TABLE_ERROR_READ,
    CODE_TABLE_ERROR_OUT_OF_MEMORY,
    CODE_TABLE_ERROR_MISSING_KEY_CODES,
    CODE_TABLE_ERROR_INVALID_KEY_CODES,
    CODE_TABLE_ERROR_MISSING_CODE_LENGTH,
    CODE_TABLE_ERROR_INVALID_CODE_LENGTH,
    CODE_TABLE_ERROR_MISSING_DATA_SECTION,
    CODE_TABLE_ERROR_EMPTY_DATA,
    CODE_TABLE_ERROR_INVALID_ENTRY,
    CODE_TABLE_ERROR_INVALID_CODE,
};

struct code_table_error {
    enum code_table_error_code code;
    size_t line;
    int system_error;
};

/* The returned table owns all strings referenced by its entries. */
struct code_table *code_table_load(
    const char *path,
    struct code_table_error *error
);

void code_table_destroy(struct code_table *table);

const char *code_table_key_codes(const struct code_table *table);
size_t code_table_max_code_length(const struct code_table *table);
size_t code_table_entry_count(const struct code_table *table);
const struct code_table_entry *code_table_entries(
    const struct code_table *table
);
size_t code_table_lookup(
    const struct code_table *table,
    const char *code,
    const struct code_table_entry **matches,
    size_t capacity
);


const char *code_table_error_message(enum code_table_error_code code);

#endif
