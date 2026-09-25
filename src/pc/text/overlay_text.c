#include "overlay_text.h"
#include "glyphs.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

/* Bounded raster cache; every size is rendered directly, never enlarged. */
typedef struct {
    uint32_t character;
    int pixels, width, height, left, top, advance;
    unsigned char *coverage;
} Letter;
static Letter letters[256];
static unsigned next_letter;

static Letter *letter(uint32_t character, int pixels)
{
    Letter *entry;
    FT_Face face;
    unsigned i;
    if (pixels < 8) pixels = 8;
    if (pixels > 64) pixels = 64;
    for (i = 0; i < 256; i++)
        if (letters[i].character == character && letters[i].pixels == pixels) return &letters[i];
    entry = &letters[next_letter++ % 256];
    free(entry->coverage);
    memset(entry, 0, sizeof(*entry));
    entry->character = character;
    entry->pixels = pixels;
    entry->advance = pixels / 2;
    face = Glyphs_Face(character);
    if (!face) { character = '?'; face = Glyphs_Face(character); }
    if (!face || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixels) ||
        FT_Load_Char(face, character, FT_LOAD_RENDER)) return entry;
    entry->advance = (int)(face->glyph->advance.x >> 6);
    entry->left = face->glyph->bitmap_left;
    entry->top = face->glyph->bitmap_top;
    entry->width = (int)face->glyph->bitmap.width;
    entry->height = (int)face->glyph->bitmap.rows;
    if (!entry->width || !entry->height) return entry;
    if (face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
        face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_MONO) return entry;
    entry->coverage = malloc((size_t)entry->width * (size_t)entry->height);
    if (entry->coverage) {
        int row, col, pitch = face->glyph->bitmap.pitch;
        for (row = 0; row < entry->height; row++) {
            const unsigned char *source = face->glyph->bitmap.buffer +
                (pitch < 0 ? (entry->height - 1 - row) * -pitch : row * pitch);
            for (col = 0; col < entry->width; col++)
                entry->coverage[row * entry->width + col] = face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                    ? ((source[col / 8] & (0x80 >> (col % 8))) ? 255 : 0) : source[col];
        }
    }
    return entry;
}

void OverlayText_Blend(const MenuCanvas *canvas, int x, int y, uint32_t colour, unsigned alpha)
{
    uint32_t *at, under;
    unsigned back, total, r, g, b;
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height || !alpha) return;
    at = canvas->pixels + (size_t)y * (size_t)canvas->stride + (size_t)x;
    under = *at;
    /* An opaque canvas holds the picture; an overlay's own alpha counts too. */
    back = canvas->alpha ? (under >> 24) * (255 - alpha) / 255 : 255 - alpha;
    total = alpha + back;
    if (!total) return;
    r = ((colour >> 16 & 255) * alpha + (under >> 16 & 255) * back) / total;
    g = ((colour >> 8 & 255) * alpha + (under >> 8 & 255) * back) / total;
    b = ((colour & 255) * alpha + (under & 255) * back) / total;
    *at = (canvas->alpha ? total : 255) << 24 | r << 16 | g << 8 | b;
}

int OverlayText_Width(const char *text, int pixels)
{
    int width = 0;
    while (*text) width += letter(Glyphs_NextCharacter(&text), pixels)->advance;
    return width;
}

void OverlayText_Draw(MenuCanvas *canvas, int x, int middle, int right, const char *text, int pixels, uint32_t colour)
{
    char clipped[1024];
    int baseline = middle + pixels / 3;
    if (right > canvas->width) right = canvas->width;
    if (OverlayText_Width(text, pixels) > right - x) {
        const char *start = text, *end = text;
        int width = OverlayText_Width("...", pixels);
        size_t size;
        if (width > right - x) return;
        while (*end) {
            const char *next = end;
            int advance = letter(Glyphs_NextCharacter(&next), pixels)->advance;
            if (width + advance > right - x || next - start > (int)sizeof(clipped) - 4) break;
            width += advance;
            end = next;
        }
        size = (size_t)(end - start);
        memcpy(clipped, start, size);
        memcpy(clipped + size, "...", 4);
        text = clipped;
    }
    while (*text && x < right) {
        Letter *g = letter(Glyphs_NextCharacter(&text), pixels);
        int row, col;
        if (x + g->advance > right) break;
        for (row = 0; row < g->height && g->coverage; row++) {
            int y = baseline - g->top + row;
            if (y < 0 || y >= canvas->height) continue;
            for (col = 0; col < g->width; col++) {
                int xx = x + g->left + col;
                if (xx < right) OverlayText_Blend(canvas, xx, y, colour, g->coverage[row * g->width + col]);
            }
        }
        x += g->advance;
    }
}
