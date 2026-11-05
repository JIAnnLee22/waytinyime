#define _POSIX_C_SOURCE 200809L

#include "input_processor.h"

#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

struct default_keys {
    const struct table_key *items;
    size_t count;
};

static const struct table_key default_prev_page_keys[] = {
    {XKB_KEY_Up, 0},
};
static const struct table_key default_next_page_keys[] = {
    {XKB_KEY_Down, 0},
};
static const struct table_key default_prev_candidate_keys[] = {
    {XKB_KEY_Left, 0},
};
static const struct table_key default_next_candidate_keys[] = {
    {XKB_KEY_Right, 0},
};
static const struct table_key default_default_candidate_keys[] = {
    {XKB_KEY_space, 0},
};
static const struct table_key default_selection_keys[] = {
    {XKB_KEY_1, 0},
    {XKB_KEY_2, 0},
    {XKB_KEY_3, 0},
    {XKB_KEY_4, 0},
    {XKB_KEY_5, 0},
    {XKB_KEY_6, 0},
    {XKB_KEY_7, 0},
    {XKB_KEY_8, 0},
    {XKB_KEY_9, 0},
    {XKB_KEY_0, 0},
};

static struct default_keys action_keys(
    const struct table_key_list *configured,
    const struct table_key *defaults,
    size_t default_count
) {
    if (configured != NULL && configured->count != 0) {
        return (struct default_keys){
            .items = configured->items,
            .count = configured->count,
        };
    }
    return (struct default_keys){
        .items = defaults,
        .count = default_count,
    };
}

static void notify_updated(struct input_processor *processor) {
    processor->listener->updated(processor->listener_data, &processor->engine);
}

static void cancel_composition(struct input_processor *processor) {
    input_engine_reset(&processor->engine);
    processor->listener->cancelled(processor->listener_data);
}

static const struct code_table_entry *first_candidate(
    const struct input_processor *processor
) {
    const struct code_table_entry *candidate = input_engine_candidate(
        &processor->engine,
        0
    );
    const struct code_table_entry *matches[1];

    if (candidate != NULL) {
        return candidate;
    }
    if (input_engine_total_candidate_count(&processor->engine) == 0) {
        return NULL;
    }
    size_t total = input_engine_prefix_match(&processor->engine)
        ? code_table_lookup_prefix_page(
            processor->engine.table,
            input_engine_code(&processor->engine),
            0,
            matches,
            1
        )
        : code_table_lookup_page(
            processor->engine.table,
            input_engine_code(&processor->engine),
            0,
            matches,
            1
        );
    if (total == 0) {
        return NULL;
    }
    return matches[0];
}

static bool commit_text(
    struct input_processor *processor,
    const char *text
) {
    if (text == NULL || *text == '\0') {
        return false;
    }
    processor->listener->committed(processor->listener_data, text);
    input_engine_reset(&processor->engine);
    return true;
}

static bool commit_candidate(
    struct input_processor *processor,
    size_t candidate_index
) {
    const struct code_table_entry *candidate = input_engine_candidate(
        &processor->engine,
        candidate_index
    );

    if (candidate != NULL) {
        return commit_text(processor, candidate->text);
    }
    return commit_text(processor, input_engine_code(&processor->engine));
}

static bool commit_first_candidate(struct input_processor *processor) {
    const struct code_table_entry *candidate = first_candidate(processor);

    if (candidate != NULL) {
        return commit_text(processor, candidate->text);
    }
    return commit_text(processor, input_engine_code(&processor->engine));
}

static bool key_matches(
    const struct table_key *key,
    uint32_t symbol,
    uint32_t modifiers
) {
    return key->symbol == symbol && key->modifiers == modifiers;
}

static bool key_list_index(
    struct default_keys keys,
    uint32_t symbol,
    uint32_t modifiers,
    size_t *index
) {
    for (size_t position = 0; position < keys.count; position++) {
        if (key_matches(&keys.items[position], symbol, modifiers)) {
            if (index != NULL) {
                *index = position;
            }
            return true;
        }
    }
    return false;
}

static bool key_list_matches(
    struct default_keys keys,
    uint32_t symbol,
    uint32_t modifiers
) {
    return key_list_index(keys, symbol, modifiers, NULL);
}

static bool regex_matches(
    const regex_t *regex,
    bool valid,
    const char *code
) {
    return valid && code != NULL && *code != '\0'
        && regexec(regex, code, 0, NULL, 0) == 0;
}

static bool auto_select_length_matches(
    const struct input_processor *processor,
    const char *code,
    size_t code_length,
    size_t max_code_length
) {
    if (processor->config == NULL) {
        return false;
    }
    if (
        regex_matches(
            &processor->auto_select_regex,
            processor->auto_select_regex_valid,
            code
        )
    ) {
        return true;
    }
    int limit = processor->config->auto_select_length;
    if (limit == -1) {
        return code_length >= max_code_length;
    }
    return limit > 0 && code_length >= (size_t)limit;
}

static bool no_match_length_matches(
    const struct input_processor *processor,
    const char *code,
    size_t code_length,
    size_t max_code_length
) {
    if (processor->config == NULL) {
        return false;
    }
    if (
        regex_matches(
            &processor->no_match_regex,
            processor->no_match_regex_valid,
            code
        )
    ) {
        return true;
    }
    int limit = processor->config->no_match_auto_select_length;
    if (limit == -1) {
        return code_length >= max_code_length;
    }
    return limit > 0 && code_length >= (size_t)limit;
}

static bool maybe_auto_select(struct input_processor *processor) {
    const struct table_config *config = processor->config;
    const char *code = input_engine_code(&processor->engine);
    size_t code_length = strlen(code);
    size_t max_code_length = code_table_max_code_length(
        processor->engine.table
    );

    if (
        config == NULL || !config->auto_select || code_length == 0
        || strchr(code, CODE_TABLE_WILDCARD) != NULL
        || code_table_label(processor->engine.table, code) != NULL
        || input_engine_total_candidate_count(&processor->engine) == 0
    ) {
        /* Keep wildcard results available for manual selection. Unmatched
         * codes also stay editable at/after the maximum code length. */
        return false;
    }

    if (
        code_length >= max_code_length
        || auto_select_length_matches(
            processor,
            code,
            code_length,
            max_code_length
        )
    ) {
        /* The configured length (four for flypy) selects the first match even
         * when several candidates share that code. */
        return commit_first_candidate(processor);
    }
    return false;
}

static bool handle_code_character(
    struct input_processor *processor,
    char character
) {
    const struct input_engine *engine = &processor->engine;
    char *old_code = strdup(input_engine_code(engine));
    size_t old_length = strlen(input_engine_code(engine));
    size_t old_total = input_engine_total_candidate_count(engine);
    const struct code_table_entry *old_first = input_engine_candidate(
        engine,
        0
    );
    const struct table_config *config = processor->config;
    size_t max_code_length = code_table_max_code_length(engine->table);

    if (old_code == NULL) {
        return false;
    }

    bool wildcard_query = character == CODE_TABLE_WILDCARD
        || strchr(old_code, CODE_TABLE_WILDCARD) != NULL;

    if (!input_engine_append(&processor->engine, character)) {
        if (old_length == 0 || old_total == 0 || wildcard_query) {
            free(old_code);
            return false;
        }
        commit_candidate(processor, 0);
        if (!input_engine_append(&processor->engine, character)) {
            free(old_code);
            return false;
        }
        free(old_code);
        if (!maybe_auto_select(processor)) {
            notify_updated(processor);
        }
        return true;
    }

    if (
        config != NULL
        && config->auto_select
        && !wildcard_query
        && input_engine_total_candidate_count(&processor->engine) == 0
        && old_total > 0
        && old_first != NULL
        && no_match_length_matches(
            processor,
            old_code,
            old_length,
            max_code_length
        )
    ) {
        commit_text(processor, old_first->text);
        free(old_code);
        if (!input_engine_append(&processor->engine, character)) {
            return true;
        }
        if (!maybe_auto_select(processor)) {
            notify_updated(processor);
        }
        return true;
    }

    free(old_code);
    if (!maybe_auto_select(processor)) {
        notify_updated(processor);
    }
    return true;
}

static bool symbol_to_code_character(uint32_t symbol, char *character) {
    if (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z) {
        *character = (char)('a' + symbol - XKB_KEY_a);
        return true;
    }
    if (symbol >= XKB_KEY_0 && symbol <= XKB_KEY_9) {
        *character = (char)('0' + symbol - XKB_KEY_0);
        return true;
    }

    switch (symbol) {
    case XKB_KEY_semicolon:
        *character = ';';
        return true;
    case XKB_KEY_apostrophe:
        *character = '\'';
        return true;
    case XKB_KEY_comma:
        *character = ',';
        return true;
    case XKB_KEY_period:
        *character = '.';
        return true;
    case XKB_KEY_slash:
        *character = '/';
        return true;
    case XKB_KEY_backslash:
        *character = '\\';
        return true;
    case XKB_KEY_minus:
        *character = '-';
        return true;
    case XKB_KEY_equal:
        *character = '=';
        return true;
    case XKB_KEY_bracketleft:
        *character = '[';
        return true;
    case XKB_KEY_bracketright:
        *character = ']';
        return true;
    case XKB_KEY_grave:
        *character = '`';
        return true;
    default:
        return false;
    }
}

static bool code_character_for_key(
    const struct input_processor *processor,
    uint32_t symbol,
    char *character
) {
    const char *key_codes = code_table_key_codes(processor->engine.table);

    if (
        key_codes == NULL
        || !symbol_to_code_character(symbol, character)
    ) {
        return false;
    }
    return strchr(key_codes, *character) != NULL;
}

static bool code_continues_with(
    const struct input_processor *processor,
    char character
) {
    const char *code = input_engine_code(&processor->engine);
    size_t length = strlen(code);
    char *next;
    bool matches;

    if (length == 0 || length > SIZE_MAX - 2) {
        return false;
    }
    next = malloc(length + 2);
    if (next == NULL) {
        return false;
    }
    memcpy(next, code, length);
    next[length] = character;
    next[length + 1] = '\0';
    matches = code_table_lookup_prefix_page(
        processor->engine.table, next, 0, NULL, 0
    ) != 0;
    free(next);
    return matches;
}

static bool handle_chinese_punctuation(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    const char *text;
    char converted[4];
    char character;

    /* Shift selects punctuation; Ctrl/Alt/Super combinations stay shortcuts. */
    if (modifiers & ~INPUT_PROCESSOR_MODIFIER_SHIFT) {
        return false;
    }
    switch (symbol) {
    case XKB_KEY_KP_Add: symbol = XKB_KEY_plus; break;
    case XKB_KEY_KP_Subtract: symbol = XKB_KEY_minus; break;
    case XKB_KEY_KP_Multiply: symbol = XKB_KEY_asterisk; break;
    case XKB_KEY_KP_Divide: symbol = XKB_KEY_slash; break;
    case XKB_KEY_KP_Decimal: symbol = XKB_KEY_period; break;
    default: break;
    }
    if (
        symbol < 0x21 || symbol > 0x7e
        || (symbol >= XKB_KEY_0 && symbol <= XKB_KEY_9)
        || (symbol >= XKB_KEY_A && symbol <= XKB_KEY_Z)
        || (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z)
    ) {
        return false;
    }
    /* Keep punctuation that genuinely continues a table code; candidate and
     * paging bindings have already been handled before reaching this point. */
    if (
        modifiers == 0
        && code_character_for_key(processor, symbol, &character)
        && code_continues_with(processor, character)
    ) {
        return handle_code_character(processor, character);
    }

    switch (symbol) {
    case XKB_KEY_comma: text = "，"; break;
    case XKB_KEY_period: text = "。"; break;
    case XKB_KEY_less: text = "《"; break;
    case XKB_KEY_greater: text = "》"; break;
    case XKB_KEY_bracketleft: text = "【"; break;
    case XKB_KEY_bracketright: text = "】"; break;
    case XKB_KEY_asciicircum: text = "……"; break;
    case XKB_KEY_underscore: text = "——"; break;
    case XKB_KEY_apostrophe:
        text = processor->mode->single_quote_open ? "’" : "‘";
        processor->mode->single_quote_open = !processor->mode->single_quote_open;
        break;
    case XKB_KEY_quotedbl:
        text = processor->mode->double_quote_open ? "”" : "“";
        processor->mode->double_quote_open = !processor->mode->double_quote_open;
        break;
    default: {
        uint32_t codepoint = symbol + 0xfee0;
        converted[0] = (char)(0xe0 | (codepoint >> 12));
        converted[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        converted[2] = (char)(0x80 | (codepoint & 0x3f));
        converted[3] = '\0';
        text = converted;
        break;
    }
    }
    if (*input_engine_code(&processor->engine) != '\0') {
        if (input_engine_candidate_count(&processor->engine) != 0) {
            commit_candidate(processor, input_engine_cursor(&processor->engine));
        } else {
            commit_first_candidate(processor);
        }
    }
    return commit_text(processor, text);
}

static bool handle_page_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    struct default_keys previous = action_keys(
        processor->config == NULL ? NULL : &processor->config->prev_page,
        default_prev_page_keys,
        sizeof(default_prev_page_keys) / sizeof(default_prev_page_keys[0])
    );
    struct default_keys next = action_keys(
        processor->config == NULL ? NULL : &processor->config->next_page,
        default_next_page_keys,
        sizeof(default_next_page_keys) / sizeof(default_next_page_keys[0])
    );

    if (key_list_matches(previous, symbol, modifiers)) {
        if (input_engine_page_previous(&processor->engine)) {
            notify_updated(processor);
        }
        return true;
    }
    if (key_list_matches(next, symbol, modifiers)) {
        if (input_engine_page_next(&processor->engine)) {
            notify_updated(processor);
        }
        return true;
    }
    return false;
}

static bool handle_candidate_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    struct default_keys previous = action_keys(
        processor->config == NULL ? NULL : &processor->config->prev_candidate,
        default_prev_candidate_keys,
        sizeof(default_prev_candidate_keys)
            / sizeof(default_prev_candidate_keys[0])
    );
    struct default_keys next = action_keys(
        processor->config == NULL ? NULL : &processor->config->next_candidate,
        default_next_candidate_keys,
        sizeof(default_next_candidate_keys)
            / sizeof(default_next_candidate_keys[0])
    );

    if (input_engine_candidate_count(&processor->engine) == 0) {
        return false;
    }
    if (key_list_matches(previous, symbol, modifiers)) {
        if (input_engine_candidate_previous(&processor->engine)) {
            notify_updated(processor);
        }
        return true;
    }
    if (key_list_matches(next, symbol, modifiers)) {
        if (input_engine_candidate_next(&processor->engine)) {
            notify_updated(processor);
        }
        return true;
    }
    return false;
}

static bool handle_selection_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    struct default_keys selection = action_keys(
        processor->config == NULL ? NULL : &processor->config->selection,
        default_selection_keys,
        sizeof(default_selection_keys) / sizeof(default_selection_keys[0])
    );
    struct default_keys default_candidate = action_keys(
        processor->config == NULL
            ? NULL
            : &processor->config->default_candidate,
        default_default_candidate_keys,
        sizeof(default_default_candidate_keys)
            / sizeof(default_default_candidate_keys[0])
    );
    struct default_keys second = action_keys(
        processor->config == NULL
            ? NULL
            : &processor->config->second_candidate,
        NULL,
        0
    );
    struct default_keys third = action_keys(
        processor->config == NULL
            ? NULL
            : &processor->config->third_candidate,
        NULL,
        0
    );
    size_t index;

    if (input_engine_page_size(&processor->engine) == 0) {
        /*
         * PageSize=0 is fcitx5's compose-table mode: no candidate window or
         * numeric selection, but the default candidate key still commits the
         * first candidate instead of forwarding the key to the client.
         */
        if (
            key_list_matches(default_candidate, symbol, modifiers)
            && input_engine_total_candidate_count(&processor->engine) > 0
        ) {
            commit_first_candidate(processor);
            return true;
        }
        return false;
    }

    if (key_list_index(selection, symbol, modifiers, &index)) {
        if (index < input_engine_candidate_count(&processor->engine)) {
            commit_candidate(processor, index);
        }
        return true;
    }
    if (key_list_matches(default_candidate, symbol, modifiers)) {
        if (input_engine_candidate_count(&processor->engine) == 0) {
            return false;
        }
        commit_candidate(processor, input_engine_cursor(&processor->engine));
        return true;
    }
    if (
        key_list_matches(second, symbol, modifiers)
        && input_engine_candidate_count(&processor->engine) > 1
    ) {
        commit_candidate(processor, 1);
        return true;
    }
    if (
        key_list_matches(third, symbol, modifiers)
        && input_engine_candidate_count(&processor->engine) > 2
    ) {
        commit_candidate(processor, 2);
        return true;
    }
    return false;
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

void input_processor_set_config(
    struct input_processor *processor,
    const struct table_config *config
) {
    if (processor == NULL || processor->engine.table == NULL) {
        return;
    }

    if (processor->auto_select_regex_valid) {
        regfree(&processor->auto_select_regex);
        processor->auto_select_regex_valid = false;
    }
    if (processor->no_match_regex_valid) {
        regfree(&processor->no_match_regex);
        processor->no_match_regex_valid = false;
    }

    processor->config = config;
    size_t page_size = config == NULL
        ? INPUT_ENGINE_DEFAULT_PAGE_SIZE
        : (size_t)config->page_size;
    input_engine_set_prefix_match(
        &processor->engine,
        config != NULL && !config->exact_match
    );
    input_engine_set_page_size(&processor->engine, page_size);

    if (config != NULL && config->auto_select_regex != NULL
        && *config->auto_select_regex != '\0') {
        if (
            regcomp(
                &processor->auto_select_regex,
                config->auto_select_regex,
                REG_EXTENDED | REG_NOSUB
            )
            == 0
        ) {
            processor->auto_select_regex_valid = true;
        }
    }
    if (config != NULL && config->no_match_auto_select_regex != NULL
        && *config->no_match_auto_select_regex != '\0') {
        if (
            regcomp(
                &processor->no_match_regex,
                config->no_match_auto_select_regex,
                REG_EXTENDED | REG_NOSUB
            )
            == 0
        ) {
            processor->no_match_regex_valid = true;
        }
    }
}

void input_processor_destroy(struct input_processor *processor) {
    if (processor == NULL) {
        return;
    }

    if (processor->auto_select_regex_valid) {
        regfree(&processor->auto_select_regex);
    }
    if (processor->no_match_regex_valid) {
        regfree(&processor->no_match_regex);
    }
    input_engine_destroy(&processor->engine);
    *processor = (struct input_processor){0};
}

void input_processor_reset(struct input_processor *processor) {
    if (processor != NULL) {
        input_engine_reset(&processor->engine);
    }
}

void input_processor_cancel(struct input_processor *processor) {
    if (processor == NULL) {
        return;
    }
    if (*input_engine_code(&processor->engine) != '\0') {
        cancel_composition(processor);
    } else {
        input_engine_reset(&processor->engine);
    }
}

static bool is_standalone_modifier(uint32_t symbol) {
    switch (symbol) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Shift_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Scroll_Lock:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level5_Shift:
        return true;
    default:
        return false;
    }
}

bool input_processor_handle_key(
    struct input_processor *processor,
    uint32_t symbol,
    uint32_t modifiers
) {
    bool composing;
    char character;

    if (processor == NULL) {
        return false;
    }
    /*
     * XIM delivers a modifier key press with the modifier state before that
     * key is applied.  Treat standalone modifiers as pass-through instead of
     * committing the current composition, so Ctrl+Space and application
     * shortcuts keep the expected key order.
     */
    if (is_standalone_modifier(symbol)) {
        return false;
    }
    composing = *input_engine_code(&processor->engine) != '\0';

    if (
        symbol == XKB_KEY_space
        && modifiers == INPUT_PROCESSOR_MODIFIER_CONTROL
    ) {
        if (composing) {
            /* Switching modes preserves the typed code, not the selected
             * Chinese/Emoji candidate, and does not insert a space. */
            commit_text(processor, input_engine_code(&processor->engine));
        }
        processor->mode->chinese = !processor->mode->chinese;
        processor->mode->single_quote_open = false;
        processor->mode->double_quote_open = false;
        return true;
    }
    if (!processor->mode->chinese) {
        return false;
    }

    if (composing) {
        if (symbol == XKB_KEY_Escape) {
            cancel_composition(processor);
            return true;
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
        /* Enter always commits the raw code, even when candidates exist or
         * the table binds Enter to a candidate-selection action. */
        if (
            modifiers == 0
            && (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter)
        ) {
            return commit_text(processor, input_engine_code(&processor->engine));
        }
        if (handle_page_key(processor, symbol, modifiers)) {
            return true;
        }
        if (handle_candidate_key(processor, symbol, modifiers)) {
            return true;
        }
        if (handle_selection_key(processor, symbol, modifiers)) {
            return true;
        }
    }

    /* The unshifted backtick is a query wildcard, even without a match or
     * an existing composition; never convert it to full-width punctuation. */
    if (modifiers == 0 && symbol == XKB_KEY_grave) {
        return handle_code_character(processor, CODE_TABLE_WILDCARD);
    }
    if (handle_chinese_punctuation(processor, symbol, modifiers)) {
        return true;
    }
    if (modifiers != 0) {
        if (composing) {
            cancel_composition(processor);
        }
        return false;
    }

    if (code_character_for_key(processor, symbol, &character)) {
        return handle_code_character(processor, character);
    }
    if (!composing) {
        return false;
    }

    commit_first_candidate(processor);
    return false;
}
