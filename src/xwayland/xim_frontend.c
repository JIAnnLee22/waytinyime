#include "xim_frontend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imdkit.h>
#include <xcb-imdkit/ximproto.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "input_processor.h"

struct xim_frontend {
    xcb_connection_t *connection;
    xcb_window_t server_window;
    xcb_im_t *im;
    xcb_key_symbols_t *key_symbols;
    xcb_im_input_context_t *current_ic;
    struct input_processor processor;
    char server_name[64];
    bool im_open;
};

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number) {
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(
        xcb_get_setup(connection)
    );

    while (iterator.rem > 0 && number-- > 0) {
        xcb_screen_next(&iterator);
    }
    return iterator.rem == 0 ? NULL : iterator.data;
}

static void processor_updated(
    void *data,
    const struct input_engine *engine
) {
    (void)data;
    (void)engine;
}

static void processor_committed(void *data, const char *text) {
    struct xim_frontend *frontend = data;
    size_t compound_length;
    char *compound_text;

    if (frontend->current_ic == NULL) {
        return;
    }

    compound_text = xcb_utf8_to_compound_text(
        text,
        strlen(text),
        &compound_length
    );
    if (compound_text == NULL || compound_length > UINT32_MAX) {
        free(compound_text);
        return;
    }

    xcb_im_commit_string(
        frontend->im,
        frontend->current_ic,
        XCB_XIM_LOOKUP_CHARS,
        compound_text,
        (uint32_t)compound_length,
        0
    );
    free(compound_text);
}

static void processor_cancelled(void *data) {
    (void)data;
}

static const struct input_processor_listener processor_listener = {
    .updated = processor_updated,
    .committed = processor_committed,
    .cancelled = processor_cancelled,
};

static uint32_t event_modifiers(const xcb_key_press_event_t *event) {
    uint32_t modifiers = 0;

    if (event->state & XCB_MOD_MASK_CONTROL) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_CONTROL;
    }
    if (event->state & XCB_MOD_MASK_1) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_ALT;
    }
    if (event->state & XCB_MOD_MASK_4) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_LOGO;
    }
    if (event->state & XCB_MOD_MASK_SHIFT) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_SHIFT;
    }
    return modifiers;
}

static bool handle_key_press(
    struct xim_frontend *frontend,
    xcb_key_press_event_t *event
) {
    int column = event->state & XCB_MOD_MASK_SHIFT ? 1 : 0;
    xcb_keysym_t symbol = xcb_key_symbols_get_keysym(
        frontend->key_symbols,
        event->detail,
        column
    );

    return input_processor_handle_key(
        &frontend->processor,
        symbol,
        event_modifiers(event)
    );
}

static void xim_callback(
    xcb_im_t *im,
    xcb_im_client_t *client,
    xcb_im_input_context_t *ic,
    const xcb_im_packet_header_fr_t *header,
    void *frame,
    void *argument,
    void *user_data
) {
    struct xim_frontend *frontend = user_data;
    (void)client;
    (void)frame;
    if (header->major_opcode == XCB_XIM_SET_IC_FOCUS) {
        frontend->current_ic = ic;
        input_processor_reset(&frontend->processor);
        return;
    }
    if (
        header->major_opcode == XCB_XIM_UNSET_IC_FOCUS
        || header->major_opcode == XCB_XIM_DESTROY_IC
    ) {
        if (frontend->current_ic == ic) {
            frontend->current_ic = NULL;
            input_processor_reset(&frontend->processor);
        }
        return;
    }
    if (header->major_opcode != XCB_XIM_FORWARD_EVENT || argument == NULL) {
        return;
    }

    xcb_key_press_event_t *event = argument;
    uint8_t event_type = event->response_type & 0x7f;
    bool consumed = false;

    frontend->current_ic = ic;
    if (event_type == XCB_KEY_PRESS) {
        consumed = handle_key_press(frontend, event);
    }

    if (!consumed) {
        xcb_im_forward_event(im, ic, event);
    }
}

struct xim_frontend *xim_frontend_create(
    const struct code_table *table,
    struct input_mode *mode
) {
    static uint32_t styles_data[] = {
        XCB_IM_PreeditNothing | XCB_IM_StatusNothing,
        XCB_IM_PreeditNothing | XCB_IM_StatusNone,
    };
    static char *encoding_data[] = {
        "COMPOUND_TEXT",
    };
    static xcb_im_styles_t styles = {
        .nStyles = 2,
        .styles = styles_data,
    };
    static xcb_im_encodings_t encodings = {
        .nEncodings = 1,
        .encodings = encoding_data,
    };
    struct xim_frontend *frontend;
    xcb_screen_t *screen;
    int screen_number;

    if (table == NULL || mode == NULL || getenv("DISPLAY") == NULL) {
        return NULL;
    }

    frontend = calloc(1, sizeof(*frontend));
    if (frontend == NULL) {
        return NULL;
    }
    const char *modifiers = getenv("XMODIFIERS");
    const char *server_name = "waytinyime";

    if (
        modifiers != NULL
        && strncmp(modifiers, "@im=", 4) == 0
        && modifiers[4] != '\0'
        && strlen(modifiers + 4) < sizeof(frontend->server_name)
    ) {
        server_name = modifiers + 4;
    }
    strcpy(frontend->server_name, server_name);
    frontend->connection = xcb_connect(NULL, &screen_number);
    if (
        frontend->connection == NULL
        || xcb_connection_has_error(frontend->connection) != 0
    ) {
        xim_frontend_destroy(frontend);
        return NULL;
    }
    screen = get_screen(frontend->connection, screen_number);
    if (screen == NULL) {
        xim_frontend_destroy(frontend);
        return NULL;
    }

    frontend->server_window = xcb_generate_id(frontend->connection);
    xcb_create_window(
        frontend->connection,
        XCB_COPY_FROM_PARENT,
        frontend->server_window,
        screen->root,
        0,
        0,
        1,
        1,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        0,
        NULL
    );
    frontend->key_symbols = xcb_key_symbols_alloc(frontend->connection);
    if (
        frontend->key_symbols == NULL
        || !input_processor_init(
            &frontend->processor,
            table,
            mode,
            &processor_listener,
            frontend
        )
    ) {
        xim_frontend_destroy(frontend);
        return NULL;
    }

    xcb_compound_text_init();
    frontend->im = xcb_im_create(
        frontend->connection,
        screen_number,
        frontend->server_window,
        frontend->server_name,
        XCB_IM_ALL_LOCALES,
        &styles,
        NULL,
        NULL,
        &encodings,
        0,
        xim_callback,
        frontend
    );
    if (frontend->im == NULL || !xcb_im_open_im(frontend->im)) {
        xim_frontend_destroy(frontend);
        return NULL;
    }
    frontend->im_open = true;
    xcb_flush(frontend->connection);
    return frontend;
}

void xim_frontend_destroy(struct xim_frontend *frontend) {
    if (frontend == NULL) {
        return;
    }

    if (frontend->im != NULL) {
        if (frontend->im_open) {
            xcb_im_close_im(frontend->im);
        }
        xcb_im_destroy(frontend->im);
    }
    input_processor_destroy(&frontend->processor);
    if (frontend->key_symbols != NULL) {
        xcb_key_symbols_free(frontend->key_symbols);
    }
    if (
        frontend->connection != NULL
        && frontend->server_window != XCB_WINDOW_NONE
    ) {
        xcb_destroy_window(frontend->connection, frontend->server_window);
    }
    if (frontend->connection != NULL) {
        xcb_disconnect(frontend->connection);
    }
    free(frontend);
}

int xim_frontend_get_fd(const struct xim_frontend *frontend) {
    return frontend == NULL ? -1 : xcb_get_file_descriptor(frontend->connection);
}

bool xim_frontend_dispatch(struct xim_frontend *frontend) {
    xcb_generic_event_t *event;

    if (frontend == NULL) {
        return true;
    }
    while ((event = xcb_poll_for_event(frontend->connection)) != NULL) {
        xcb_im_filter_event(frontend->im, event);
        free(event);
    }
    xcb_flush(frontend->connection);
    return xcb_connection_has_error(frontend->connection) == 0;
}

const char *xim_frontend_name(const struct xim_frontend *frontend) {
    return frontend == NULL ? NULL : frontend->server_name;
}
