#ifndef WAYTINYIME_INPUT_PROCESSOR_H
#define WAYTINYIME_INPUT_PROCESSOR_H

#include <stdbool.h>
#include <stdint.h>
#include <regex.h>

#include "input_engine.h"
#include "table_config.h"

enum input_processor_modifier {
    INPUT_PROCESSOR_MODIFIER_CONTROL = 1 << 0,
    INPUT_PROCESSOR_MODIFIER_ALT = 1 << 1,
    INPUT_PROCESSOR_MODIFIER_LOGO = 1 << 2,
    INPUT_PROCESSOR_MODIFIER_SHIFT = 1 << 3,
};

struct input_mode {
    bool chinese;
    bool single_quote_open;
    bool double_quote_open;
};

struct input_processor;

struct input_processor_listener {
    void (*updated)(void *data, const struct input_engine *engine);
    void (*committed)(void *data, const char *text);
    void (*cancelled)(void *data);
};

struct input_processor {
    struct input_engine engine;
    struct input_mode *mode;
    const struct input_processor_listener *listener;
    void *listener_data;
    const struct table_config *config;
    regex_t auto_select_regex;
    bool auto_select_regex_valid;
    regex_t no_match_regex;
    bool no_match_regex_valid;
};

bool input_processor_init(
    struct input_processor *processor,
    const struct code_table *table,
    struct input_mode *mode,
    const struct input_processor_listener *listener,
    void *listener_data
);

/*
 * Apply the fcitx5 table settings that affect key handling.  Passing NULL
 * restores the built-in defaults (9 candidates per page, Up/Down paging,
 * 1..9/0 selection, no auto select).
 */
void input_processor_set_config(
    struct input_processor *processor,
    const struct table_config *config
);

void input_processor_destroy(struct input_processor *processor);
void input_processor_reset(struct input_processor *processor);
void input_processor_cancel(struct input_processor *processor);
bool input_processor_handle_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
);

#endif
