#include "input_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t effective_page_size(const struct input_engine *engine) {
    if (engine->page_size > INPUT_ENGINE_MAX_CANDIDATES) {
        return INPUT_ENGINE_MAX_CANDIDATES;
    }
    return engine->page_size;
}

static size_t page_count(const struct input_engine *engine) {
    size_t page_size = effective_page_size(engine);

    if (page_size == 0 || engine->total_candidate_count == 0) {
        return 0;
    }
    return (engine->total_candidate_count + page_size - 1) / page_size;
}

static void update_candidates(struct input_engine *engine) {
    size_t page_size = effective_page_size(engine);
    size_t skip = engine->page * page_size;
    size_t stored_count = 0;

    engine->total_candidate_count = engine->prefix_match
        ? code_table_lookup_prefix_page(
            engine->table,
            engine->code,
            skip,
            engine->candidates,
            page_size
        )
        : code_table_lookup_page(
            engine->table,
            engine->code,
            skip,
            engine->candidates,
            page_size
        );
    if (engine->total_candidate_count > skip) {
        stored_count = engine->total_candidate_count - skip;
        if (stored_count > page_size) {
            stored_count = page_size;
        }
    }
    engine->candidate_count = stored_count;
}

bool input_engine_init(
    struct input_engine *engine,
    const struct code_table *table
) {
    size_t max_code_length;

    if (engine == NULL || table == NULL) {
        return false;
    }

    *engine = (struct input_engine){0};
    max_code_length = code_table_max_code_length(table);
    if (max_code_length == 0 || max_code_length == SIZE_MAX) {
        return false;
    }

    engine->code = calloc(max_code_length + 1, 1);
    if (engine->code == NULL) {
        return false;
    }
    engine->table = table;
    engine->code_capacity = max_code_length;
    engine->page_size = INPUT_ENGINE_DEFAULT_PAGE_SIZE;
    return true;
}

void input_engine_destroy(struct input_engine *engine) {
    if (engine == NULL) {
        return;
    }

    free(engine->code);
    *engine = (struct input_engine){0};
}

void input_engine_reset(struct input_engine *engine) {
    if (engine == NULL || engine->code == NULL) {
        return;
    }

    engine->code[0] = '\0';
    engine->code_length = 0;
    engine->candidate_count = 0;
    engine->total_candidate_count = 0;
    engine->page = 0;
    engine->cursor = 0;
}

bool input_engine_append(struct input_engine *engine, char key) {
    const char *key_codes;

    if (engine == NULL || engine->code == NULL || key == '\0') {
        return false;
    }

    key_codes = code_table_key_codes(engine->table);
    if (
        key != CODE_TABLE_WILDCARD
        && (key_codes == NULL || strchr(key_codes, key) == NULL)
    ) {
        return false;
    }

    /* A matched full-length code ends a table segment, except while editing
     * a wildcard query. An unmatched code may instead be an English word,
     * so the dictionary's maximum length must not truncate or split it. */
    if (
        engine->code_length >= code_table_max_code_length(engine->table)
        && engine->total_candidate_count != 0
        && key != CODE_TABLE_WILDCARD
        && strchr(engine->code, CODE_TABLE_WILDCARD) == NULL
    ) {
        return false;
    }
    if (engine->code_length >= engine->code_capacity) {
        size_t capacity = engine->code_capacity <= (SIZE_MAX - 1) / 2
            ? engine->code_capacity * 2
            : SIZE_MAX - 1;
        char *code;

        if (capacity <= engine->code_length) {
            return false;
        }
        code = realloc(engine->code, capacity + 1);
        if (code == NULL) {
            return false;
        }
        engine->code = code;
        engine->code_capacity = capacity;
    }

    engine->code[engine->code_length++] = key;
    engine->code[engine->code_length] = '\0';
    engine->page = 0;
    engine->cursor = 0;
    update_candidates(engine);
    return true;
}

bool input_engine_backspace(struct input_engine *engine) {
    if (engine == NULL || engine->code == NULL || engine->code_length == 0) {
        return false;
    }

    engine->code[--engine->code_length] = '\0';
    engine->page = 0;
    engine->cursor = 0;
    if (engine->code_length == 0) {
        engine->candidate_count = 0;
        engine->total_candidate_count = 0;
    } else {
        update_candidates(engine);
    }
    return true;
}

const char *input_engine_code(const struct input_engine *engine) {
    return engine == NULL || engine->code == NULL ? "" : engine->code;
}

size_t input_engine_candidate_count(const struct input_engine *engine) {
    return engine == NULL ? 0 : engine->candidate_count;
}

size_t input_engine_total_candidate_count(const struct input_engine *engine) {
    return engine == NULL ? 0 : engine->total_candidate_count;
}

const struct code_table_entry *input_engine_candidate(
    const struct input_engine *engine,
    size_t index
) {
    if (engine == NULL || index >= engine->candidate_count) {
        return NULL;
    }

    return engine->candidates[index];
}

void input_engine_set_page_size(struct input_engine *engine, size_t page_size) {
    if (engine == NULL) {
        return;
    }
    engine->page_size = page_size;
    engine->page = 0;
    engine->cursor = 0;
    if (engine->code_length != 0) {
        update_candidates(engine);
    } else {
        engine->candidate_count = 0;
        engine->total_candidate_count = 0;
    }
}

size_t input_engine_page_size(const struct input_engine *engine) {
    return engine == NULL ? 0 : engine->page_size;
}

void input_engine_set_prefix_match(
    struct input_engine *engine,
    bool prefix_match
) {
    if (engine == NULL || engine->prefix_match == prefix_match) {
        return;
    }
    engine->prefix_match = prefix_match;
    engine->page = 0;
    engine->cursor = 0;
    if (engine->code_length != 0) {
        update_candidates(engine);
    }
}

bool input_engine_prefix_match(const struct input_engine *engine) {
    return engine != NULL && engine->prefix_match;
}

size_t input_engine_page(const struct input_engine *engine) {
    return engine == NULL ? 0 : engine->page;
}

size_t input_engine_page_count(const struct input_engine *engine) {
    return engine == NULL ? 0 : page_count(engine);
}

bool input_engine_page_next(struct input_engine *engine) {
    if (engine == NULL || engine->page + 1 >= page_count(engine)) {
        return false;
    }
    engine->page++;
    engine->cursor = 0;
    update_candidates(engine);
    return true;
}

bool input_engine_page_previous(struct input_engine *engine) {
    if (engine == NULL || engine->page == 0) {
        return false;
    }
    engine->page--;
    engine->cursor = 0;
    update_candidates(engine);
    return true;
}

size_t input_engine_cursor(const struct input_engine *engine) {
    return engine == NULL ? 0 : engine->cursor;
}

/*
 * Move the candidate cursor across the whole candidate list, wrapping at
 * either end, and follow the cursor to the page that contains it.  This is
 * the behavior of fcitx5's CommonCandidateList::moveCursor for table input
 * methods (cursorKeepInSamePage is false).
 */
static bool move_candidate_cursor(struct input_engine *engine, bool previous) {
    size_t page_size = effective_page_size(engine);
    size_t total = engine->total_candidate_count;
    size_t global;
    size_t page;
    size_t cursor;

    if (page_size == 0 || total == 0) {
        return false;
    }
    global = engine->page * page_size + engine->cursor;
    if (global >= total) {
        global = total - 1;
    }
    if (previous) {
        global = global == 0 ? total - 1 : global - 1;
    } else {
        global = global + 1 == total ? 0 : global + 1;
    }
    page = global / page_size;
    cursor = global % page_size;
    if (page != engine->page) {
        engine->page = page;
        update_candidates(engine);
    }
    if (cursor >= engine->candidate_count) {
        return false;
    }
    engine->cursor = cursor;
    return true;
}

bool input_engine_candidate_next(struct input_engine *engine) {
    return engine != NULL && move_candidate_cursor(engine, false);
}

bool input_engine_candidate_previous(struct input_engine *engine) {
    return engine != NULL && move_candidate_cursor(engine, true);
}

size_t input_engine_candidate_global_index(
    const struct input_engine *engine,
    size_t index
) {
    if (engine == NULL || index >= engine->candidate_count) {
        return SIZE_MAX;
    }
    return engine->page * effective_page_size(engine) + index;
}
