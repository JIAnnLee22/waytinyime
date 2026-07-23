#include <stdio.h>
#include <string.h>

#include "code_table.h"
#include "input_engine.h"
#include "input_processor.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(                                                         \
                stderr,                                                      \
                "check failed at %s:%d: %s\n",                              \
                __FILE__,                                                    \
                __LINE__,                                                    \
                #condition                                                   \
            );                                                               \
            return 1;                                                        \
        }                                                                    \
    } while (0)

struct processor_events {
    size_t updates;
    size_t cancellations;
    char committed[64];
};

static void processor_updated(
    void *data,
    const struct input_engine *engine
) {
    struct processor_events *events = data;

    (void)engine;
    events->updates++;
}

static void processor_committed(void *data, const char *text) {
    struct processor_events *events = data;

    snprintf(events->committed, sizeof(events->committed), "%s", text);
}

static void processor_cancelled(void *data) {
    struct processor_events *events = data;

    events->cancellations++;
}

static const struct input_processor_listener processor_listener = {
    .updated = processor_updated,
    .committed = processor_committed,
    .cancelled = processor_cancelled,
};

int main(int argc, char **argv) {
    struct code_table_error error;
    struct code_table *table;
    struct input_engine engine;
    const struct code_table_entry *candidate;
    struct input_processor processor;
    struct input_mode mode = {
        .chinese = true,
    };
    struct processor_events processor_events = {0};

    CHECK(argc == 2);
    table = code_table_load(argv[1], &error);
    if (table == NULL) {
        fprintf(
            stderr,
            "failed to load table at line %zu: %s\n",
            error.line,
            code_table_error_message(error.code)
        );
        return 1;
    }
    CHECK(input_engine_init(&engine, table));

    CHECK(input_engine_append(&engine, 'x'));
    CHECK(strcmp(input_engine_code(&engine), "x") == 0);
    CHECK(input_engine_candidate_count(&engine) == 2);
    candidate = input_engine_candidate(&engine, 0);
    CHECK(candidate != NULL && strcmp(candidate->text, "小") == 0);

    CHECK(input_engine_append(&engine, 'm'));
    CHECK(strcmp(input_engine_code(&engine), "xm") == 0);
    CHECK(input_engine_candidate_count(&engine) == 2);
    CHECK(strcmp(input_engine_candidate(&engine, 0)->text, "先") == 0);
    CHECK(strcmp(input_engine_candidate(&engine, 1)->text, "下面") == 0);

    CHECK(input_engine_backspace(&engine));
    CHECK(strcmp(input_engine_code(&engine), "x") == 0);
    CHECK(input_engine_backspace(&engine));
    CHECK(strcmp(input_engine_code(&engine), "") == 0);
    CHECK(input_engine_candidate_count(&engine) == 0);
    CHECK(!input_engine_backspace(&engine));
    CHECK(!input_engine_append(&engine, 'A'));

    CHECK(input_engine_append(&engine, 'a'));
    CHECK(input_engine_append(&engine, 'a'));
    CHECK(input_engine_append(&engine, 'b'));
    CHECK(input_engine_append(&engine, 'a'));
    CHECK(!input_engine_append(&engine, 'a'));
    CHECK(strcmp(input_engine_code(&engine), "aaba") == 0);

    input_engine_destroy(&engine);
    CHECK(
        input_processor_init(
            &processor,
            table,
            &mode,
            &processor_listener,
            &processor_events
        )
    );
    CHECK(input_processor_handle_key(&processor, 'x', 0));
    CHECK(
        input_processor_handle_key(
            &processor,
            ' ',
            INPUT_PROCESSOR_MODIFIER_CONTROL
        )
    );
    CHECK(!mode.chinese);
    CHECK(processor_events.cancellations == 1);
    CHECK(strcmp(input_engine_code(&processor.engine), "") == 0);
    CHECK(!input_processor_handle_key(&processor, 'x', 0));
    CHECK(
        !input_processor_handle_key(
            &processor,
            ' ',
            INPUT_PROCESSOR_MODIFIER_CONTROL
                | INPUT_PROCESSOR_MODIFIER_SHIFT
        )
    );
    CHECK(!mode.chinese);
    CHECK(
        input_processor_handle_key(
            &processor,
            ' ',
            INPUT_PROCESSOR_MODIFIER_CONTROL
        )
    );
    CHECK(mode.chinese);
    CHECK(input_processor_handle_key(&processor, 'x', 0));
    CHECK(input_processor_handle_key(&processor, 'm', 0));
    CHECK(input_processor_handle_key(&processor, ' ', 0));
    CHECK(processor_events.updates == 3);
    CHECK(strcmp(processor_events.committed, "先") == 0);
    CHECK(strcmp(input_engine_code(&processor.engine), "") == 0);
    input_processor_destroy(&processor);

    code_table_destroy(table);
    return 0;
}
