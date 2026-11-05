#ifndef WAYTINYIME_XIM_WINDOW_H
#define WAYTINYIME_XIM_WINDOW_H

#include <stdbool.h>
#include <stddef.h>

#include <xcb/xcb.h>

struct code_table_entry;
struct xim_window;

struct xim_window *xim_window_create(
    xcb_connection_t *connection,
    xcb_screen_t *screen
);

void xim_window_destroy(struct xim_window *window);
bool xim_window_update(
    struct xim_window *window,
    int root_x,
    int root_y,
    const char *code,
    const char *label,
    bool show_code,
    const struct code_table_entry *const *candidates,
    size_t candidate_count,
    size_t cursor,
    size_t total_candidate_count
);
void xim_window_redraw(struct xim_window *window);
xcb_window_t xim_window_xid(const struct xim_window *window);
void xim_window_hide(struct xim_window *window);
bool xim_window_is_visible(const struct xim_window *window);

#endif
