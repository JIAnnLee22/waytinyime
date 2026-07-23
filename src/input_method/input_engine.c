#include "input_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void update_candidates(struct input_engine *engine) {
    size_t stored_count;

    engine->total_candidate_count = code_table_lookup(
        engine->table,
        engine->code,
        engine->candidates,
        INPUT_ENGINE_MAX_CANDIDATES
    );
    stored_count = engine->total_candidate_count;
    if (stored_count > INPUT_ENGINE_MAX_CANDIDATES) {
        stored_count = INPUT_ENGINE_MAX_CANDIDATES;
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
}

bool input_engine_append(struct input_engine *engine, char key) {
    const char *key_codes;

    if (
        engine == NULL
        || engine->code == NULL
        || engine->code_length >= engine->code_capacity
    ) {
        return false;
    }

    key_codes = code_table_key_codes(engine->table);
    if (key_codes == NULL || strchr(key_codes, key) == NULL) {
        return false;
    }

    engine->code[engine->code_length++] = key;
    engine->code[engine->code_length] = '\0';
    update_candidates(engine);
    return true;
}

bool input_engine_backspace(struct input_engine *engine) {
    if (engine == NULL || engine->code == NULL || engine->code_length == 0) {
        return false;
    }

    engine->code[--engine->code_length] = '\0';
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
