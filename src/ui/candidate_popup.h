#ifndef WAYTINYIME_CANDIDATE_POPUP_H
#define WAYTINYIME_CANDIDATE_POPUP_H

#include <stdbool.h>
#include <stddef.h>

struct candidate_popup;
struct code_table_entry;
struct wl_compositor;
struct wl_shm;
struct zwp_input_method_v2;

struct candidate_popup *candidate_popup_create(
    struct wl_compositor *compositor,
    struct wl_shm *shm,
    struct zwp_input_method_v2 *input_method
);

void candidate_popup_destroy(struct candidate_popup *popup);
bool candidate_popup_update(
    struct candidate_popup *popup,
    const char *code,
    const char *label,
    const struct code_table_entry *const *candidates,
    size_t candidate_count,
    size_t cursor,
    size_t total_candidate_count
);
void candidate_popup_hide(struct candidate_popup *popup);

#endif
