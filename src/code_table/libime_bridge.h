#ifndef WAYTINYIME_LIBIME_BRIDGE_H
#define WAYTINYIME_LIBIME_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a libime binary table (.dict) into the official UTF-8 text format.
 * The caller owns *text (free with free()) and *error_message when non-NULL.
 * Returns 0 on success and -1 on failure.
 */
int waytinyime_libime_decode_table(
    const char *path,
    char **text,
    size_t *size,
    char **error_message
);

#ifdef __cplusplus
}
#endif

#endif
