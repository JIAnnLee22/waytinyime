#ifndef WAYTINYIME_INPUT_ENGINE_H
#define WAYTINYIME_INPUT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

#include "code_table.h"

#define INPUT_ENGINE_MAX_CANDIDATES 9

struct input_engine {
    const struct code_table *table;
    char *code;
    size_t code_length;
    size_t code_capacity;
    const struct code_table_entry *candidates[INPUT_ENGINE_MAX_CANDIDATES];
    size_t candidate_count;
    size_t total_candidate_count;
};

bool input_engine_init(
    struct input_engine *engine,
    const struct code_table *table
);
void input_engine_destroy(struct input_engine *engine);
void input_engine_reset(struct input_engine *engine);
bool input_engine_append(struct input_engine *engine, char key);
bool input_engine_backspace(struct input_engine *engine);

const char *input_engine_code(const struct input_engine *engine);
size_t input_engine_candidate_count(const struct input_engine *engine);
size_t input_engine_total_candidate_count(const struct input_engine *engine);
const struct code_table_entry *input_engine_candidate(
    const struct input_engine *engine,
    size_t index
);

#endif
