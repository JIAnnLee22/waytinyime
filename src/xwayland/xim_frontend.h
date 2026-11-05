#ifndef WAYTINYIME_XIM_FRONTEND_H
#define WAYTINYIME_XIM_FRONTEND_H

#include <stdbool.h>

struct code_table;
struct input_mode;
struct table_config;
struct xim_frontend;

typedef void (*xim_frontend_focus_handler)(void *data);

struct xim_frontend *xim_frontend_create(
    const struct code_table *table,
    const struct table_config *config,
    struct input_mode *mode
);

void xim_frontend_destroy(struct xim_frontend *frontend);
void xim_frontend_reset(struct xim_frontend *frontend);
void xim_frontend_set_focus_handler(
    struct xim_frontend *frontend,
    xim_frontend_focus_handler handler,
    void *data
);

int xim_frontend_get_fd(const struct xim_frontend *frontend);
bool xim_frontend_dispatch(struct xim_frontend *frontend);
const char *xim_frontend_name(const struct xim_frontend *frontend);

#endif
