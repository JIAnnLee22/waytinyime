#define _GNU_SOURCE

#include "candidate_popup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <wayland-client.h>

#include "code_table.h"
#include "emoji_font.h"
#include "input-method-v2.h"

#define POPUP_BUFFER_COUNT 2
#define POPUP_TEXT_CAPACITY 4096
#define POPUP_MAX_WIDTH 900
#define POPUP_HORIZONTAL_PADDING 12
#define POPUP_VERTICAL_PADDING 8
#define POPUP_FONT_SIZE 18
/* Catppuccin Mocha, matching mangobar: crust, surface1, text, sapphire. */
#define POPUP_BACKGROUND 0xff11111bU
#define POPUP_BORDER 0xff45475aU
#define POPUP_TEXT 0xffcdd6f4U
#define POPUP_HIGHLIGHT 0xff74c7ecU

struct candidate_popup;

struct popup_buffer {
    struct candidate_popup *popup;
    struct wl_buffer *buffer;
    uint32_t *pixels;
    size_t size;
    int width;
    int height;
    int stride;
    bool busy;
};

struct candidate_popup {
    struct wl_shm *shm;
    struct wl_surface *surface;
    struct zwp_input_popup_surface_v2 *role;
    struct popup_buffer buffers[POPUP_BUFFER_COUNT];
    FT_Library freetype;
    FT_Face font;
    struct emoji_font emoji;
    int scale;
    char text[POPUP_TEXT_CAPACITY];
    size_t cursor_candidate_start;
    size_t cursor_candidate_end;
    bool redraw_pending;
    bool visible;
};

static bool render_popup(struct candidate_popup *popup);

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

static bool load_font(struct candidate_popup *popup, int scale) {
    FcPattern *pattern;
    FcPattern *match;
    FcResult result;
    FcChar8 *font_path = NULL;
    FT_Face font = NULL;
    int face_index = 0;
    int pixels = POPUP_FONT_SIZE * scale;
    bool loaded = false;

    if (
        !FcInit()
        || (popup->freetype == NULL && FT_Init_FreeType(&popup->freetype) != 0)
    ) {
        return false;
    }

    pattern = FcNameParse((const FcChar8 *)"sans-serif:lang=zh-cn");
    if (pattern == NULL) {
        return false;
    }
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, pixels);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (
        match != NULL
        && FcPatternGetString(match, FC_FILE, 0, &font_path) == FcResultMatch
    ) {
        /* FC_INDEX includes the collection face and variable-font instance. */
        FcPatternGetInteger(match, FC_INDEX, 0, &face_index);
        loaded = FT_New_Face(
            popup->freetype, (const char *)font_path, face_index, &font
        ) == 0 && FT_Set_Pixel_Sizes(font, 0, pixels) == 0;
    }
    if (match != NULL) {
        FcPatternDestroy(match);
    }
    if (!loaded) {
        if (font != NULL) {
            FT_Done_Face(font);
        }
        return false;
    }
    if (popup->font != NULL) {
        FT_Done_Face(popup->font);
    }
    popup->font = font;
    popup->scale = scale;
    emoji_font_destroy(&popup->emoji);
    emoji_font_init(&popup->emoji, popup->freetype, pixels);
    return true;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
    struct popup_buffer *popup_buffer = data;

    (void)buffer;
    popup_buffer->busy = false;
    if (popup_buffer->popup->redraw_pending) {
        render_popup(popup_buffer->popup);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void destroy_buffer(struct popup_buffer *buffer) {
    if (buffer->buffer != NULL) {
        wl_buffer_destroy(buffer->buffer);
    }
    if (buffer->pixels != NULL && buffer->pixels != MAP_FAILED) {
        munmap(buffer->pixels, buffer->size);
    }

    buffer->buffer = NULL;
    buffer->pixels = NULL;
    buffer->size = 0;
    buffer->width = 0;
    buffer->height = 0;
    buffer->stride = 0;
    buffer->busy = false;
}

static bool prepare_buffer(
    struct popup_buffer *buffer,
    struct wl_shm *shm,
    int width,
    int height
) {
    struct wl_shm_pool *pool;
    int fd;
    int stride;
    size_t size;
    void *pixels;

    if (
        buffer->buffer != NULL
        && buffer->width == width
        && buffer->height == height
    ) {
        return true;
    }

    destroy_buffer(buffer);
    if (width <= 0 || height <= 0 || width > INT32_MAX / 4) {
        return false;
    }
    stride = width * 4;
    if ((size_t)height > SIZE_MAX / (size_t)stride) {
        return false;
    }
    size = (size_t)stride * (size_t)height;
    if (size > INT32_MAX) {
        return false;
    }

    fd = memfd_create("waytinyime-popup", MFD_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return false;
    }

    pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return false;
    }

    pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    close(fd);
    if (pool == NULL) {
        munmap(pixels, size);
        return false;
    }

    buffer->buffer = wl_shm_pool_create_buffer(
        pool,
        0,
        width,
        height,
        stride,
        WL_SHM_FORMAT_ARGB8888
    );
    wl_shm_pool_destroy(pool);
    if (buffer->buffer == NULL) {
        munmap(pixels, size);
        return false;
    }

    buffer->pixels = pixels;
    buffer->size = size;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
    return true;
}

static void measure_popup(
    struct candidate_popup *popup,
    int *width,
    int *height,
    int *baseline
) {
    const char *cursor = popup->text;
    int text_width = 0;
    int ascender = (int)(popup->font->size->metrics.ascender >> 6);
    int descender = (int)(-popup->font->size->metrics.descender >> 6);

    while (*cursor != '\0') {
        uint32_t codepoint = next_codepoint(&cursor);

        if (codepoint == 0xfe0f || codepoint == 0xfe0e) {
            continue;
        }
        if (
            (FT_Get_Char_Index(popup->font, codepoint) == 0
             || strncmp(cursor, "\xef\xb8\x8f", 3) == 0)
            && emoji_font_load(&popup->emoji, codepoint)
        ) {
            text_width += emoji_font_advance(&popup->emoji);
        } else if (FT_Load_Char(popup->font, codepoint, FT_LOAD_DEFAULT) == 0) {
            text_width += (int)(popup->font->glyph->advance.x >> 6);
        }
    }

    *width = text_width + 2 * POPUP_HORIZONTAL_PADDING * popup->scale;
    if (*width < 80 * popup->scale) {
        *width = 80 * popup->scale;
    }
    if (*width > POPUP_MAX_WIDTH * popup->scale) {
        *width = POPUP_MAX_WIDTH * popup->scale;
    }
    *height = ascender + descender + 2 * POPUP_VERTICAL_PADDING * popup->scale;
    *baseline = POPUP_VERTICAL_PADDING * popup->scale + ascender;
    /* Buffer dimensions must cover a whole number of surface-local pixels. */
    *width = (*width + popup->scale - 1) / popup->scale * popup->scale;
    *height = (*height + popup->scale - 1) / popup->scale * popup->scale;
}

static uint32_t blend_color(uint32_t background, uint32_t foreground, uint8_t alpha) {
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
    struct popup_buffer *buffer,
    const FT_Bitmap *bitmap,
    int origin_x,
    int origin_y,
    uint32_t color,
    int border
) {
    for (unsigned int row = 0; row < bitmap->rows; row++) {
        int y = origin_y + (int)row;
        const unsigned char *row_pixels = bitmap->pitch >= 0
            ? bitmap->buffer + (size_t)row * (size_t)bitmap->pitch
            : bitmap->buffer
                + (size_t)(bitmap->rows - 1 - row)
                    * (size_t)(-bitmap->pitch);

        if (y < border || y >= buffer->height - border) {
            continue;
        }
        for (unsigned int column = 0; column < bitmap->width; column++) {
            int x = origin_x + (int)column;
            uint8_t alpha;

            if (x < border || x >= buffer->width - border) {
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
                uint32_t *pixel = &buffer->pixels[
                    (size_t)y * (size_t)(buffer->stride / 4) + (size_t)x
                ];
                *pixel = blend_color(*pixel, color, alpha);
            }
        }
    }
}

static bool draw_buffer(
    struct candidate_popup *popup,
    struct popup_buffer *buffer,
    int baseline
) {
    const char *cursor = popup->text;
    int padding = POPUP_HORIZONTAL_PADDING * popup->scale;
    int pen_x = padding;

    for (int y = 0; y < buffer->height; y++) {
        for (int x = 0; x < buffer->width; x++) {
            bool border = x < popup->scale || x >= buffer->width - popup->scale
                || y < popup->scale || y >= buffer->height - popup->scale;
            buffer->pixels[(size_t)y * (size_t)(buffer->stride / 4) + (size_t)x]
                = border ? POPUP_BORDER : POPUP_BACKGROUND;
        }
    }

    while (*cursor != '\0' && pen_x < buffer->width - padding) {
        size_t byte_offset = (size_t)(cursor - popup->text);
        uint32_t codepoint = next_codepoint(&cursor);
        uint32_t color = byte_offset >= popup->cursor_candidate_start
                && byte_offset < popup->cursor_candidate_end
            ? POPUP_HIGHLIGHT
            : POPUP_TEXT;

        if (codepoint == 0xfe0f || codepoint == 0xfe0e) {
            continue;
        }
        if (
            (FT_Get_Char_Index(popup->font, codepoint) == 0
             || strncmp(cursor, "\xef\xb8\x8f", 3) == 0)
            && emoji_font_load(&popup->emoji, codepoint)
        ) {
            emoji_font_draw(
                &popup->emoji, buffer->pixels, buffer->width, buffer->height,
                buffer->stride / 4, pen_x, baseline, color
            );
            pen_x += emoji_font_advance(&popup->emoji);
            continue;
        }
        if (FT_Load_Char(popup->font, codepoint, FT_LOAD_RENDER) != 0) {
            continue;
        }
        draw_glyph(
            buffer,
            &popup->font->glyph->bitmap,
            pen_x + popup->font->glyph->bitmap_left,
            baseline - popup->font->glyph->bitmap_top,
            color,
            popup->scale
        );
        pen_x += (int)(popup->font->glyph->advance.x >> 6);
    }
    return true;
}

static bool render_popup(struct candidate_popup *popup) {
    struct popup_buffer *buffer = NULL;
    int width;
    int height;
    int baseline;

    measure_popup(popup, &width, &height, &baseline);
    for (size_t index = 0; index < POPUP_BUFFER_COUNT; index++) {
        if (!popup->buffers[index].busy) {
            buffer = &popup->buffers[index];
            break;
        }
    }
    if (buffer == NULL) {
        popup->redraw_pending = true;
        return true;
    }
    if (
        !prepare_buffer(buffer, popup->shm, width, height)
        || !draw_buffer(popup, buffer, baseline)
    ) {
        return false;
    }

    buffer->busy = true;
    popup->redraw_pending = false;
    if (
        wl_surface_get_version(popup->surface)
            >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    ) {
        wl_surface_set_buffer_scale(popup->surface, popup->scale);
    }
    wl_surface_attach(popup->surface, buffer->buffer, 0, 0);
    wl_surface_damage(
        popup->surface, 0, 0, width / popup->scale, height / popup->scale
    );
    wl_surface_commit(popup->surface);
    return true;
}

static void surface_output(void *data, struct wl_surface *surface,
    struct wl_output *output) {
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_preferred_buffer_scale(void *data, struct wl_surface *surface,
    int32_t factor) {
    struct candidate_popup *popup = data;
    (void)surface;

    if (
        factor < 1 || factor > INT32_MAX / POPUP_MAX_WIDTH
        || factor == popup->scale
    ) {
        return;
    }
    /* Rasterize at output resolution rather than stretching an 18px bitmap. */
    if (!load_font(popup, factor)) {
        return;
    }
    if (popup->visible) {
        popup->redraw_pending = true;
        render_popup(popup);
    }
}

static void surface_preferred_buffer_transform(void *data,
    struct wl_surface *surface, uint32_t transform) {
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_output,
    .leave = surface_output,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

static void popup_text_input_rectangle(
    void *data,
    struct zwp_input_popup_surface_v2 *role,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    (void)data;
    (void)role;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static const struct zwp_input_popup_surface_v2_listener popup_listener = {
    .text_input_rectangle = popup_text_input_rectangle,
};

struct candidate_popup *candidate_popup_create(
    struct wl_compositor *compositor,
    struct wl_shm *shm,
    struct zwp_input_method_v2 *input_method
) {
    struct candidate_popup *popup;

    if (compositor == NULL || shm == NULL || input_method == NULL) {
        return NULL;
    }

    popup = calloc(1, sizeof(*popup));
    if (popup == NULL) {
        return NULL;
    }
    popup->shm = shm;
    for (size_t index = 0; index < POPUP_BUFFER_COUNT; index++) {
        popup->buffers[index].popup = popup;
    }
    if (!load_font(popup, 1)) {
        candidate_popup_destroy(popup);
        return NULL;
    }

    popup->surface = wl_compositor_create_surface(compositor);
    if (
        popup->surface != NULL
        && wl_surface_add_listener(popup->surface, &surface_listener, popup) == 0
    ) {
        popup->role = zwp_input_method_v2_get_input_popup_surface(
            input_method,
            popup->surface
        );
    }
    if (
        popup->surface == NULL
        || popup->role == NULL
        || zwp_input_popup_surface_v2_add_listener(
            popup->role,
            &popup_listener,
            popup
        ) < 0
    ) {
        candidate_popup_destroy(popup);
        return NULL;
    }

    return popup;
}

void candidate_popup_destroy(struct candidate_popup *popup) {
    if (popup == NULL) {
        return;
    }

    for (size_t index = 0; index < POPUP_BUFFER_COUNT; index++) {
        destroy_buffer(&popup->buffers[index]);
    }
    if (popup->role != NULL) {
        zwp_input_popup_surface_v2_destroy(popup->role);
    }
    if (popup->surface != NULL) {
        wl_surface_destroy(popup->surface);
    }
    emoji_font_destroy(&popup->emoji);
    if (popup->font != NULL) {
        FT_Done_Face(popup->font);
    }
    if (popup->freetype != NULL) {
        FT_Done_FreeType(popup->freetype);
    }
    free(popup);
}

bool candidate_popup_update(
    struct candidate_popup *popup,
    const char *code,
    const char *label,
    const struct code_table_entry *const *candidates,
    size_t candidate_count,
    size_t cursor,
    size_t total_candidate_count
) {
    size_t offset;
    int written;

    if (popup == NULL || code == NULL || *code == '\0') {
        return false;
    }
    bool show_codes = strchr(code, CODE_TABLE_WILDCARD) != NULL;

    written = snprintf(
        popup->text, sizeof(popup->text), "%s%s",
        label != NULL ? label : "",
        label != NULL && *label != '\0' ? "  " : ""
    );
    if (written < 0 || (size_t)written >= sizeof(popup->text)) {
        return false;
    }
    offset = (size_t)written;
    popup->cursor_candidate_start = 0;
    popup->cursor_candidate_end = 0;

    if (candidate_count == 0) {
        written = snprintf(
            popup->text + offset,
            sizeof(popup->text) - offset,
            "无候选"
        );
        if (written < 0 || (size_t)written >= sizeof(popup->text) - offset) {
            return false;
        }
    } else {
        for (size_t index = 0; index < candidate_count; index++) {
            size_t start = offset;

            written = snprintf(
                popup->text + offset,
                sizeof(popup->text) - offset,
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
                || (size_t)written >= sizeof(popup->text) - offset
            ) {
                return false;
            }
            offset += (size_t)written;
            if (index == cursor) {
                popup->cursor_candidate_start = start;
                popup->cursor_candidate_end = offset;
            }
        }
    }

    if (total_candidate_count > candidate_count) {
        written = snprintf(
            popup->text + offset,
            sizeof(popup->text) - offset,
            "  +%zu",
            total_candidate_count - candidate_count
        );
        if (written < 0 || (size_t)written >= sizeof(popup->text) - offset) {
            return false;
        }
    }

    popup->redraw_pending = true;
    if (!render_popup(popup)) {
        return false;
    }
    popup->visible = true;
    return true;
}

void candidate_popup_hide(struct candidate_popup *popup) {
    if (popup == NULL || !popup->visible) {
        return;
    }

    popup->text[0] = '\0';
    popup->redraw_pending = false;
    popup->visible = false;
    wl_surface_attach(popup->surface, NULL, 0, 0);
    wl_surface_commit(popup->surface);
}
