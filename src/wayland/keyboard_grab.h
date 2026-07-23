#ifndef WAYTINYIME_KEYBOARD_GRAB_H
#define WAYTINYIME_KEYBOARD_GRAB_H

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct input_keyboard;
struct zwp_input_method_v2;
struct zwp_virtual_keyboard_v1;

enum input_keyboard_modifier {
    INPUT_KEYBOARD_MODIFIER_CONTROL = 1 << 0,
    INPUT_KEYBOARD_MODIFIER_ALT = 1 << 1,
    INPUT_KEYBOARD_MODIFIER_LOGO = 1 << 2,
    INPUT_KEYBOARD_MODIFIER_SHIFT = 1 << 3,
};

typedef bool (*input_keyboard_key_handler)(
    void *data,
    uint32_t time,
    uint32_t key,
    xkb_keysym_t symbol,
    uint32_t modifiers
);

struct input_keyboard *input_keyboard_create(
    struct zwp_input_method_v2 *input_method,
    struct zwp_virtual_keyboard_v1 *virtual_keyboard,
    input_keyboard_key_handler key_handler,
    void *key_handler_data
);

void input_keyboard_destroy(struct input_keyboard *keyboard);

#endif
