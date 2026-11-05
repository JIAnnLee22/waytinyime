#include "keyboard_grab.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "input-method-v2.h"
#include "virtual-keyboard-v1.h"

#define TRACKED_KEY_COUNT 768

struct input_keyboard {
    struct zwp_input_method_keyboard_grab_v2 *grab;
    struct zwp_virtual_keyboard_v1 *virtual_keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    input_keyboard_key_handler key_handler;
    void *key_handler_data;
    bool consumed_keys[TRACKED_KEY_COUNT];
};

static uint32_t effective_modifiers(const struct input_keyboard *keyboard) {
    uint32_t modifiers = 0;

    if (keyboard->xkb_state == NULL) {
        return 0;
    }
    if (
        xkb_state_mod_name_is_active(
            keyboard->xkb_state,
            XKB_MOD_NAME_SHIFT,
            XKB_STATE_MODS_EFFECTIVE
        ) > 0
    ) {
        modifiers |= INPUT_KEYBOARD_MODIFIER_SHIFT;
    }
    if (
        xkb_state_mod_name_is_active(
            keyboard->xkb_state,
            XKB_MOD_NAME_CTRL,
            XKB_STATE_MODS_EFFECTIVE
        ) > 0
    ) {
        modifiers |= INPUT_KEYBOARD_MODIFIER_CONTROL;
    }
    if (
        xkb_state_mod_name_is_active(
            keyboard->xkb_state,
            XKB_MOD_NAME_ALT,
            XKB_STATE_MODS_EFFECTIVE
        ) > 0
    ) {
        modifiers |= INPUT_KEYBOARD_MODIFIER_ALT;
    }
    if (
        xkb_state_mod_name_is_active(
            keyboard->xkb_state,
            XKB_MOD_NAME_LOGO,
            XKB_STATE_MODS_EFFECTIVE
        ) > 0
    ) {
        modifiers |= INPUT_KEYBOARD_MODIFIER_LOGO;
    }

    return modifiers;
}

static void keyboard_keymap(
    void *data,
    struct zwp_input_method_keyboard_grab_v2 *grab,
    uint32_t format,
    int32_t fd,
    uint32_t size
) {
    struct input_keyboard *keyboard = data;
    void *mapping = MAP_FAILED;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;

    (void)grab;
    /*
     * A zero-sized or non-XKB keymap cannot be forwarded: the virtual
     * keyboard protocol rejects it and wlroots disconnects the client with
     * "no memory".  Headless compositors can send such a keymap when no
     * physical keyboard is present, so keep the old state and ignore it.
     */
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    zwp_virtual_keyboard_v1_keymap(
        keyboard->virtual_keyboard,
        format,
        fd,
        size
    );

    mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping != MAP_FAILED) {
        keymap = xkb_keymap_new_from_buffer(
            keyboard->xkb_context,
            mapping,
            size,
            XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS
        );
        munmap(mapping, size);
    }
    if (keymap != NULL) {
        state = xkb_state_new(keymap);
    }
    close(fd);

    if (state == NULL) {
        xkb_keymap_unref(keymap);
        return;
    }

    xkb_state_unref(keyboard->xkb_state);
    xkb_keymap_unref(keyboard->xkb_keymap);
    keyboard->xkb_keymap = keymap;
    keyboard->xkb_state = state;
}

static void keyboard_key(
    void *data,
    struct zwp_input_method_keyboard_grab_v2 *grab,
    uint32_t serial,
    uint32_t time,
    uint32_t key,
    uint32_t state
) {
    struct input_keyboard *keyboard = data;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    bool consumed = false;
    xkb_keycode_t keycode = key + 8;
    xkb_keysym_t symbol = XKB_KEY_NoSymbol;

    (void)grab;
    (void)serial;

    if (pressed && keyboard->xkb_state != NULL) {
        xkb_state_update_key(keyboard->xkb_state, keycode, XKB_KEY_DOWN);
        symbol = xkb_state_key_get_one_sym(keyboard->xkb_state, keycode);
        consumed = keyboard->key_handler(
            keyboard->key_handler_data,
            time,
            key,
            symbol,
            effective_modifiers(keyboard)
        );
        if (key < TRACKED_KEY_COUNT) {
            keyboard->consumed_keys[key] = consumed;
        }
    } else if (!pressed) {
        if (key < TRACKED_KEY_COUNT) {
            consumed = keyboard->consumed_keys[key];
            keyboard->consumed_keys[key] = false;
        }
        if (keyboard->xkb_state != NULL) {
            xkb_state_update_key(keyboard->xkb_state, keycode, XKB_KEY_UP);
        }
    }

    if (!consumed) {
        zwp_virtual_keyboard_v1_key(
            keyboard->virtual_keyboard,
            time,
            key,
            state
        );
    }
}

static void keyboard_modifiers(
    void *data,
    struct zwp_input_method_keyboard_grab_v2 *grab,
    uint32_t serial,
    uint32_t mods_depressed,
    uint32_t mods_latched,
    uint32_t mods_locked,
    uint32_t group
) {
    struct input_keyboard *keyboard = data;

    (void)grab;
    (void)serial;
    if (keyboard->xkb_state != NULL) {
        xkb_state_update_mask(
            keyboard->xkb_state,
            mods_depressed,
            mods_latched,
            mods_locked,
            0,
            0,
            group
        );
    }
    /*
     * wlroots rejects modifier state before the virtual keyboard has a
     * keymap.  Some headless seats emit modifiers before the first keymap
     * event; drop those instead of disconnecting the Wayland connection.
     */
    if (keyboard->xkb_keymap != NULL) {
        zwp_virtual_keyboard_v1_modifiers(
            keyboard->virtual_keyboard,
            mods_depressed,
            mods_latched,
            mods_locked,
            group
        );
    }
}

static void keyboard_repeat_info(
    void *data,
    struct zwp_input_method_keyboard_grab_v2 *grab,
    int32_t rate,
    int32_t delay
) {
    (void)data;
    (void)grab;
    (void)rate;
    (void)delay;
}

static const struct zwp_input_method_keyboard_grab_v2_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

struct input_keyboard *input_keyboard_create(
    struct zwp_input_method_v2 *input_method,
    struct zwp_virtual_keyboard_v1 *virtual_keyboard,
    input_keyboard_key_handler key_handler,
    void *key_handler_data
) {
    struct input_keyboard *keyboard;

    if (
        input_method == NULL
        || virtual_keyboard == NULL
        || key_handler == NULL
    ) {
        return NULL;
    }

    keyboard = calloc(1, sizeof(*keyboard));
    if (keyboard == NULL) {
        return NULL;
    }
    keyboard->virtual_keyboard = virtual_keyboard;
    keyboard->key_handler = key_handler;
    keyboard->key_handler_data = key_handler_data;
    keyboard->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (keyboard->xkb_context == NULL) {
        free(keyboard);
        return NULL;
    }

    keyboard->grab = zwp_input_method_v2_grab_keyboard(input_method);
    if (
        keyboard->grab == NULL
        || zwp_input_method_keyboard_grab_v2_add_listener(
            keyboard->grab,
            &keyboard_listener,
            keyboard
        ) < 0
    ) {
        input_keyboard_destroy(keyboard);
        return NULL;
    }

    return keyboard;
}

void input_keyboard_destroy(struct input_keyboard *keyboard) {
    if (keyboard == NULL) {
        return;
    }

    if (keyboard->grab != NULL) {
        zwp_input_method_keyboard_grab_v2_release(keyboard->grab);
    }
    xkb_state_unref(keyboard->xkb_state);
    xkb_keymap_unref(keyboard->xkb_keymap);
    xkb_context_unref(keyboard->xkb_context);
    free(keyboard);
}
