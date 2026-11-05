#include "xim_frontend.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imdkit.h>
#include <xcb-imdkit/ximproto.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "input_processor.h"
#include "xim_window.h"

struct xim_frontend {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t server_window;
    xcb_im_t *im;
    xcb_key_symbols_t *key_symbols;
    xcb_im_input_context_t *current_ic;
    xcb_window_t watched_window;
    xcb_window_t watched_client_window;
    struct input_processor processor;
    struct xim_window *window;
    xim_frontend_focus_handler focus_handler;
    void *focus_handler_data;
    char server_name[64];
    uint32_t style;
    size_t preedit_length;
    int spot_x;
    int spot_y;
    bool have_spot;
    bool spot_dirty;
    bool preedit_started;
    bool composing;
    bool im_open;
};

static bool debug_enabled(void) {
    const char *value = getenv("WAYTINYIME_DEBUG");

    return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

static void debug_log(const char *format, ...) {
    va_list arguments;

    if (!debug_enabled()) {
        return;
    }
    va_start(arguments, format);
    fputs("waytinyime: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number) {
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(
        xcb_get_setup(connection)
    );

    while (iterator.rem > 0 && number-- > 0) {
        xcb_screen_next(&iterator);
    }
    return iterator.rem == 0 ? NULL : iterator.data;
}

static bool preedit_callbacks(const struct xim_frontend *frontend) {
    return (frontend->style & XCB_IM_PreeditCallbacks) != 0;
}

static bool style_has_spot(const struct xim_frontend *frontend) {
    return (
        frontend->style & XCB_IM_PreeditPosition
    ) != 0;
}

static void send_preedit_done(struct xim_frontend *frontend) {
    if (!frontend->preedit_started) {
        return;
    }
    if (frontend->current_ic != NULL) {
        xcb_im_preedit_done_callback(frontend->im, frontend->current_ic);
    }
    frontend->preedit_started = false;
    frontend->preedit_length = 0;
}

static void send_preedit_update(
    struct xim_frontend *frontend,
    const char *code
) {
    size_t code_length;
    size_t compound_length;
    char *compound_text;
    xcb_im_preedit_draw_fr_t draw;
    xcb_im_preedit_caret_fr_t caret;

    if (
        !preedit_callbacks(frontend)
        || frontend->current_ic == NULL
        || code == NULL
        || *code == '\0'
    ) {
        return;
    }
    if (!frontend->preedit_started) {
        xcb_im_preedit_start_callback(frontend->im, frontend->current_ic);
        frontend->preedit_started = true;
    }

    code_length = strlen(code);
    compound_text = xcb_utf8_to_compound_text(
        code,
        code_length,
        &compound_length
    );
    if (compound_text == NULL || compound_length > UINT16_MAX) {
        free(compound_text);
        return;
    }

    memset(&draw, 0, sizeof(draw));
    draw.caret = (uint32_t)code_length;
    draw.chg_first = 0;
    draw.chg_length = (uint32_t)frontend->preedit_length;
    draw.length_of_preedit_string = (uint16_t)compound_length;
    draw.preedit_string = (uint8_t *)compound_text;
    xcb_im_preedit_draw_callback(
        frontend->im,
        frontend->current_ic,
        &draw
    );
    free(compound_text);
    frontend->preedit_length = code_length;

    memset(&caret, 0, sizeof(caret));
    caret.position = (uint32_t)code_length;
    xcb_im_preedit_caret_callback(
        frontend->im,
        frontend->current_ic,
        &caret
    );
}

static bool spot_from_ic(
    struct xim_frontend *frontend,
    int *root_x,
    int *root_y
) {
    const xcb_im_preedit_attr_t *attributes;
    uint32_t mask;
    xcb_window_t client_window;
    xcb_window_t focus_window;
    xcb_window_t source;
    xcb_translate_coordinates_reply_t *reply;

    if (frontend->current_ic == NULL) {
        return false;
    }
    attributes = xcb_im_input_context_get_preedit_attr(
        frontend->current_ic
    );
    mask = xcb_im_input_context_get_preedit_attr_mask(
        frontend->current_ic
    );
    if (
        attributes == NULL
        || (mask & XCB_XIM_XNSpotLocation_MASK) == 0
    ) {
        if (frontend->have_spot) {
            *root_x = frontend->spot_x;
            *root_y = frontend->spot_y;
            return true;
        }
        return false;
    }

    client_window = xcb_im_input_context_get_client_window(
        frontend->current_ic
    );
    focus_window = xcb_im_input_context_get_focus_window(
        frontend->current_ic
    );
    source = focus_window != XCB_WINDOW_NONE ? focus_window : client_window;
    if (source == XCB_WINDOW_NONE) {
        return false;
    }
    reply = xcb_translate_coordinates_reply(
        frontend->connection,
        xcb_translate_coordinates(
            frontend->connection,
            source,
            frontend->screen->root,
            attributes->spot_location.x,
            attributes->spot_location.y
        ),
        NULL
    );
    if (reply == NULL) {
        return false;
    }

    *root_x = reply->dst_x;
    *root_y = reply->dst_y;
    free(reply);
    if (
        !frontend->have_spot
        || frontend->spot_x != *root_x
        || frontend->spot_y != *root_y
    ) {
        debug_log(
            "XIM spot root=(%d,%d) from client spot=(%d,%d) style=0x%x",
            *root_x,
            *root_y,
            attributes->spot_location.x,
            attributes->spot_location.y,
            frontend->style
        );
    }
    frontend->spot_x = *root_x;
    frontend->spot_y = *root_y;
    frontend->have_spot = true;
    return true;
}

static void refresh_window(struct xim_frontend *frontend) {
    const struct input_engine *engine = &frontend->processor.engine;
    const char *code = input_engine_code(engine);
    int root_x;
    int root_y;

    if (code == NULL || *code == '\0') {
        xim_window_hide(frontend->window);
        frontend->spot_dirty = false;
        return;
    }
    if (!spot_from_ic(frontend, &root_x, &root_y)) {
        xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(
            frontend->connection,
            xcb_query_pointer(frontend->connection, frontend->screen->root),
            NULL
        );
        if (pointer != NULL) {
            root_x = pointer->root_x;
            root_y = pointer->root_y;
            free(pointer);
            debug_log(
                "XIM IC has no spot location; using pointer fallback "
                "root=(%d,%d) style=0x%x",
                root_x,
                root_y,
                frontend->style
            );
        } else {
            root_x = (int)frontend->screen->width_in_pixels / 2;
            root_y = 100;
            debug_log(
                "XIM IC has no spot location and no pointer; "
                "using screen fallback (%d,%d)",
                root_x,
                root_y
            );
        }
    }
    if (!style_has_spot(frontend)) {
        debug_log(
            "XIM style 0x%x does not use PreeditPosition; using %s",
            frontend->style,
            frontend->have_spot ? "last known spot" : "pointer fallback"
        );
    }

    debug_log(
        "XIM candidate root=(%d,%d) code='%s' page=%zu count=%zu "
        "total=%zu cursor=%zu",
        root_x,
        root_y,
        code,
        input_engine_page(engine),
        input_engine_candidate_count(engine),
        input_engine_total_candidate_count(engine),
        input_engine_cursor(engine)
    );
    xim_window_update(
        frontend->window,
        root_x,
        root_y,
        code,
        code_table_label(engine->table, code),
        engine->candidates,
        input_engine_candidate_count(engine),
        input_engine_cursor(engine),
        input_engine_total_candidate_count(engine)
    );
    frontend->spot_dirty = false;
}

static void processor_updated(
    void *data,
    const struct input_engine *engine
) {
    struct xim_frontend *frontend = data;
    const char *code = input_engine_code(engine);

    if (code == NULL || *code == '\0') {
        send_preedit_done(frontend);
        frontend->composing = false;
        xim_window_hide(frontend->window);
        return;
    }

    frontend->composing = true;
    send_preedit_update(frontend, code);
    refresh_window(frontend);
}

static void processor_committed(void *data, const char *text) {
    struct xim_frontend *frontend = data;
    size_t compound_length;
    char *compound_text;

    send_preedit_done(frontend);
    frontend->composing = false;
    xim_window_hide(frontend->window);

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
    struct xim_frontend *frontend = data;

    debug_log("XIM composition cancelled");
    send_preedit_done(frontend);
    frontend->composing = false;
    xim_window_hide(frontend->window);
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

    uint32_t modifiers = event_modifiers(event);
    bool consumed = input_processor_handle_key(
        &frontend->processor,
        symbol,
        modifiers
    );

    debug_log(
        "XIM key symbol=0x%x mods=0x%x consumed=%d code='%s'",
        symbol,
        modifiers,
        consumed ? 1 : 0,
        input_engine_code(&frontend->processor.engine)
    );
    return consumed;
}

static void notify_focus(struct xim_frontend *frontend) {
    if (frontend->focus_handler != NULL) {
        frontend->focus_handler(frontend->focus_handler_data);
    }
}

static xcb_window_t toplevel_window(
    struct xim_frontend *frontend,
    xcb_window_t window
) {
    xcb_window_t current = window;

    while (current != XCB_WINDOW_NONE) {
        xcb_query_tree_reply_t *reply = xcb_query_tree_reply(
            frontend->connection,
            xcb_query_tree(frontend->connection, current),
            NULL
        );
        xcb_window_t parent;

        if (reply == NULL) {
            return current;
        }
        parent = reply->parent;
        free(reply);
        if (
            parent == XCB_WINDOW_NONE
            || parent == frontend->screen->root
        ) {
            return current;
        }
        current = parent;
    }
    return window;
}

static void watch_ic_window(struct xim_frontend *frontend) {
    xcb_window_t focus_window;
    xcb_window_t client_window;
    uint32_t structure_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    uint32_t focus_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY
        | XCB_EVENT_MASK_FOCUS_CHANGE;

    if (frontend->current_ic == NULL) {
        return;
    }
    focus_window = xcb_im_input_context_get_focus_window(
        frontend->current_ic
    );
    client_window = xcb_im_input_context_get_client_window(
        frontend->current_ic
    );
    if (focus_window == XCB_WINDOW_NONE) {
        focus_window = client_window;
    }
    /*
     * XIM gives xterm's VT100 child as both the client and focus window.
     * Watch the focus window for FocusOut, but watch the top-level ancestor
     * for StructureNotify: moving the toplevel does not send ConfigureNotify
     * to the child even though the child's root-relative position changes.
     */
    xcb_window_t toplevel = toplevel_window(frontend, client_window);
    if (
        toplevel != XCB_WINDOW_NONE
        && toplevel != frontend->watched_client_window
    ) {
        xcb_change_window_attributes(
            frontend->connection,
            toplevel,
            XCB_CW_EVENT_MASK,
            &structure_mask
        );
        frontend->watched_client_window = toplevel;
        debug_log("XIM watching toplevel window 0x%x", toplevel);
    }
    if (
        focus_window != XCB_WINDOW_NONE
        && focus_window != frontend->watched_window
    ) {
        xcb_change_window_attributes(
            frontend->connection,
            focus_window,
            XCB_CW_EVENT_MASK,
            &focus_mask
        );
        frontend->watched_window = focus_window;
        debug_log("XIM watching focus window 0x%x", focus_window);
    }
    xcb_flush(frontend->connection);
}

static void reset_composition(struct xim_frontend *frontend) {
    debug_log(
        "XIM reset composition composing=%d code='%s'",
        frontend->composing ? 1 : 0,
        input_engine_code(&frontend->processor.engine)
    );
    if (frontend->composing) {
        input_processor_cancel(&frontend->processor);
    } else {
        input_processor_reset(&frontend->processor);
        send_preedit_done(frontend);
        xim_window_hide(frontend->window);
    }
    frontend->composing = false;
    frontend->spot_dirty = false;
}

static void update_ic(struct xim_frontend *frontend,
    xcb_im_input_context_t *ic) {
    if (ic == NULL) {
        return;
    }
    if (frontend->current_ic != ic) {
        frontend->current_ic = ic;
        frontend->preedit_started = false;
        frontend->preedit_length = 0;
        frontend->style = xcb_im_input_context_get_input_style(ic);
        debug_log("XIM IC style=0x%x", frontend->style);
    }
    watch_ic_window(frontend);
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
        /*
         * Cancel a composition belonging to the previously focused IC
         * before switching, so PREEDIT_DONE is sent to the right client.
         * Then make the new IC current and clear any stale state.
         */
        if (
            frontend->current_ic != NULL
            && frontend->current_ic != ic
        ) {
            reset_composition(frontend);
        }
        frontend->current_ic = ic;
        if (ic != NULL) {
            frontend->style = xcb_im_input_context_get_input_style(ic);
        }
        reset_composition(frontend);
        watch_ic_window(frontend);
        notify_focus(frontend);
        debug_log(
            "XIM focus in style=0x%x",
            frontend->style
        );
        return;
    }
    if (
        header->major_opcode == XCB_XIM_UNSET_IC_FOCUS
        || header->major_opcode == XCB_XIM_DESTROY_IC
    ) {
        if (frontend->current_ic == ic) {
            reset_composition(frontend);
            frontend->current_ic = NULL;
        }
        debug_log("XIM focus out style=0x%x", frontend->style);
        return;
    }
    if (header->major_opcode == XCB_XIM_SET_IC_VALUES) {
        if (frontend->current_ic == NULL) {
            update_ic(frontend, ic);
        }
        if (frontend->current_ic != ic) {
            return;
        }
        const xcb_im_preedit_attr_t *attributes =
            xcb_im_input_context_get_preedit_attr(ic);
        uint32_t mask = xcb_im_input_context_get_preedit_attr_mask(ic);
        debug_log(
            "XIM set ic values spot=(%d,%d) mask=0x%x style=0x%x",
            attributes == NULL ? -1 : attributes->spot_location.x,
            attributes == NULL ? -1 : attributes->spot_location.y,
            mask,
            frontend->style
        );
        frontend->spot_dirty = true;
        return;
    }
    if (header->major_opcode != XCB_XIM_FORWARD_EVENT || argument == NULL) {
        return;
    }

    xcb_key_press_event_t *event = argument;
    uint8_t event_type = event->response_type & 0x7f;
    bool consumed = false;

    update_ic(frontend, ic);
    notify_focus(frontend);
    if (event_type == XCB_KEY_PRESS) {
        consumed = handle_key_press(frontend, event);
    }

    if (!consumed) {
        xcb_im_forward_event(im, ic, event);
    }
}

struct xim_frontend *xim_frontend_create(
    const struct code_table *table,
    const struct table_config *config,
    struct input_mode *mode
) {
    static uint32_t styles_data[] = {
        XCB_IM_PreeditPosition | XCB_IM_StatusNothing,
        XCB_IM_PreeditCallbacks | XCB_IM_StatusNothing,
        XCB_IM_PreeditNothing | XCB_IM_StatusNothing,
        XCB_IM_PreeditNothing | XCB_IM_StatusNone,
    };
    static char *encoding_data[] = {
        "COMPOUND_TEXT",
    };
    static xcb_im_styles_t styles = {
        .nStyles = sizeof(styles_data) / sizeof(styles_data[0]),
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
    frontend->screen = screen;

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
    frontend->window = xim_window_create(frontend->connection, screen);
    if (
        frontend->key_symbols == NULL
        || frontend->window == NULL
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
    input_processor_set_config(&frontend->processor, config);

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

void xim_frontend_reset(struct xim_frontend *frontend) {
    if (frontend == NULL) {
        return;
    }
    reset_composition(frontend);
}

void xim_frontend_set_focus_handler(
    struct xim_frontend *frontend,
    xim_frontend_focus_handler handler,
    void *data
) {
    if (frontend == NULL) {
        return;
    }
    frontend->focus_handler = handler;
    frontend->focus_handler_data = data;
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
    xim_window_destroy(frontend->window);
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

static xcb_window_t event_window(
    xcb_generic_event_t *event,
    uint8_t type
) {
    switch (type) {
    case XCB_EXPOSE:
        return ((xcb_expose_event_t *)event)->window;
    case XCB_CONFIGURE_NOTIFY:
        return ((xcb_configure_notify_event_t *)event)->window;
    case XCB_FOCUS_OUT:
        return ((xcb_focus_out_event_t *)event)->event;
    case XCB_DESTROY_NOTIFY:
        return ((xcb_destroy_notify_event_t *)event)->window;
    default:
        return XCB_WINDOW_NONE;
    }
}

bool xim_frontend_dispatch(struct xim_frontend *frontend) {
    xcb_generic_event_t *event;

    if (frontend == NULL) {
        return true;
    }
    while ((event = xcb_poll_for_event(frontend->connection)) != NULL) {
        uint8_t type = event->response_type & 0x7f;
        xcb_window_t window = event_window(event, type);

        if (type == XCB_EXPOSE && window == xim_window_xid(frontend->window)) {
            xim_window_redraw(frontend->window);
        } else if (type == XCB_CONFIGURE_NOTIFY) {
            if (
                window == frontend->watched_window
                || window == frontend->watched_client_window
            ) {
                debug_log(
                    "XIM ConfigureNotify window=0x%x refreshes candidate spot",
                    window
                );
                frontend->spot_dirty = true;
            }
        } else if (
            type == XCB_FOCUS_OUT
            && window == frontend->watched_window
        ) {
            debug_log("XIM X FocusOut on 0x%x", window);
            reset_composition(frontend);
        } else if (
            type == XCB_DESTROY_NOTIFY
            && (
                window == frontend->watched_window
                || window == frontend->watched_client_window
            )
        ) {
            reset_composition(frontend);
            frontend->watched_window = XCB_WINDOW_NONE;
            frontend->watched_client_window = XCB_WINDOW_NONE;
        }

        xcb_im_filter_event(frontend->im, event);
        free(event);
        if (frontend->composing && frontend->spot_dirty) {
            refresh_window(frontend);
        }
    }
    xcb_flush(frontend->connection);
    return xcb_connection_has_error(frontend->connection) == 0;
}

const char *xim_frontend_name(const struct xim_frontend *frontend) {
    return frontend == NULL ? NULL : frontend->server_name;
}
