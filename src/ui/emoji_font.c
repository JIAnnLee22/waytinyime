#include "emoji_font.h"

#include <fontconfig/fontconfig.h>

static int scaled(const struct emoji_font *font, int value) {
    double result = value * font->scale;
    return (int)(result + (result < 0 ? -0.5 : 0.5));
}

bool emoji_font_init(struct emoji_font *font, FT_Library library, int pixels) {
    FcPattern *pattern;
    FcPattern *match;
    FcResult result;
    FcChar8 *path = NULL;
    int face_index = 0;
    bool loaded = false;

    *font = (struct emoji_font){0};
    pattern = FcNameParse((const FcChar8 *)"emoji");
    if (pattern == NULL) {
        return false;
    }
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (
        match != NULL
        && FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch
    ) {
        FcPatternGetInteger(match, FC_INDEX, 0, &face_index);
        loaded = FT_New_Face(library, (const char *)path, face_index, &font->face) == 0;
    }
    if (match != NULL) {
        FcPatternDestroy(match);
    }
    if (!loaded) {
        return false;
    }
    font->scale = 1.0;
    if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)pixels) == 0) {
        return true;
    }
    /* Noto Color Emoji commonly has one ~109px CBDT strike rather than an
     * outline that FreeType can resize to 18px. Render it at its native size
     * and scale both metrics and bitmap into the candidate line. */
    int strike = -1;
    long distance = 0;
    for (int index = 0; index < font->face->num_fixed_sizes; index++) {
        long current = font->face->available_sizes[index].y_ppem - pixels * 64L;
        if (current < 0) {
            current = -current;
        }
        if (strike < 0 || current < distance) {
            strike = index;
            distance = current;
        }
    }
    if (
        strike >= 0 && FT_Select_Size(font->face, strike) == 0
        && font->face->available_sizes[strike].y_ppem > 0
    ) {
        font->scale = pixels * 64.0 / font->face->available_sizes[strike].y_ppem;
        return true;
    }
    emoji_font_destroy(font);
    return false;
}

void emoji_font_destroy(struct emoji_font *font) {
    if (font->face != NULL) {
        FT_Done_Face(font->face);
    }
    *font = (struct emoji_font){0};
}

bool emoji_font_load(struct emoji_font *font, uint32_t codepoint) {
    return font->face != NULL
        && FT_Get_Char_Index(font->face, codepoint) != 0
        && FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER | FT_LOAD_COLOR) == 0;
}

int emoji_font_advance(const struct emoji_font *font) {
    return scaled(font, (int)(font->face->glyph->advance.x >> 6));
}

void emoji_font_draw(
    const struct emoji_font *font,
    uint32_t *pixels,
    int width,
    int height,
    int stride,
    int pen_x,
    int baseline,
    uint32_t foreground
) {
    const FT_GlyphSlot glyph = font->face->glyph;
    const FT_Bitmap *bitmap = &glyph->bitmap;
    int draw_width = scaled(font, (int)bitmap->width);
    int draw_height = scaled(font, (int)bitmap->rows);
    int origin_x = pen_x + scaled(font, glyph->bitmap_left);
    int origin_y = baseline - scaled(font, glyph->bitmap_top);

    if (draw_width <= 0 || draw_height <= 0 || bitmap->buffer == NULL) {
        return;
    }
    for (int row = 0; row < draw_height; row++) {
        int y = origin_y + row;
        unsigned int source_y = (unsigned int)((uint64_t)row * bitmap->rows / draw_height);
        const unsigned char *source = bitmap->pitch >= 0
            ? bitmap->buffer + (size_t)source_y * (size_t)bitmap->pitch
            : bitmap->buffer + (size_t)(bitmap->rows - 1 - source_y) * (size_t)(-bitmap->pitch);

        if (y < 1 || y >= height - 1) {
            continue;
        }
        for (int column = 0; column < draw_width; column++) {
            int x = origin_x + column;
            unsigned int source_x = (unsigned int)((uint64_t)column * bitmap->width / draw_width);
            uint32_t red, green, blue, alpha;
            uint32_t background;

            if (x < 1 || x >= width - 1) {
                continue;
            }
            if (bitmap->pixel_mode == FT_PIXEL_MODE_BGRA) {
                const unsigned char *color = source + (size_t)source_x * 4;
                /* FreeType BGRA channels are already premultiplied by alpha. */
                blue = color[0];
                green = color[1];
                red = color[2];
                alpha = color[3];
            } else {
                if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
                    alpha = source[source_x / 8] & (0x80U >> (source_x % 8)) ? 255 : 0;
                } else if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
                    alpha = source[source_x];
                } else {
                    continue;
                }
                red = ((foreground >> 16) & 0xffU) * alpha / 255U;
                green = ((foreground >> 8) & 0xffU) * alpha / 255U;
                blue = (foreground & 0xffU) * alpha / 255U;
            }
            background = pixels[(size_t)y * (size_t)stride + (size_t)x];
            red += ((background >> 16) & 0xffU) * (255U - alpha) / 255U;
            green += ((background >> 8) & 0xffU) * (255U - alpha) / 255U;
            blue += (background & 0xffU) * (255U - alpha) / 255U;
            pixels[(size_t)y * (size_t)stride + (size_t)x] =
                0xff000000U | (red << 16) | (green << 8) | blue;
        }
    }
}
