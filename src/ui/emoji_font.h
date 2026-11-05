#ifndef WAYTINYIME_EMOJI_FONT_H
#define WAYTINYIME_EMOJI_FONT_H

#include <stdbool.h>
#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H

struct emoji_font {
    FT_Face face;
    double scale;
};

/* Optional fallback: a missing emoji font must not break ordinary Chinese UI. */
bool emoji_font_init(struct emoji_font *font, FT_Library library, int pixels);
void emoji_font_destroy(struct emoji_font *font);
bool emoji_font_load(struct emoji_font *font, uint32_t codepoint);
int emoji_font_advance(const struct emoji_font *font);
void emoji_font_draw(
    const struct emoji_font *font,
    uint32_t *pixels,
    int width,
    int height,
    int stride,
    int pen_x,
    int baseline,
    uint32_t foreground
);

#endif
