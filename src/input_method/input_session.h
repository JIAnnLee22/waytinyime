#ifndef WAYTINYIME_INPUT_SESSION_H
#define WAYTINYIME_INPUT_SESSION_H

#include <stdbool.h>
#include <stdint.h>

struct code_table;
struct input_session;
struct input_mode;
struct wl_compositor;
struct wl_shm;
struct zwp_input_method_v2;
struct zwp_virtual_keyboard_v1;

struct input_session *input_session_create(
    const struct code_table *table,
    struct input_mode *mode,
    struct wl_compositor *compositor,
    struct wl_shm *shm,
    struct zwp_input_method_v2 *input_method,
    struct zwp_virtual_keyboard_v1 *virtual_keyboard
);

void input_session_destroy(struct input_session *session);
bool input_session_activate(struct input_session *session);
void input_session_deactivate(struct input_session *session);
void input_session_set_serial(struct input_session *session, uint32_t serial);

#endif
