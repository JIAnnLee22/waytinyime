#define _POSIX_C_SOURCE 200809L

#include "table_source.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "code_table.h"
#include "libime_bridge.h"
#include "table_config.h"
#include "waytinyime-config.h"

struct table_source {
    struct table_config *config;
    struct code_table *table;
    char *config_path;
    char *table_path;
    bool binary;
    size_t emoji_entries;
};

static void set_error(
    struct table_source_error *error,
    enum table_source_error_code code,
    const char *path,
    size_t line,
    const char *detail
) {
    if (error == NULL) {
        return;
    }
    error->code = code;
    error->line = line;
    if (path != NULL) {
        snprintf(error->path, sizeof(error->path), "%s", path);
    } else {
        error->path[0] = '\0';
    }
    if (detail != NULL) {
        snprintf(error->detail, sizeof(error->detail), "%s", detail);
    } else {
        error->detail[0] = '\0';
    }
}

static bool path_is_readable(const char *path) {
    return path != NULL && *path != '\0' && access(path, R_OK) == 0;
}

static char *join_path(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool needs_slash = left_length != 0 && left[left_length - 1] != '/';
    char *result = malloc(left_length + (needs_slash ? 1 : 0) + right_length + 1);

    if (result == NULL) {
        return NULL;
    }
    memcpy(result, left, left_length);
    size_t offset = left_length;
    if (needs_slash) {
        result[offset++] = '/';
    }
    memcpy(result + offset, right, right_length);
    result[offset + right_length] = '\0';
    return result;
}

static char *xdg_data_home(void) {
    const char *value = getenv("XDG_DATA_HOME");

    if (value != NULL && *value != '\0') {
        return strdup(value);
    }
    const char *home = getenv("HOME");
    if (home == NULL || *home == '\0') {
        return NULL;
    }
    return join_path(home, ".local/share");
}

static char *search_data_file(
    const char *relative,
    struct table_source_error *error
) {
    const char *explicit_dir = getenv("WAYTINYIME_FCITX_DIR");
    char *candidate = NULL;

    if (explicit_dir != NULL && *explicit_dir != '\0') {
        candidate = join_path(explicit_dir, relative);
        if (candidate == NULL) {
            set_error(
                error,
                TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
                NULL,
                0,
                NULL
            );
            return NULL;
        }
        if (path_is_readable(candidate)) {
            return candidate;
        }
        free(candidate);
        candidate = NULL;
    }

    char *home = xdg_data_home();
    if (home != NULL) {
        char *fcitx_home = join_path(home, "fcitx5");
        free(home);
        if (fcitx_home == NULL) {
            set_error(
                error,
                TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
                NULL,
                0,
                NULL
            );
            return NULL;
        }
        candidate = join_path(fcitx_home, relative);
        free(fcitx_home);
        if (candidate == NULL) {
            set_error(
                error,
                TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
                NULL,
                0,
                NULL
            );
            return NULL;
        }
        if (path_is_readable(candidate)) {
            return candidate;
        }
        free(candidate);
        candidate = NULL;
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (data_dirs == NULL || *data_dirs == '\0') {
        data_dirs = "/usr/local/share:/usr/share";
    }
    char *copy = strdup(data_dirs);
    if (copy == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        return NULL;
    }
    char *save = NULL;
    for (char *directory = strtok_r(copy, ":", &save); directory != NULL;
         directory = strtok_r(NULL, ":", &save)) {
        if (*directory == '\0') {
            continue;
        }
        char *fcitx_dir = join_path(directory, "fcitx5");
        if (fcitx_dir == NULL) {
            free(copy);
            set_error(
                error,
                TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
                NULL,
                0,
                NULL
            );
            return NULL;
        }
        candidate = join_path(fcitx_dir, relative);
        free(fcitx_dir);
        if (candidate == NULL) {
            free(copy);
            set_error(
                error,
                TABLE_SOURCE_ERROR_OUT_OF_MEMORY,
                NULL,
                0,
                NULL
            );
            return NULL;
        }
        if (path_is_readable(candidate)) {
            free(copy);
            return candidate;
        }
        free(candidate);
        candidate = NULL;
    }
    free(copy);

    set_error(
        error,
        TABLE_SOURCE_ERROR_NOT_FOUND,
        relative,
        0,
        "no matching file in the fcitx5 data directories"
    );
    return NULL;
}

static char *resolve_table_path(
    const char *file,
    struct table_source_error *error
) {
    if (file == NULL || *file == '\0') {
        set_error(
            error,
            TABLE_SOURCE_ERROR_CONFIG,
            NULL,
            0,
            "the [Table] File= entry is missing or empty"
        );
        return NULL;
    }
    if (file[0] == '/') {
        if (path_is_readable(file)) {
            return strdup(file);
        }
        set_error(
            error,
            TABLE_SOURCE_ERROR_NOT_FOUND,
            file,
            0,
            "configured table file does not exist"
        );
        return NULL;
    }
    return search_data_file(file, error);
}

static bool file_looks_binary(const char *path) {
    static const unsigned char main_magic[] = {0x00, 0x0f, 0xca, 0xbe};
    static const unsigned char user_magic[] = {0x35, 0x6f, 0xca, 0xbe};
    static const unsigned char extra_magic[] = {0x6b, 0x0f, 0xca, 0xbe};
    unsigned char header[4] = {0};
    FILE *file = fopen(path, "rb");
    size_t read_size;
    size_t length = strlen(path);

    if (file != NULL) {
        read_size = fread(header, 1, sizeof(header), file);
        fclose(file);
        if (read_size == sizeof(header)) {
            if (
                memcmp(header, main_magic, sizeof(main_magic)) == 0
                || memcmp(header, user_magic, sizeof(user_magic)) == 0
                || memcmp(header, extra_magic, sizeof(extra_magic)) == 0
            ) {
                return true;
            }
        }
    }

    return (
        length >= 5
        && strcasecmp(path + length - 5, ".dict") == 0
    );
}

static bool path_has_suffix(const char *path, const char *suffix) {
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);

    return (
        path_length >= suffix_length
        && strcasecmp(path + path_length - suffix_length, suffix) == 0
    );
}

static void report_code_table_error(
    struct table_source_error *error,
    const char *path,
    const struct code_table_error *table_error
) {
    char detail[512];

    if (table_error->system_error != 0) {
        snprintf(
            detail,
            sizeof(detail),
            "%s: %s",
            code_table_error_message(table_error->code),
            strerror(table_error->system_error)
        );
    } else {
        snprintf(
            detail,
            sizeof(detail),
            "%s",
            code_table_error_message(table_error->code)
        );
    }
    set_error(
        error,
        TABLE_SOURCE_ERROR_TABLE,
        path,
        table_error->line,
        detail
    );
}

static bool add_emoji_overlay(
    struct table_source *source,
    struct table_source_error *error
) {
    const char *path = path_is_readable(WAYTINYIME_EMOJI_PATH)
        ? WAYTINYIME_EMOJI_PATH
        : WAYTINYIME_BUILD_EMOJI_PATH;
    struct code_table_error table_error;
    struct code_table *overlay = code_table_load(path, &table_error);
    bool result;

    if (overlay == NULL) {
        report_code_table_error(error, path, &table_error);
        return false;
    }
    const struct code_table_entry *entries = code_table_entries(overlay);
    size_t count = code_table_entry_count(overlay);
    const char *code = entries[0].code;
    bool single_family = strlen(code) == 2
        && code[0] >= 'a' && code[0] <= 'z'
        && code[1] >= 'a' && code[1] <= 'z';
    uint32_t categories = 0;
    for (size_t index = 1; index < count && single_family; index++) {
        const char *entry = entries[index].code;
        size_t length = strlen(entry);
        single_family = strcmp(entry, code) == 0
            || (length == 3 && strncmp(entry, code, 2) == 0
                && entry[2] >= 'a' && entry[2] <= 'z');
        if (single_family && length == 3) {
            categories |= UINT32_C(1) << (entry[2] - 'a');
        }
    }
    if (!single_family || categories != (UINT32_C(1) << 26) - 1) {
        code_table_destroy(overlay);
        set_error(
            error, TABLE_SOURCE_ERROR_TABLE, path, 0,
            "the Emoji catalogue needs one two-letter entry and its a-z categories"
        );
        return false;
    }
    /* A conflicting root disables the whole family; never introduce category
     * aliases when the sole entry itself belongs to the original dictionary. */
    if (code_table_lookup(source->table, code, NULL, 0) != 0) {
        code_table_destroy(overlay);
        return true;
    }
    result = code_table_overlay_missing(
        source->table, overlay, &source->emoji_entries, &table_error
    );
    code_table_destroy(overlay);
    if (!result) {
        report_code_table_error(error, path, &table_error);
    }
    return result;
}

static bool load_table_into_source(
    struct table_source *source,
    const char *path,
    struct table_source_error *error
) {
    struct code_table_error table_error;
    bool binary = file_looks_binary(path);

    if (path_has_suffix(path, ".user.dict") || path_has_suffix(path, ".history")) {
        set_error(
            error,
            TABLE_SOURCE_ERROR_UNSUPPORTED,
            path,
            0,
            "refusing to load personal user dictionaries or history files"
        );
        return false;
    }

    if (binary) {
        unsigned char header[4] = {0};
        FILE *file = fopen(path, "rb");
        size_t read_size = 0;
        char *decoded = NULL;
        size_t decoded_size = 0;
        char *libime_error = NULL;

        if (file != NULL) {
            read_size = fread(header, 1, sizeof(header), file);
            fclose(file);
        }
        if (
            read_size == sizeof(header)
            && header[0] == 0x35
            && header[1] == 0x6f
            && header[2] == 0xca
            && header[3] == 0xbe
        ) {
            set_error(
                error,
                TABLE_SOURCE_ERROR_UNSUPPORTED,
                path,
                0,
                "refusing to load a libime user dictionary"
            );
            return false;
        }

        if (
            waytinyime_libime_decode_table(
                path,
                &decoded,
                &decoded_size,
                &libime_error
            )
            != 0
        ) {
            set_error(
                error,
                TABLE_SOURCE_ERROR_LIBIME,
                path,
                0,
                libime_error != NULL
                    ? libime_error
                    : "libime could not decode this binary table"
            );
            free(libime_error);
            return false;
        }

        source->table = code_table_load_memory(
            decoded,
            decoded_size,
            &table_error
        );
        free(decoded);
        free(libime_error);
        if (source->table == NULL) {
            report_code_table_error(error, path, &table_error);
            return false;
        }
        source->binary = true;
    } else {
        source->table = code_table_load(path, &table_error);
        if (source->table == NULL) {
            report_code_table_error(error, path, &table_error);
            return false;
        }
        source->binary = false;
    }
    return add_emoji_overlay(source, error);
}

struct table_source *table_source_load_config(
    const char *config_path,
    struct table_source_error *error
) {
    struct table_config_error config_error;
    struct table_source *source;
    char *table_path;

    set_error(error, TABLE_SOURCE_ERROR_NONE, NULL, 0, NULL);
    if (config_path == NULL || *config_path == '\0') {
        set_error(
            error,
            TABLE_SOURCE_ERROR_INVALID_ARGUMENT,
            config_path,
            0,
            NULL
        );
        return NULL;
    }

    source = calloc(1, sizeof(*source));
    if (source == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        return NULL;
    }
    source->config = table_config_load(config_path, &config_error);
    if (source->config == NULL) {
        set_error(
            error,
            TABLE_SOURCE_ERROR_CONFIG,
            config_path,
            config_error.line,
            config_error.detail[0] != '\0'
                ? config_error.detail
                : table_config_error_message(config_error.code)
        );
        table_source_destroy(source);
        return NULL;
    }
    source->config_path = strdup(config_path);
    if (source->config_path == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        table_source_destroy(source);
        return NULL;
    }

    table_path = resolve_table_path(source->config->file, error);
    if (table_path == NULL) {
        table_source_destroy(source);
        return NULL;
    }
    source->table_path = table_path;
    if (!load_table_into_source(source, table_path, error)) {
        table_source_destroy(source);
        return NULL;
    }
    return source;
}

struct table_source *table_source_load_table(
    const char *table_path,
    struct table_source_error *error
) {
    struct table_source *source;

    set_error(error, TABLE_SOURCE_ERROR_NONE, NULL, 0, NULL);
    if (table_path == NULL || *table_path == '\0') {
        set_error(
            error,
            TABLE_SOURCE_ERROR_INVALID_ARGUMENT,
            table_path,
            0,
            NULL
        );
        return NULL;
    }
    if (!path_is_readable(table_path)) {
        set_error(
            error,
            TABLE_SOURCE_ERROR_NOT_FOUND,
            table_path,
            0,
            "table file does not exist or is not readable"
        );
        return NULL;
    }

    source = calloc(1, sizeof(*source));
    if (source == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        return NULL;
    }
    source->table_path = strdup(table_path);
    if (source->table_path == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        table_source_destroy(source);
        return NULL;
    }
    if (!load_table_into_source(source, table_path, error)) {
        table_source_destroy(source);
        return NULL;
    }
    return source;
}

struct table_source *table_source_load_default(
    const char *input_method,
    struct table_source_error *error
) {
    const char *explicit_config = getenv("WAYTINYIME_CONF");
    struct table_source *source;

    set_error(error, TABLE_SOURCE_ERROR_NONE, NULL, 0, NULL);
    if (input_method == NULL || *input_method == '\0') {
        input_method = "flypy";
    }
    if (explicit_config != NULL && *explicit_config != '\0') {
        return table_source_load_config(explicit_config, error);
    }

    char *relative = NULL;
    size_t name_length = strlen(input_method);
    relative = malloc(name_length + sizeof("inputmethod/.conf"));
    if (relative == NULL) {
        set_error(error, TABLE_SOURCE_ERROR_OUT_OF_MEMORY, NULL, 0, NULL);
        return NULL;
    }
    snprintf(
        relative,
        name_length + sizeof("inputmethod/.conf"),
        "inputmethod/%s.conf",
        input_method
    );
    char *config_path = search_data_file(relative, error);
    free(relative);
    if (config_path != NULL) {
        source = table_source_load_config(config_path, error);
        free(config_path);
        return source;
    }
    if (error != NULL && error->code != TABLE_SOURCE_ERROR_NOT_FOUND) {
        return NULL;
    }

    /* No fcitx5 configuration: keep the built-in text table usable. */
    source = table_source_load_table(WAYTINYIME_TABLE_PATH, error);
    if (
        source == NULL
        && error != NULL
        && error->code == TABLE_SOURCE_ERROR_NOT_FOUND
        && WAYTINYIME_BUILD_TABLE_PATH[0] != '\0'
    ) {
        struct table_source_error build_error;
        source = table_source_load_table(
            WAYTINYIME_BUILD_TABLE_PATH,
            &build_error
        );
        if (source == NULL && error != NULL) {
            *error = build_error;
        }
    }
    return source;
}

void table_source_destroy(struct table_source *source) {
    if (source == NULL) {
        return;
    }
    table_config_destroy(source->config);
    code_table_destroy(source->table);
    free(source->config_path);
    free(source->table_path);
    free(source);
}

const struct table_config *table_source_config(
    const struct table_source *source
) {
    return source == NULL ? NULL : source->config;
}

const struct code_table *table_source_table(
    const struct table_source *source
) {
    return source == NULL ? NULL : source->table;
}

const char *table_source_config_path(const struct table_source *source) {
    return source == NULL ? NULL : source->config_path;
}

const char *table_source_table_path(const struct table_source *source) {
    return source == NULL ? NULL : source->table_path;
}

size_t table_source_emoji_entries(const struct table_source *source) {
    return source == NULL ? 0 : source->emoji_entries;
}

bool table_source_table_is_binary(const struct table_source *source) {
    return source != NULL && source->binary;
}

const char *table_source_error_message(enum table_source_error_code code) {
    switch (code) {
    case TABLE_SOURCE_ERROR_NONE:
        return "no error";
    case TABLE_SOURCE_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case TABLE_SOURCE_ERROR_NOT_FOUND:
        return "file not found";
    case TABLE_SOURCE_ERROR_CONFIG:
        return "cannot load fcitx5 table configuration";
    case TABLE_SOURCE_ERROR_TABLE:
        return "cannot load code table";
    case TABLE_SOURCE_ERROR_LIBIME:
        return "cannot decode libime binary table";
    case TABLE_SOURCE_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case TABLE_SOURCE_ERROR_UNSUPPORTED:
        return "unsupported table file";
    }
    return "unknown error";
}
