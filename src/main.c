#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <wayland-client.h>

#include "code_table.h"
#include "input_session.h"
#include "input_processor.h"
#include "xim_frontend.h"
#include "waytinyime-config.h"
#include "input-method-v2.h"
#include "virtual-keyboard-v1.h"

#define WAYTINYIME_VERSION "0.1.0"

struct application {
    struct wl_display *display;
    struct code_table *code_table;
    struct input_session *input_session;
    struct xim_frontend *xim_frontend;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwp_input_method_manager_v2 *input_method_manager;
    struct zwp_input_method_v2 *input_method;
    struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
    struct zwp_virtual_keyboard_v1 *virtual_keyboard;
    uint32_t seat_name;
    uint32_t manager_name;
    uint32_t compositor_name;
    uint32_t shm_name;
    uint32_t virtual_keyboard_manager_name;
    uint32_t serial;
    bool pending_active;
    bool active;
    bool available;
};

static volatile sig_atomic_t stop_requested;

static uint32_t min_version(uint32_t advertised, uint32_t supported) {
    return advertised < supported ? advertised : supported;
}

static void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void input_method_activate(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    (void)input_method;
    app->pending_active = true;
}

static void input_method_deactivate(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    (void)input_method;
    app->pending_active = false;
}

static void input_method_surrounding_text(
    void *data,
    struct zwp_input_method_v2 *input_method,
    const char *text,
    uint32_t cursor,
    uint32_t anchor
) {
    (void)data;
    (void)input_method;
    (void)text;
    (void)cursor;
    (void)anchor;
}

static void input_method_text_change_cause(
    void *data,
    struct zwp_input_method_v2 *input_method,
    uint32_t cause
) {
    (void)data;
    (void)input_method;
    (void)cause;
}

static void input_method_content_type(
    void *data,
    struct zwp_input_method_v2 *input_method,
    uint32_t hint,
    uint32_t purpose
) {
    (void)data;
    (void)input_method;
    (void)hint;
    (void)purpose;
}

static void input_method_done(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    bool was_active = app->active;

    (void)input_method;
    app->active = app->pending_active;
    app->serial++;
    input_session_set_serial(app->input_session, app->serial);

    if (app->active == was_active) {
        return;
    }
    if (app->active) {
        if (!input_session_activate(app->input_session)) {
            fputs("waytinyime: cannot grab the input keyboard\n", stderr);
            app->available = false;
            stop_requested = 1;
            return;
        }
    } else {
        input_session_deactivate(app->input_session);
    }
    fprintf(
        stderr,
        "waytinyime: input method %s\n",
        app->active ? "activated" : "deactivated"
    );
}

static void input_method_unavailable(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    (void)input_method;
    app->available = false;
    stop_requested = 1;
    fputs("waytinyime: input method is unavailable\n", stderr);
}

static const struct zwp_input_method_v2_listener input_method_listener = {
    .activate = input_method_activate,
    .deactivate = input_method_deactivate,
    .surrounding_text = input_method_surrounding_text,
    .text_change_cause = input_method_text_change_cause,
    .content_type = input_method_content_type,
    .done = input_method_done,
    .unavailable = input_method_unavailable,
};

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
) {
    struct application *app = data;

    if (strcmp(interface, wl_seat_interface.name) == 0 && app->seat == NULL) {
        app->seat = wl_registry_bind(
            registry,
            name,
            &wl_seat_interface,
            min_version(version, 7)
        );
        app->seat_name = name;
    } else if (
        strcmp(interface, wl_compositor_interface.name) == 0
        && app->compositor == NULL
    ) {
        app->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            min_version(version, 6)
        );
        app->compositor_name = name;
    } else if (
        strcmp(interface, wl_shm_interface.name) == 0
        && app->shm == NULL
    ) {
        app->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            min_version(version, 1)
        );
        app->shm_name = name;
    } else if (
        strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0
        && app->input_method_manager == NULL
    ) {
        app->input_method_manager = wl_registry_bind(
            registry,
            name,
            &zwp_input_method_manager_v2_interface,
            min_version(version, 1)
        );
        app->manager_name = name;
    } else if (
        strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0
        && app->virtual_keyboard_manager == NULL
    ) {
        app->virtual_keyboard_manager = wl_registry_bind(
            registry,
            name,
            &zwp_virtual_keyboard_manager_v1_interface,
            min_version(version, 1)
        );
        app->virtual_keyboard_manager_name = name;
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name
) {
    struct application *app = data;
    (void)registry;

    if (
        name == app->seat_name
        || name == app->compositor_name
        || name == app->shm_name
        || name == app->manager_name
        || name == app->virtual_keyboard_manager_name
    ) {
        stop_requested = 1;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void application_destroy(struct application *app) {
    xim_frontend_destroy(app->xim_frontend);
    input_session_destroy(app->input_session);
    if (app->virtual_keyboard != NULL) {
        zwp_virtual_keyboard_v1_destroy(app->virtual_keyboard);
    }
    if (app->input_method != NULL) {
        zwp_input_method_v2_destroy(app->input_method);
    }
    if (app->virtual_keyboard_manager != NULL) {
        zwp_virtual_keyboard_manager_v1_destroy(
            app->virtual_keyboard_manager
        );
    }
    if (app->input_method_manager != NULL) {
        zwp_input_method_manager_v2_destroy(app->input_method_manager);
    }
    if (app->shm != NULL) {
        wl_shm_destroy(app->shm);
    }
    if (app->compositor != NULL) {
        wl_compositor_destroy(app->compositor);
    }
    if (app->seat != NULL) {
        wl_seat_destroy(app->seat);
    }
    if (app->registry != NULL) {
        wl_registry_destroy(app->registry);
    }
    if (app->display != NULL) {
        wl_display_disconnect(app->display);
    }
    code_table_destroy(app->code_table);
}

static void report_code_table_error(
    const char *path,
    const struct code_table_error *error
) {
    fprintf(stderr, "waytinyime: cannot load code table '%s'", path);
    if (error->line != 0) {
        fprintf(stderr, " at line %zu", error->line);
    }
    fprintf(stderr, ": %s", code_table_error_message(error->code));
    if (error->system_error != 0) {
        fprintf(stderr, ": %s", strerror(error->system_error));
    }
    fputc('\n', stderr);
}

static int application_dispatch_events(struct application *app) {
    struct pollfd descriptors[2] = {
        {
            .fd = wl_display_get_fd(app->display),
            .events = POLLIN,
        },
        {
            .fd = xim_frontend_get_fd(app->xim_frontend),
            .events = POLLIN,
        },
    };
    int poll_result;
    int flush_result;

    if (
        app->xim_frontend != NULL
        && !xim_frontend_dispatch(app->xim_frontend)
    ) {
        fputs("waytinyime: XIM connection closed\n", stderr);
        xim_frontend_destroy(app->xim_frontend);
        app->xim_frontend = NULL;
        descriptors[1].fd = -1;
    }

    while (wl_display_prepare_read(app->display) != 0) {
        if (wl_display_dispatch_pending(app->display) < 0) {
            return -1;
        }
    }
    flush_result = wl_display_flush(app->display);
    if (flush_result < 0 && errno != EAGAIN) {
        wl_display_cancel_read(app->display);
        return -1;
    }
    if (flush_result < 0) {
        descriptors[0].events |= POLLOUT;
    }

    do {
        poll_result = poll(descriptors, 2, -1);
    } while (poll_result < 0 && errno == EINTR && !stop_requested);
    if (poll_result < 0 || stop_requested) {
        wl_display_cancel_read(app->display);
        return poll_result < 0 ? -1 : 0;
    }

    if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        wl_display_cancel_read(app->display);
        return -1;
    }
    if (descriptors[0].revents & POLLIN) {
        if (wl_display_read_events(app->display) < 0) {
            return -1;
        }
    } else {
        wl_display_cancel_read(app->display);
    }
    if (wl_display_dispatch_pending(app->display) < 0) {
        return -1;
    }

    if (
        app->xim_frontend != NULL
        && descriptors[1].revents != 0
        && !xim_frontend_dispatch(app->xim_frontend)
    ) {
        fputs("waytinyime: XIM connection closed\n", stderr);
        xim_frontend_destroy(app->xim_frontend);
        app->xim_frontend = NULL;
    }
    return 1;
}

static int application_run(void) {
    struct application app = {
        .available = true,
    };
    struct input_mode mode = {
        .chinese = true,
    };
    int result = 1;
    struct code_table_error table_error;
    const char *table_path = getenv("WAYTINYIME_TABLE");

    if (table_path == NULL || *table_path == '\0') {
        table_path = WAYTINYIME_TABLE_PATH;
    }
    app.code_table = code_table_load(table_path, &table_error);
    if (app.code_table == NULL) {
        report_code_table_error(table_path, &table_error);
        return 1;
    }
    app.xim_frontend = xim_frontend_create(app.code_table, &mode);
    if (getenv("DISPLAY") != NULL) {
        if (app.xim_frontend == NULL) {
            fputs("waytinyime: cannot start the XIM frontend\n", stderr);
        } else {
            fprintf(
                stderr,
                "waytinyime: XIM frontend ready (@im=%s)\n",
                xim_frontend_name(app.xim_frontend)
            );
        }
    }

    app.display = wl_display_connect(NULL);
    if (app.display == NULL) {
        fputs("waytinyime: cannot connect to the Wayland display\n", stderr);
        goto out;
    }

    app.registry = wl_display_get_registry(app.display);
    if (
        app.registry == NULL
        || wl_registry_add_listener(app.registry, &registry_listener, &app) < 0
        || wl_display_roundtrip(app.display) < 0
    ) {
        fputs("waytinyime: cannot read the Wayland registry\n", stderr);
        goto out;
    }

    if (app.seat == NULL) {
        fputs("waytinyime: compositor does not advertise a wl_seat\n", stderr);
        goto out;
    }
    if (app.compositor == NULL || app.shm == NULL) {
        fputs("waytinyime: compositor cannot create popup surfaces\n", stderr);
        goto out;
    }
    if (app.input_method_manager == NULL) {
        fputs(
            "waytinyime: compositor does not support input-method-unstable-v2\n",
            stderr
        );
        goto out;
    }
    if (app.virtual_keyboard_manager == NULL) {
        fputs(
            "waytinyime: compositor does not support virtual-keyboard-v1\n",
            stderr
        );
        goto out;
    }

    app.input_method = zwp_input_method_manager_v2_get_input_method(
        app.input_method_manager,
        app.seat
    );
    if (
        app.input_method == NULL
        || zwp_input_method_v2_add_listener(
            app.input_method,
            &input_method_listener,
            &app
        ) < 0
    ) {
        fputs("waytinyime: cannot create the input method\n", stderr);
        goto out;
    }

    app.virtual_keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
            app.virtual_keyboard_manager,
            app.seat
        );
    app.input_session = input_session_create(
        app.code_table,
        &mode,
        app.compositor,
        app.shm,
        app.input_method,
        app.virtual_keyboard
    );
    if (app.virtual_keyboard == NULL || app.input_session == NULL) {
        fputs("waytinyime: cannot initialize the input session\n", stderr);
        goto out;
    }

    while (!stop_requested) {
        int dispatch_result = application_dispatch_events(&app);

        if (dispatch_result <= 0) {
            break;
        }
    }
    if (!stop_requested && wl_display_get_error(app.display) != 0) {
        const struct wl_interface *interface = NULL;
        uint32_t object_id = 0;
        uint32_t protocol_error = wl_display_get_protocol_error(
            app.display,
            &interface,
            &object_id
        );

        if (protocol_error != 0 && interface != NULL) {
            fprintf(
                stderr,
                "waytinyime: Wayland protocol error %u on %s@%u\n",
                protocol_error,
                interface->name,
                object_id
            );
        } else {
            fprintf(
                stderr,
                "waytinyime: Wayland connection failed: %s\n",
                strerror(wl_display_get_error(app.display))
            );
        }
        result = 1;
    } else {
        result = app.available ? 0 : 1;
    }

out:
    application_destroy(&app);
    return result;
}

static void print_usage(FILE *stream, const char *program) {
    fprintf(stream, "Usage: %s [--help] [--version]\n", program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("waytinyime " WAYTINYIME_VERSION);
        return 0;
    }
    if (argc != 1) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    return application_run();
}
