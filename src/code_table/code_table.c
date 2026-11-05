#define _POSIX_C_SOURCE 200809L

#include "code_table.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum build_phase {
    BUILD_PHASE_CONFIG,
    BUILD_PHASE_RULE,
    BUILD_PHASE_DATA,
    BUILD_PHASE_PHRASE,
};

struct code_table_rule_entry {
    bool from_back;
    unsigned char character;
    int code_index;
};

struct code_table_rule {
    bool length_equal;
    unsigned char phrase_length;
    struct code_table_rule_entry *entries;
    size_t entry_count;
};

struct reverse_entry {
    const char *text;
    const char *code;
    size_t code_length;
    size_t order;
};

struct code_table {
    char *key_codes;
    char *ignore_chars;
    char *pinyin_key;
    char *prompt_key;
    char *construct_phrase_key;
    uint32_t *key_code_points;
    size_t key_code_point_count;
    size_t max_code_length;

    struct code_table_entry *entries;
    size_t entry_count;
    size_t entry_capacity;

    struct code_table_entry *labels;
    size_t label_count;
    size_t label_capacity;

    struct code_table_entry *prompt_entries;
    size_t prompt_entry_count;
    size_t prompt_entry_capacity;

    struct code_table_rule *rules;
    size_t rule_count;
    size_t rule_capacity;

    struct reverse_entry *reverse;
    size_t reverse_count;
    struct reverse_entry *construct_reverse;
    size_t construct_reverse_count;

    struct code_table_stats stats;
};

static void set_error(
    struct code_table_error *error,
    enum code_table_error_code code,
    size_t line,
    int system_error
) {
    if (error == NULL) {
        return;
    }

    error->code = code;
    error->line = line;
    error->system_error = system_error;
}

static bool is_ascii_space(char character) {
    return (
        character == ' '
        || character == '\t'
        || character == '\r'
        || character == '\n'
        || character == '\f'
        || character == '\v'
    );
}

static char *trim_ascii_whitespace(char *text) {
    char *end;

    while (is_ascii_space(*text)) {
        text++;
    }

    end = text + strlen(text);
    while (end > text && is_ascii_space(end[-1])) {
        *--end = '\0';
    }

    return text;
}

static bool utf8_decode(
    const char *text,
    size_t size,
    uint32_t *codepoint,
    size_t *advance
) {
    const unsigned char *bytes = (const unsigned char *)text;
    uint32_t value;
    size_t length;
    uint32_t minimum;

    if (size == 0) {
        return false;
    }
    if (bytes[0] < 0x80) {
        *codepoint = bytes[0];
        *advance = 1;
        return true;
    }
    if ((bytes[0] & 0xe0) == 0xc0) {
        value = bytes[0] & 0x1f;
        length = 2;
        minimum = 0x80;
    } else if ((bytes[0] & 0xf0) == 0xe0) {
        value = bytes[0] & 0x0f;
        length = 3;
        minimum = 0x800;
    } else if ((bytes[0] & 0xf8) == 0xf0) {
        value = bytes[0] & 0x07;
        length = 4;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (size < length) {
        return false;
    }
    for (size_t index = 1; index < length; index++) {
        if ((bytes[index] & 0xc0) != 0x80) {
            return false;
        }
        value = (value << 6) | (bytes[index] & 0x3f);
    }
    if (
        value < minimum
        || value > 0x10ffff
        || (value >= 0xd800 && value <= 0xdfff)
    ) {
        return false;
    }
    *codepoint = value;
    *advance = length;
    return true;
}

static bool utf8_validate(const char *text, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        if (!utf8_decode(text + offset, size - offset, &codepoint, &advance)) {
            return false;
        }
        offset += advance;
    }
    return true;
}

static size_t utf8_length(const char *text) {
    size_t length = 0;
    size_t size = strlen(text);
    size_t offset = 0;

    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        if (!utf8_decode(text + offset, size - offset, &codepoint, &advance)) {
            return SIZE_MAX;
        }
        offset += advance;
        length++;
    }
    return length;
}

static bool utf8_first_codepoint(
    const char *text,
    uint32_t *codepoint,
    size_t *advance
) {
    return utf8_decode(text, strlen(text), codepoint, advance);
}

static bool utf8_nth_codepoint(
    const char *text,
    size_t index,
    const char **start,
    size_t *length
) {
    size_t size = strlen(text);
    size_t offset = 0;
    size_t current = 0;

    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        if (!utf8_decode(text + offset, size - offset, &codepoint, &advance)) {
            return false;
        }
        if (current == index) {
            *start = text + offset;
            *length = advance;
            return true;
        }
        offset += advance;
        current++;
    }
    return false;
}

static bool codepoint_in_list(
    const uint32_t *list,
    size_t count,
    uint32_t codepoint
) {
    for (size_t index = 0; index < count; index++) {
        if (list[index] == codepoint) {
            return true;
        }
    }
    return false;
}

static bool code_uses_key_codes(
    const struct code_table *table,
    const char *code
) {
    size_t size = strlen(code);
    size_t offset = 0;

    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        if (!utf8_decode(code + offset, size - offset, &codepoint, &advance)) {
            return false;
        }
        if (
            !codepoint_in_list(
                table->key_code_points,
                table->key_code_point_count,
                codepoint
            )
        ) {
            return false;
        }
        offset += advance;
    }
    return true;
}

static bool first_code_codepoint_is_ignored(
    const struct code_table *table,
    const char *code
) {
    uint32_t codepoint;
    size_t advance;

    if (
        table->ignore_chars == NULL
        || *table->ignore_chars == '\0'
        || !utf8_first_codepoint(code, &codepoint, &advance)
    ) {
        return false;
    }

    size_t size = strlen(table->ignore_chars);
    size_t offset = 0;
    while (offset < size) {
        uint32_t ignored;
        size_t ignored_advance;

        if (
            !utf8_decode(
                table->ignore_chars + offset,
                size - offset,
                &ignored,
                &ignored_advance
            )
        ) {
            return false;
        }
        if (ignored == codepoint) {
            return true;
        }
        offset += ignored_advance;
    }
    return false;
}

static bool parse_key_codes(
    struct code_table *table,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    size_t size;
    size_t offset = 0;
    size_t count = 0;
    uint32_t *points;
    char *copy;

    if (text == NULL || *text == '\0') {
        set_error(error, CODE_TABLE_ERROR_INVALID_KEY_CODES, line, 0);
        return false;
    }
    size = strlen(text);
    if (!utf8_validate(text, size)) {
        set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line, 0);
        return false;
    }

    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        if (!utf8_decode(text + offset, size - offset, &codepoint, &advance)) {
            set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line, 0);
            return false;
        }
        offset += advance;
        count++;
    }

    points = calloc(count == 0 ? 1 : count, sizeof(*points));
    copy = strdup(text);
    if (points == NULL || copy == NULL) {
        free(points);
        free(copy);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }

    offset = 0;
    count = 0;
    while (offset < size) {
        uint32_t codepoint;
        size_t advance;

        utf8_decode(text + offset, size - offset, &codepoint, &advance);
        points[count++] = codepoint;
        offset += advance;
    }

    free(table->key_codes);
    free(table->key_code_points);
    table->key_codes = copy;
    table->key_code_points = points;
    table->key_code_point_count = count;
    return true;
}

static bool parse_single_prefix(
    char **destination,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    uint32_t codepoint;
    size_t advance;

    if (text == NULL || *text == '\0') {
        return true;
    }
    if (!utf8_first_codepoint(text, &codepoint, &advance)) {
        set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line, 0);
        return false;
    }

    char *copy = strndup(text, advance);
    if (copy == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    free(*destination);
    *destination = copy;
    return true;
}

static bool parse_code_length(
    size_t *length,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    char *end;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (
        errno == ERANGE
        || end == text
        || *end != '\0'
        || value == 0
        || value > SIZE_MAX
    ) {
        set_error(error, CODE_TABLE_ERROR_INVALID_CODE_LENGTH, line, 0);
        return false;
    }

    *length = (size_t)value;
    return true;
}

static bool table_validate_config(
    const struct code_table *table,
    struct code_table_error *error,
    size_t line
) {
    uint32_t codepoint;
    size_t advance;

    if (table->key_codes == NULL || *table->key_codes == '\0') {
        set_error(error, CODE_TABLE_ERROR_MISSING_KEY_CODES, line, 0);
        return false;
    }
    if (table->max_code_length == 0) {
        set_error(error, CODE_TABLE_ERROR_MISSING_CODE_LENGTH, line, 0);
        return false;
    }

    if (
        table->pinyin_key != NULL
        && utf8_first_codepoint(
            table->pinyin_key,
            &codepoint,
            &advance
        )
        && codepoint_in_list(
            table->key_code_points,
            table->key_code_point_count,
            codepoint
        )
    ) {
        set_error(error, CODE_TABLE_ERROR_PREFIX_CONFLICT, line, 0);
        return false;
    }
    if (
        table->prompt_key != NULL
        && utf8_first_codepoint(
            table->prompt_key,
            &codepoint,
            &advance
        )
        && codepoint_in_list(
            table->key_code_points,
            table->key_code_point_count,
            codepoint
        )
    ) {
        set_error(error, CODE_TABLE_ERROR_PREFIX_CONFLICT, line, 0);
        return false;
    }
    if (
        table->construct_phrase_key != NULL
        && utf8_first_codepoint(
            table->construct_phrase_key,
            &codepoint,
            &advance
        )
        && codepoint_in_list(
            table->key_code_points,
            table->key_code_point_count,
            codepoint
        )
    ) {
        set_error(error, CODE_TABLE_ERROR_PREFIX_CONFLICT, line, 0);
        return false;
    }

    return true;
}

static bool reserve_entries(
    struct code_table_entry **entries,
    size_t *capacity,
    size_t needed,
    struct code_table_error *error,
    size_t line
) {
    struct code_table_entry *grown;

    if (needed <= *capacity) {
        return true;
    }

    size_t new_capacity = *capacity == 0 ? 64 : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
            return false;
        }
        new_capacity *= 2;
    }
    grown = realloc(*entries, new_capacity * sizeof(**entries));
    if (grown == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    *entries = grown;
    *capacity = new_capacity;
    return true;
}

static bool add_entry(
    struct code_table *table,
    const char *code,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    char *code_copy;
    char *text_copy;

    if (
        !reserve_entries(
            &table->entries,
            &table->entry_capacity,
            table->entry_count + 1,
            error,
            line
        )
    ) {
        return false;
    }

    code_copy = strdup(code);
    text_copy = strdup(text);
    if (code_copy == NULL || text_copy == NULL) {
        free(code_copy);
        free(text_copy);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->entries[table->entry_count++] = (struct code_table_entry){
        .code = code_copy,
        .text = text_copy,
    };
    return true;
}

static bool add_label(
    struct code_table *table,
    const char *code,
    const char *label,
    struct code_table_error *error,
    size_t line
) {
    char *value;
    char *key;

    if (*code == '\0' || *label == '\0') {
        set_error(error, CODE_TABLE_ERROR_INVALID_ENTRY, line, 0);
        return false;
    }
    value = strdup(label);
    if (value == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    for (size_t index = 0; index < table->label_count; index++) {
        if (strcmp(table->labels[index].code, code) == 0) {
            free((char *)table->labels[index].text);
            table->labels[index].text = value;
            return true;
        }
    }
    if (!reserve_entries(&table->labels, &table->label_capacity,
                         table->label_count + 1, error, line)) {
        free(value);
        return false;
    }
    key = strdup(code);
    if (key == NULL) {
        free(value);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->labels[table->label_count++] = (struct code_table_entry){key, value};
    return true;
}

static bool add_prompt_entry(
    struct code_table *table,
    const char *code,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    char *code_copy;
    char *text_copy;

    if (
        !reserve_entries(
            &table->prompt_entries,
            &table->prompt_entry_capacity,
            table->prompt_entry_count + 1,
            error,
            line
        )
    ) {
        return false;
    }

    code_copy = strdup(code);
    text_copy = strdup(text);
    if (code_copy == NULL || text_copy == NULL) {
        free(code_copy);
        free(text_copy);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->prompt_entries[table->prompt_entry_count++] = (
        struct code_table_entry
    ){
        .code = code_copy,
        .text = text_copy,
    };
    return true;
}

static bool add_construct_entry(
    struct code_table *table,
    const char *code,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    struct reverse_entry *grown;

    grown = realloc(
        table->construct_reverse,
        (table->construct_reverse_count + 1) * sizeof(*grown)
    );
    if (grown == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->construct_reverse = grown;

    char *code_copy = strdup(code);
    char *text_copy = strdup(text);
    if (code_copy == NULL || text_copy == NULL) {
        free(code_copy);
        free(text_copy);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->construct_reverse[table->construct_reverse_count++] = (
        struct reverse_entry
    ){
        .text = text_copy,
        .code = code_copy,
        .code_length = strlen(code),
        .order = table->construct_reverse_count,
    };
    return true;
}

static bool reserve_rules(
    struct code_table *table,
    struct code_table_error *error,
    size_t line
) {
    struct code_table_rule *grown;

    if (table->rule_count < table->rule_capacity) {
        return true;
    }
    size_t new_capacity = table->rule_capacity == 0 ? 4 : table->rule_capacity * 2;
    grown = realloc(table->rules, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }
    table->rules = grown;
    table->rule_capacity = new_capacity;
    return true;
}

static bool parse_rule_component(
    struct code_table_rule_entry *entry,
    const char *text,
    size_t max_code_length,
    struct code_table_error *error,
    size_t line
) {
    unsigned char character;
    int index;

    if (text[0] == '\0' || text[1] == '\0' || text[2] == '\0' || text[3] != '\0') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    if (text[0] != 'p' && text[0] != 'P' && text[0] != 'n' && text[0] != 'N') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    if (text[1] < '0' || text[1] > '9') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    character = (unsigned char)(text[1] - '0');
    if (text[2] >= '0' && text[2] <= '9') {
        index = text[2] - '0';
    } else if (
        (text[2] >= 'a' && text[2] <= 'z')
        || (text[2] >= 'A' && text[2] <= 'Z')
    ) {
        char lower = text[2];
        if (lower >= 'A' && lower <= 'Z') {
            lower = (char)(lower - 'A' + 'a');
        }
        index = -('z' - lower + 1);
    } else {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }

    if (
        character > max_code_length
        || (size_t)(index < 0 ? -index : index) > max_code_length
        || ((character == 0) != (index == 0))
    ) {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }

    *entry = (struct code_table_rule_entry){
        .from_back = text[0] == 'n' || text[0] == 'N',
        .character = character,
        .code_index = index,
    };
    return true;
}

static bool add_rule(
    struct code_table *table,
    const char *text,
    struct code_table_error *error,
    size_t line
) {
    const char *equal;
    const char *component;
    struct code_table_rule rule = {0};
    size_t component_count = 0;
    char prefix;
    char length_character;

    if (text[0] == '\0' || text[1] == '\0' || text[2] != '=') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    prefix = text[0];
    if (prefix != 'e' && prefix != 'E' && prefix != 'a' && prefix != 'A') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    length_character = text[1];
    if (length_character < '1' || length_character > '9') {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }
    rule.length_equal = prefix == 'e' || prefix == 'E';
    rule.phrase_length = (unsigned char)(length_character - '0');
    if (rule.phrase_length > table->max_code_length) {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }

    equal = text + 2;
    for (const char *cursor = equal + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '+') {
            component_count++;
        }
    }
    component_count++;
    if (component_count > table->max_code_length) {
        set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
        return false;
    }

    rule.entries = calloc(component_count, sizeof(*rule.entries));
    rule.entry_count = component_count;
    if (rule.entries == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
        return false;
    }

    component = equal + 1;
    for (size_t index = 0; index < component_count; index++) {
        const char *end = strchr(component, '+');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        char buffer[4];

        if (length != 3) {
            free(rule.entries);
            set_error(error, CODE_TABLE_ERROR_INVALID_RULE, line, 0);
            return false;
        }
        memcpy(buffer, component, length);
        buffer[length] = '\0';
        if (
            !parse_rule_component(
                &rule.entries[index],
                buffer,
                table->max_code_length,
                error,
                line
            )
        ) {
            free(rule.entries);
            return false;
        }
        component = end == NULL ? component + length : end + 1;
    }

    if (!reserve_rules(table, error, line)) {
        free(rule.entries);
        return false;
    }
    table->rules[table->rule_count++] = rule;
    return true;
}

static char *unescape_value(const char *value) {
    size_t size;
    char *result;
    size_t output = 0;
    bool escaped = false;

    size = strlen(value);
    if (size < 2 || value[0] != '"' || value[size - 1] != '"') {
        return strdup(value);
    }

    result = malloc(size);
    if (result == NULL) {
        return NULL;
    }
    for (size_t index = 1; index + 1 < size; index++) {
        char character = value[index];

        if (!escaped) {
            if (character == '\\') {
                escaped = true;
                continue;
            }
            result[output++] = character;
            continue;
        }

        switch (character) {
        case 'n':
            result[output++] = '\n';
            break;
        case 'r':
            result[output++] = '\r';
            break;
        case 't':
            result[output++] = '\t';
            break;
        case 'f':
            result[output++] = '\f';
            break;
        case 'v':
            result[output++] = '\v';
            break;
        case '\\':
        case '"':
            result[output++] = character;
            break;
        default:
            result[output++] = character;
            break;
        }
        escaped = false;
    }
    if (escaped) {
        result[output++] = '\\';
    }
    result[output] = '\0';
    return result;
}

static bool data_key_matches_prefix(
    const char *key,
    const char *prefix,
    const char **remainder
) {
    uint32_t key_codepoint;
    uint32_t prefix_codepoint;
    size_t key_advance;
    size_t prefix_advance;

    if (
        prefix == NULL
        || *prefix == '\0'
        || !utf8_first_codepoint(key, &key_codepoint, &key_advance)
        || !utf8_first_codepoint(prefix, &prefix_codepoint, &prefix_advance)
    ) {
        return false;
    }
    if (key_codepoint != prefix_codepoint) {
        return false;
    }
    *remainder = key + key_advance;
    return true;
}

static bool parse_data_line(
    struct code_table *table,
    char *line,
    struct code_table_error *error,
    size_t line_number
) {
    char *separator = line;
    char *value;
    char *key = line;
    char *unescaped;
    const char *special_key = NULL;
    enum {
        DATA_NORMAL,
        DATA_PINYIN,
        DATA_PROMPT,
        DATA_CONSTRUCT,
    } kind = DATA_NORMAL;
    size_t key_length;
    size_t value_length;

    while (*separator != '\0' && !is_ascii_space(*separator)) {
        separator++;
    }
    if (*separator == '\0') {
        table->stats.ignored_lines++;
        return true;
    }
    *separator = '\0';
    value = separator + 1;
    while (is_ascii_space(*value)) {
        value++;
    }
    if (*key == '\0' || *value == '\0') {
        table->stats.ignored_lines++;
        return true;
    }

    if (data_key_matches_prefix(key, table->pinyin_key, &special_key)) {
        kind = DATA_PINYIN;
        key = (char *)special_key;
    } else if (data_key_matches_prefix(key, table->prompt_key, &special_key)) {
        kind = DATA_PROMPT;
        key = (char *)special_key;
    } else if (
        data_key_matches_prefix(
            key,
            table->construct_phrase_key,
            &special_key
        )
    ) {
        kind = DATA_CONSTRUCT;
        key = (char *)special_key;
    }

    if (*key == '\0') {
        table->stats.ignored_lines++;
        return true;
    }

    unescaped = unescape_value(value);
    if (unescaped == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line_number, 0);
        return false;
    }
    value = unescaped;
    if (*value == '\0') {
        free(unescaped);
        table->stats.ignored_lines++;
        return true;
    }

    key_length = utf8_length(key);
    value_length = utf8_length(value);
    if (key_length == SIZE_MAX || value_length == SIZE_MAX) {
        free(unescaped);
        set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line_number, 0);
        return false;
    }

    if (kind != DATA_PINYIN) {
        if (
            key_length > table->max_code_length
            || !code_uses_key_codes(table, key)
        ) {
            free(unescaped);
            set_error(error, CODE_TABLE_ERROR_INVALID_CODE, line_number, 0);
            return false;
        }
    }

    bool ok;
    switch (kind) {
    case DATA_PINYIN:
        table->stats.pinyin_entries++;
        ok = true;
        break;
    case DATA_PROMPT:
        ok = add_prompt_entry(table, key, value, error, line_number);
        if (ok) {
            table->stats.prompt_entries++;
        }
        break;
    case DATA_CONSTRUCT:
        ok = add_construct_entry(table, key, value, error, line_number);
        if (ok) {
            table->stats.construct_phrase_entries++;
        }
        break;
    case DATA_NORMAL:
        ok = add_entry(table, key, value, error, line_number);
        break;
    }

    free(unescaped);
    return ok;
}

static int compare_reverse_entries(const void *left, const void *right) {
    const struct reverse_entry *lhs = left;
    const struct reverse_entry *rhs = right;
    int text_order = strcmp(lhs->text, rhs->text);

    if (text_order != 0) {
        return text_order;
    }
    if (lhs->code_length != rhs->code_length) {
        return lhs->code_length < rhs->code_length ? -1 : 1;
    }
    if (lhs->order != rhs->order) {
        return lhs->order < rhs->order ? -1 : 1;
    }
    return 0;
}

static bool build_reverse_map(
    struct code_table *table,
    struct code_table_error *error,
    size_t line
) {
    struct reverse_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    for (size_t index = 0; index < table->entry_count; index++) {
        const struct code_table_entry *entry = &table->entries[index];

        if (utf8_length(entry->text) != 1) {
            continue;
        }
        if (first_code_codepoint_is_ignored(table, entry->code)) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 256 : capacity * 2;
            struct reverse_entry *grown = realloc(
                entries,
                new_capacity * sizeof(*entries)
            );
            if (grown == NULL) {
                free(entries);
                set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line, 0);
                return false;
            }
            entries = grown;
            capacity = new_capacity;
        }
        entries[count++] = (struct reverse_entry){
            .text = entry->text,
            .code = entry->code,
            .code_length = strlen(entry->code),
            .order = index,
        };
    }

    if (count > 1) {
        qsort(entries, count, sizeof(*entries), compare_reverse_entries);
    }
    table->reverse = entries;
    table->reverse_count = count;

    if (table->construct_reverse_count > 1) {
        qsort(
            table->construct_reverse,
            table->construct_reverse_count,
            sizeof(*table->construct_reverse),
            compare_reverse_entries
        );
    }
    return true;
}

static const char *reverse_lookup_in(
    const struct reverse_entry *entries,
    size_t count,
    const char *text
) {
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(entries[middle].text, text);

        if (order < 0) {
            low = middle + 1;
        } else if (order > 0) {
            high = middle;
        } else {
            while (middle > 0 && strcmp(entries[middle - 1].text, text) == 0) {
                middle--;
            }
            return entries[middle].code;
        }
    }
    return NULL;
}

static const char *phrase_reverse_lookup(
    const struct code_table *table,
    const char *text
) {
    const char *code = reverse_lookup_in(
        table->construct_reverse,
        table->construct_reverse_count,
        text
    );

    if (code != NULL || table->construct_phrase_key != NULL) {
        return code;
    }
    return reverse_lookup_in(table->reverse, table->reverse_count, text);
}

static bool generate_phrase_code(
    const struct code_table *table,
    const char *phrase,
    char **generated,
    size_t *generated_size
) {
    size_t phrase_length = utf8_length(phrase);
    char *key;

    if (phrase_length == 0 || phrase_length == SIZE_MAX) {
        return false;
    }

    for (size_t rule_index = 0; rule_index < table->rule_count; rule_index++) {
        const struct code_table_rule *rule = &table->rules[rule_index];
        bool applicable =
            (rule->length_equal && phrase_length == rule->phrase_length)
            || (!rule->length_equal && phrase_length >= rule->phrase_length);

        if (!applicable) {
            continue;
        }

        key = calloc(table->max_code_length + 1, 1);
        if (key == NULL) {
            return false;
        }
        size_t key_length = 0;
        bool success = true;
        struct used_pair {
            size_t character;
            int code_index;
        } *used = calloc(
            rule->entry_count == 0 ? 1 : rule->entry_count,
            sizeof(*used)
        );
        size_t used_count = 0;

        if (used == NULL) {
            free(key);
            return false;
        }

        for (size_t index = 0; index < rule->entry_count; index++) {
            const struct code_table_rule_entry *rule_entry =
                &rule->entries[index];
            const char *character_text;
            size_t character_size;
            size_t position;
            const char *code;
            size_t code_length;

            if (rule_entry->character == 0 || rule_entry->code_index == 0) {
                continue;
            }
            if (rule_entry->character > phrase_length) {
                success = false;
                break;
            }
            size_t character_position = (size_t)rule_entry->character;
            position = rule_entry->from_back
                ? phrase_length - character_position
                : character_position - 1;
            if (
                !utf8_nth_codepoint(
                    phrase,
                    position,
                    &character_text,
                    &character_size
                )
            ) {
                success = false;
                break;
            }
            char character[8];
            if (character_size >= sizeof(character)) {
                success = false;
                break;
            }
            memcpy(character, character_text, character_size);
            character[character_size] = '\0';

            code = phrase_reverse_lookup(table, character);
            if (code == NULL) {
                success = false;
                break;
            }
            code_length = utf8_length(code);
            if (code_length == 0 || code_length == SIZE_MAX) {
                success = false;
                break;
            }

            int code_index = rule_entry->code_index;
            if (code_index > 0) {
                code_index -= 1;
            } else {
                code_index = (int)code_length + code_index;
            }
            if (code_index < 0 || (size_t)code_index >= code_length) {
                success = false;
                break;
            }

            bool duplicate = false;
            for (size_t used_index = 0; used_index < used_count; used_index++) {
                if (
                    used[used_index].character == position
                    && used[used_index].code_index == code_index
                ) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (used_count < rule->entry_count) {
                used[used_count++] = (struct used_pair){
                    .character = position,
                    .code_index = code_index,
                };
            }

            const char *code_character;
            size_t code_character_size;
            if (
                !utf8_nth_codepoint(
                    code,
                    (size_t)code_index,
                    &code_character,
                    &code_character_size
                )
                || key_length + code_character_size > table->max_code_length
            ) {
                success = false;
                break;
            }
            memcpy(key + key_length, code_character, code_character_size);
            key_length += code_character_size;
        }

        free(used);
        if (success && key_length > 0) {
            key[key_length] = '\0';
            *generated = key;
            *generated_size = key_length;
            return true;
        }
        free(key);
    }

    return false;
}

static bool parse_phrase_line(
    struct code_table *table,
    char *line,
    struct code_table_error *error,
    size_t line_number
) {
    char *unescaped;
    char *generated = NULL;
    size_t generated_size = 0;

    line = trim_ascii_whitespace(line);
    if (*line == '\0') {
        return true;
    }
    unescaped = unescape_value(line);
    if (unescaped == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line_number, 0);
        return false;
    }
    if (!utf8_validate(unescaped, strlen(unescaped))) {
        free(unescaped);
        set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line_number, 0);
        return false;
    }

    if (
        !generate_phrase_code(
            table,
            unescaped,
            &generated,
            &generated_size
        )
    ) {
        table->stats.rejected_phrases++;
        free(unescaped);
        return true;
    }
    (void)generated_size;

    bool ok = add_entry(table, generated, unescaped, error, line_number);
    free(generated);
    free(unescaped);
    if (ok) {
        table->stats.phrases++;
    }
    return ok;
}

static bool consume_config_line(
    struct code_table *table,
    char *line,
    struct code_table_error *error,
    size_t line_number
) {
    if (*line == '\0' || line[0] == '#') {
        return true;
    }

    /* Local candidate-caption extension; libime safely ignores this key. */
    if (strncmp(line, "Label[", 6) == 0) {
        char *end = strstr(line + 6, "]=");
        if (end == NULL) {
            set_error(error, CODE_TABLE_ERROR_INVALID_ENTRY, line_number, 0);
            return false;
        }
        *end = '\0';
        return add_label(table, line + 6, trim_ascii_whitespace(end + 2),
                         error, line_number);
    }
    if (strncmp(line, "KeyCode=", 8) == 0) {
        parse_key_codes(
            table,
            trim_ascii_whitespace(line + 8),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "键码=", strlen("键码=")) == 0) {
        parse_key_codes(
            table,
            trim_ascii_whitespace(line + strlen("键码=")),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "Length=", 7) == 0) {
        parse_code_length(
            &table->max_code_length,
            trim_ascii_whitespace(line + 7),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "码长=", strlen("码长=")) == 0) {
        parse_code_length(
            &table->max_code_length,
            trim_ascii_whitespace(line + strlen("码长=")),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "InvalidChar=", 12) == 0) {
        char *value = trim_ascii_whitespace(line + 12);
        char *copy = strdup(value);
        if (copy == NULL) {
            set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line_number, 0);
            return error->code == CODE_TABLE_ERROR_NONE;
        }
        free(table->ignore_chars);
        table->ignore_chars = copy;
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "规避字符=", strlen("规避字符=")) == 0) {
        char *value = trim_ascii_whitespace(line + strlen("规避字符="));
        char *copy = strdup(value);
        if (copy == NULL) {
            set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, line_number, 0);
            return error->code == CODE_TABLE_ERROR_NONE;
        }
        free(table->ignore_chars);
        table->ignore_chars = copy;
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (
        strncmp(line, "PinyinLength=", 13) == 0
        || strncmp(line, "拼音长度=", strlen("拼音长度=")) == 0
    ) {
        /* Deprecated by libime; accepted and ignored. */
        return true;
    }
    if (strncmp(line, "Pinyin=", 7) == 0) {
        parse_single_prefix(
            &table->pinyin_key,
            trim_ascii_whitespace(line + 7),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "拼音=", strlen("拼音=")) == 0) {
        parse_single_prefix(
            &table->pinyin_key,
            trim_ascii_whitespace(line + strlen("拼音=")),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "Prompt=", 7) == 0) {
        parse_single_prefix(
            &table->prompt_key,
            trim_ascii_whitespace(line + 7),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "提示=", strlen("提示=")) == 0) {
        parse_single_prefix(
            &table->prompt_key,
            trim_ascii_whitespace(line + strlen("提示=")),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "ConstructPhrase=", 16) == 0) {
        parse_single_prefix(
            &table->construct_phrase_key,
            trim_ascii_whitespace(line + 16),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    if (strncmp(line, "构词=", strlen("构词=")) == 0) {
        parse_single_prefix(
            &table->construct_phrase_key,
            trim_ascii_whitespace(line + strlen("构词=")),
            error,
            line_number
        );
        return error->code == CODE_TABLE_ERROR_NONE;
    }
    /* Unknown configuration lines are ignored, matching libime. */
    return true;
}

static bool section_is(const char *line, const char *english, const char *chinese) {
    return strcmp(line, english) == 0 || strcmp(line, chinese) == 0;
}

static bool parse_content(
    struct code_table *table,
    char *content,
    size_t size,
    struct code_table_error *error
) {
    enum build_phase phase = BUILD_PHASE_CONFIG;
    char *cursor = content;
    char *end = content + size;
    size_t line_number = 0;
    bool utf8_ok = true;

    if (
        size >= 3
        && (unsigned char)cursor[0] == 0xef
        && (unsigned char)cursor[1] == 0xbb
        && (unsigned char)cursor[2] == 0xbf
    ) {
        cursor += 3;
    }

    while (cursor < end) {
        char *line = cursor;
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        char *line_end = newline == NULL ? end : newline;

        line_number++;
        cursor = newline == NULL ? end : newline + 1;
        if (line_end > line && line_end[-1] == '\r') {
            line_end--;
        }
        *line_end = '\0';

        if (!utf8_validate(line, (size_t)(line_end - line))) {
            set_error(error, CODE_TABLE_ERROR_INVALID_UTF8, line_number, 0);
            utf8_ok = false;
            break;
        }

        switch (phase) {
        case BUILD_PHASE_CONFIG: {
            char *trimmed = trim_ascii_whitespace(line);

            if (*trimmed == '\0' || trimmed[0] == '#') {
                break;
            }
            if (section_is(trimmed, "[Data]", "[数据]")) {
                if (!table_validate_config(table, error, line_number)) {
                    return false;
                }
                phase = BUILD_PHASE_DATA;
                break;
            }
            if (section_is(trimmed, "[Rule]", "[组词规则]")) {
                phase = BUILD_PHASE_RULE;
                break;
            }
            if (!consume_config_line(table, trimmed, error, line_number)) {
                return false;
            }
            break;
        }
        case BUILD_PHASE_RULE: {
            char *trimmed = trim_ascii_whitespace(line);

            if (*trimmed == '\0' || trimmed[0] == '#') {
                break;
            }
            if (section_is(trimmed, "[Data]", "[数据]")) {
                if (!table_validate_config(table, error, line_number)) {
                    return false;
                }
                phase = BUILD_PHASE_DATA;
                break;
            }
            if (!add_rule(table, trimmed, error, line_number)) {
                return false;
            }
            break;
        }
        case BUILD_PHASE_DATA: {
            char *trimmed = trim_ascii_whitespace(line);

            if (section_is(trimmed, "[Phrase]", "[词组]")) {
                if (table->rule_count == 0) {
                    set_error(error, CODE_TABLE_ERROR_MISSING_RULES, line_number, 0);
                    return false;
                }
                if (!build_reverse_map(table, error, line_number)) {
                    return false;
                }
                phase = BUILD_PHASE_PHRASE;
                break;
            }
            if (!parse_data_line(table, trimmed, error, line_number)) {
                return false;
            }
            break;
        }
        case BUILD_PHASE_PHRASE:
            if (!parse_phrase_line(table, line, error, line_number)) {
                return false;
            }
            break;
        }
    }

    if (!utf8_ok) {
        return false;
    }
    if (phase != BUILD_PHASE_DATA && phase != BUILD_PHASE_PHRASE) {
        set_error(error, CODE_TABLE_ERROR_MISSING_DATA_SECTION, line_number, 0);
        return false;
    }
    if (table->entry_count == 0) {
        set_error(error, CODE_TABLE_ERROR_EMPTY_DATA, line_number, 0);
        return false;
    }

    return true;
}

static char *read_file(
    const char *path,
    size_t *size,
    struct code_table_error *error
) {
    FILE *file;
    char *content = NULL;
    long file_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, CODE_TABLE_ERROR_OPEN, 0, errno);
        return NULL;
    }

    if (
        fseek(file, 0, SEEK_END) != 0
        || (file_size = ftell(file)) < 0
        || fseek(file, 0, SEEK_SET) != 0
    ) {
        int saved_errno = errno;
        fclose(file);
        set_error(error, CODE_TABLE_ERROR_READ, 0, saved_errno);
        return NULL;
    }

    if ((uintmax_t)file_size > SIZE_MAX - 1) {
        fclose(file);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }

    content = malloc((size_t)file_size + 1);
    if (content == NULL) {
        fclose(file);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }

    if (fread(content, 1, (size_t)file_size, file) != (size_t)file_size) {
        int saved_errno = ferror(file) && errno != 0 ? errno : EIO;
        free(content);
        fclose(file);
        set_error(error, CODE_TABLE_ERROR_READ, 0, saved_errno);
        return NULL;
    }

    fclose(file);
    content[file_size] = '\0';
    *size = (size_t)file_size;
    return content;
}

struct code_table *code_table_load_memory(
    const char *data,
    size_t size,
    struct code_table_error *error
) {
    struct code_table *table;
    char *storage;

    set_error(error, CODE_TABLE_ERROR_NONE, 0, 0);
    if (data == NULL || size == 0) {
        set_error(error, CODE_TABLE_ERROR_INVALID_ARGUMENT, 0, 0);
        return NULL;
    }

    storage = malloc(size + 1);
    if (storage == NULL) {
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }
    memcpy(storage, data, size);
    storage[size] = '\0';

    table = calloc(1, sizeof(*table));
    if (table == NULL) {
        free(storage);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }

    if (!parse_content(table, storage, size, error)) {
        free(storage);
        code_table_destroy(table);
        return NULL;
    }
    free(storage);
    return table;
}

struct code_table *code_table_load(
    const char *path,
    struct code_table_error *error
) {
    size_t size;
    char *content;
    struct code_table *table;

    set_error(error, CODE_TABLE_ERROR_NONE, 0, 0);
    if (path == NULL || *path == '\0') {
        set_error(error, CODE_TABLE_ERROR_INVALID_ARGUMENT, 0, 0);
        return NULL;
    }

    content = read_file(path, &size, error);
    if (content == NULL) {
        return NULL;
    }
    table = code_table_load_memory(content, size, error);
    free(content);
    return table;
}

bool code_table_overlay_missing(
    struct code_table *table,
    const struct code_table *overlay,
    size_t *added,
    struct code_table_error *error
) {
    size_t original_count;
    size_t original_labels;
    const char *previous_code = NULL;
    bool occupied = false;

    set_error(error, CODE_TABLE_ERROR_NONE, 0, 0);
    if (added != NULL) {
        *added = 0;
    }
    if (table == NULL || overlay == NULL || table == overlay) {
        set_error(error, CODE_TABLE_ERROR_INVALID_ARGUMENT, 0, 0);
        return false;
    }
    original_count = table->entry_count;
    original_labels = table->label_count;
    for (size_t index = 0; index < overlay->entry_count; index++) {
        const struct code_table_entry *entry = &overlay->entries[index];

        if (
            utf8_length(entry->code) > table->max_code_length
            || !code_uses_key_codes(table, entry->code)
        ) {
            continue;
        }
        /* Check each grouped code once, against ORIGINAL entries only. A
         * catalogue can contain thousands of candidates for just 27 codes. */
        if (previous_code == NULL || strcmp(previous_code, entry->code) != 0) {
            const char *label = code_table_label(overlay, entry->code);
            previous_code = entry->code;
            occupied = false;
            for (size_t old = 0; old < original_count; old++) {
                if (strcmp(table->entries[old].code, entry->code) == 0) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied && label != NULL && code_table_label(table, entry->code) == NULL
                && !add_label(table, entry->code, label, error, 0)) {
                goto rollback;
            }
        }
        if (!occupied && !add_entry(table, entry->code, entry->text, error, 0)) {
            goto rollback;
        }
    }
    if (added != NULL) {
        *added = table->entry_count - original_count;
    }
    return true;

rollback:
    while (table->entry_count > original_count) {
        struct code_table_entry *entry = &table->entries[--table->entry_count];
        free((char *)entry->code);
        free((char *)entry->text);
    }
    while (table->label_count > original_labels) {
        struct code_table_entry *label = &table->labels[--table->label_count];
        free((char *)label->code);
        free((char *)label->text);
    }
    return false;
}

const char *code_table_label(const struct code_table *table, const char *code) {
    if (table == NULL || code == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < table->label_count; index++) {
        if (strcmp(table->labels[index].code, code) == 0) {
            return table->labels[index].text;
        }
    }
    return NULL;
}

void code_table_destroy(struct code_table *table) {
    if (table == NULL) {
        return;
    }

    for (size_t index = 0; index < table->entry_count; index++) {
        free((char *)table->entries[index].code);
        free((char *)table->entries[index].text);
    }
    for (size_t index = 0; index < table->prompt_entry_count; index++) {
        free((char *)table->prompt_entries[index].code);
        free((char *)table->prompt_entries[index].text);
    }
    for (size_t index = 0; index < table->rule_count; index++) {
        free(table->rules[index].entries);
    }
    for (size_t index = 0; index < table->construct_reverse_count; index++) {
        free((char *)table->construct_reverse[index].code);
        free((char *)table->construct_reverse[index].text);
    }
    for (size_t index = 0; index < table->label_count; index++) {
        free((char *)table->labels[index].code);
        free((char *)table->labels[index].text);
    }
    free(table->labels);
    free(table->entries);
    free(table->prompt_entries);
    free(table->rules);
    free(table->reverse);
    free(table->construct_reverse);
    free(table->key_codes);
    free(table->key_code_points);
    free(table->ignore_chars);
    free(table->pinyin_key);
    free(table->prompt_key);
    free(table->construct_phrase_key);
    free(table);
}

const char *code_table_key_codes(const struct code_table *table) {
    return table == NULL ? NULL : table->key_codes;
}

const char *code_table_ignore_chars(const struct code_table *table) {
    return table == NULL ? NULL : table->ignore_chars;
}

const char *code_table_pinyin_key(const struct code_table *table) {
    return table == NULL ? NULL : table->pinyin_key;
}

const char *code_table_prompt_key(const struct code_table *table) {
    return table == NULL ? NULL : table->prompt_key;
}

const char *code_table_construct_phrase_key(const struct code_table *table) {
    return table == NULL ? NULL : table->construct_phrase_key;
}

size_t code_table_max_code_length(const struct code_table *table) {
    return table == NULL ? 0 : table->max_code_length;
}

size_t code_table_rule_count(const struct code_table *table) {
    return table == NULL ? 0 : table->rule_count;
}

size_t code_table_entry_count(const struct code_table *table) {
    return table == NULL ? 0 : table->entry_count;
}

const struct code_table_entry *code_table_entries(
    const struct code_table *table
) {
    return table == NULL ? NULL : table->entries;
}

struct code_table_stats code_table_stats(const struct code_table *table) {
    if (table == NULL) {
        return (struct code_table_stats){0};
    }
    return table->stats;
}

static bool code_matches_pattern(const char *code, const char *pattern) {
    while (*pattern != '\0') {
        if (*pattern == CODE_TABLE_WILDCARD) {
            if (*code < 'a' || *code > 'z') {
                return false;
            }
        } else if (*code != *pattern) {
            return false;
        }
        code++;
        pattern++;
    }
    return *code == '\0';
}

static size_t lookup_page(
    const struct code_table *table,
    const char *code,
    bool prefix,
    size_t skip,
    const struct code_table_entry **matches,
    size_t capacity
) {
    size_t match_count = 0;
    size_t stored = 0;
    size_t code_length = strlen(code);
    bool wildcard = strchr(code, CODE_TABLE_WILDCARD) != NULL;

    /* A labelled catalogue page is an exact group, not its descendants. This
     * prevents oj's overview and oja..ojz from duplicating each other in
     * ExactMatch=False input methods. */
    if (code_table_label(table, code) != NULL) {
        prefix = false;
    }
    for (size_t index = 0; index < table->entry_count; index++) {
        /* Wildcards constrain every position and the complete code length,
         * even when ordinary input uses prefix matching. */
        bool match = wildcard
            ? code_matches_pattern(table->entries[index].code, code)
            : prefix
                ? strncmp(table->entries[index].code, code, code_length) == 0
                : strcmp(table->entries[index].code, code) == 0;

        if (!match) {
            continue;
        }
        if (match_count >= skip && stored < capacity) {
            matches[stored++] = &table->entries[index];
        }
        match_count++;
    }

    return match_count;
}

size_t code_table_lookup_page(
    const struct code_table *table,
    const char *code,
    size_t skip,
    const struct code_table_entry **matches,
    size_t capacity
) {
    if (
        table == NULL
        || code == NULL
        || (capacity != 0 && matches == NULL)
    ) {
        return 0;
    }
    return lookup_page(table, code, false, skip, matches, capacity);
}

size_t code_table_lookup_prefix_page(
    const struct code_table *table,
    const char *code,
    size_t skip,
    const struct code_table_entry **matches,
    size_t capacity
) {
    if (
        table == NULL
        || code == NULL
        || *code == '\0'
        || (capacity != 0 && matches == NULL)
    ) {
        return 0;
    }
    return lookup_page(table, code, true, skip, matches, capacity);
}

size_t code_table_lookup(
    const struct code_table *table,
    const char *code,
    const struct code_table_entry **matches,
    size_t capacity
) {
    return code_table_lookup_page(table, code, 0, matches, capacity);
}

const char *code_table_error_message(enum code_table_error_code code) {
    switch (code) {
    case CODE_TABLE_ERROR_NONE:
        return "no error";
    case CODE_TABLE_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case CODE_TABLE_ERROR_OPEN:
        return "cannot open file";
    case CODE_TABLE_ERROR_READ:
        return "cannot read file";
    case CODE_TABLE_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case CODE_TABLE_ERROR_INVALID_UTF8:
        return "invalid UTF-8";
    case CODE_TABLE_ERROR_MISSING_KEY_CODES:
        return "missing key codes";
    case CODE_TABLE_ERROR_INVALID_KEY_CODES:
        return "invalid key codes";
    case CODE_TABLE_ERROR_MISSING_CODE_LENGTH:
        return "missing maximum code length";
    case CODE_TABLE_ERROR_INVALID_CODE_LENGTH:
        return "invalid maximum code length";
    case CODE_TABLE_ERROR_PREFIX_CONFLICT:
        return "pinyin, prompt or phrase prefix conflicts with key codes";
    case CODE_TABLE_ERROR_MISSING_DATA_SECTION:
        return "missing data section";
    case CODE_TABLE_ERROR_EMPTY_DATA:
        return "data section is empty";
    case CODE_TABLE_ERROR_INVALID_ENTRY:
        return "invalid data entry";
    case CODE_TABLE_ERROR_INVALID_CODE:
        return "invalid entry code";
    case CODE_TABLE_ERROR_INVALID_RULE:
        return "invalid rule";
    case CODE_TABLE_ERROR_MISSING_RULES:
        return "phrase section without a rule section";
    }

    return "unknown error";
}
