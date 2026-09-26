/* Confirm Menu Circle Exit.
 *
 * Circle on the mode-select wheel (CAMPAIGN / FREE DUEL / BUILD DECK /
 * LIBRARY / PASSWORD / SAVE) drops straight to the title menu, so one stray
 * press loses the player's place. This raises a yes/no prompt instead.
 * Cross confirms, Circle cancels, Left/Right choose, NO is the default.
 *
 * Ported from YuGiOhForbiddenMemoriesRecomp, feature/disable-menu-back-fm,
 * src/psx_mode_select_confirm.c. That port is a static recompilation and works
 * from outside the game: it pokes guest RAM by address, hooks the routine that
 * fills the BIOS pad buffer, and steers the leave after the fact. Here the
 * same bytes have names and the functions that read them can be hooked
 * directly, so most of that machinery has nothing to do.
 *
 *   0x80184594 -> gMain_bMenuID      the highlighted row, and the destination
 *                                    a leave carries (MainMenuSelection)
 *   0x80184595 -> D_80184595         the leave-requested flag
 *   0x800EF66A -> gInput_wPad1Pressed and its Held/Repeat twins, already
 *                                    derived, active high, byte order sorted
 *
 * HOW THE LEAVE IS CANCELLED
 *
 * MainMenu_UpdateFrontendMenu's tail reads the pad. Circle on a row >= 5 plays
 * SE 8, sets D_80184595 and falls into MainMenu_StartFrontendEntryTransition(1).
 * A later call sees the transition running, finds the flag set with the row
 * still >= 5, and rewrites gMain_bMenuID to 1 (LOAD) -- which builds the title
 * screen.
 *
 * So the hook is the transition, not the pad. Mode 1 with the flag already set
 * and the wheel on a mode-select row IS the leave, at the one instant it
 * starts and before any state has moved. Refusing to call the original and
 * clearing the flag cancels it outright: no transition begins, nothing to snap
 * back from, and the wheel is left exactly where it stood.
 *
 * Three of upstream's load-bearing rules are then absent. It must never clear
 * held state (the repeat engine derives new-press from it), must hold a
 * release latch, and must eat Circle while the prompt is up -- all because it
 * edits the pad upstream of the repeat engine. We mask the derived words
 * around one call to one consumer and put them back immediately, so the
 * engine's own history never sees an edit. Its bounded 90-frame YES drive is
 * not needed either: on YES we do what the stock branch does, in its order.
 *
 * HOW IT IS DRAWN
 *
 * By the game, with the game's own font. Established by measurement against
 * two VRAM captures (the wheel and the password screen), cross-checked with
 * notes/boot-frontend-sequence.md and upstream's tools/font_extract.py:
 *
 *   - the charset is 4bpp at VRAM word (640,0), texel (2560,0): texture page
 *     10, and the page's first 128 texels by 72 rows;
 *   - 96 cells, 16 per row, each glyph 8 wide and 12 tall, pitch 8 and 12;
 *   - ASCII 0x21..0x5A are cells 0..57, six non-ASCII symbols follow, and
 *     0x60..0x7F resume at cell 64. Space is an advance, not a cell;
 *   - the 4-bit value is a brightness ramp, not a palette index: 0 is
 *     transparent, 1 the dark outline the game draws round every glyph, the
 *     high values the core. The tint therefore comes from which ramp the CLUT
 *     points at -- the eight rows at VRAM (640,232), which boot stage 2 loads
 *     and which are the game's own text colours;
 *   - the block is byte-identical in both captures, so it is resident here.
 *
 * The frame art is loaded once from the disc and uploaded when opened.
 * GsSortPoly copies a
 * primitive into its own packet buffer before linking it (src/pc/sdk/libgs.c),
 * so the primitives below can be locals and need no guest-addressable arena.
 *
 * The one thing not measured is the ordering-table priority, which decides
 * whether the panel lands in front of the wheel. It is a setting.
 */

#include <stdlib.h>

#include "pc/mods/modapi.h"

#include "game/input.h"
#include "game/main_menu_selection.h"
#include "overlays/main_menu/frontend.h"

s32 MainMenu_UpdateFrontendMenu(void);
void MainMenu_StartFrontendEntryTransition(s32 mode);
void SD_SEPlay(s32 id, s32 volume, s32 pan);

/* The port's own overlays. Their Circle closes them, and that press reaches
 * the game underneath as well -- on the SAVE row that is a leave request, so
 * backing out of the save menu was raising this prompt. */
int SaveMenu_Active(void);
int Menu_IsOpen(void);

/* The sprite ordering table the game's own screen-space primitives go to:
 * func_80015EF4.c takes `tab = D_800E9D90` and sorts into `tab[2]`. */
extern void *D_800E9D90[];
void GsSortPoly(void *primitive, void *ot, unsigned short priority);

typedef struct { s16 x, y, w, h; } ModRect;   /* psyq/libgpu.h RECT */
int LoadImage(ModRect *rect, u32 *pixels);
int DrawSync(int mode);

/* psyq/libgpu.h layouts, spelled here so the mod carries no SDK headers it
 * would otherwise only need for two structs. The port's GsSortPoly reads the
 * code out of word 1 and walks the vertices from it, so these must match. */
typedef struct {
    u32 tag;
    u8 r0, g0, b0, code;
    s16 x0, y0;
    s16 x1, y1;
    s16 x2, y2;
    s16 x3, y3;
} ModPolyF4;

typedef struct {
    u32 tag;
    u8 r0, g0, b0, code;
    s16 x0, y0; u8 u0, v0; u16 clut;
    s16 x1, y1; u8 u1, v1; u16 tpage;
    s16 x2, y2; u8 u2, v2; u16 pad1;
    s16 x3, y3; u8 u3, v3; u16 pad2;
} ModPolyFT4;

#define CODE_F4  0x28
#define CODE_FT4 0x2C

/* The password screen's box furniture, read from the player's own disc at
 * runtime -- the mod ships no game art. Offsets verified by decoding them
 * against a real disc and comparing with the asset manager's own cuts:
 * ui/password/frame is 256x256 at 4bpp from archive 0xFAF800, its 16-entry
 * palette at 0xFB7A00. Both land on sector boundaries of WA_MRG, so one
 * 17-sector read covers the sheet and the palette together. */
#define WA_PATH        "\\DATA\\WA_MRG.MRG;1"
#define FRAME_SECTOR   8031          /* 0xFAF800 / 2048 */
#define FRAME_SECTORS  17            /* the sheet, then the palette */
#define FRAME_BYTES    (FRAME_SECTORS * 2048)
#define CLUT_AT        0x8200        /* 0xFB7A00 - 0xFAF800: strips and edges */
/* The field is the same sheet through the NEXT palette. Drawing it through
 * the strips' palette is what made the interior stone instead of navy: the
 * rect was right all along, the CLUT was one along. Both hashes in the asset
 * manager's own cut names say so, and decoding the disc through 0xFB7A20
 * reproduces its extraction of the field exactly. */
#define FIELD_CLUT_AT  0x8220        /* 0xFB7A20 - 0xFAF800 */
#define FIELD_CX 912
#define FIELD_CY 448
#define FIELD_CLUT (u16)(((FIELD_CY & 0x1FF) << 6) | ((FIELD_CX >> 4) & 0x3F))

/* A 64-word by 256-row page free on this screen in both VRAM captures, which
 * is exactly what a 256x256 4bpp sheet occupies, and sixteen spare words for
 * the palette. */
#define ART_VX   832
#define ART_VY   0
#define ART_VW   64
#define ART_VH   256
#define ART_CX   896
#define ART_CY   448
#define ART_TPAGE 13                 /* 832 / 64, 4bpp, ty 0 */
#define ART_CLUT (u16)(((ART_CY & 0x1FF) << 6) | ((ART_CX >> 4) & 0x3F))

/* The pieces the game itself cuts from that sheet, as its own draw list does:
 * a stretched field, two tiled side edges, and top and bottom strips whose
 * ends carry the corners. */
/* Measured off the sheet rather than taken from the cut names: the strips are
 * seven rows, not eight, and rows 7-8 between them are blank. Starting the
 * bottom strip at 8 led it with that blank row, which showed as a band under
 * the box. */
#define SRC_TOP_X 0
#define SRC_TOP_Y 0
#define SRC_BOT_Y 9
#define SRC_STRIP_W 128
#define SRC_STRIP_H 7
#define SRC_EDGE_Y 24
#define SRC_EDGE_W 8
#define SRC_EDGE_H 64
#define SRC_FIELD_X 16
#define SRC_FIELD_Y 24
#define SRC_FIELD_W 112
#define SRC_FIELD_H 66   /* rows 66..71 carry nothing */


/* The charset, as measured above. */
#define FONT_TPAGE   10
#define FONT_CLUT_X  640
#define FONT_CLUT_Y  232
#define GLYPH_H      12
#define CELL_PITCH_X 8
#define CELL_PITCH_Y 12
#define CELLS_PER_ROW 16

/* A CLUT word addresses the palette by its VRAM position: y in the high bits,
 * x in sixteens. Row 0 of the ramp block, plus the row a tint asks for. */
#define CLUT_WORD(row) \
    (u16)((((FONT_CLUT_Y + (row)) & 0x1FF) << 6) | ((FONT_CLUT_X >> 4) & 0x3F))

/* Measured ink width of every cell, which is the advance the game's own text
 * uses: proportional, not fixed. Zero where the atlas has no glyph. */
static const u8 GLYPH_W[96] = {
    5, 6, 8, 8, 8, 8, 5, 5, 6, 8, 8, 5, 8, 5, 7, 7,
    7, 8, 8, 7, 7, 8, 8, 8, 8, 6, 6, 8, 8, 8, 8, 0,
    8, 8, 8, 8, 8, 8, 8, 8, 7, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0, 0, 0, 0, 0, 0,
    6, 8, 8, 8, 8, 8, 7, 8, 8, 6, 7, 8, 6, 8, 8, 8,
    8, 8, 8, 8, 7, 8, 8, 8, 8, 8, 8, 0, 0, 8, 8, 8,
};

static int glyph_cell(unsigned char c)
{
    if (c >= 0x21u && c <= 0x5Au) {
        return (int)c - 0x21;
    }
    if (c >= 0x60u && c <= 0x7Fu) {
        return (int)c - 0x20;
    }
    return -1;   /* space included: an advance, not a cell */
}

/* The cross and circle buttons, from the OK/END row the password screen
 * prints. They need neither a disc read nor an upload: they sit in the same
 * persistent boot block as the charset, at VRAM words 708 and 720, row 128 --
 * page 11, so uv (16,128) and (64,128) with the palette at VRAM (512,252).
 * Upstream's own manifest (tools/disc_assets.py) gives these coordinates and
 * its comment names the circle's uv, which is what confirmed the page. */
#define BTN_TPAGE 11
#define BTN_CLUT  (u16)(((252 & 0x1FF) << 6) | ((512 >> 4) & 0x3F))
#define BTN_V     128
#define BTN_SIZE  16
#define XBTN_U    16
#define OBTN_U    64

/* The box, in the guest's own 320x240 picture, at upstream's size and place. */
#define BOX_W 160
#define BOX_H 76
#define BOX_X ((320 - BOX_W) / 2)
#define BOX_Y ((240 - BOX_H) / 2)
#define ROW_TITLE  13
#define ROW_CHOICE 32
#define ROW_HINT   52
#define PLATE_H    16

#define FIRST_MODE_ROW MAIN_MENU_SELECTION_CAMPAIGN
#define SE_MOVE 6
#define SE_BACK 8
#define SE_REFUSED 9

enum { SEL_YES = 0, SEL_NO = 1 };

static const MemoriesModHost *host;
static void *orig_update;
static void *orig_transition;

static int s_open;
static int s_art;        /* 0 not tried, 1 loaded, -1 failed: flat quads */
static u32 *s_sheet;     /* the sheet and both palettes, kept in RAM */
static int s_sel;
extern u8 D_8009B26C[];
int DeckMenu_Active(void);
static unsigned s_sig;

/* ---- drawing ------------------------------------------------------------ */

/* Which of the game's ordering tables to sort into. The wheel's own entries
 * take OT 1 (DisplayObject_SelectOrderingTable1) and composite in front of
 * OT 2, so this is a setting rather than a constant until the screen says
 * which one puts the panel over them. */
/* The wheel's own entries take ordering table 1 (DisplayObject_SelectOrderingTable1)
 * and composite in front of table 2, so the prompt goes to the same one to sit
 * over them. Settled on screen; a constant rather than a knob. */
#define OT_INDEX 1

static void *table(void)
{
    return D_800E9D90[OT_INDEX];
}

static void quad(int x, int y, int w, int h, u8 r, u8 g, u8 b, unsigned pri)
{
    ModPolyF4 p;
    p.tag = 0;
    p.code = CODE_F4;
    p.r0 = r; p.g0 = g; p.b0 = b;
    p.x0 = (s16)x;       p.y0 = (s16)y;
    p.x1 = (s16)(x + w); p.y1 = (s16)y;
    p.x2 = (s16)x;       p.y2 = (s16)(y + h);
    p.x3 = (s16)(x + w); p.y3 = (s16)(y + h);
    GsSortPoly(&p, table(), (unsigned short)pri);
}

static void glyph(int x, int y, int cell, int tint, unsigned pri)
{
    ModPolyFT4 p;
    const int u = (cell % CELLS_PER_ROW) * CELL_PITCH_X;
    const int v = (cell / CELLS_PER_ROW) * CELL_PITCH_Y;

    p.tag = 0;
    p.code = CODE_FT4;
    /* Neutral modulation: the colour comes from the ramp the CLUT selects. */
    p.r0 = 0x80; p.g0 = 0x80; p.b0 = 0x80;
    if (tint < 0 || tint > 6) tint = 0;
    p.clut = CLUT_WORD(tint);
    p.tpage = FONT_TPAGE;
    p.pad1 = 0;
    p.pad2 = 0;
    p.x0 = (s16)x;                   p.y0 = (s16)y;
    p.u0 = (u8)u;                    p.v0 = (u8)v;
    p.x1 = (s16)(x + CELL_PITCH_X);  p.y1 = (s16)y;
    p.u1 = (u8)(u + CELL_PITCH_X);   p.v1 = (u8)v;
    p.x2 = (s16)x;                   p.y2 = (s16)(y + GLYPH_H);
    p.u2 = (u8)u;                    p.v2 = (u8)(v + GLYPH_H);
    p.x3 = (s16)(x + CELL_PITCH_X);  p.y3 = (s16)(y + GLYPH_H);
    p.u3 = (u8)(u + CELL_PITCH_X);   p.v3 = (u8)(v + GLYPH_H);
    GsSortPoly(&p, table(), (unsigned short)pri);
}

/* One slice of the sheet, source rect stretched onto dest rect. */
static void slice_from(int dx, int dy, int dw, int dh,
                       int su, int sv, int sw, int sh,
                       u16 tpage, u16 clut, unsigned pri)
{
    ModPolyFT4 p;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    p.tag = 0;
    p.code = CODE_FT4;
    p.r0 = 0x80; p.g0 = 0x80; p.b0 = 0x80;
    p.clut = clut;
    p.tpage = tpage;
    p.pad1 = 0;
    p.pad2 = 0;
    p.x0 = (s16)dx;        p.y0 = (s16)dy;
    p.u0 = (u8)su;         p.v0 = (u8)sv;
    p.x1 = (s16)(dx + dw); p.y1 = (s16)dy;
    p.u1 = (u8)(su + sw - 1); p.v1 = (u8)sv;
    p.x2 = (s16)dx;        p.y2 = (s16)(dy + dh);
    p.u2 = (u8)su;         p.v2 = (u8)(sv + sh - 1);
    p.x3 = (s16)(dx + dw); p.y3 = (s16)(dy + dh);
    p.u3 = (u8)(su + sw - 1); p.v3 = (u8)(sv + sh - 1);
    GsSortPoly(&p, table(), (unsigned short)pri);
}

static void slice(int dx, int dy, int dw, int dh,
                  int su, int sv, int sw, int sh, unsigned pri)
{
    slice_from(dx, dy, dw, dh, su, sv, sw, sh, ART_TPAGE, ART_CLUT, pri);
}

/* The box the way the password screen's own draw list assembles one: the
 * field stretched (its art carries the box's light-to-dark shading, so tiling
 * would put the dark bottom mid-box), the two side edges tiled down, then the
 * top and bottom strips whose ends carry the corners. */
static void skin_box(int x0, int y0, int w, int h, unsigned pri)
{
    const int end = SRC_STRIP_W / 2;        /* each half owns a corner */
    int pass, x, y;

    /* The interior: the sheet's own field, stretched. Its art carries the
     * box's light-to-dark shading, so it is stretched rather than tiled. */
    slice_from(x0 + 4, y0 + 4, w - 8, h - 8,
               SRC_FIELD_X, SRC_FIELD_Y, SRC_FIELD_W, SRC_FIELD_H,
               ART_TPAGE, FIELD_CLUT, pri + 1);

    for (y = SRC_STRIP_H; y < h - SRC_STRIP_H; y += SRC_EDGE_H) {
        const int room = h - SRC_STRIP_H - y;
        const int hh = room < SRC_EDGE_H ? room : SRC_EDGE_H;
        slice(x0, y0 + y, SRC_EDGE_W, hh, 0, SRC_EDGE_Y, SRC_EDGE_W, hh, pri);
        slice(x0 + w - SRC_EDGE_W, y0 + y, SRC_EDGE_W, hh,
              SRC_EDGE_W, SRC_EDGE_Y, SRC_EDGE_W, hh, pri);
    }

    for (pass = 0; pass < 2; pass++) {
        const int sv = pass ? SRC_BOT_Y : SRC_TOP_Y;
        const int dy = pass ? y0 + h - SRC_STRIP_H : y0;
        slice(x0, dy, end, SRC_STRIP_H, 0, sv, end, SRC_STRIP_H, pri);
        slice(x0 + w - end, dy, end, SRC_STRIP_H,
              SRC_STRIP_W - end, sv, end, SRC_STRIP_H, pri);
        for (x = end; x < w - end; ) {
            const int room = w - end - x;
            /* the strip's middle, repeated; 108 is what is left of the
             * source from x=20, the point its run of middle begins */
            const int seg = room < 108 ? room : 108;
            slice(x0 + x, dy, seg, SRC_STRIP_H, 20, sv, seg, SRC_STRIP_H, pri);
            x += seg;
        }
    }
}

/* Read the sheet and its palettes off the player's disc. Once: the bytes are
 * then kept, because they have to go back into VRAM more than once. */
static void load_art(void)
{
    u32 *buffer;
    int lba;

    if (s_art != 0) {
        return;
    }
    s_art = -1;                    /* whatever happens below, do not retry */

    lba = host->disc_file_start(host, WA_PATH);
    if (lba < 0) {
        host->log(host, "menu-back-confirm: no %s on the disc", WA_PATH);
        return;
    }
    buffer = malloc(FRAME_BYTES);
    if (buffer == NULL) {
        return;
    }
    if (host->disc_read(host, lba + FRAME_SECTOR, FRAME_SECTORS, buffer) <= 0) {
        host->log(host, "menu-back-confirm: could not read the panel");
        free(buffer);
        return;
    }
    s_sheet = buffer;              /* kept for the life of the process */
    s_art = 1;
}

/* Put the sheet back in VRAM. Every time the prompt opens, not once: the page
 * at (832,0) is free while the wheel is up but it is not ours to keep -- the
 * deck and chest screens upload their own card-template atlas over exactly
 * that page, so coming back from a mode finds our panel gone. The charset and
 * the two buttons survive because they live in the persistent boot block,
 * which is why the text kept drawing after the box had vanished. */
static void upload_art(void)
{
    ModRect rect;

    if (s_art <= 0 || s_sheet == NULL) {
        return;
    }
    rect.x = ART_VX; rect.y = ART_VY; rect.w = ART_VW; rect.h = ART_VH;
    LoadImage(&rect, s_sheet);
    DrawSync(0);
    rect.x = ART_CX; rect.y = ART_CY; rect.w = 16; rect.h = 1;
    LoadImage(&rect, (u32 *)(void *)((u8 *)s_sheet + CLUT_AT));
    DrawSync(0);
    rect.x = FIELD_CX; rect.y = FIELD_CY; rect.w = 16; rect.h = 1;
    LoadImage(&rect, (u32 *)(void *)((u8 *)s_sheet + FIELD_CLUT_AT));
    DrawSync(0);
}

static void button(int x, int y, int u, unsigned pri)
{
    slice_from(x, y, BTN_SIZE, BTN_SIZE, u, BTN_V, BTN_SIZE, BTN_SIZE,
               BTN_TPAGE, BTN_CLUT, pri);
}

static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const int cell = glyph_cell((unsigned char)*s);
        w += (cell < 0 ? 4 : GLYPH_W[cell] + 1);
    }
    return w;
}

static void text_at(int x, int y, const char *s, int tint, unsigned pri)
{
    for (; *s; s++) {
        const int cell = glyph_cell((unsigned char)*s);
        if (cell < 0) {
            x += 4;                  /* space */
            continue;
        }
        glyph(x, y, cell, tint, pri);
        x += GLYPH_W[cell] + 1;
    }
}

static void centred(int cx, int y, const char *s, int tint, unsigned pri)
{
    text_at(cx - text_width(s) / 2, y, s, tint, pri);
}

/* Four layers deep, nearest last: border, panel, selection plate, then text.
 * The plate once shared the panel's depth, which left their order undefined
 * and the panel covering it. */
#define PRI_BASE 8

/* Which of the game's eight text colour ramps each part takes by default.
 * Ramp 0 is white and ramp 1 the gold the game titles with, both read off the
 * screen; the rest are the player's to choose in the Mods window. */
#define TINT_TITLE   1   /* gold, as the screen showed */
#define TINT_CHOSEN  1
#define TINT_DIMMED  0   /* white */
#define TINT_OK      0
#define TINT_CANCEL  0

static void draw_prompt(void)
{
    static const char *const CHOICE[2] = { "YES", "NO" };
    /* Four layers, nearest last. The plate used to share the panel's depth,
     * which left their order undefined and the panel covering it. */
    const unsigned base = PRI_BASE;
    const unsigned pri_border = base + 3;
    const unsigned pri_panel = base + 2;
    const unsigned pri_plate = base + 1;
    const unsigned pri_text = base;
    const int title_tint = host->setting(host, "tint_title", TINT_TITLE);
    const int ok_tint = host->setting(host, "tint_ok", TINT_OK);
    const int cancel_tint = host->setting(host, "tint_cancel", TINT_CANCEL);
    const int chosen_tint = host->setting(host, "tint_chosen", TINT_CHOSEN);
    const int dimmed_tint = host->setting(host, "tint_dimmed", TINT_DIMMED);
    int i;

    /* Panel: a stone edge round a navy field, the password screen's colours.
     * Flat quads for now -- the sheet those are cut from is not resident on
     * this screen, and there is a free 64x256 page at VRAM (832,0) it would
     * fit exactly once this much is confirmed on screen. */
    if (s_art > 0) {
        skin_box(BOX_X, BOX_Y, BOX_W, BOX_H, pri_border);
    } else {
        quad(BOX_X - 3, BOX_Y - 3, BOX_W + 6, BOX_H + 6, 0xB9, 0xB0, 0xA0, pri_border);
        quad(BOX_X, BOX_Y, BOX_W, BOX_H, 0x2A, 0x3A, 0x5E, pri_panel);
    }

    centred(BOX_X + BOX_W / 2, BOX_Y + ROW_TITLE, "RETURN TO TITLE?",
            title_tint, pri_text);

    for (i = 0; i < 2; i++) {
        const int cx = BOX_X + BOX_W / 4 + i * (BOX_W / 2);
        const int w = text_width(CHOICE[i]);
        if (s_sel == i) {
            quad(cx - w / 2 - 6, BOX_Y + ROW_CHOICE - 2, w + 12, PLATE_H,
                 0x44, 0x54, 0x8A, pri_plate);
        }
        centred(cx, BOX_Y + ROW_CHOICE, CHOICE[i],
                s_sel == i ? chosen_tint : dimmed_tint, pri_text);
    }

    /* The button row upstream's wording, now that the sprites themselves are
     * available: each button then its label. The buttons are 16 tall against
     * 12 of text, so they sit two rows higher to share its centre. */
    {
        const int wok = text_width("OK");
        const int wcancel = text_width("CANCEL");
        const int total = BTN_SIZE + 3 + wok + 12 + BTN_SIZE + 3 + wcancel;
        int x = BOX_X + (BOX_W - total) / 2;
        const int y = BOX_Y + ROW_HINT;
        button(x, y - 2, XBTN_U, pri_text);
        x += BTN_SIZE + 3;
        text_at(x, y, "OK", ok_tint, pri_text);
        x += wok + 12;
        button(x, y - 2, OBTN_U, pri_text);
        x += BTN_SIZE + 3;
        text_at(x, y, "CANCEL", cancel_tint, pri_text);
    }
}

/* ---- the prompt --------------------------------------------------------- */

static void prompt_close(void)
{
    if (!s_open) {
        return;
    }
    s_open = 0;
    s_sig++;
}

static void prompt_open(void)
{
    load_art();
    upload_art();
    s_open = 1;
    s_sel = host->setting(host, "default_answer", SEL_NO) == SEL_YES ? SEL_YES : SEL_NO;
    s_sig++;
}

static void leave_to_title(void)
{
    SD_SEPlay(SE_BACK, 0xFF, 0);
    D_80184595 = 1;
    ((void (*)(s32))orig_transition)(1);
}

static int port_ui_open(void)
{
    return SaveMenu_Active() || Menu_IsOpen() || DeckMenu_Active();
}

/* MainMenu_StartFrontendEntryTransition. Mode 1 with the leave flag set and
 * the wheel on a mode-select row is the leave starting. */
static void transition(s32 mode)
{
    const int leaving = mode == 1 && D_80184595 != 0 &&
                        gMain_bMenuID >= FIRST_MODE_ROW;

    /* A leave that begins while one of the port's own overlays is up came
     * from that overlay's own Circle, not from the wheel. Take it back
     * without asking: the player was closing a menu, not leaving the game.
     * Stock would drop to the title here. */
    if (!host->setting(host, "confirm_exit", 1)) {
        ((void (*)(s32))orig_transition)(mode);
        return;
    }
    if (leaving && port_ui_open()) {
        D_80184595 = 0;
        return;
    }

    if (s_open || !leaving) {
        ((void (*)(s32))orig_transition)(mode);
        return;
    }
    D_80184595 = 0;
    prompt_open();
    if (host->log_enabled(host)) {
        host->log(host, "menu-back-confirm: asking, row %u", (unsigned)gMain_bMenuID);
    }
}

/* MainMenu_UpdateFrontendMenu. While the prompt is up this owns the pad, and
 * the screen underneath is handed a frame with no buttons in it. The words go
 * back immediately after: they are the repeat engine's outputs and other
 * readers run later in the frame. */
static s32 update_frontend(void)
{
    const u16 pressed = gInput_wPad1Pressed;
    const u16 held = gInput_wPad1Held;
    const u16 repeat = gInput_wPad1Repeat;
    int leave = 0;
    s32 result;

    /* An overlay opening over the prompt takes the input with it. */
    if (s_open && (port_ui_open() || !host->setting(host, "confirm_exit", 1))) {
        prompt_close();
    }

    if (s_open) {

        if ((pressed & PAD_DIRECTION_LEFT) != 0 && s_sel != SEL_YES) {
            s_sel = SEL_YES;
            SD_SEPlay(SE_MOVE, 0xFF, 0);
            s_sig++;
        } else if ((pressed & PAD_DIRECTION_RIGHT) != 0 && s_sel != SEL_NO) {
            s_sel = SEL_NO;
            SD_SEPlay(SE_MOVE, 0xFF, 0);
            s_sig++;
        }

        if ((pressed & PAD_BUTTON_CANCEL) != 0) {
            prompt_close();
            SD_SEPlay(SE_REFUSED, 0xFF, 0);
        } else if ((pressed & PAD_BUTTON_CONFIRM_MASK) != 0) {
            leave = s_sel == SEL_YES;
            prompt_close();
            if (!leave) {
                SD_SEPlay(SE_REFUSED, 0xFF, 0);
            }
        }

        gInput_wPad1Pressed = 0;
        gInput_wPad1Held = 0;
        gInput_wPad1Repeat = 0;
    }

    result = ((s32 (*)(void))orig_update)();

    gInput_wPad1Pressed = pressed;
    gInput_wPad1Held = held;
    gInput_wPad1Repeat = repeat;

    /* Queued here, inside the frame the screen just built, rather than from
     * the frame callback -- by then the ordering table has been handed off. */
    if (s_open) {
        draw_prompt();
    }

    /* After the call, not before it: the stock Circle branch starts the
     * transition as the last thing it does. */
    if (leave) {
        leave_to_title();
    }
    return result;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void frame(void)
{
    if (s_open && ((D_8009B26C[0] & 0x1f) != 8 ||
                   !host->setting(host, "confirm_exit", 1))) {
        prompt_close();
    }
}

static void applied(int on)
{
    if (!on) {
        prompt_close();
    }
}

static void reset(void)
{
    prompt_close();
}

int YamyiConfirm_Init(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4) {
        return 0;
    }
    host = from;

    mod->api = MEMORIES_MOD_API;
    mod->frame = frame;
    mod->applied = applied;
    mod->reset = reset;

    if (host->hook(host, (void *)MainMenu_StartFrontendEntryTransition,
                   (void *)transition, &orig_transition) == 0) {
        return 0;
    }
    if (host->hook(host, (void *)MainMenu_UpdateFrontendMenu,
                   (void *)update_frontend, &orig_update) == 0) {
        return 0;
    }
    return 1;
}
