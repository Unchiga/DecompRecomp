/* The letters text can be written in (glyphs.h, notes/translation.md). */
#include "glyphs.h"
#include "pc/render/soft_gpu.h"
#include "pc/debug/log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef _WIN32
#include "pc/platform/win32.h"
#else
#include <fontconfig/fontconfig.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_TABLE 0x801D9000u
#define RETAIL_CODES 0x5C
#define ADDED_SJIS 0xF040u           /* Shift-JIS's user area: no retail glyph has one */
#define ADDED_MAX 672                /* small cells in one page: 32 x 21 of 8x12 */

/* --- the retail glyphs ------------------------------------------------ */

/* The Shift-JIS the retail glyphs are, and the characters they write
 * (the letters and digits are worked out). */
static const struct { uint16_t sjis; uint32_t character; } punctuation[] = {
    {0x8149, '!'}, {0x8168, '"'}, {0x8194, '#'}, {0x8190, '$'}, {0x8193, '%'}, {0x8195, '&'}, {0x8166, '\''},
    {0x8169, '('}, {0x816A, ')'}, {0x8196, '*'}, {0x817B, '+'}, {0x8143, ','}, {0x817C, '-'}, {0x8144, '.'},
    {0x815E, '/'}, {0x8146, ':'}, {0x8147, ';'}, {0x8183, '<'}, {0x8181, '='}, {0x8184, '>'}, {0x8148, '?'},
    {0x8173, 0xAB}, {0x8174, 0xBB}, {0x8145, 0xB7}, {0x818A, 0x2640}, {0x8189, 0x2642}, {0x81BC, 0x2282},
    {0x81BD, 0x2283}, {0x83BF, 0x3B1}, {0x83C0, 0x3B2}, {0x83C1, 0x3B3}, {0x81A9, 0x2190}, {0x81A8, 0x2192}};
/* Other ways to type a retail glyph. */
static const struct { uint32_t typed, character; } aliases[] = {
    {0x2019, '\''}, {0x2018, '\''}, {0x201D, '"'}, {0x201C, '"'}, {0x2212, '-'}, {0x2013, '-'}, {0x2014, '-'},
    {0x300A, 0xAB}, {0x300B, 0xBB}, {0x30FB, 0xB7}, {0xA0, ' '}};

static uint32_t retail[RETAIL_CODES];   /* by code: the character, 0 for none */
static int retail_read;

static uint32_t sjis_character(unsigned sjis)
{
    size_t i;
    if (sjis >= 0x824F && sjis <= 0x8258) return '0' + (sjis - 0x824F);
    if (sjis >= 0x8260 && sjis <= 0x8279) return 'A' + (sjis - 0x8260);
    if (sjis >= 0x8281 && sjis <= 0x829A) return 'a' + (sjis - 0x8281);
    for (i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        if (punctuation[i].sjis == sjis) return punctuation[i].character;
    }
    return 0;
}

static unsigned character_sjis(uint32_t character)
{
    size_t i;
    if (character >= '0' && character <= '9') return 0x824F + (character - '0');
    if (character >= 'A' && character <= 'Z') return 0x8260 + (character - 'A');
    if (character >= 'a' && character <= 'z') return 0x8281 + (character - 'a');
    for (i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        if (punctuation[i].character == character) return punctuation[i].sjis;
    }
    return 0;
}

static const uint32_t *table(void)
{
    return (const uint32_t *)(uintptr_t)GLYPH_TABLE;
}

static void read_retail(void)
{
    int code;
    if (retail_read) return;
    retail_read = 1;
    retail[0] = ' ';
    for (code = 1; code < RETAIL_CODES; code++) retail[code] = sjis_character(table()[code] & 0xFFFF);
}

static int retail_code(uint32_t character)
{
    int code;
    size_t i;
    read_retail();
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (aliases[i].typed == character) character = aliases[i].character;
    }
    for (code = 0; code < RETAIL_CODES; code++) {
        if (retail[code] == character) return code;
    }
    return -1;
}

uint32_t Glyphs_Character(int code)
{
    read_retail();
    return code >= 0 && code < RETAIL_CODES ? retail[code] : 0;
}

/* --- what an added glyph is made of ----------------------------------- */

enum {
    MARK_NONE, MARK_GRAVE, MARK_ACUTE, MARK_CIRCUMFLEX, MARK_TILDE, MARK_MACRON, MARK_BREVE, MARK_DOT,
    MARK_DIAERESIS, MARK_RING, MARK_DOUBLE_ACUTE, MARK_CARON, MARK_CEDILLA, MARK_OGONEK, MARK_COUNT,
    /* Not marks of Unicode's, but drawn the same way. */
    MARK_FLIP = MARK_COUNT, MARK_DOTLESS, MARK_SLASH, MARK_BAR
};

static const struct { uint32_t character; char base; unsigned char mark; } accents[] = {
#include "accents.inc"
    {0x00BF, '?', MARK_FLIP}, {0x00A1, '!', MARK_FLIP}, {0x0131, 'i', MARK_DOTLESS},
    {0x00F8, 'o', MARK_SLASH}, {0x00D8, 'O', MARK_SLASH}, {0x0142, 'l', MARK_SLASH}, {0x0141, 'L', MARK_SLASH},
    {0x0111, 'd', MARK_BAR}, {0x0110, 'D', MARK_BAR}, {0x0127, 'h', MARK_BAR}, {0x0126, 'H', MARK_BAR},
};

/* The marks, drawn for the two sizes: '#' is the mark, and the retail
 * glyphs' dark outline goes round it. Rows top to bottom. */
static const char *const small_marks[MARK_COUNT][3] = {
    {0}, {"#..", ".#."}, {"..#", ".#."}, {".#.", "#.#"}, {".#.#", "#.#."}, {"###"}, {"#..#", ".##."},
    {"#"}, {"#.#"}, {".#.", "#.#", ".#."}, {".#.#", "#.#."}, {"#.#", ".#."}, {".#", "#."}, {"#.", ".#"}};
static const char *const large_marks[MARK_COUNT][3] = {
    {0}, {"##...", ".##..", "..##."}, {"...##", "..##.", ".##.."}, {".##.", "#..#"}, {".##..#", "#..##."},
    {"#####"}, {"#..#", ".##."}, {"##", "##"}, {"##.##", "##.##"}, {".##.", "#..#", ".##."},
    {"..#..#", ".#..#.", "#..#.."}, {"#..#", ".##."}, {"..##", "...#", ".##."}, {"##..", ".##."}};

static const struct {
    uint32_t character;
    const char *small[12], *large[16];
} letters[] = {
#include "letters.inc"
};

typedef struct {
    uint32_t character;
    int base;              /* the retail glyph it stands for where it is not drawn */
    char letter;           /* composed: the retail letter */
    unsigned char mark;
    unsigned char made;    /* its pictures are in the bank */
    short letter_shape;    /* one of `letters`, or -1 */
} Added;

static Added added[ADDED_MAX];
static int added_count;

/* --- fonts ---------------------------------------------------------------- */

static FT_Library library;
static FT_Face faces[8];
static int face_count, system_tried;

static void open_face(const char *path)
{
    if (!path || face_count >= (int)(sizeof(faces) / sizeof(faces[0]))) return;
    if (!library && FT_Init_FreeType(&library)) return;
    if (FT_New_Face(library, path, 0, &faces[face_count]) == 0) {
        LOG(LOG_MODS, "glyphs: font %s", path);
        face_count++;
    }
}

void Glyphs_AddFont(const char *path)
{
    open_face(path);
}

static void open_system_face(void)
{
    if (system_tried) return;
    system_tried = 1;
#ifdef _WIN32
    open_face(Win32_FontPath(0));
#else
    {
        FcPattern *pattern, *match;
        FcResult result;
        FcChar8 *file = NULL;
        if (!FcInit()) return;
        pattern = FcNameParse((const FcChar8 *)"sans-serif:bold");
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(NULL, pattern, &result);
        if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) open_face((const char *)file);
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(pattern);
    }
#endif
}

static FT_Face face_for(uint32_t character)
{
    int i;
    open_system_face();
    for (i = 0; i < face_count; i++) {
        if (FT_Get_Char_Index(faces[i], character)) return faces[i];
    }
    return NULL;
}

/* --- codes ---------------------------------------------------------------- */

int Glyphs_Code(uint32_t character)
{
    int code = retail_code(character), i;
    size_t a;
    Added glyph;
    if (code >= 0) return code;
    for (i = 0; i < added_count; i++) {
        if (added[i].character == character) return GLYPHS_EXTENDED_FIRST + i;
    }
    if (added_count >= ADDED_MAX) return -1;
    memset(&glyph, 0, sizeof(glyph));
    glyph.character = character;
    glyph.letter_shape = -1;
    for (a = 0; a < sizeof(letters) / sizeof(letters[0]); a++) {
        if (letters[a].character == character) glyph.letter_shape = (short)a;
    }
    for (a = 0; a < sizeof(accents) / sizeof(accents[0]); a++) {
        if (accents[a].character == character && retail_code((unsigned char)accents[a].base) >= 0) {
            glyph.letter = accents[a].base;
            glyph.mark = accents[a].mark;
            glyph.base = retail_code((unsigned char)accents[a].base);
            break;
        }
    }
    if (!glyph.letter && glyph.letter_shape < 0 && !face_for(character)) return -1;
    added[added_count] = glyph;
    return GLYPHS_EXTENDED_FIRST + added_count++;
}

uint32_t Glyphs_Word(int code)
{
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        const Added *glyph = &added[code - GLYPHS_EXTENDED_FIRST];
        /* Its own character; the small font's index and the mouth's kind
         * are its retail letter's. */
        uint32_t base = glyph->letter ? table()[glyph->base] : 0;
        return (ADDED_SJIS + (uint32_t)(code - GLYPHS_EXTENDED_FIRST)) | (base & 0x0FF70000u);
    }
    return code >= 0 && code < RETAIL_CODES ? table()[code] : 0;
}

uint32_t Glyphs_SortCharacter(int code)
{
    uint32_t character;
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        const Added *glyph = &added[code - GLYPHS_EXTENDED_FIRST];
        character = glyph->letter ? (unsigned char)glyph->letter : glyph->character;
    } else {
        character = Glyphs_Character(code);
    }
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
}

int Glyphs_Base(int code)
{
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        return added[code - GLYPHS_EXTENDED_FIRST].base;
    }
    return code;
}

/* --- pictures ------------------------------------------------------------- */

typedef struct {
    int width, height;
    unsigned char pixels[16][16];   /* 4-bit indices into the text palettes */
} Cell;

/* Where the retail font has a glyph in its page, as func_80035E20 finds it:
 * letters and digits by their Shift-JIS, punctuation by its list. */
static int retail_cell(unsigned sjis, int large, int *u, int *v)
{
    static const unsigned short listed[] = {0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169, 0x816A,
                                            0x8196, 0x817B, 0x8143, 0x817C, 0x8144, 0x815E, 0x8146, 0x8147, 0x8183,
                                            0x8181, 0x8184, 0x8148, 0x8140};
    int i;
    if (sjis - 0x824Fu < 0x4C && !(sjis - 0x8259u < 7 || sjis - 0x827Au < 7)) {
        *u = (int)(sjis & 0xF) * (large ? 16 : 8);
        *v = large ? (int)((sjis - 0x8240) >> 4) * 16 + 0x48 : (int)((sjis - 0x8240) >> 4) * 12;
        return 1;
    }
    for (i = 0; i < (int)(sizeof(listed) / sizeof(listed[0])); i++) {
        if (listed[i] != sjis) continue;
        if (i < 15) {
            *u = i * (large ? 16 : 8);
            *v = large ? 0x48 : 0;
        } else {
            *u = large ? i * 16 - 0x160 : i * 8 - 0x30;
            *v = large ? 0x58 : 0xC;
        }
        return 1;
    }
    return 0;
}

static int page_x(int page) { return (page & 0xF) * 64; }
static int page_y(int page) { return ((page >> 4) & 1) * 256; }

static void read_cell(int font_page, char letter, int large, Cell *cell)
{
    const uint16_t *vram = SoftGpu_Vram();
    int u, v, x, y;
    memset(cell, 0, sizeof(*cell));
    cell->width = large ? 16 : 8;
    cell->height = large ? 16 : 12;
    if (!retail_cell(character_sjis((unsigned char)letter), large, &u, &v)) return;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int tu = u + x;
            uint16_t word = vram[(page_y(font_page) + v + y) * SOFT_GPU_WIDTH + page_x(font_page) + tu / 4];
            cell->pixels[y][x] = (unsigned char)((word >> ((tu & 3) * 4)) & 0xF);
        }
    }
}

static int ink(const Cell *cell, int *top, int *bottom, int *left, int *right)
{
    int x, y, any = 0;
    *top = cell->height; *bottom = -1; *left = cell->width; *right = -1;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            if (!cell->pixels[y][x]) continue;
            any = 1;
            if (y < *top) *top = y;
            if (y > *bottom) *bottom = y;
            if (x < *left) *left = x;
            if (x > *right) *right = x;
        }
    }
    return any;
}

/* The letter's own colour: the most common of its bright indices. */
static int bright(const Cell *cell)
{
    int count[16] = {0}, x, y, best = 14;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) count[cell->pixels[y][x]]++;
    }
    for (x = 8; x < 16; x++) {
        if (count[x] > count[best] || (count[best] == 0 && count[x])) best = x;
    }
    return best;
}

/* Rows top..bottom of the glyph fitted into rows to..bottom, first and last
 * rows kept: room above for a mark, on the same baseline. */
static void squash(Cell *cell, int top, int bottom, int to)
{
    Cell source = *cell;
    int y;
    for (y = 0; y < cell->height; y++) {
        if (y < to || y > bottom) {
            if (y < to) memset(cell->pixels[y], 0, sizeof(cell->pixels[y]));
            continue;
        }
        memcpy(cell->pixels[y], source.pixels[top + ((y - to) * (bottom - top) + (bottom - to) / 2) / (bottom - to)],
               sizeof(cell->pixels[y]));
    }
}

/* Mark pixels in the fill colour, and the outline round them. */
static void stamp(Cell *cell, const int (*points)[2], int count, int fill)
{
    int i, dx, dy;
    for (i = 0; i < count; i++) {
        int x = points[i][0], y = points[i][1];
        if (x >= 0 && x < cell->width && y >= 0 && y < cell->height) cell->pixels[y][x] = (unsigned char)fill;
    }
    for (i = 0; i < count; i++) {
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int x = points[i][0] + dx, y = points[i][1] + dy;
                if (x >= 0 && x < cell->width && y >= 0 && y < cell->height && !cell->pixels[y][x]) cell->pixels[y][x] = 1;
            }
        }
    }
}

static void compose(const Added *glyph, int font_page, int large, Cell *cell)
{
    int top, bottom, left, right, fill, count = 0, x, y;
    int points[64][2];
    read_cell(font_page, glyph->letter, large, cell);
    if (!ink(cell, &top, &bottom, &left, &right)) return;
    fill = bright(cell);
    if ((glyph->letter == 'i' || glyph->letter == 'j') && glyph->mark != MARK_CEDILLA && glyph->mark != MARK_OGONEK) {
        /* The dot goes: the letter keeps what is below the x-height. */
        Cell n;
        int n_top, n_bottom, n_left, n_right;
        read_cell(font_page, 'n', large, &n);
        if (ink(&n, &n_top, &n_bottom, &n_left, &n_right)) {
            for (y = 0; y < n_top; y++) memset(cell->pixels[y], 0, sizeof(cell->pixels[y]));
            ink(cell, &top, &bottom, &left, &right);
        }
    }
    if (glyph->mark == MARK_DOTLESS) return;
    if (glyph->mark == MARK_FLIP) {
        /* Turned upside down within its ink, as an inverted ? and ! are. */
        Cell source = *cell;
        for (y = top; y <= bottom; y++) {
            for (x = left; x <= right; x++) cell->pixels[y][x] = source.pixels[top + bottom - y][left + right - x];
        }
        return;
    }
    if (glyph->mark == MARK_SLASH || glyph->mark == MARK_BAR) {
        int height = bottom - top;
        if (glyph->mark == MARK_BAR) {
            y = top + height / 4 + 1;
            for (x = left > 0 ? left - 1 : 0; x <= right && count < 64; x++) {
                points[count][0] = x; points[count][1] = y; count++;
            }
        } else {
            int from = glyph->letter == 'l' || glyph->letter == 'L' ? top + height / 3 : top + 1;
            int to = glyph->letter == 'l' || glyph->letter == 'L' ? top + (2 * height) / 3 : bottom - 1;
            for (y = from; y <= to && count < 64; y++) {
                points[count][0] = right - 1 - (y - from) * (right - left - 2) / (to - from > 0 ? to - from : 1);
                points[count][1] = y;
                count++;
            }
        }
        stamp(cell, (const int (*)[2])points, count, fill);
        return;
    }
    {
        const char *const *rows = large ? large_marks[glyph->mark] : small_marks[glyph->mark];
        int height = 0, width = 0, x0, y0, below = glyph->mark == MARK_CEDILLA || glyph->mark == MARK_OGONEK;
        while (height < 3 && rows[height]) {
            if ((int)strlen(rows[height]) > width) width = (int)strlen(rows[height]);
            height++;
        }
        if (below) {
            y0 = bottom < cell->height - height ? bottom : cell->height - height;
        } else if (top < height + 1) {
            squash(cell, top, bottom, height + 1);
            y0 = 0;
        } else {
            y0 = top - 1 - height;
        }
        x0 = (left + right + 1) / 2 - width / 2;
        if (glyph->mark == MARK_OGONEK) x0 = right - width + 1;
        if (glyph->mark == MARK_CEDILLA) x0 = (left + right + 1) / 2 - width / 2;
        for (y = 0; y < height; y++) {
            for (x = 0; rows[y][x] && count < 64; x++) {
                if (rows[y][x] != '#') continue;
                points[count][0] = x0 + x; points[count][1] = y0 + y; count++;
            }
        }
        stamp(cell, (const int (*)[2])points, count, fill);
    }
}

/* A character of the built-in letters or from a font, with the retail
 * glyphs' look: their shading by row inside, their dark outline round it. */
static void render(const Added *glyph, int font_page, int large, Cell *cell)
{
    static const char capitals[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int shade[16], y, x, i, baseline = large ? 13 : 10, room = large ? 14 : 6;
    unsigned char mask[16][16];
    FT_Face face = glyph->letter_shape >= 0 ? NULL : face_for(glyph->character);
    memset(cell, 0, sizeof(*cell));
    cell->width = large ? 16 : 8;
    cell->height = large ? 16 : 12;
    /* The shading: per row, the retail capitals' most common inner index. */
    {
        int count[16][16];
        memset(count, 0, sizeof(count));
        for (i = 0; capitals[i]; i++) {
            Cell letter;
            read_cell(font_page, capitals[i], large, &letter);
            for (y = 0; y < letter.height; y++) {
                for (x = 0; x < letter.width; x++) {
                    if (letter.pixels[y][x] >= 2) count[y][letter.pixels[y][x]]++;
                }
            }
        }
        for (y = 0; y < cell->height; y++) {
            int best = 0;
            for (x = 2; x < 16; x++) {
                if (count[y][x] > count[y][best]) best = x;
            }
            shade[y] = best ? best : (y ? shade[y - 1] : 12);
        }
    }
    memset(mask, 0, sizeof(mask));
    if (glyph->letter_shape >= 0) {
        const char *const *rows = large ? letters[glyph->letter_shape].large : letters[glyph->letter_shape].small;
        for (y = 0; y < cell->height; y++) {
            for (x = 0; x < cell->width && rows[y][x]; x++) mask[y][x] = rows[y][x] == '#';
        }
    } else if (!face || FT_Set_Pixel_Sizes(face, 0, large ? 14 : 10) ||
               FT_Load_Char(face, glyph->character, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) {
        return;
    } else {
        FT_Bitmap *bitmap = &face->glyph->bitmap;
        int width = (int)bitmap->width, rows = (int)bitmap->rows;
        int top = baseline - face->glyph->bitmap_top;
        int squeezed = width > room ? room : width;
        int left = 1 + (room - squeezed) / 2;
        for (y = 0; y < rows; y++) {
            int cy = top + y;
            if (cy < 1 || cy > cell->height - 2) continue;
            for (x = 0; x < squeezed; x++) {
                int from = width > room ? x * width / room : x;
                unsigned char level = bitmap->buffer[y * bitmap->pitch + from];
                if (level >= 96) mask[cy][left + x] = 1;
            }
        }
    }
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int dx, dy, edge = 0;
            if (mask[y][x]) {
                cell->pixels[y][x] = (unsigned char)shade[y];
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < cell->width && ny >= 0 && ny < cell->height && mask[ny][nx]) edge = 1;
                }
            }
            if (edge) cell->pixels[y][x] = 1;
        }
    }
}

/* Where added glyph `n` is in the bank: small ones on page 0, large ones on
 * pages 1 on, 256 to a page. */
static void place(int n, int large, int *page, int *u, int *v)
{
    if (large) {
        *page = 1 + n / 256;
        *u = (n % 16) * 16;
        *v = ((n % 256) / 16) * 16;
    } else {
        *page = 0;
        *u = (n % 32) * 8;
        *v = (n / 32) * 12;
    }
}

static void store(uint16_t *bank, int n, int large, const Cell *cell)
{
    int page, u, v, x, y;
    place(n, large, &page, &u, &v);
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int tu = u + x;
            uint16_t *word = &bank[(v + y) * SOFT_GPU_WIDTH + page_x(page) + tu / 4];
            int shift = (tu & 3) * 4;
            *word = (uint16_t)((*word & ~(0xF << shift)) | (cell->pixels[y][x] << shift));
        }
    }
}

/* The text palettes, where the glyphs' primitives read them: the bank is
 * VRAM-shaped and a primitive sampling it takes its palette from it too. */
static void copy_palettes(uint16_t *bank)
{
    const uint16_t *vram = SoftGpu_Vram();
    int y;
    for (y = 0xE8; y <= 0xFF; y++) {
        memcpy(&bank[y * SOFT_GPU_WIDTH + 640], &vram[y * SOFT_GPU_WIDTH + 640], 16 * sizeof(uint16_t));
    }
    memcpy(&bank[0xFF * SOFT_GPU_WIDTH + 544], &vram[0xFF * SOFT_GPU_WIDTH + 544], 16 * sizeof(uint16_t));
}

int Glyphs_Cell(uint32_t sjis, int large, int font_page, int *tpage, int *u, int *v)
{
    uint16_t *bank;
    Added *glyph;
    int n = (int)sjis - (int)ADDED_SJIS, page;
    if (n < 0 || n >= added_count) return 0;
    bank = SoftGpu_Bank(GLYPHS_BANK);
    if (!bank) return 0;
    glyph = &added[n];
    if (!glyph->made) {
        int size;
        for (size = 0; size < 2; size++) {
            Cell cell;
            if (glyph->letter) compose(glyph, font_page, size, &cell);
            else render(glyph, font_page, size, &cell);
            store(bank, n, size, &cell);
        }
        glyph->made = 1;
    }
    copy_palettes(bank);
    place(n, large, &page, u, v);
    *tpage = page | (GLYPHS_BANK << 11);
    return 1;
}

/* --- for HD text (hd_text.h) ---------------------------------------------- */

uint32_t Glyphs_CellCharacter(int in_bank, int page, int large, int u, int v)
{
    uint32_t character;
    int n, cu, cv;
    if (in_bank) {
        /* place(), backwards. */
        if (large) {
            if (page < 1 || u % 16 || v % 16) return 0;
            n = (page - 1) * 256 + (v / 16) * 16 + u / 16;
        } else {
            if (page != 0 || u % 8 || v % 12) return 0;
            n = (v / 12) * 32 + u / 8;
        }
        return n < added_count && added[n].made ? added[n].character : 0;
    }
    /* The retail letters, digits and punctuation a font sets as they are. */
    for (character = '!'; character <= '~'; character++) {
        if (retail_cell(character_sjis(character), large, &cu, &cv) && cu == u && cv == v) return character;
    }
    return 0;
}

int Glyphs_RetailCell(uint32_t character, int large, int *u, int *v)
{
    return retail_cell(character_sjis(character), large, u, v);
}

void *Glyphs_Face(uint32_t character)
{
    return face_for(character);
}
