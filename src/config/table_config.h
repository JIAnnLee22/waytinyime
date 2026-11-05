#ifndef WAYTINYIME_TABLE_CONFIG_H
#define WAYTINYIME_TABLE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct table_key {
    uint32_t symbol;
    uint32_t modifiers;
};

struct table_key_list {
    struct table_key *items;
    size_t count;
};

enum table_config_modifier {
    TABLE_CONFIG_MODIFIER_CONTROL = 1 << 0,
    TABLE_CONFIG_MODIFIER_ALT = 1 << 1,
    TABLE_CONFIG_MODIFIER_LOGO = 1 << 2,
    TABLE_CONFIG_MODIFIER_SHIFT = 1 << 3,
};

enum table_config_error_code {
    TABLE_CONFIG_ERROR_NONE = 0,
    TABLE_CONFIG_ERROR_INVALID_ARGUMENT,
    TABLE_CONFIG_ERROR_OPEN,
    TABLE_CONFIG_ERROR_READ,
    TABLE_CONFIG_ERROR_OUT_OF_MEMORY,
    TABLE_CONFIG_ERROR_INVALID_UTF8,
    TABLE_CONFIG_ERROR_INVALID_SYNTAX,
    TABLE_CONFIG_ERROR_INVALID_VALUE,
};

struct table_config_error {
    enum table_config_error_code code;
    size_t line;
    int system_error;
    char detail[256];
};

struct table_config {
    char *path;

    /* [InputMethod] */
    char *name;
    char *label;
    char *icon;
    char *lang_code;
    char *addon;
    bool configurable;

    /* [Table] */
    char *file;
    int page_size;
    bool auto_select;
    int auto_select_length;
    char *auto_select_regex;
    int no_match_auto_select_length;
    char *no_match_auto_select_regex;
    bool exact_match;
    bool commit_after_select;
    bool commit_when_deactivate;
    bool commit_invalid_segment;
    bool use_full_width;
    bool ignore_punc;
    bool first_candidate_as_preedit;
    int no_sort_input_length;
    char *order_policy;
    bool use_system_language_model;
    bool use_context_related_order;
    char *matching_key;
    char *pinyin_key;
    char *quick_phrase_key;
    char *quick_phrase_text;
    int auto_phrase_length;
    int save_auto_phrase_after;
    bool learning;
    bool hint;
    bool display_custom_hint;
    char *hint_separator;
    bool space_before_hint;
    char *candidate_layout_hint;
    bool keypad_as_input;

    /* [Table/PrevPage] and friends. */
    struct table_key_list prev_page;
    struct table_key_list next_page;
    struct table_key_list prev_candidate;
    struct table_key_list next_candidate;
    struct table_key_list default_candidate;
    struct table_key_list second_candidate;
    struct table_key_list third_candidate;
    struct table_key_list selection;
};

struct table_config *table_config_load(
    const char *path,
    struct table_config_error *error
);

void table_config_destroy(struct table_config *config);

const char *table_config_error_message(enum table_config_error_code code);

#endif
