#ifndef MEMORIES_DECOMP_CARD_GRID_H
#define MEMORIES_DECOMP_CARD_GRID_H

#include "../types.h"

#define CARD_GRID_SECTION_SIDE_LENGTH 10
#define CARD_GRID_SECTIONS_PER_ROW 2
#define CARD_GRID_SECTION_CARD_COUNT \
    (CARD_GRID_SECTION_SIDE_LENGTH * CARD_GRID_SECTION_SIDE_LENGTH)
#define CARD_GRID_SECTION_ROW_CARD_COUNT \
    (CARD_GRID_SECTION_CARD_COUNT * CARD_GRID_SECTIONS_PER_ROW)

/* How many section rows the grid has: the four that hold the disc's 722
 * cards, or on the PC port as many as the cards this run has need
 * (card_constants.h). */
#ifdef MEMORIES_PC
#define CARD_GRID_SECTION_ROW_COUNT \
    ((CARD_COUNT_LIVE + CARD_GRID_SECTION_ROW_CARD_COUNT - 1) / \
     CARD_GRID_SECTION_ROW_CARD_COUNT)
#else
#define CARD_GRID_SECTION_ROW_COUNT 4
#endif
#define CARD_GRID_ROW_COUNT \
    (CARD_GRID_SECTION_ROW_COUNT * CARD_GRID_SECTION_SIDE_LENGTH)

/* The card grid's cursor position.
 *
 * Library_UpdateGridCursor navigates the grid and reads both into s32 locals;
 * func_8002BFCC places the cursor sprite within a section from them, deriving
 * x from the column remainder and y from the row quotient/remainder. The
 * cursor spans two side-by-side 10x10 sections per section row.
 *
 * Both must stay small-data eligible, and that is not a style question here:
 * Library_UpdateGridCursor's retail loads are gp-relative. A plain s8 scalar
 * is eligible under its uniform -G8 profile; an oversized array or a .data
 * attribute would change that address formation.
 *
 * The s8 spelling is measured rather than preferred. #3054 established that
 * what this code requires is a signed read: declaring them u8 and dropping
 * the casts builds an executable sixteen bytes short, while u8 with an
 * explicit `(s8)` cast, s8 with the cast, and s8 without it all produce the
 * same instructions. Both sources now take the last of those.
 */
#ifdef MEMORIES_PC
/* Wider on the PC port, whose grid can run past 127 rows; defined in
 * src/pc/game/card_storage.c. */
extern s16 gCardGrid_bCursorColumn;
extern s16 gCardGrid_bCursorRow;
#else
extern s8 gCardGrid_bCursorColumn;
extern s8 gCardGrid_bCursorRow;
#endif

#endif
