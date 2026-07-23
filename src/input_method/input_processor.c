#include "input_processor.h"

#include <xkbcommon/xkbcommon-keysyms.h>


static void notify_updated(struct input_processor *processor) {
    processor->listener->updated(processor->listener_data, &processor->engine);
}

static void cancel_composition(struct input_processor *processor) {
    input_engine_reset(&processor->engine);
    processor->listener->cancelled(processor->listener_data);
}

static void commit_candidate(
    struct input_processor *processor,
    size_t candidate_index
) {
    const struct code_table_entry *candidate = input_engine_candidate(
        &processor->engine,
        candidate_index
    );
    const char *text = candidate == NULL
        ? input_engine_code(&processor->engine)
        : candidate->text;

    processor->listener->committed(processor->listener_data, text);
    input_engine_reset(&processor->engine);
}

static bool handle_letter(struct input_processor *processor, char letter) {
    if (input_engine_append(&processor->engine, letter)) {
        notify_updated(processor);
        return true;
    }

    if (*input_engine_code(&processor->engine) == '\0') {
        return false;
    }

    commit_candidate(processor, 0);
    if (!input_engine_append(&processor->engine, letter)) {
        return false;
    }
    notify_updated(processor);
    return true;
}

bool input_processor_init(
    struct input_processor *processor,
    const struct code_table *table,
    struct input_mode *mode,
    const struct input_processor_listener *listener,
    void *listener_data
) {
    if (
        processor == NULL
        || mode == NULL
        || listener == NULL
        || listener->updated == NULL
        || listener->committed == NULL
        || listener->cancelled == NULL
    ) {
        return false;
    }

    *processor = (struct input_processor){
        .mode = mode,
        .listener = listener,
        .listener_data = listener_data,
    };
    return input_engine_init(&processor->engine, table);
}

void input_processor_destroy(struct input_processor *processor) {
    if (processor == NULL) {
        return;
    }

    input_engine_destroy(&processor->engine);
    *processor = (struct input_processor){0};
}

void input_processor_reset(struct input_processor *processor) {
    if (processor != NULL) {
        input_engine_reset(&processor->engine);
    }
}

bool input_processor_handle_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    bool composing;

    if (processor == NULL) {
        return false;
    }
    composing = *input_engine_code(&processor->engine) != '\0';
    if (
        symbol == XKB_KEY_space
        && modifiers == INPUT_PROCESSOR_MODIFIER_CONTROL
    ) {
        if (composing) {
            cancel_composition(processor);
        }
        processor->mode->chinese = !processor->mode->chinese;
        return true;
    }
    if (!processor->mode->chinese) {
        return false;
    }


    if (modifiers != 0) {
        if (composing) {
            cancel_composition(processor);
        }
        return false;
    }
    if (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z) {
        return handle_letter(processor, (char)('a' + symbol - XKB_KEY_a));
    }
    if (!composing) {
        return false;
    }

    if (symbol == XKB_KEY_BackSpace) {
        input_engine_backspace(&processor->engine);
        if (*input_engine_code(&processor->engine) == '\0') {
            processor->listener->cancelled(processor->listener_data);
        } else {
            notify_updated(processor);
        }
        return true;
    }
    if (symbol == XKB_KEY_Escape) {
        cancel_composition(processor);
        return true;
    }
    if (symbol == XKB_KEY_space || symbol == XKB_KEY_Return) {
        commit_candidate(processor, 0);
        return true;
    }
    if (symbol >= XKB_KEY_1 && symbol <= XKB_KEY_9) {
        size_t candidate_index = (size_t)(symbol - XKB_KEY_1);

        if (
            candidate_index
            < input_engine_candidate_count(&processor->engine)
        ) {
            commit_candidate(processor, candidate_index);
        }
        return true;
    }

    commit_candidate(processor, 0);
    return false;
}
