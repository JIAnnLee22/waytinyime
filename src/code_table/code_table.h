#ifndef WAYTINYIME_CODE_TABLE_H
#define WAYTINYIME_CODE_TABLE_H

#include <stdbool.h>
#include <stddef.h>

/* A query-only wildcard matching exactly one lowercase ASCII letter. */
#define CODE_TABLE_WILDCARD '`'

struct code_table;

struct code_table_entry {
    const char *code;
    const char *text;
};

struct code_table_stats {
    size_t pinyin_entries;
    size_t prompt_entries;
    size_t construct_phrase_entries;
    size_t phrases;
    size_t rejected_phrases;
    size_t ignored_lines;
};

enum code_table_error_code {
    CODE_TABLE_ERROR_NONE = 0,
    CODE_TABLE_ERROR_INVALID_ARGUMENT,
    CODE_TABLE_ERROR_OPEN,
    CODE_TABLE_ERROR_READ,
    CODE_TABLE_ERROR_OUT_OF_MEMORY,
    CODE_TABLE_ERROR_INVALID_UTF8,
    CODE_TABLE_ERROR_MISSING_KEY_CODES,
    CODE_TABLE_ERROR_INVALID_KEY_CODES,
    CODE_TABLE_ERROR_MISSING_CODE_LENGTH,
    CODE_TABLE_ERROR_INVALID_CODE_LENGTH,
    CODE_TABLE_ERROR_PREFIX_CONFLICT,
    CODE_TABLE_ERROR_MISSING_DATA_SECTION,
    CODE_TABLE_ERROR_EMPTY_DATA,
    CODE_TABLE_ERROR_INVALID_ENTRY,
    CODE_TABLE_ERROR_INVALID_CODE,
    CODE_TABLE_ERROR_INVALID_RULE,
    CODE_TABLE_ERROR_MISSING_RULES,
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

/*
 * Parse an in-memory UTF-8 table.  This is used for binary libime
 * dictionaries after they have been decoded to the official text format.
 * The buffer does not need to be NUL terminated.
 */
struct code_table *code_table_load_memory(
    const char *data,
    size_t size,
    struct code_table_error *error
);

/* Append all overlay candidates for codes absent from the original table.
 * Incompatible codes are skipped. On failure, appended entries are rolled back. */
bool code_table_overlay_missing(
    struct code_table *table,
    const struct code_table *overlay,
    size_t *added,
    struct code_table_error *error
);

/* Optional local Label[code]=... caption (never committed as candidate text). */
const char *code_table_label(const struct code_table *table, const char *code);
void code_table_destroy(struct code_table *table);

const char *code_table_key_codes(const struct code_table *table);
const char *code_table_ignore_chars(const struct code_table *table);
const char *code_table_pinyin_key(const struct code_table *table);
const char *code_table_prompt_key(const struct code_table *table);
const char *code_table_construct_phrase_key(const struct code_table *table);
size_t code_table_max_code_length(const struct code_table *table);
size_t code_table_rule_count(const struct code_table *table);
size_t code_table_entry_count(const struct code_table *table);
const struct code_table_entry *code_table_entries(
    const struct code_table *table
);
struct code_table_stats code_table_stats(const struct code_table *table);

/* Each ` in a query matches one a-z letter; wildcard queries are exact-length. */
size_t code_table_lookup(
    const struct code_table *table,
    const char *code,
    const struct code_table_entry **matches,
    size_t capacity
);

/*
 * Look up a code or wildcard pattern, skipping the first skip matches. The
 * return value is always the total number of matches in the table, even
 * when capacity is smaller than the number of remaining matches.
 */
size_t code_table_lookup_page(
    const struct code_table *table,
    const char *code,
    size_t skip,
    const struct code_table_entry **matches,
    size_t capacity
);

/* fcitx5 ExactMatch=False uses prefix matching, except for wildcard queries. */
size_t code_table_lookup_prefix_page(
    const struct code_table *table,
    const char *code,
    size_t skip,
    const struct code_table_entry **matches,
    size_t capacity
);

const char *code_table_error_message(enum code_table_error_code code);

#endif
