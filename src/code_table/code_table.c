#include "code_table.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct code_table {
    char *storage;
    const char *key_codes;
    size_t max_code_length;
    size_t entry_count;
    struct code_table_entry entries[];
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

static char *trim_ascii_whitespace(char *text) {
    char *end;

    while (*text == ' ' || *text == '\t') {
        text++;
    }

    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    return text;
}

static int parse_code_length(const char *text, size_t *length) {
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
        return 0;
    }

    *length = (size_t)value;
    return 1;
}

static int code_uses_key_codes(const char *code, const char *key_codes) {
    for (const char *character = code; *character != '\0'; character++) {
        if (strchr(key_codes, *character) == NULL) {
            return 0;
        }
    }

    return 1;
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

struct code_table *code_table_load(
    const char *path,
    struct code_table_error *error
) {
    struct code_table *table = NULL;
    char *storage;
    char *cursor;
    char *storage_end;
    size_t storage_size;
    size_t line_capacity = 1;
    size_t line_number = 0;
    int in_data_section = 0;

    set_error(error, CODE_TABLE_ERROR_NONE, 0, 0);
    if (path == NULL || *path == '\0') {
        set_error(error, CODE_TABLE_ERROR_INVALID_ARGUMENT, 0, 0);
        return NULL;
    }

    storage = read_file(path, &storage_size, error);
    if (storage == NULL) {
        return NULL;
    }

    for (size_t index = 0; index < storage_size; index++) {
        if (storage[index] == '\n') {
            line_capacity++;
        }
    }
    if (
        line_capacity
        > (SIZE_MAX - sizeof(struct code_table))
            / sizeof(struct code_table_entry)
    ) {
        free(storage);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }

    table = calloc(
        1,
        sizeof(struct code_table)
            + line_capacity * sizeof(struct code_table_entry)
    );
    if (table == NULL) {
        free(storage);
        set_error(error, CODE_TABLE_ERROR_OUT_OF_MEMORY, 0, 0);
        return NULL;
    }
    table->storage = storage;

    cursor = storage;
    storage_end = storage + storage_size;
    while (cursor < storage_end) {
        char *line = cursor;
        char *newline = memchr(cursor, '\n', (size_t)(storage_end - cursor));
        char *line_end = newline == NULL ? storage_end : newline;
        char *separator;
        char *text;
        size_t code_length;

        line_number++;
        cursor = newline == NULL ? storage_end : newline + 1;
        if (line_end > line && line_end[-1] == '\r') {
            line_end--;
        }
        *line_end = '\0';

        if (
            line_number == 1
            && line_end - line >= 3
            && (unsigned char)line[0] == 0xef
            && (unsigned char)line[1] == 0xbb
            && (unsigned char)line[2] == 0xbf
        ) {
            line += 3;
        }
        line = trim_ascii_whitespace(line);
        if (*line == '\0' || *line == ';') {
            continue;
        }

        if (!in_data_section) {
            if (strcmp(line, "[数据]") == 0) {
                if (table->key_codes == NULL) {
                    set_error(
                        error,
                        CODE_TABLE_ERROR_MISSING_KEY_CODES,
                        line_number,
                        0
                    );
                    goto fail;
                }
                if (table->max_code_length == 0) {
                    set_error(
                        error,
                        CODE_TABLE_ERROR_MISSING_CODE_LENGTH,
                        line_number,
                        0
                    );
                    goto fail;
                }
                in_data_section = 1;
                continue;
            }

            if (strncmp(line, "键码=", strlen("键码=")) == 0) {
                table->key_codes = trim_ascii_whitespace(
                    line + strlen("键码=")
                );
                if (*table->key_codes == '\0') {
                    set_error(
                        error,
                        CODE_TABLE_ERROR_INVALID_KEY_CODES,
                        line_number,
                        0
                    );
                    goto fail;
                }
                continue;
            }

            if (strncmp(line, "码长=", strlen("码长=")) == 0) {
                text = trim_ascii_whitespace(line + strlen("码长="));
                if (!parse_code_length(text, &table->max_code_length)) {
                    set_error(
                        error,
                        CODE_TABLE_ERROR_INVALID_CODE_LENGTH,
                        line_number,
                        0
                    );
                    goto fail;
                }
            }
            continue;
        }

        separator = strpbrk(line, " \t");
        if (separator == NULL) {
            set_error(
                error,
                CODE_TABLE_ERROR_INVALID_ENTRY,
                line_number,
                0
            );
            goto fail;
        }
        *separator++ = '\0';
        while (*separator == ' ' || *separator == '\t') {
            separator++;
        }
        text = separator;
        code_length = strlen(line);
        if (code_length == 0 || *text == '\0') {
            set_error(
                error,
                CODE_TABLE_ERROR_INVALID_ENTRY,
                line_number,
                0
            );
            goto fail;
        }
        if (
            code_length > table->max_code_length
            || !code_uses_key_codes(line, table->key_codes)
        ) {
            set_error(
                error,
                CODE_TABLE_ERROR_INVALID_CODE,
                line_number,
                0
            );
            goto fail;
        }

        table->entries[table->entry_count++] = (struct code_table_entry){
            .code = line,
            .text = text,
        };
    }

    if (!in_data_section) {
        set_error(
            error,
            CODE_TABLE_ERROR_MISSING_DATA_SECTION,
            line_number,
            0
        );
        goto fail;
    }
    if (table->entry_count == 0) {
        set_error(error, CODE_TABLE_ERROR_EMPTY_DATA, line_number, 0);
        goto fail;
    }

    return table;

fail:
    code_table_destroy(table);
    return NULL;
}

void code_table_destroy(struct code_table *table) {
    if (table == NULL) {
        return;
    }

    free(table->storage);
    free(table);
}

const char *code_table_key_codes(const struct code_table *table) {
    return table == NULL ? NULL : table->key_codes;
}

size_t code_table_max_code_length(const struct code_table *table) {
    return table == NULL ? 0 : table->max_code_length;
}

size_t code_table_entry_count(const struct code_table *table) {
    return table == NULL ? 0 : table->entry_count;
}

const struct code_table_entry *code_table_entries(
    const struct code_table *table
) {
    return table == NULL ? NULL : table->entries;
}

size_t code_table_lookup(
    const struct code_table *table,
    const char *code,
    const struct code_table_entry **matches,
    size_t capacity
) {
    size_t match_count = 0;

    if (table == NULL || code == NULL || (capacity != 0 && matches == NULL)) {
        return 0;
    }

    for (size_t index = 0; index < table->entry_count; index++) {
        if (strcmp(table->entries[index].code, code) != 0) {
            continue;
        }
        if (match_count < capacity) {
            matches[match_count] = &table->entries[index];
        }
        match_count++;
    }

    return match_count;
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
    case CODE_TABLE_ERROR_MISSING_KEY_CODES:
        return "missing key codes";
    case CODE_TABLE_ERROR_INVALID_KEY_CODES:
        return "invalid key codes";
    case CODE_TABLE_ERROR_MISSING_CODE_LENGTH:
        return "missing maximum code length";
    case CODE_TABLE_ERROR_INVALID_CODE_LENGTH:
        return "invalid maximum code length";
    case CODE_TABLE_ERROR_MISSING_DATA_SECTION:
        return "missing data section";
    case CODE_TABLE_ERROR_EMPTY_DATA:
        return "data section is empty";
    case CODE_TABLE_ERROR_INVALID_ENTRY:
        return "invalid data entry";
    case CODE_TABLE_ERROR_INVALID_CODE:
        return "invalid entry code";
    }

    return "unknown error";
}
