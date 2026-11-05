#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "code_table.h"
#include "input_session.h"
#include "input_processor.h"
#include "table_config.h"
#include "table_source.h"
#include "xim_frontend.h"
#include "input-method-v2.h"
#include "virtual-keyboard-v1.h"

#define WAYTINYIME_VERSION "0.1.0"

struct application {
    struct wl_display *display;
    struct table_source *source;
    const struct code_table *code_table;
    const struct table_config *table_config;
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

static bool debug_enabled(void) {
    const char *value = getenv("WAYTINYIME_DEBUG");

    return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

static void input_method_activate(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    (void)input_method;
    app->pending_active = true;
    if (debug_enabled()) {
        fputs("waytinyime: Wayland input method pending activate\n", stderr);
    }
}

static void input_method_deactivate(
    void *data,
    struct zwp_input_method_v2 *input_method
) {
    struct application *app = data;
    (void)input_method;
    app->pending_active = false;
    if (debug_enabled()) {
        fputs("waytinyime: Wayland input method pending deactivate\n", stderr);
    }
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

    if (debug_enabled() && app->active != was_active) {
        fprintf(
            stderr,
            "waytinyime: Wayland input method active=%d\n",
            app->active ? 1 : 0
        );
    }
    /*
     * Wayland and Xwayland have independent focus state.  A native Wayland
     * text-input can send activate/done while the IM was already active, so
     * reset the XIM composition on every pending activation as well as on
     * an actual active-state change.  An unfinished code must not survive a
     * switch between the two frontends.
     */
    if (app->pending_active || app->active != was_active) {
        xim_frontend_reset(app->xim_frontend);
    }
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

static void xim_focus_changed(void *data) {
    struct application *app = data;

    /*
     * The XIM frontend is reached through Xwayland, so native Wayland
     * composition held by the Wayland input session is stale.  Cancel it
     * before the XIM key is processed.
     */
    input_session_cancel(app->input_session);
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
    table_source_destroy(app->source);
}

static void report_source_error(
    const char *stage,
    const struct table_source_error *error
) {
    fprintf(stderr, "waytinyime: %s", stage);
    if (error->path[0] != '\0') {
        fprintf(stderr, " '%s'", error->path);
    }
    if (error->line != 0) {
        fprintf(stderr, " at line %zu", error->line);
    }
    fprintf(
        stderr,
        ": %s",
        table_source_error_message(error->code)
    );
    if (error->detail[0] != '\0') {
        fprintf(stderr, " (%s)", error->detail);
    }
    fputc('\n', stderr);
}

static struct table_source *load_source(
    const char *config_path,
    const char *table_path,
    const char *input_method,
    struct table_source_error *error
) {
    const char *environment_table;

    if (table_path != NULL && *table_path != '\0') {
        return table_source_load_table(table_path, error);
    }
    if (config_path != NULL && *config_path != '\0') {
        return table_source_load_config(config_path, error);
    }
    environment_table = getenv("WAYTINYIME_TABLE");
    if (environment_table != NULL && *environment_table != '\0') {
        return table_source_load_table(environment_table, error);
    }
    if (input_method == NULL || *input_method == '\0') {
        input_method = getenv("WAYTINYIME_INPUT_METHOD");
    }
    if (input_method == NULL || *input_method == '\0') {
        input_method = "flypy";
    }
    return table_source_load_default(input_method, error);
}

static void print_source_summary(const struct table_source *source) {
    const struct table_config *config = table_source_config(source);
    const struct code_table *table = table_source_table(source);
    const char *config_path = table_source_config_path(source);
    const char *table_path = table_source_table_path(source);

    if (config_path != NULL) {
        fprintf(stderr, "waytinyime: config %s\n", config_path);
    }
    if (table_path != NULL) {
        fprintf(
            stderr,
            "waytinyime: table %s (%s, %zu entries)\n",
            table_path,
            table_source_table_is_binary(source)
                ? "libime binary .dict"
                : "text",
            code_table_entry_count(table)
        );
    }
    if (config != NULL) {
        fprintf(
            stderr,
            "waytinyime: page_size=%d auto_select=%d auto_select_length=%d "
            "exact_match=%d\n",
            config->page_size,
            config->auto_select,
            config->auto_select_length,
            config->exact_match
        );
    }
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

static int application_run(
    const char *config_path,
    const char *table_path,
    const char *input_method
) {
    struct application app = {
        .available = true,
    };
    struct input_mode mode = {
        .chinese = true,
    };
    struct table_source_error source_error;
    int result = 1;

    app.source = load_source(
        config_path,
        table_path,
        input_method,
        &source_error
    );
    if (app.source == NULL) {
        report_source_error("cannot load the code table", &source_error);
        return 1;
    }
    app.code_table = table_source_table(app.source);
    app.table_config = table_source_config(app.source);
    print_source_summary(app.source);

    app.xim_frontend = xim_frontend_create(
        app.code_table,
        app.table_config,
        &mode
    );
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
        app.table_config,
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
    xim_frontend_set_focus_handler(
        app.xim_frontend,
        xim_focus_changed,
        &app
    );

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

static void print_modifiers(FILE *stream, uint32_t modifiers) {
    if (modifiers & TABLE_CONFIG_MODIFIER_CONTROL) {
        fputs("Control+", stream);
    }
    if (modifiers & TABLE_CONFIG_MODIFIER_ALT) {
        fputs("Alt+", stream);
    }
    if (modifiers & TABLE_CONFIG_MODIFIER_LOGO) {
        fputs("Logo+", stream);
    }
    if (modifiers & TABLE_CONFIG_MODIFIER_SHIFT) {
        fputs("Shift+", stream);
    }
}

static void print_key_list(
    FILE *stream,
    const char *label,
    const struct table_key_list *list
) {
    fprintf(stream, "  %-18s ", label);
    if (list == NULL || list->count == 0) {
        fputs("-", stream);
    } else {
        for (size_t index = 0; index < list->count; index++) {
            char name[64];

            if (index != 0) {
                fputs(", ", stream);
            }
            print_modifiers(stream, list->items[index].modifiers);
            if (
                xkb_keysym_get_name(
                    list->items[index].symbol,
                    name,
                    sizeof(name)
                )
                <= 0
            ) {
                snprintf(name, sizeof(name), "0x%x", list->items[index].symbol);
            }
            fputs(name, stream);
        }
    }
    fputc('\n', stream);
}

static int inspect_source(
    const char *config_path,
    const char *table_path,
    const char *input_method,
    const char *query
) {
    struct table_source_error source_error;
    struct table_source *source = load_source(
        config_path,
        table_path,
        input_method,
        &source_error
    );
    const struct table_config *config;
    const struct code_table *table;
    struct code_table_stats stats;
    const struct code_table_entry *matches[10];
    size_t total;

    if (source == NULL) {
        report_source_error("cannot load the code table", &source_error);
        return 1;
    }
    config = table_source_config(source);
    table = table_source_table(source);

    if (table_source_config_path(source) != NULL) {
        printf("config: %s\n", table_source_config_path(source));
    } else {
        printf("config: (none, direct table)\n");
    }
    printf(
        "table: %s (%s)\n",
        table_source_table_path(source),
        table_source_table_is_binary(source)
            ? "libime binary .dict"
            : "UTF-8 text"
    );

    if (config != NULL) {
        printf(
            "input-method: name=%s label=%s icon=%s lang=%s addon=%s "
            "configurable=%s\n",
            config->name,
            config->label,
            config->icon,
            config->lang_code,
            config->addon,
            config->configurable ? "yes" : "no"
        );
        printf("table-options:\n");
        printf("  %-18s %d\n", "PageSize", config->page_size);
        printf(
            "  %-18s %s\n",
            "AutoSelect",
            config->auto_select ? "true" : "false"
        );
        printf(
            "  %-18s %d\n",
            "AutoSelectLength",
            config->auto_select_length
        );
        printf(
            "  %-18s %s\n",
            "AutoSelectRegex",
            config->auto_select_regex
        );
        printf(
            "  %-18s %d\n",
            "NoMatchLength",
            config->no_match_auto_select_length
        );
        printf(
            "  %-18s %s\n",
            "NoMatchRegex",
            config->no_match_auto_select_regex
        );
        printf(
            "  %-18s %s\n",
            "ExactMatch",
            config->exact_match ? "true" : "false"
        );
        printf(
            "  %-18s %s\n",
            "CommitAfterSelect",
            config->commit_after_select ? "true" : "false"
        );
        printf(
            "  %-18s %s\n",
            "CommitInvalid",
            config->commit_invalid_segment ? "true" : "false"
        );
        printf(
            "  %-18s %s\n",
            "UseFullWidth",
            config->use_full_width ? "true" : "false"
        );
        printf(
            "  %-18s %s\n",
            "Hint",
            config->hint ? "true" : "false"
        );
        printf(
            "  %-18s %s\n",
            "OrderPolicy",
            config->order_policy
        );
        printf(
            "  %-18s %s\n",
            "CandidateLayout",
            config->candidate_layout_hint
        );
        print_key_list(stdout, "PrevPage", &config->prev_page);
        print_key_list(stdout, "NextPage", &config->next_page);
        print_key_list(stdout, "PrevCandidate", &config->prev_candidate);
        print_key_list(stdout, "NextCandidate", &config->next_candidate);
        print_key_list(stdout, "DefaultCandidate", &config->default_candidate);
        print_key_list(stdout, "SecondCandidate", &config->second_candidate);
        print_key_list(stdout, "ThirdCandidate", &config->third_candidate);
        print_key_list(stdout, "Selection", &config->selection);
    }

    printf(
        "emoji-overlay: added=%zu (existing codes unchanged)\n",
        table_source_emoji_entries(source)
    );
    stats = code_table_stats(table);
    printf(
        "code-table: entries=%zu rules=%zu pinyin=%zu prompt=%zu "
        "construct=%zu phrases=%zu rejected-phrases=%zu ignored-lines=%zu\n",
        code_table_entry_count(table),
        code_table_rule_count(table),
        stats.pinyin_entries,
        stats.prompt_entries,
        stats.construct_phrase_entries,
        stats.phrases,
        stats.rejected_phrases,
        stats.ignored_lines
    );
    printf(
        "code-table: key-codes=%s max-length=%zu\n",
        code_table_key_codes(table),
        code_table_max_code_length(table)
    );

    if (query == NULL || *query == '\0') {
        query = "a";
    }
    const char *label = code_table_label(table, query);
    if (label != NULL) {
        printf("category: %s\n", label);
    }
    total = code_table_lookup_page(table, query, 0, matches, 10);
    printf("lookup '%s': total=%zu", query, total);
    for (size_t index = 0; index < total && index < 10; index++) {
        printf(" [%s]", matches[index]->text);
    }
    fputc('\n', stdout);

    table_source_destroy(source);
    return 0;
}

static void print_usage(FILE *stream, const char *program) {
    fprintf(
        stream,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "  --config PATH        load an fcitx5 table .conf\n"
        "  --table PATH         load a .txt table or libime .dict directly\n"
        "  --input-method NAME  configuration name used for discovery "
        "(default: flypy)\n"
        "  --inspect            print the resolved configuration and table\n"
        "  --query CODE         code to query in --inspect mode (default: a)\n"
        "  --help               show this help\n"
        "  --version            show the version\n"
        "\n"
        "Environment: WAYTINYIME_CONF, WAYTINYIME_TABLE, "
        "WAYTINYIME_FCITX_DIR,\n"
        "             WAYTINYIME_INPUT_METHOD\n",
        program
    );
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *table_path = NULL;
    const char *input_method = NULL;
    const char *query = NULL;
    bool inspect = false;
    int index;

    for (index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argument, "--version") == 0) {
            puts("waytinyime " WAYTINYIME_VERSION);
            return 0;
        }
        if (strcmp(argument, "--inspect") == 0) {
            inspect = true;
            continue;
        }
        if (strcmp(argument, "--config") == 0 && index + 1 < argc) {
            config_path = argv[++index];
            continue;
        }
        if (strcmp(argument, "--table") == 0 && index + 1 < argc) {
            table_path = argv[++index];
            continue;
        }
        if (strcmp(argument, "--input-method") == 0 && index + 1 < argc) {
            input_method = argv[++index];
            continue;
        }
        if (strcmp(argument, "--query") == 0 && index + 1 < argc) {
            query = argv[++index];
            continue;
        }
        if (strncmp(argument, "--config=", 9) == 0) {
            config_path = argument + 9;
            continue;
        }
        if (strncmp(argument, "--table=", 8) == 0) {
            table_path = argument + 8;
            continue;
        }
        if (strncmp(argument, "--input-method=", 15) == 0) {
            input_method = argument + 15;
            continue;
        }
        if (strncmp(argument, "--query=", 8) == 0) {
            query = argument + 8;
            continue;
        }

        print_usage(stderr, argv[0]);
        return 2;
    }

    if (inspect) {
        return inspect_source(config_path, table_path, input_method, query);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    return application_run(config_path, table_path, input_method);
}
