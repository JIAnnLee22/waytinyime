#include "input_session.h"

#include <stdlib.h>
#include <string.h>

#include "candidate_popup.h"
#include "input-method-v2.h"
#include "input_processor.h"
#include "keyboard_grab.h"

struct input_session {
    struct input_processor processor;
    struct candidate_popup *popup;
    struct input_keyboard *keyboard;
    struct zwp_input_method_v2 *input_method;
    struct zwp_virtual_keyboard_v1 *virtual_keyboard;
    uint32_t serial;
};

static void session_updated(
    void *data,
    const struct input_engine *engine
) {
    struct input_session *session = data;
    const char *code = input_engine_code(engine);
    size_t code_length = strlen(code);

    zwp_input_method_v2_set_preedit_string(
        session->input_method,
        code,
        (int32_t)code_length,
        (int32_t)code_length
    );
    zwp_input_method_v2_commit(session->input_method, session->serial);
    if (input_engine_page_size(engine) == 0) {
        candidate_popup_hide(session->popup);
        return;
    }
    size_t page = input_engine_page(engine);
    size_t page_size = input_engine_page_size(engine);
    size_t total = input_engine_total_candidate_count(engine);
    size_t offset = page * page_size;
    size_t remaining = total > offset ? total - offset : 0;

    candidate_popup_update(
        session->popup,
        code,
        code_table_label(engine->table, code),
        engine->candidates,
        input_engine_candidate_count(engine),
        input_engine_cursor(engine),
        remaining
    );
}

static void session_committed(void *data, const char *text) {
    struct input_session *session = data;

    zwp_input_method_v2_commit_string(session->input_method, text);
    zwp_input_method_v2_set_preedit_string(session->input_method, "", 0, 0);
    zwp_input_method_v2_commit(session->input_method, session->serial);
    candidate_popup_hide(session->popup);
}

static void session_cancelled(void *data) {
    struct input_session *session = data;

    zwp_input_method_v2_set_preedit_string(session->input_method, "", 0, 0);
    zwp_input_method_v2_commit(session->input_method, session->serial);
    candidate_popup_hide(session->popup);
}

static const struct input_processor_listener processor_listener = {
    .updated = session_updated,
    .committed = session_committed,
    .cancelled = session_cancelled,
};

static uint32_t processor_modifiers(uint32_t keyboard_modifiers) {
    uint32_t modifiers = 0;

    if (keyboard_modifiers & INPUT_KEYBOARD_MODIFIER_CONTROL) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_CONTROL;
    }
    if (keyboard_modifiers & INPUT_KEYBOARD_MODIFIER_ALT) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_ALT;
    }
    if (keyboard_modifiers & INPUT_KEYBOARD_MODIFIER_LOGO) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_LOGO;
    }
    if (keyboard_modifiers & INPUT_KEYBOARD_MODIFIER_SHIFT) {
        modifiers |= INPUT_PROCESSOR_MODIFIER_SHIFT;
    }
    return modifiers;
}

static bool handle_key(
    void *data,
    uint32_t time,
    uint32_t key,
    xkb_keysym_t symbol,
    uint32_t modifiers
) {
    struct input_session *session = data;

    (void)time;
    (void)key;
    return input_processor_handle_key(
        &session->processor,
        symbol,
        processor_modifiers(modifiers)
    );
}

struct input_session *input_session_create(
    const struct code_table *table,
    const struct table_config *config,
    struct input_mode *mode,
    struct wl_compositor *compositor,
    struct wl_shm *shm,
    struct zwp_input_method_v2 *input_method,
    struct zwp_virtual_keyboard_v1 *virtual_keyboard
) {
    struct input_session *session;

    if (
        table == NULL
        || mode == NULL
        || compositor == NULL
        || shm == NULL
        || input_method == NULL
        || virtual_keyboard == NULL
    ) {
        return NULL;
    }

    session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->input_method = input_method;
    session->virtual_keyboard = virtual_keyboard;
    if (
        !input_processor_init(
            &session->processor,
            table,
            mode,
            &processor_listener,
            session
        )
    ) {
        free(session);
        return NULL;
    }
    input_processor_set_config(&session->processor, config);

    session->popup = candidate_popup_create(compositor, shm, input_method);
    if (session->popup == NULL) {
        input_session_destroy(session);
        return NULL;
    }
    return session;
}

void input_session_destroy(struct input_session *session) {
    if (session == NULL) {
        return;
    }

    input_keyboard_destroy(session->keyboard);
    candidate_popup_destroy(session->popup);
    input_processor_destroy(&session->processor);
    free(session);
}

bool input_session_activate(struct input_session *session) {
    if (session == NULL) {
        return false;
    }
    if (session->keyboard != NULL) {
        return true;
    }

    session->keyboard = input_keyboard_create(
        session->input_method,
        session->virtual_keyboard,
        handle_key,
        session
    );
    return session->keyboard != NULL;
}

void input_session_deactivate(struct input_session *session) {
    if (session == NULL) {
        return;
    }

    input_processor_reset(&session->processor);
    candidate_popup_hide(session->popup);
}

void input_session_cancel(struct input_session *session) {
    if (session == NULL) {
        return;
    }

    if (*input_engine_code(&session->processor.engine) != '\0') {
        input_processor_cancel(&session->processor);
    } else {
        input_processor_reset(&session->processor);
        candidate_popup_hide(session->popup);
    }
}

void input_session_set_serial(struct input_session *session, uint32_t serial) {
    if (session != NULL) {
        session->serial = serial;
    }
}
