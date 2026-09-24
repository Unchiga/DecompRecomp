/* LIBAPI Krom2RawAdd/Krom2RawAdd2: the address of a character's pattern in
 * the BIOS kanji ROM, which the console keeps at 0xBFC64000 and the port has
 * no copy of. The credits module (SU sectors 0x4C7-0x4D7, run interpreted at
 * 0x80180000) draws every name with it, reading 30 bytes per Shift-JIS
 * character: 16 pixels wide, 15 rows, two bytes a row, the leftmost pixel in
 * the top bit. Glyphs are rendered on demand with FreeType from the face
 * fontconfig names for Japanese (a CJK face when one is installed, else
 * whatever sans-serif exists: the US credits are full-width Latin) and kept
 * for the run; the interpreter reads host memory through the address like
 * any guest address. A code that cannot be converted or rendered gets a
 * blank pattern rather than the ROM's -1, which the module never checks.
 * On Windows the face is a Japanese system font (Win32_FontPath) and
 * Shift-JIS converts through code page 932. */
#include "pc/debug/log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#ifdef _WIN32
#include "pc/platform/win32.h"
#include <windows.h>
#else
#include <fontconfig/fontconfig.h>
#include <iconv.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Prototypes as psyq/libapi.h has them; that header's own libc declarations
 * clash with the host's, so it is not included here. */
long Krom2RawAdd(unsigned long sjis);
long Krom2RawAdd2(unsigned short sjis);

#define GLYPH_BYTES 30
#define GLYPH_W 16
#define GLYPH_H 15

static FT_Library library;
static FT_Face face;
static int face_tried;
static unsigned char *patterns[65536];
static unsigned char blank[GLYPH_BYTES];

#ifdef _WIN32
static void open_face(void)
{
    const char *file = Win32_FontPath(1);
    face_tried = 1;
    if (FT_Init_FreeType(&library)) return;
    if (file && FT_New_Face(library, file, 0, &face) == 0) {
        FT_Set_Pixel_Sizes(face, 0, 14);
        LOG(LOG_WINDOW, "kanji ROM glyphs from %s", file);
    } else {
        face = NULL;
    }
}

/* Shift-JIS is Windows code page 932. */
static uint32_t sjis_to_unicode(unsigned code)
{
    char in[2];
    wchar_t out[2];
    in[0] = (char)(code >> 8);
    in[1] = (char)code;
    return MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, in, 2, out, 2) == 1 ? out[0] : 0;
}
#else
static void open_face(void)
{
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
    face_tried = 1;
    if (!FcInit() || FT_Init_FreeType(&library)) return;
    pattern = FcNameParse((const FcChar8 *)"sans-serif:lang=ja");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FT_New_Face(library, (const char *)file, 0, &face) == 0) {
        /* 15 rows including descenders: an em of 14 leaves a pixel of air. */
        FT_Set_Pixel_Sizes(face, 0, 14);
        LOG(LOG_WINDOW, "kanji ROM glyphs from %s", (const char *)file);
    } else {
        face = NULL;
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
}

static uint32_t sjis_to_unicode(unsigned code)
{
    static iconv_t converter = (iconv_t)-1;
    static int tried;
    char in[2], out[4];
    char *in_at = in, *out_at = out;
    size_t in_left = 2, out_left = sizeof(out);
    if (!tried) {
        tried = 1;
        converter = iconv_open("UTF-32LE", "SHIFT_JIS");
    }
    if (converter == (iconv_t)-1) {
        /* Without iconv: the full-width Latin block, which is what the US credits use. */
        if (code >= 0x824F && code <= 0x8258) return '0' + (code - 0x824F);
        if (code >= 0x8260 && code <= 0x8279) return 'A' + (code - 0x8260);
        if (code >= 0x8281 && code <= 0x829A) return 'a' + (code - 0x8281);
        return code == 0x8140 ? ' ' : 0;
    }
    in[0] = (char)(code >> 8);
    in[1] = (char)code;
    iconv(converter, NULL, NULL, NULL, NULL);
    if (iconv(converter, &in_at, &in_left, &out_at, &out_left) == (size_t)-1 || out_left != 0) return 0;
    return (uint32_t)(unsigned char)out[0] | (uint32_t)(unsigned char)out[1] << 8 |
           (uint32_t)(unsigned char)out[2] << 16 | (uint32_t)(unsigned char)out[3] << 24;
}
#endif

/* Render into a 16x15 cell: horizontally centred, baseline on row 11 so
 * ascenders and descenders both fit at a 14-pixel em. */
static void render(unsigned code, unsigned char *out)
{
    uint32_t unicode = sjis_to_unicode(code);
    FT_Bitmap *bitmap;
    int row, column, left, top;
    memset(out, 0, GLYPH_BYTES);
    if (!face || !unicode || unicode == ' ' || unicode == 0x3000) return;
    if (FT_Load_Char(face, unicode, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME)) return;
    bitmap = &face->glyph->bitmap;
    left = (GLYPH_W - (int)bitmap->width) / 2;
    top = 11 - face->glyph->bitmap_top;
    for (row = 0; row < (int)bitmap->rows; row++) {
        int y = top + row;
        if (y < 0 || y >= GLYPH_H) continue;
        for (column = 0; column < (int)bitmap->width; column++) {
            int x = left + column;
            if (x < 0 || x >= GLYPH_W) continue;
            if (bitmap->buffer[row * bitmap->pitch + column / 8] & (0x80 >> (column % 8))) {
                out[y * 2 + x / 8] |= (unsigned char)(0x80 >> (x % 8));
            }
        }
    }
}

long Krom2RawAdd2(unsigned short sjis)
{
    unsigned code = sjis;
    if (!face_tried) open_face();
    if (!patterns[code]) {
        patterns[code] = malloc(GLYPH_BYTES);
        if (!patterns[code]) return (long)(uintptr_t)blank;
        render(code, patterns[code]);
    }
    return (long)(uintptr_t)patterns[code];
}

long Krom2RawAdd(unsigned long sjis)
{
    return Krom2RawAdd2((unsigned short)sjis);
}
