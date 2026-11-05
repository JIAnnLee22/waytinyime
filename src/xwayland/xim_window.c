#include "xim_window.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "code_table.h"
#include "emoji_font.h"

#define XIM_WINDOW_TEXT_CAPACITY 4096
#define XIM_WINDOW_MAX_WIDTH 900
#define XIM_WINDOW_HORIZONTAL_PADDING 12
#define XIM_WINDOW_VERTICAL_PADDING 8
#define XIM_WINDOW_FONT_SIZE 18
/* Catppuccin Mocha, matching mangobar: crust, surface1, text, sapphire. */
#define XIM_WINDOW_BACKGROUND 0xff11111bU
#define XIM_WINDOW_BORDER 0xff45475aU
#define XIM_WINDOW_TEXT 0xffcdd6f4U
#define XIM_WINDOW_HIGHLIGHT 0xff74c7ecU

struct xim_window {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_gcontext_t gc;
    FT_Library freetype;
    FT_Face font;
    struct emoji_font emoji;
    uint32_t *pixels;
    size_t pixel_capacity;
    int width;
    int height;
    int baseline;
    bool visible;
    bool has_text;
    char text[XIM_WINDOW_TEXT_CAPACITY];
    size_t cursor_candidate_start;
    size_t cursor_candidate_end;
};

static uint32_t next_codepoint(const char **cursor) {
    const unsigned char *text = (const unsigned char *)*cursor;
    uint32_t codepoint;
    size_t length;

    if (text[0] < 0x80) {
        *cursor += 1;
        return text[0];
    }
    if ((text[0] & 0xe0) == 0xc0) {
        codepoint = text[0] & 0x1f;
        length = 2;
    } else if ((text[0] & 0xf0) == 0xe0) {
        codepoint = text[0] & 0x0f;
        length = 3;
    } else if ((text[0] & 0xf8) == 0xf0) {
        codepoint = text[0] & 0x07;
        length = 4;
    } else {
        *cursor += 1;
        return 0xfffd;
    }

    for (size_t index = 1; index < length; index++) {
        if ((text[index] & 0xc0) != 0x80) {
            *cursor += 1;
            return 0xfffd;
        }
        codepoint = (codepoint << 6) | (text[index] & 0x3f);
    }
    *cursor += length;
    return codepoint;
}

static bool load_font(struct xim_window *window) {
    FcPattern *pattern;
    FcPattern *match;
    FcResult result;
    FcChar8 *font_path = NULL;
    bool loaded = false;

    if (!FcInit() || FT_Init_FreeType(&window->freetype) != 0) {
        return false;
    }

    pattern = FcNameParse((const FcChar8 *)"sans-serif:lang=zh-cn");
    if (pattern == NULL) {
        return false;
    }
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (
        match != NULL
        && FcPatternGetString(match, FC_FILE, 0, &font_path) == FcResultMatch
        && FT_New_Face(window->freetype, (const char *)font_path, 0,
            &window->font) == 0
        && FT_Set_Pixel_Sizes(window->font, 0, XIM_WINDOW_FONT_SIZE) == 0
    ) {
        loaded = true;
    }
    if (match != NULL) {
        FcPatternDestroy(match);
    }
    if (loaded) {
        emoji_font_init(&window->emoji, window->freetype, XIM_WINDOW_FONT_SIZE);
    }
    return loaded;
}

static void set_window_properties(struct xim_window *window) {
    static const char name[] = "waytinyime-candidate";
    static const char wm_class[] = "waytinyime\0Waytinyime\0";
    xcb_intern_atom_cookie_t utf8_cookie;
    xcb_intern_atom_cookie_t atom_cookie;
    xcb_intern_atom_reply_t *utf8_reply;
    xcb_intern_atom_reply_t *atom_reply;
    xcb_atom_t utf8_string;
    xcb_atom_t window_type;
    xcb_atom_t window_type_popup;
    uint32_t type = XCB_ATOM_ATOM;

    xcb_change_property(
        window->connection,
        XCB_PROP_MODE_REPLACE,
        window->window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        (uint32_t)(sizeof(name) - 1),
        name
    );
    xcb_change_property(
        window->connection,
        XCB_PROP_MODE_REPLACE,
        window->window,
        XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING,
        8,
        (uint32_t)(sizeof(wm_class) - 1),
        wm_class
    );

    utf8_cookie = xcb_intern_atom(
        window->connection,
        0,
        strlen("UTF8_STRING"),
        "UTF8_STRING"
    );
    atom_cookie = xcb_intern_atom(
        window->connection,
        0,
        strlen("_NET_WM_WINDOW_TYPE"),
        "_NET_WM_WINDOW_TYPE"
    );
    utf8_reply = xcb_intern_atom_reply(window->connection, utf8_cookie, NULL);
    atom_reply = xcb_intern_atom_reply(window->connection, atom_cookie, NULL);
    if (utf8_reply != NULL) {
        utf8_string = utf8_reply->atom;
        xcb_change_property(
            window->connection,
            XCB_PROP_MODE_REPLACE,
            window->window,
            utf8_string,
            utf8_string,
            8,
            (uint32_t)(sizeof(name) - 1),
            name
        );
        free(utf8_reply);
    }
    if (atom_reply != NULL) {
        utf8_cookie = xcb_intern_atom(
            window->connection,
            0,
            strlen("_NET_WM_WINDOW_TYPE_POPUP_MENU"),
            "_NET_WM_WINDOW_TYPE_POPUP_MENU"
        );
        utf8_reply = xcb_intern_atom_reply(
            window->connection,
            utf8_cookie,
            NULL
        );
        if (utf8_reply != NULL) {
            window_type = atom_reply->atom;
            window_type_popup = utf8_reply->atom;
            xcb_change_property(
                window->connection,
                XCB_PROP_MODE_REPLACE,
                window->window,
                window_type,
                type,
                32,
                1,
                &window_type_popup
            );
            free(utf8_reply);
        }
        free(atom_reply);
    }
}

static void ensure_gc(struct xim_window *window) {
    uint32_t values[1];

    if (window->gc != XCB_NONE) {
        return;
    }
    window->gc = xcb_generate_id(window->connection);
    values[0] = XIM_WINDOW_BACKGROUND;
    xcb_create_gc(
        window->connection,
        window->gc,
        window->window,
        XCB_GC_FOREGROUND,
        values
    );
}

struct xim_window *xim_window_create(
    xcb_connection_t *connection,
    xcb_screen_t *screen
) {
    struct xim_window *window;
    uint32_t values[2];
    uint32_t mask;

    if (connection == NULL || screen == NULL) {
        return NULL;
    }

    window = calloc(1, sizeof(*window));
    if (window == NULL) {
        return NULL;
    }
    window->connection = connection;
    window->screen = screen;
    window->window = xcb_generate_id(connection);
    mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    values[0] = 1;
    values[1] = XCB_EVENT_MASK_EXPOSURE;
    xcb_create_window(
        connection,
        XCB_COPY_FROM_PARENT,
        window->window,
        screen->root,
        0,
        0,
        1,
        1,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        mask,
        values
    );
    set_window_properties(window);
    ensure_gc(window);
    if (!load_font(window)) {
        fprintf(
            stderr,
            "waytinyime: XIM candidate font is unavailable; "
            "candidate text will not be rendered\n"
        );
    }
    return window;
}

void xim_window_destroy(struct xim_window *window) {
    if (window == NULL) {
        return;
    }

    if (window->gc != XCB_NONE) {
        xcb_free_gc(window->connection, window->gc);
    }
    if (window->window != XCB_NONE) {
        xcb_destroy_window(window->connection, window->window);
    }
    emoji_font_destroy(&window->emoji);
    if (window->font != NULL) {
        FT_Done_Face(window->font);
    }
    if (window->freetype != NULL) {
        FT_Done_FreeType(window->freetype);
    }
    free(window->pixels);
    free(window);
}

static bool ensure_pixels(struct xim_window *window) {
    size_t required;

    if (window->width <= 0 || window->height <= 0) {
        return false;
    }
    if ((size_t)window->width > SIZE_MAX / (size_t)window->height) {
        return false;
    }
    required = (size_t)window->width * (size_t)window->height;
    if (required <= window->pixel_capacity) {
        return true;
    }
    uint32_t *pixels = realloc(window->pixels, required * sizeof(uint32_t));

    if (pixels == NULL) {
        return false;
    }
    window->pixels = pixels;
    window->pixel_capacity = required;
    return true;
}

static void measure_text(struct xim_window *window) {
    const char *cursor = window->text;
    int text_width = 0;
    int ascender = window->font == NULL
        ? 15
        : (int)(window->font->size->metrics.ascender >> 6);
    int descender = window->font == NULL
        ? 5
        : (int)(-window->font->size->metrics.descender >> 6);

    if (window->font != NULL) {
        while (*cursor != '\0') {
            uint32_t codepoint = next_codepoint(&cursor);

            if (codepoint == 0xfe0f || codepoint == 0xfe0e) {
                continue;
            }
            if (
                (FT_Get_Char_Index(window->font, codepoint) == 0
                 || strncmp(cursor, "\xef\xb8\x8f", 3) == 0)
                && emoji_font_load(&window->emoji, codepoint)
            ) {
                text_width += emoji_font_advance(&window->emoji);
            } else if (FT_Load_Char(window->font, codepoint, FT_LOAD_DEFAULT) == 0) {
                text_width += (int)(window->font->glyph->advance.x >> 6);
            }
        }
    } else {
        text_width = (int)strlen(window->text) * 8;
    }

    window->width = text_width + 2 * XIM_WINDOW_HORIZONTAL_PADDING;
    if (window->width < 80) {
        window->width = 80;
    }
    if (window->width > XIM_WINDOW_MAX_WIDTH) {
        window->width = XIM_WINDOW_MAX_WIDTH;
    }
    window->height = ascender + descender + 2 * XIM_WINDOW_VERTICAL_PADDING;
    window->baseline = XIM_WINDOW_VERTICAL_PADDING + ascender;
}

static uint32_t blend_color(uint32_t background, uint32_t foreground,
    uint8_t alpha) {
    uint32_t inverse = 255U - alpha;
    uint32_t red = (
        ((foreground >> 16) & 0xffU) * alpha
        + ((background >> 16) & 0xffU) * inverse
    ) / 255U;
    uint32_t green = (
        ((foreground >> 8) & 0xffU) * alpha
        + ((background >> 8) & 0xffU) * inverse
    ) / 255U;
    uint32_t blue = (
        (foreground & 0xffU) * alpha
        + (background & 0xffU) * inverse
    ) / 255U;

    return 0xff000000U | (red << 16) | (green << 8) | blue;
}

static void draw_glyph(
    struct xim_window *window,
    const FT_Bitmap *bitmap,
    int origin_x,
    int origin_y,
    uint32_t color
) {
    int stride = window->width;

    for (unsigned int row = 0; row < bitmap->rows; row++) {
        int y = origin_y + (int)row;
        const unsigned char *row_pixels = bitmap->pitch >= 0
            ? bitmap->buffer + (size_t)row * (size_t)bitmap->pitch
            : bitmap->buffer
                + (size_t)(bitmap->rows - 1 - row)
                    * (size_t)(-bitmap->pitch);

        if (y < 1 || y >= window->height - 1) {
            continue;
        }
        for (unsigned int column = 0; column < bitmap->width; column++) {
            int x = origin_x + (int)column;
            uint8_t alpha;

            if (x < 1 || x >= window->width - 1) {
                continue;
            }
            if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
                alpha = row_pixels[column / 8]
                    & (0x80U >> (column % 8))
                    ? 255
                    : 0;
            } else {
                alpha = row_pixels[column];
            }
            if (alpha != 0) {
                uint32_t *pixel = &window->pixels[
                    (size_t)y * (size_t)stride + (size_t)x
                ];
                *pixel = blend_color(*pixel, color, alpha);
            }
        }
    }
}

static void draw_text(struct xim_window *window) {
    const char *cursor = window->text;
    int pen_x = XIM_WINDOW_HORIZONTAL_PADDING;
    size_t pixel_count = (size_t)window->width * (size_t)window->height;

    for (size_t index = 0; index < pixel_count; index++) {
        window->pixels[index] = XIM_WINDOW_BACKGROUND;
    }
    for (int x = 0; x < window->width; x++) {
        window->pixels[x] = XIM_WINDOW_BORDER;
        window->pixels[
            (size_t)(window->height - 1) * (size_t)window->width
                + (size_t)x
        ] = XIM_WINDOW_BORDER;
    }
    for (int y = 0; y < window->height; y++) {
        window->pixels[(size_t)y * (size_t)window->width] = XIM_WINDOW_BORDER;
        window->pixels[
            (size_t)y * (size_t)window->width
                + (size_t)(window->width - 1)
        ] = XIM_WINDOW_BORDER;
    }
    if (window->font == NULL) {
        return;
    }

    while (*cursor != '\0' && pen_x < window->width - XIM_WINDOW_HORIZONTAL_PADDING) {
        size_t byte_offset = (size_t)(cursor - window->text);
        uint32_t codepoint = next_codepoint(&cursor);
        uint32_t color = byte_offset >= window->cursor_candidate_start
                && byte_offset < window->cursor_candidate_end
            ? XIM_WINDOW_HIGHLIGHT
            : XIM_WINDOW_TEXT;

        if (codepoint == 0xfe0f || codepoint == 0xfe0e) {
            continue;
        }
        if (
            (FT_Get_Char_Index(window->font, codepoint) == 0
             || strncmp(cursor, "\xef\xb8\x8f", 3) == 0)
            && emoji_font_load(&window->emoji, codepoint)
        ) {
            emoji_font_draw(
                &window->emoji, window->pixels, window->width, window->height,
                window->width, pen_x, window->baseline, color
            );
            pen_x += emoji_font_advance(&window->emoji);
            continue;
        }
        if (FT_Load_Char(window->font, codepoint, FT_LOAD_RENDER) != 0) {
            continue;
        }
        draw_glyph(
            window,
            &window->font->glyph->bitmap,
            pen_x + window->font->glyph->bitmap_left,
            window->baseline - window->font->glyph->bitmap_top,
            color
        );
        pen_x += (int)(window->font->glyph->advance.x >> 6);
    }
}

static bool render(struct xim_window *window) {
    if (!window->has_text || window->width <= 0 || window->height <= 0) {
        return false;
    }
    ensure_gc(window);
    if (!ensure_pixels(window)) {
        return false;
    }
    draw_text(window);
    xcb_put_image(
        window->connection,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        window->window,
        window->gc,
        (uint16_t)window->width,
        (uint16_t)window->height,
        0,
        0,
        0,
        window->screen->root_depth,
        (uint32_t)((size_t)window->width * (size_t)window->height
            * sizeof(uint32_t)),
        (const uint8_t *)window->pixels
    );
    return true;
}

static void position_window(struct xim_window *window, int root_x, int root_y) {
    uint32_t values[4];
    int x = root_x;
    int y = root_y + 2;

    if (x + window->width > (int)window->screen->width_in_pixels) {
        x = (int)window->screen->width_in_pixels - window->width;
    }
    if (y + window->height > (int)window->screen->height_in_pixels) {
        y = root_y - window->height - 2;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    values[0] = (uint32_t)x;
    values[1] = (uint32_t)y;
    values[2] = (uint32_t)window->width;
    values[3] = (uint32_t)window->height;
    xcb_configure_window(
        window->connection,
        window->window,
        XCB_CONFIG_WINDOW_X
            | XCB_CONFIG_WINDOW_Y
            | XCB_CONFIG_WINDOW_WIDTH
            | XCB_CONFIG_WINDOW_HEIGHT,
        values
    );
}

bool xim_window_update(
    struct xim_window *window,
    int root_x,
    int root_y,
    const char *code,
    const char *label,
    bool show_code,
    const struct code_table_entry *const *candidates,
    size_t candidate_count,
    size_t cursor,
    size_t total_candidate_count
) {
    size_t offset;
    int written;

    if (window == NULL || code == NULL || *code == '\0') {
        xim_window_hide(window);
        return false;
    }
    bool show_codes = strchr(code, CODE_TABLE_WILDCARD) != NULL;

    written = snprintf(
        window->text, sizeof(window->text), "%s%s%s%s",
        label != NULL ? label : "",
        label != NULL && *label != '\0' ? "  " : "",
        show_code ? code : "",
        show_code ? "    " : ""
    );
    if (written < 0 || (size_t)written >= sizeof(window->text)) {
        return false;
    }
    offset = (size_t)written;
    window->cursor_candidate_start = 0;
    window->cursor_candidate_end = 0;

    if (candidate_count == 0) {
        written = snprintf(
            window->text + offset,
            sizeof(window->text) - offset,
            "无候选"
        );
        if (written < 0 || (size_t)written >= sizeof(window->text) - offset) {
            return false;
        }
    } else {
        for (size_t index = 0; index < candidate_count; index++) {
            size_t start = offset;

            written = snprintf(
                window->text + offset,
                sizeof(window->text) - offset,
                "%zu.%s%s%s%s%s",
                index + 1,
                candidates[index]->text,
                show_codes ? " [" : "",
                show_codes ? candidates[index]->code : "",
                show_codes ? "]" : "",
                index + 1 == candidate_count ? "" : "  "
            );
            if (
                written < 0
                || (size_t)written >= sizeof(window->text) - offset
            ) {
                return false;
            }
            offset += (size_t)written;
            if (index == cursor) {
                window->cursor_candidate_start = start;
                window->cursor_candidate_end = offset;
            }
        }
    }

    if (total_candidate_count > candidate_count) {
        written = snprintf(
            window->text + offset,
            sizeof(window->text) - offset,
            "  +%zu",
            total_candidate_count - candidate_count
        );
        if (written < 0 || (size_t)written >= sizeof(window->text) - offset) {
            return false;
        }
    }

    window->has_text = true;
    measure_text(window);
    position_window(window, root_x, root_y);
    if (!window->visible) {
        xcb_map_window(window->connection, window->window);
        window->visible = true;
    }
    render(window);
    xcb_flush(window->connection);
    return true;
}

xcb_window_t xim_window_xid(const struct xim_window *window) {
    return window == NULL ? XCB_WINDOW_NONE : window->window;
}

void xim_window_redraw(struct xim_window *window) {
    if (window == NULL || !window->visible) {
        return;
    }
    render(window);
    xcb_flush(window->connection);
}

void xim_window_hide(struct xim_window *window) {
    if (window == NULL) {
        return;
    }
    window->has_text = false;
    if (window->visible) {
        xcb_unmap_window(window->connection, window->window);
        window->visible = false;
    }
    xcb_flush(window->connection);
}

bool xim_window_is_visible(const struct xim_window *window) {
    return window != NULL && window->visible;
}
