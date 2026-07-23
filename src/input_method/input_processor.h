#ifndef WAYTINYIME_INPUT_PROCESSOR_H
#define WAYTINYIME_INPUT_PROCESSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "input_engine.h"
enum input_processor_modifier {
    INPUT_PROCESSOR_MODIFIER_CONTROL = 1 << 0,
    INPUT_PROCESSOR_MODIFIER_ALT = 1 << 1,
    INPUT_PROCESSOR_MODIFIER_LOGO = 1 << 2,
    INPUT_PROCESSOR_MODIFIER_SHIFT = 1 << 3,
};

struct input_mode {
    bool chinese;
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
};

bool input_processor_init(
    struct input_processor *processor,
    const struct code_table *table,
    struct input_mode *mode,
    const struct input_processor_listener *listener,
    void *listener_data
);
void input_processor_destroy(struct input_processor *processor);
void input_processor_reset(struct input_processor *processor);
bool input_processor_handle_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
);

#endif
