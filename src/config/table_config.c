#define _POSIX_C_SOURCE 200809L

#include "table_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

struct ini_entry {
    char *section;
    char *key;
    char *value;
    size_t line;
};

struct ini_file {
    struct ini_entry *entries;
    size_t count;
    size_t capacity;
};

struct key_entry {
    long index;
    char *value;
    size_t line;
};

static void set_error(
    struct table_config_error *error,
    enum table_config_error_code code,
    size_t line,
    const char *detail
) {
    if (error == NULL) {
        return;
    }
    error->code = code;
    error->line = line;
    error->system_error = 0;
    if (detail != NULL) {
        snprintf(error->detail, sizeof(error->detail), "%s", detail);
    } else {
        error->detail[0] = '\0';
    }
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

static char *trim(char *text) {
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

static char *unescape_value(const char *value) {
    size_t size = strlen(value);
    char *result;
    size_t output = 0;
    bool escaped = false;

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

static char *read_file(
    const char *path,
    size_t *size,
    struct table_config_error *error
) {
    FILE *file = fopen(path, "rb");
    char *content;
    long file_size;

    if (file == NULL) {
        set_error(error, TABLE_CONFIG_ERROR_OPEN, 0, strerror(errno));
        error->system_error = errno;
        return NULL;
    }
    if (
        fseek(file, 0, SEEK_END) != 0
        || (file_size = ftell(file)) < 0
        || fseek(file, 0, SEEK_SET) != 0
    ) {
        int saved_errno = errno;
        fclose(file);
        set_error(error, TABLE_CONFIG_ERROR_READ, 0, strerror(saved_errno));
        error->system_error = saved_errno;
        return NULL;
    }
    content = malloc((size_t)file_size + 1);
    if (content == NULL) {
        fclose(file);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        return NULL;
    }
    if (fread(content, 1, (size_t)file_size, file) != (size_t)file_size) {
        int saved_errno = ferror(file) && errno != 0 ? errno : EIO;
        free(content);
        fclose(file);
        set_error(error, TABLE_CONFIG_ERROR_READ, 0, strerror(saved_errno));
        error->system_error = saved_errno;
        return NULL;
    }
    fclose(file);
    content[file_size] = '\0';
    *size = (size_t)file_size;
    return content;
}

static bool add_ini_entry(
    struct ini_file *ini,
    const char *section,
    const char *key,
    const char *value,
    size_t line,
    struct table_config_error *error
) {
    struct ini_entry *grown;
    char *section_copy;
    char *key_copy;
    char *value_copy;

    if (ini->count == ini->capacity) {
        size_t new_capacity = ini->capacity == 0 ? 32 : ini->capacity * 2;
        grown = realloc(ini->entries, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, line, NULL);
            return false;
        }
        ini->entries = grown;
        ini->capacity = new_capacity;
    }

    section_copy = strdup(section);
    key_copy = strdup(key);
    value_copy = strdup(value);
    if (section_copy == NULL || key_copy == NULL || value_copy == NULL) {
        free(section_copy);
        free(key_copy);
        free(value_copy);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, line, NULL);
        return false;
    }
    ini->entries[ini->count++] = (struct ini_entry){
        .section = section_copy,
        .key = key_copy,
        .value = value_copy,
        .line = line,
    };
    return true;
}

static bool parse_ini(
    char *content,
    size_t size,
    struct ini_file *ini,
    struct table_config_error *error
) {
    char *cursor = content;
    char *end = content + size;
    char *section = NULL;
    size_t line_number = 0;

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
        char *trimmed;

        line_number++;
        cursor = newline == NULL ? end : newline + 1;
        if (line_end > line && line_end[-1] == '\r') {
            line_end--;
        }
        *line_end = '\0';
        trimmed = trim(line);
        if (*trimmed == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        if (trimmed[0] == '[') {
            char *close = strchr(trimmed + 1, ']');
            if (close == NULL || close[1] != '\0') {
                set_error(
                    error,
                    TABLE_CONFIG_ERROR_INVALID_SYNTAX,
                    line_number,
                    "invalid section header"
                );
                return false;
            }
            *close = '\0';
            free(section);
            section = strdup(trim(trimmed + 1));
            if (section == NULL) {
                set_error(
                    error,
                    TABLE_CONFIG_ERROR_OUT_OF_MEMORY,
                    line_number,
                    NULL
                );
                return false;
            }
            continue;
        }

        char *separator = strchr(trimmed, '=');
        if (separator == NULL) {
            free(section);
            set_error(
                error,
                TABLE_CONFIG_ERROR_INVALID_SYNTAX,
                line_number,
                "expected key=value"
            );
            return false;
        }
        *separator = '\0';
        char *key = trim(trimmed);
        char *value = trim(separator + 1);
        char *unescaped = unescape_value(value);

        if (section == NULL || *key == '\0' || unescaped == NULL) {
            free(unescaped);
            free(section);
            set_error(
                error,
                unescaped == NULL
                    ? TABLE_CONFIG_ERROR_OUT_OF_MEMORY
                    : TABLE_CONFIG_ERROR_INVALID_SYNTAX,
                line_number,
                unescaped == NULL ? NULL : "entry outside a section"
            );
            return false;
        }
        bool ok = add_ini_entry(
            ini,
            section,
            key,
            unescaped,
            line_number,
            error
        );
        free(unescaped);
        if (!ok) {
            free(section);
            return false;
        }
    }
    free(section);
    return true;
}

static char *locale_name(void) {
    const char *value = getenv("LC_ALL");

    if (value == NULL || *value == '\0') {
        value = getenv("LC_MESSAGES");
    }
    if (value == NULL || *value == '\0') {
        value = getenv("LANG");
    }
    if (value == NULL || *value == '\0') {
        value = "C";
    }

    char *copy = strdup(value);
    if (copy == NULL) {
        return NULL;
    }
    char *dot = strchr(copy, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    char *modifier = strchr(copy, '@');
    if (modifier != NULL) {
        *modifier = '\0';
    }
    return copy;
}

static int localized_rank(
    const char *key,
    const char *base,
    const char *locale
) {
    size_t base_length = strlen(base);

    if (strcmp(key, base) == 0) {
        return 1;
    }
    if (
        strncmp(key, base, base_length) != 0
        || key[base_length] != '['
    ) {
        return 0;
    }
    const char *locale_start = key + base_length + 1;
    const char *locale_end = strchr(locale_start, ']');
    if (locale_end == NULL || locale_end[1] != '\0') {
        return 0;
    }

    size_t length = (size_t)(locale_end - locale_start);
    if (strlen(locale) == length && strncmp(locale, locale_start, length) == 0) {
        return 3;
    }
    const char *underscore = strchr(locale, '_');
    size_t language_length = underscore == NULL
        ? strlen(locale)
        : (size_t)(underscore - locale);
    if (
        language_length == length
        && strncmp(locale, locale_start, length) == 0
    ) {
        return 2;
    }
    return 0;
}

static const char *ini_get_localized(
    const struct ini_file *ini,
    const char *section,
    const char *base,
    const char *locale,
    size_t *line
) {
    const char *best = NULL;
    int best_rank = 0;

    for (size_t index = 0; index < ini->count; index++) {
        const struct ini_entry *entry = &ini->entries[index];
        int rank;

        if (strcmp(entry->section, section) != 0) {
            continue;
        }
        rank = localized_rank(entry->key, base, locale);
        if (rank >= best_rank && rank != 0) {
            best = entry->value;
            best_rank = rank;
            if (line != NULL) {
                *line = entry->line;
            }
        }
    }
    return best;
}

static const char *ini_get(
    const struct ini_file *ini,
    const char *section,
    const char *key,
    size_t *line
) {
    const char *value = NULL;

    for (size_t index = 0; index < ini->count; index++) {
        const struct ini_entry *entry = &ini->entries[index];

        if (
            strcmp(entry->section, section) == 0
            && strcmp(entry->key, key) == 0
        ) {
            value = entry->value;
            if (line != NULL) {
                *line = entry->line;
            }
        }
    }
    return value;
}

static bool ini_has_section(const struct ini_file *ini, const char *section) {
    for (size_t index = 0; index < ini->count; index++) {
        if (strcmp(ini->entries[index].section, section) == 0) {
            return true;
        }
    }
    return false;
}

static bool set_string(char **destination, const char *value) {
    char *copy = strdup(value);

    if (copy == NULL) {
        return false;
    }
    free(*destination);
    *destination = copy;
    return true;
}

static bool parse_bool(
    const char *text,
    bool *value,
    struct table_config_error *error,
    size_t line,
    const char *key
) {
    char detail[128];

    if (*text == '\0') {
        return true;
    }
    if (
        strcasecmp(text, "true") == 0
        || strcasecmp(text, "yes") == 0
        || strcmp(text, "1") == 0
        || strcasecmp(text, "on") == 0
    ) {
        *value = true;
        return true;
    }
    if (
        strcasecmp(text, "false") == 0
        || strcasecmp(text, "no") == 0
        || strcmp(text, "0") == 0
        || strcasecmp(text, "off") == 0
    ) {
        *value = false;
        return true;
    }

    snprintf(detail, sizeof(detail), "invalid boolean for %s: %s", key, text);
    set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
    return false;
}

static bool parse_int(
    const char *text,
    long minimum,
    long maximum,
    int *value,
    struct table_config_error *error,
    size_t line,
    const char *key
) {
    char *end;
    long parsed;
    char detail[160];

    if (*text == '\0') {
        return true;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (
        errno == ERANGE
        || end == text
        || *end != '\0'
        || parsed < minimum
        || parsed > maximum
    ) {
        snprintf(detail, sizeof(detail), "invalid integer for %s: %s", key, text);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool set_int_if_present(
    const struct ini_file *ini,
    const char *key,
    long minimum,
    long maximum,
    int *destination,
    struct table_config_error *error
) {
    size_t line = 0;
    const char *value = ini_get(ini, "Table", key, &line);

    if (value == NULL) {
        return true;
    }
    return parse_int(value, minimum, maximum, destination, error, line, key);
}

static bool set_bool_if_present(
    const struct ini_file *ini,
    const char *key,
    bool *destination,
    struct table_config_error *error
) {
    size_t line = 0;
    const char *value = ini_get(ini, "Table", key, &line);

    if (value == NULL) {
        return true;
    }
    return parse_bool(value, destination, error, line, key);
}

static bool set_string_if_present(
    const struct ini_file *ini,
    const char *section,
    const char *key,
    char **destination,
    struct table_config_error *error
) {
    size_t line = 0;
    const char *value = ini_get(ini, section, key, &line);

    if (value == NULL) {
        return true;
    }
    if (!set_string(destination, value)) {
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, line, NULL);
        return false;
    }
    return true;
}

static uint32_t modifier_from_name(const char *name) {
    if (strcasecmp(name, "control") == 0 || strcasecmp(name, "ctrl") == 0) {
        return TABLE_CONFIG_MODIFIER_CONTROL;
    }
    if (strcasecmp(name, "alt") == 0 || strcasecmp(name, "mod1") == 0) {
        return TABLE_CONFIG_MODIFIER_ALT;
    }
    if (
        strcasecmp(name, "logo") == 0
        || strcasecmp(name, "super") == 0
        || strcasecmp(name, "meta") == 0
    ) {
        return TABLE_CONFIG_MODIFIER_LOGO;
    }
    if (strcasecmp(name, "shift") == 0) {
        return TABLE_CONFIG_MODIFIER_SHIFT;
    }
    return 0;
}

static bool parse_key_binding(
    const char *text,
    struct table_key *binding,
    struct table_config_error *error,
    size_t line
) {
    char *copy = strdup(text);
    char *parts[8];
    size_t part_count = 0;
    uint32_t modifiers = 0;
    char detail[160];

    if (copy == NULL) {
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, line, NULL);
        return false;
    }
    char *save = NULL;
    for (char *part = strtok_r(copy, "+", &save); part != NULL;
         part = strtok_r(NULL, "+", &save)) {
        if (part_count >= sizeof(parts) / sizeof(parts[0])) {
            free(copy);
            set_error(
                error,
                TABLE_CONFIG_ERROR_INVALID_VALUE,
                line,
                "too many key components"
            );
            return false;
        }
        parts[part_count++] = trim(part);
    }
    if (part_count == 0) {
        free(copy);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, "empty key");
        return false;
    }

    for (size_t index = 0; index + 1 < part_count; index++) {
        uint32_t modifier = modifier_from_name(parts[index]);
        if (modifier == 0) {
            snprintf(detail, sizeof(detail), "unknown modifier: %s", parts[index]);
            free(copy);
            set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
            return false;
        }
        modifiers |= modifier;
    }

    const char *symbol_name = parts[part_count - 1];
    xkb_keysym_t symbol = xkb_keysym_from_name(
        symbol_name,
        XKB_KEYSYM_CASE_INSENSITIVE
    );
    if (symbol == XKB_KEY_NoSymbol) {
        snprintf(detail, sizeof(detail), "unknown key name: %s", symbol_name);
        free(copy);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
        return false;
    }

    *binding = (struct table_key){
        .symbol = symbol,
        .modifiers = modifiers,
    };
    free(copy);
    return true;
}

static bool key_list_append(
    struct table_key_list *list,
    struct table_key binding,
    struct table_config_error *error,
    size_t line
) {
    struct table_key *grown = realloc(
        list->items,
        (list->count + 1) * sizeof(*grown)
    );

    if (grown == NULL) {
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, line, NULL);
        return false;
    }
    list->items = grown;
    list->items[list->count++] = binding;
    return true;
}

static bool set_default_key_list(
    struct table_key_list *list,
    const char *const *names,
    size_t count,
    struct table_config_error *error
) {
    for (size_t index = 0; index < count; index++) {
        struct table_key binding;

        if (!parse_key_binding(names[index], &binding, error, 0)) {
            return false;
        }
        if (!key_list_append(list, binding, error, 0)) {
            return false;
        }
    }
    return true;
}

static int compare_key_entries(const void *left, const void *right) {
    const struct key_entry *lhs = left;
    const struct key_entry *rhs = right;

    if (lhs->index != rhs->index) {
        return lhs->index < rhs->index ? -1 : 1;
    }
    if (lhs->line != rhs->line) {
        return lhs->line < rhs->line ? -1 : 1;
    }
    return 0;
}

static void clear_key_list(struct table_key_list *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static bool load_key_section(
    const struct ini_file *ini,
    const char *section,
    struct table_key_list *list,
    struct table_config_error *error
) {
    struct key_entry *entries = NULL;
    size_t entry_count = 0;

    if (!ini_has_section(ini, section)) {
        return true;
    }

    for (size_t index = 0; index < ini->count; index++) {
        const struct ini_entry *entry = &ini->entries[index];
        char *end;
        long key_index;

        if (strcmp(entry->section, section) != 0) {
            continue;
        }
        errno = 0;
        key_index = strtol(entry->key, &end, 10);
        if (
            errno == ERANGE
            || end == entry->key
            || *end != '\0'
            || key_index < 0
        ) {
            free(entries);
            set_error(
                error,
                TABLE_CONFIG_ERROR_INVALID_SYNTAX,
                entry->line,
                "key list entries need a non-negative numeric index"
            );
            return false;
        }
        struct key_entry *grown = realloc(
            entries,
            (entry_count + 1) * sizeof(*grown)
        );
        if (grown == NULL) {
            free(entries);
            set_error(
                error,
                TABLE_CONFIG_ERROR_OUT_OF_MEMORY,
                entry->line,
                NULL
            );
            return false;
        }
        entries = grown;
        entries[entry_count++] = (struct key_entry){
            .index = key_index,
            .value = entry->value,
            .line = entry->line,
        };
    }

    if (entry_count > 1) {
        qsort(entries, entry_count, sizeof(*entries), compare_key_entries);
    }

    clear_key_list(list);
    for (size_t index = 0; index < entry_count; index++) {
        char *copy = strdup(entries[index].value);
        char *save = NULL;

        if (copy == NULL) {
            size_t entry_line = entries[index].line;
            free(entries);
            set_error(
                error,
                TABLE_CONFIG_ERROR_OUT_OF_MEMORY,
                entry_line,
                NULL
            );
            return false;
        }
        for (char *part = strtok_r(copy, ",", &save); part != NULL;
             part = strtok_r(NULL, ",", &save)) {
            char *trimmed = trim(part);
            struct table_key binding;

            if (*trimmed == '\0') {
                continue;
            }
            if (
                !parse_key_binding(
                    trimmed,
                    &binding,
                    error,
                    entries[index].line
                )
                || !key_list_append(
                    list,
                    binding,
                    error,
                    entries[index].line
                )
            ) {
                free(copy);
                free(entries);
                return false;
            }
        }
        free(copy);
    }

    free(entries);
    return true;
}

static bool validate_regex(
    const char *pattern,
    struct table_config_error *error,
    size_t line,
    const char *key
) {
    regex_t regex;
    int result;
    char detail[256];

    if (pattern == NULL || *pattern == '\0') {
        return true;
    }
    result = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
    if (result != 0) {
        char reason[128];
        regerror(result, &regex, reason, sizeof(reason));
        snprintf(detail, sizeof(detail), "invalid %s: %s", key, reason);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
        return false;
    }
    regfree(&regex);
    return true;
}

static bool validate_order_policy(
    const char *value,
    struct table_config_error *error,
    size_t line
) {
    if (*value == '\0') {
        return true;
    }
    if (
        strcasecmp(value, "No") != 0
        && strcasecmp(value, "Freq") != 0
        && strcasecmp(value, "Fast") != 0
    ) {
        char detail[160];
        snprintf(detail, sizeof(detail), "invalid OrderPolicy: %s", value);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
        return false;
    }
    return true;
}

static bool validate_layout_hint(
    const char *value,
    struct table_config_error *error,
    size_t line
) {
    if (*value == '\0') {
        return true;
    }
    if (
        strcasecmp(value, "NotSet") != 0
        && strcasecmp(value, "Vertical") != 0
        && strcasecmp(value, "Horizontal") != 0
    ) {
        char detail[160];
        snprintf(detail, sizeof(detail), "invalid CandidateLayoutHint: %s", value);
        set_error(error, TABLE_CONFIG_ERROR_INVALID_VALUE, line, detail);
        return false;
    }
    return true;
}

static struct table_config *table_config_new(void) {
    static const char *const prev_page[] = {"Up"};
    static const char *const next_page[] = {"Down"};
    static const char *const prev_candidate[] = {"Left"};
    static const char *const next_candidate[] = {"Right"};
    static const char *const default_candidate[] = {"space"};
    static const char *const selection[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    };
    struct table_config *config = calloc(1, sizeof(*config));
    struct table_config_error ignored = {0};

    if (config == NULL) {
        return NULL;
    }
    config->page_size = 5;
    config->auto_select_length = 0;
    config->no_match_auto_select_length = 0;
    config->commit_after_select = true;
    config->commit_when_deactivate = true;
    config->use_full_width = true;
    config->learning = true;
    config->use_system_language_model = true;
    config->use_context_related_order = true;
    config->auto_phrase_length = -1;
    config->save_auto_phrase_after = -1;
    config->space_before_hint = true;

    if (
        !set_string(&config->auto_select_regex, "")
        || !set_string(&config->no_match_auto_select_regex, "")
        || !set_string(&config->order_policy, "No")
        || !set_string(&config->hint_separator, "~ ")
        || !set_string(&config->candidate_layout_hint, "NotSet")
        || !set_string(&config->name, "")
        || !set_string(&config->label, "")
        || !set_string(&config->icon, "")
        || !set_string(&config->lang_code, "")
        || !set_string(&config->addon, "")
        || !set_string(&config->matching_key, "")
        || !set_string(&config->pinyin_key, "")
        || !set_string(&config->quick_phrase_key, "")
        || !set_string(&config->quick_phrase_text, "")
        || !set_default_key_list(
            &config->prev_page,
            prev_page,
            sizeof(prev_page) / sizeof(prev_page[0]),
            &ignored
        )
        || !set_default_key_list(
            &config->next_page,
            next_page,
            sizeof(next_page) / sizeof(next_page[0]),
            &ignored
        )
        || !set_default_key_list(
            &config->prev_candidate,
            prev_candidate,
            sizeof(prev_candidate) / sizeof(prev_candidate[0]),
            &ignored
        )
        || !set_default_key_list(
            &config->next_candidate,
            next_candidate,
            sizeof(next_candidate) / sizeof(next_candidate[0]),
            &ignored
        )
        || !set_default_key_list(
            &config->default_candidate,
            default_candidate,
            sizeof(default_candidate) / sizeof(default_candidate[0]),
            &ignored
        )
        || !set_default_key_list(
            &config->selection,
            selection,
            sizeof(selection) / sizeof(selection[0]),
            &ignored
        )
    ) {
        table_config_destroy(config);
        return NULL;
    }
    return config;
}

struct table_config *table_config_load(
    const char *path,
    struct table_config_error *error
) {
    struct table_config *config;
    struct ini_file ini = {0};
    char *content;
    size_t size;
    char *locale;
    size_t line = 0;
    const char *value;
    bool ok = false;

    set_error(error, TABLE_CONFIG_ERROR_NONE, 0, NULL);
    if (path == NULL || *path == '\0') {
        set_error(error, TABLE_CONFIG_ERROR_INVALID_ARGUMENT, 0, NULL);
        return NULL;
    }

    content = read_file(path, &size, error);
    if (content == NULL) {
        return NULL;
    }
    if (!parse_ini(content, size, &ini, error)) {
        free(content);
        goto out;
    }
    free(content);

    config = table_config_new();
    if (config == NULL) {
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        goto out;
    }
    config->path = strdup(path);
    if (config->path == NULL) {
        table_config_destroy(config);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        goto out;
    }

    locale = locale_name();
    if (locale == NULL) {
        table_config_destroy(config);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        goto out;
    }

    value = ini_get_localized(&ini, "InputMethod", "Name", locale, &line);
    if (value != NULL && !set_string(&config->name, value)) {
        free(locale);
        table_config_destroy(config);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        goto out;
    }
    value = ini_get_localized(&ini, "InputMethod", "Label", locale, &line);
    if (value != NULL && !set_string(&config->label, value)) {
        free(locale);
        table_config_destroy(config);
        set_error(error, TABLE_CONFIG_ERROR_OUT_OF_MEMORY, 0, NULL);
        goto out;
    }
    if (!set_string_if_present(
        &ini,
        "InputMethod",
        "Icon",
        &config->icon,
        error
    ) || !set_string_if_present(
        &ini,
        "InputMethod",
        "LangCode",
        &config->lang_code,
        error
    ) || !set_string_if_present(
        &ini,
        "InputMethod",
        "Addon",
        &config->addon,
        error
    )) {
        free(locale);
        table_config_destroy(config);
        goto out;
    }
    value = ini_get(&ini, "InputMethod", "Configurable", &line);
    if (
        value != NULL
        && !parse_bool(
            value,
            &config->configurable,
            error,
            line,
            "Configurable"
        )
    ) {
        free(locale);
        table_config_destroy(config);
        goto out;
    }
    free(locale);

    if (!set_string_if_present(&ini, "Table", "File", &config->file, error)
        || !set_int_if_present(
            &ini,
            "PageSize",
            0,
            10,
            &config->page_size,
            error
        )
        || !set_bool_if_present(
            &ini,
            "AutoSelect",
            &config->auto_select,
            error
        )
        || !set_int_if_present(
            &ini,
            "AutoSelectLength",
            -1,
            INT_MAX,
            &config->auto_select_length,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "AutoSelectRegex",
            &config->auto_select_regex,
            error
        )
        || !set_int_if_present(
            &ini,
            "NoMatchAutoSelectLength",
            -1,
            INT_MAX,
            &config->no_match_auto_select_length,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "NoMatchAutoSelectRegex",
            &config->no_match_auto_select_regex,
            error
        )
        || !set_bool_if_present(
            &ini,
            "ExactMatch",
            &config->exact_match,
            error
        )
        || !set_bool_if_present(
            &ini,
            "CommitAfterSelect",
            &config->commit_after_select,
            error
        )
        || !set_bool_if_present(
            &ini,
            "CommitWhenDeactivate",
            &config->commit_when_deactivate,
            error
        )
        || !set_bool_if_present(
            &ini,
            "CommitInvalidSegment",
            &config->commit_invalid_segment,
            error
        )
        || !set_bool_if_present(
            &ini,
            "UseFullWidth",
            &config->use_full_width,
            error
        )
        || !set_bool_if_present(
            &ini,
            "IgnorePunc",
            &config->ignore_punc,
            error
        )
        || !set_bool_if_present(
            &ini,
            "FirstCandidateAsPreedit",
            &config->first_candidate_as_preedit,
            error
        )
        || !set_int_if_present(
            &ini,
            "NoSortInputLength",
            0,
            INT_MAX,
            &config->no_sort_input_length,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "OrderPolicy",
            &config->order_policy,
            error
        )
        || !set_bool_if_present(
            &ini,
            "UseSystemLanguageModel",
            &config->use_system_language_model,
            error
        )
        || !set_bool_if_present(
            &ini,
            "UseContextRelatedOrder",
            &config->use_context_related_order,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "MatchingKey",
            &config->matching_key,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "PinyinKey",
            &config->pinyin_key,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "QuickPhraseKey",
            &config->quick_phrase_key,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "QuickPhraseText",
            &config->quick_phrase_text,
            error
        )
        || !set_int_if_present(
            &ini,
            "AutoPhraseLength",
            -1,
            INT_MAX,
            &config->auto_phrase_length,
            error
        )
        || !set_int_if_present(
            &ini,
            "SaveAutoPhraseAfter",
            -1,
            INT_MAX,
            &config->save_auto_phrase_after,
            error
        )
        || !set_bool_if_present(
            &ini,
            "Learning",
            &config->learning,
            error
        )
        || !set_bool_if_present(&ini, "Hint", &config->hint, error)
        || !set_bool_if_present(
            &ini,
            "DisplayCustomHint",
            &config->display_custom_hint,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "HintSeparator",
            &config->hint_separator,
            error
        )
        || !set_bool_if_present(
            &ini,
            "SpaceBeforeHint",
            &config->space_before_hint,
            error
        )
        || !set_string_if_present(
            &ini,
            "Table",
            "CandidateLayoutHint",
            &config->candidate_layout_hint,
            error
        )
        || !set_bool_if_present(
            &ini,
            "KeypadAsInput",
            &config->keypad_as_input,
            error
        )) {
        table_config_destroy(config);
        goto out;
    }

    size_t order_line = 0;
    size_t layout_line = 0;
    size_t auto_regex_line = 0;
    size_t no_match_regex_line = 0;
    ini_get(&ini, "Table", "OrderPolicy", &order_line);
    ini_get(&ini, "Table", "CandidateLayoutHint", &layout_line);
    ini_get(&ini, "Table", "AutoSelectRegex", &auto_regex_line);
    ini_get(&ini, "Table", "NoMatchAutoSelectRegex", &no_match_regex_line);

    if (
        !validate_order_policy(config->order_policy, error, order_line)
        || !validate_layout_hint(
            config->candidate_layout_hint,
            error,
            layout_line
        )
        || !validate_regex(
            config->auto_select_regex,
            error,
            auto_regex_line,
            "AutoSelectRegex"
        )
        || !validate_regex(
            config->no_match_auto_select_regex,
            error,
            no_match_regex_line,
            "NoMatchAutoSelectRegex"
        )
    ) {
        table_config_destroy(config);
        goto out;
    }

    if (
        !load_key_section(&ini, "Table/PrevPage", &config->prev_page, error)
        || !load_key_section(&ini, "Table/NextPage", &config->next_page, error)
        || !load_key_section(
            &ini,
            "Table/PrevCandidate",
            &config->prev_candidate,
            error
        )
        || !load_key_section(
            &ini,
            "Table/NextCandidate",
            &config->next_candidate,
            error
        )
        || !load_key_section(
            &ini,
            "Table/DefaultCandidate",
            &config->default_candidate,
            error
        )
        || !load_key_section(
            &ini,
            "Table/SecondCandidate",
            &config->second_candidate,
            error
        )
        || !load_key_section(
            &ini,
            "Table/ThirdCandidate",
            &config->third_candidate,
            error
        )
        || !load_key_section(&ini, "Table/Selection", &config->selection, error)
    ) {
        table_config_destroy(config);
        goto out;
    }

    *error = (struct table_config_error){0};
    ok = true;

out:
    for (size_t index = 0; index < ini.count; index++) {
        free(ini.entries[index].section);
        free(ini.entries[index].key);
        free(ini.entries[index].value);
    }
    free(ini.entries);
    return ok ? config : NULL;
}

void table_config_destroy(struct table_config *config) {
    if (config == NULL) {
        return;
    }
    free(config->path);
    free(config->name);
    free(config->label);
    free(config->icon);
    free(config->lang_code);
    free(config->addon);
    free(config->file);
    free(config->auto_select_regex);
    free(config->no_match_auto_select_regex);
    free(config->order_policy);
    free(config->matching_key);
    free(config->pinyin_key);
    free(config->quick_phrase_key);
    free(config->quick_phrase_text);
    free(config->hint_separator);
    free(config->candidate_layout_hint);
    clear_key_list(&config->prev_page);
    clear_key_list(&config->next_page);
    clear_key_list(&config->prev_candidate);
    clear_key_list(&config->next_candidate);
    clear_key_list(&config->default_candidate);
    clear_key_list(&config->second_candidate);
    clear_key_list(&config->third_candidate);
    clear_key_list(&config->selection);
    free(config);
}

const char *table_config_error_message(enum table_config_error_code code) {
    switch (code) {
    case TABLE_CONFIG_ERROR_NONE:
        return "no error";
    case TABLE_CONFIG_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case TABLE_CONFIG_ERROR_OPEN:
        return "cannot open file";
    case TABLE_CONFIG_ERROR_READ:
        return "cannot read file";
    case TABLE_CONFIG_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case TABLE_CONFIG_ERROR_INVALID_UTF8:
        return "invalid UTF-8";
    case TABLE_CONFIG_ERROR_INVALID_SYNTAX:
        return "invalid configuration syntax";
    case TABLE_CONFIG_ERROR_INVALID_VALUE:
        return "invalid configuration value";
    }
    return "unknown error";
}
