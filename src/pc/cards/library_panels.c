/* The Library's grid panels past the disc's cards (cards.h).
 *
 * The Library draws its grid over eight panels, two per section row of 200
 * cards, each a display object whose sprite sheet (resource 0/3/n of the
 * Library package) puts the section's range in big digits between a stone
 * frame: "001" over "100" and so on to "701" over "722". The digits come in
 * pieces that only spell those eight ranges (there is no 8 or 9), so with
 * more cards the sections from 701 on get a panel of the port's own: the
 * same frame from the same texture page, with the plain stone band the
 * retail panels have between their two numbers where the numbers were.
 *
 * Drawn each frame from Library_DrawCardGrid, through the game's own
 * sprite-sheet renderer and the first panel's texture, depth and ordering
 * table, only for the section rows on screen. The retail "701-722" panel is
 * moved out of sight, because its section now holds 701 to 800. */
#include "cards.h"
#include "types.h"
#include "ygo_types.h"
#include "game/display_object.h"
#include "game/display_object_render_sprite_sheet.h"
#include "game/graphics_frame.h"
#include "game/ordering_tables.h"
#include "game/card_grid.h"
#include <stdint.h>
#include <string.h>

extern u8 D_800EA1E8[];

#define PANEL_PITCH 178          /* a section row, in pixels (func_80029590) */
#define PANEL_TOP 8
#define PANEL_LEFT 8
#define PANEL_COLUMN 160         /* the second panel of a row is at x 168 */
#define RETAIL_SECTIONS 7        /* 0-6 are numbered right at any count */
#define PARTS_MAX 24

typedef struct {
    SpriteSheetHeader header;
    SpriteSheetPart parts[PARTS_MAX];
} Sheet;

static Sheet sheet;

/* A part in the sheet format DisplayObject_RenderSpriteSheet reads with
 * header flag 0x10: ten-bit offsets whose top bits ride in bits 14-15 of
 * `cell` and `size`, and flag 0x80 for the texture page step in `cell`. */
static void add_part(int dx, int dy, int u, int v, int w, int h, int page)
{
    SpriteSheetPart *part = &sheet.parts[sheet.header.count++];
    part->dx = (s8)(dx & 0xFF);
    part->dy = (s8)(dy & 0xFF);
    part->cell = (u16)((u / 8) | ((v / 8) << 5) | (page << 10) | (((dx >> 8) & 3) << 14));
    part->size = (u16)(((w / 8 - 1) << 5) | ((h / 8 - 1) << 9) | (((dy >> 8) & 3) << 14));
}

static void build_sheet(void)
{
    int band;
    memset(&sheet, 0, sizeof(sheet));
    sheet.header.flags = 0x90;
    /* Where the numbers were: the stone band from between them, five times,
     * with stone from its left end over the ":" in its middle (u 53-67). */
    for (band = 0; band < 5; band++) {
        add_part(8, 16 + band * 24, 0, 32, 48, 24, 0);
        add_part(56, 16 + band * 24, 0, 32, 24, 24, 0);
        add_part(80, 16 + band * 24, 72, 32, 56, 24, 0);
    }
    /* The frame, as every numbered panel has it. */
    add_part(8, -8, 0, 56, 128, 24, 0);
    add_part(8, 136, 0, 0, 128, 32, 0);
    add_part(136, 120, 16, 176, 16, 48, 0);
    add_part(-8, 120, 16, 128, 16, 48, 0);
    add_part(136, -8, 32, 128, 16, 128, 0);
    add_part(-8, -8, 0, 128, 16, 128, 0);
}

void Cards_DrawLibraryPanels(void)
{
    LibraryMotionState *library = (LibraryMotionState *)D_800EA1E8;
    DisplayObject *model = library->slots[0], *last = library->slots[RETAIL_SECTIONS];
    DisplayObject panel;
    int row, rows, first, column;
    if (gCard_nCount <= CARD_COUNT || !model || !last) return;
    if (!sheet.header.count) build_sheet();
    /* The retail 701-722 panel would mislabel a section of 100. */
    last->field_30.h.field_32 = -0x4000;
    /* Hidden with the others while a card is viewed: library_runtime.c
     * clears their 0x40 when the card view opens and sets it again after. */
    if (!(model->flags & 0x40)) return;
    panel = *model;
    panel.field_4C = (s32)(uintptr_t)&sheet;
    panel.update = 0;
    rows = CARD_GRID_SECTION_ROW_COUNT;
    first = ((s16)gGraphics_sViewportY - PANEL_TOP) / PANEL_PITCH;
    if (first < 0) first = 0;
    for (row = first; row < rows && row <= first + 2; row++) {
        for (column = 0; column < CARD_GRID_SECTIONS_PER_ROW; column++) {
            int section = row * CARD_GRID_SECTIONS_PER_ROW + column;
            if (section < RETAIL_SECTIONS || section * CARD_GRID_SECTION_CARD_COUNT >= gCard_nCount) continue;
            panel.field_30.h.field_30 = (s16)(PANEL_LEFT + column * PANEL_COLUMN);
            panel.field_30.h.field_32 = (s16)(PANEL_TOP + row * PANEL_PITCH);
            DisplayObject_RenderSpriteSheet(&panel, (s32)(uintptr_t)D_800E9D90[panel.ot_index],
                                            (s16)panel.field_14);
        }
    }
}
