#ifndef WAYTINYIME_XIM_FRONTEND_H
#define WAYTINYIME_XIM_FRONTEND_H

#include <stdbool.h>

struct code_table;
struct xim_frontend;
struct input_mode;

struct xim_frontend *xim_frontend_create(
    const struct code_table *table,
    struct input_mode *mode
);
void xim_frontend_destroy(struct xim_frontend *frontend);
int xim_frontend_get_fd(const struct xim_frontend *frontend);
const char *xim_frontend_name(const struct xim_frontend *frontend);
bool xim_frontend_dispatch(struct xim_frontend *frontend);

#endif
