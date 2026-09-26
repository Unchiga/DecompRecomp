/* Drop Odds - API 4 mod
 *
 * The Library tells you a card exists and nothing about how to get one. This
 * names the duelists who drop the card under the grid cursor and how often.
 *
 * WHERE THE NUMBERS COME FROM
 *
 * WA_MRG 0xE9B000 + 0x1800*(id-1) holds one block of three sectors per
 * opponent: four 1460-byte rows of 722 halfword weights, each row summing to
 * 2048 -- deck pool, S/A-POW, B/C/D, S/A-TEC -- then a 304-byte rank table.
 * Only the three drop rows count here; the deck pool is what an opponent plays
 * with, not what it gives away.
 *
 * Duel_SelectCardDrop (src/game/duel_result_runtime.c) picks one card from the
 * row for the rank earned, with `(rand() & 2047) + 1` against a running sum.
 * So a weight of w in that row IS the chance of that card, w out of 2048, once
 * per duel won at that rank -- no further scaling, which is why the figure can
 * be stated plainly rather than as a relative "rarity".
 *
 * How many opponents there are is counted, not assumed: the first block whose
 * four rows do not each sum to 2048 is one past the end, so a build with more
 * duelists needs no change here.
 *
 * Tables_PoolFor is asked for every pool first, so a mod that edits drops is
 * reflected -- the same call the game's own drop picker makes. Its array is
 * indexed by card id; the disc's row is indexed by id - 1.
 *
 * WHEN IT DRAWS
 *
 * Main_Loop dispatches on the low five bits of D_8009B26C (main_modes.c), so
 * the Library is the running screen exactly while those bits are
 * MAIN_MODE_LIBRARY and the 0x40 "already entered" bit is set. D_800EA1E8's
 * low nibble is then the screen's own state, and 1 is the card grid. Both are
 * read directly, so nothing depends on how often a callback happens to run:
 * mod->frame, for one, is called from GsDrawOt -- once per draw list, not once
 * per game frame -- and a screen that submits several would age any frame
 * counter faster than its own update runs.
 *
 * Nothing is read at init. MemoriesModInit runs at frame 0, before
 * Cards_Build, so the tables are scanned at first use instead.
 *
 * This mod shows the tables and nothing else. Card Name Color carries the same
 * panel with a SCORE column, which is the weight after that mod's own duelist
 * and rank multipliers; the two are alternatives, not companions.
 */
#include "types.h"
#include "pc/mods/modapi.h"

#include <stdio.h>
#include <string.h>

#define CARD_COUNT 722
#define WA_PATH "\\DATA\\WA_MRG.MRG;1"
#define DROPS_SECTOR 7478          /* 0xE9B000 / 2048 */
#define DROPS_STRIDE 3             /* sectors per opponent */
#define DROP_ROW 1460
#define DROP_BLOCK (DROPS_STRIDE * 2048)
#define POOL_COUNT 3               /* S/A-POW, B/C/D, S/A-TEC: rows 1, 2, 3 */
#define WEIGHT_TOTAL 2048          /* DUEL_DROP_WEIGHT_TOTAL */
#define DUELIST_MAX 64             /* room for a build with more of them */
#define NAMED_DUELISTS 40          /* what Tables_DuelistNames holds today */
#define SHOW_MAX 20                /* the most rows the table will print */

/* Tables_PoolFor's pool numbering (pc/cards/tables.h): the deck pool, then the
 * three drop pools in Duel_SelectCardDrop's order. */
enum { TABLES_POOL_DECK, TABLES_POOL_POW };

/* main_modes.c's dispatch table, and the bit Main_RunLibraryMenu sets once it
 * has opened the screen. */
#define MAIN_MODE_LIBRARY 4
#define MAIN_MODE_MASK    0x1F
#define MAIN_MODE_ENTERED 0x40
#define LIBRARY_GRID      1        /* func_8002BAB4's state for the card grid */

extern const char *const Tables_DuelistNames[];
extern int gCard_nCount;
const unsigned short *Tables_PoolFor(int duelist, int pool,
                                     const unsigned short *retail);

/* The running main mode, the Library's own state block, its per-frame
 * dispatcher, and the card under the grid cursor. */
extern u8 D_8009B26C[];
extern u8 D_800EA1E8[];
void func_8002BAB4(void);
s32 Library_GetGridCursorCardId(u8 *state);
/* Bit 0x80 is whether the grid draws the card's cell at all (func_80029EC4),
 * which is the same test the screen makes before it will open one. */
unsigned int Library_GetCardFlags(unsigned char *base, int index);

/* The port's menu bar, which the overlay canvas includes. */
int Menu_Height(void);

/* A card's name the way the game shows it now. Cards_NameUtf8 does this walk
 * but is not in the export table. */
int Cards_Valid(int id);
int Cards_BaseId(int id);
const unsigned char *Cards_NameText(int id);
const unsigned char *Text_Resolve(int id, const unsigned char *retail);
unsigned Glyphs_Character(int code);
#define RETAIL_NAME_OFFSETS 0x801D5800u
#define TEXT_BANK           0x801D0000u

static const MemoriesModHost *host;
static void *orig_dispatch;

typedef struct {
    u8 duelist;
    u8 pool;
    u16 weight;
} Source;

/* Every duelist that drops a card, not just the best few, so the whole list is
 * there to order and to count. */
static Source sources[CARD_COUNT + 1][DUELIST_MAX];
static u8 source_count[CARD_COUNT + 1];
static int duelists;
static int scanned;                /* 0 not yet, 1 done, -1 failed */

/* settings */
enum { SORT_WEIGHT, SORT_DUELIST, SORT_POOL };
static int opt_rows = 3;
static int opt_sort = SORT_WEIGHT;
static int opt_x = 8;
static int opt_y = 8;

#define PANEL_ALPHA 205

#define POOL_NAME(p) ((p) == 0 ? "S/A POW" : (p) == 1 ? "B/C/D" : "S/A TEC")

/* ---- the tables ---------------------------------------------------------- */

static void scan_drops(void)
{
    static u8 block[DROP_BLOCK];
    static u16 retail[POOL_COUNT][CARD_COUNT];
    const int lba = host->disc_file_start(host, WA_PATH);
    int opp;

    if (lba < 0) {
        host->log(host, "drop-odds: no %s on the disc", WA_PATH);
        scanned = -1;
        return;
    }

    for (opp = 1; opp <= DUELIST_MAX; opp++) {
        const u16 *pool_weights[POOL_COUNT];
        int pool, i, ok = 1;

        if (host->disc_read(host, lba + DROPS_SECTOR + (opp - 1) * DROPS_STRIDE,
                            DROPS_STRIDE, block) <= 0)
            break;

        /* Every real block's four rows each add up to 2048. */
        for (pool = 0; pool < 4 && ok; pool++) {
            unsigned long sum = 0;
            for (i = 0; i < CARD_COUNT; i++)
                sum += (unsigned)(block[pool * DROP_ROW + i * 2] |
                                  ((unsigned)block[pool * DROP_ROW + i * 2 + 1] << 8));
            if (sum != (unsigned long)WEIGHT_TOTAL)
                ok = 0;
        }
        if (!ok)
            break;

        for (pool = 0; pool < POOL_COUNT; pool++) {
            const u8 *row = block + (size_t)(pool + 1) * DROP_ROW;
            for (i = 0; i < CARD_COUNT; i++)
                retail[pool][i] = (u16)(row[i * 2] | ((unsigned)row[i * 2 + 1] << 8));
            /* What this opponent's pool is after every applied mod, or NULL
             * when no mod touches it and the disc's row stands. */
            pool_weights[pool] = Tables_PoolFor(opp, TABLES_POOL_POW + pool,
                                                retail[pool]);
        }

        for (i = 1; i <= CARD_COUNT; i++) {
            u16 best = 0;
            int best_pool = 0;
            for (pool = 0; pool < POOL_COUNT; pool++) {
                const u16 *edited = pool_weights[pool];
                /* Tables_PoolFor answers by card id; the disc's row by id - 1. */
                const u16 w = edited != NULL ? edited[i] : retail[pool][i - 1];
                if (w > best) {
                    best = w;
                    best_pool = pool;
                }
            }
            /* One entry per duelist, carrying its best pool of the three: the
             * same opponent listing a card twice is one source with the better
             * of the two, not two rows saying nearly the same thing. */
            if (best == 0 || source_count[i] >= DUELIST_MAX)
                continue;
            sources[i][source_count[i]].duelist = (u8)opp;
            sources[i][source_count[i]].pool = (u8)best_pool;
            sources[i][source_count[i]].weight = best;
            source_count[i]++;
        }
        duelists = opp;
    }

    scanned = 1;
    host->log(host, "drop-odds: read %d opponents' drop pools", duelists);
}

static void ensure_tables(void)
{
    if (scanned != 0)
        return;
    if (gCard_nCount <= 0)
        return;              /* the card tables are not built yet; later */
    scan_drops();
}

/* ---- text ---------------------------------------------------------------- */

/* The card's name in ASCII. draw_text takes ASCII, and every retail name is;
 * a glyph outside it becomes '?' rather than a broken byte sequence. */
static int card_name(int id, char *out, size_t size)
{
    const unsigned char *name;
    size_t n = 0;

    if (!Cards_Valid(id) || size == 0)
        return 0;
    name = Cards_NameText(id);
    if (name == NULL) {
        const int base = Cards_BaseId(id);
        const unsigned short *offsets =
            (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
        name = Text_Resolve(0x8000 + base,
                            (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[base]));
    }
    while (name != NULL && *name < 0xF6 && n + 1 < size) {
        int code = *name++;
        unsigned c;
        if (code >= 0xF0)
            code = ((code - 0xF0) << 8) | *name++;
        c = code ? Glyphs_Character(code) : ' ';
        out[n++] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
    }
    out[n] = '\0';
    return n != 0;
}

static void duelist_name(int duelist, char *out, size_t size)
{
    if (duelist > 0 && duelist < NAMED_DUELISTS)
        snprintf(out, size, "%s", Tables_DuelistNames[duelist]);
    else
        snprintf(out, size, "Duelist %d", duelist);
}

/* ---- the rows the table shows -------------------------------------------- */

typedef struct {
    char name[32];
    char weight[16];
    char chance[16];
    int pool;
    int by_weight;
} Row;

static int shown_id;               /* the card the rows below describe */
static char shown_name[64];
static int shown_ok;
static Row rows[SHOW_MAX];
static int row_count;
static int row_total;              /* duelists dropping it, shown or not */

/* Which of two candidates the chosen column puts first. Numbers read best
 * first; names and pools read in their own order. */
static int ahead_of(const Row *a, const Row *b)
{
    switch (opt_sort) {
    case SORT_DUELIST: return strcmp(a->name, b->name) < 0;
    case SORT_POOL:    return a->pool < b->pool;
    default:           return a->by_weight > b->by_weight;
    }
}

static void build_rows(int id)
{
    static Row all[DUELIST_MAX];
    const Source *list = sources[id];
    const int have = source_count[id];
    int i, j, taken;

    row_total = have;
    row_count = 0;
    shown_ok = card_name(id, shown_name, sizeof shown_name);
    if (!shown_ok)
        return;

    for (i = 0; i < have && i < DUELIST_MAX; i++) {
        duelist_name(list[i].duelist, all[i].name, sizeof all[i].name);
        snprintf(all[i].weight, sizeof all[i].weight, "%d/2048", (int)list[i].weight);
        all[i].by_weight = (int)list[i].weight;
        {
            /* The same weight as a percentage. It is a fixed fraction of the
             * weight, so it never needs an ordering of its own. */
            const int hundredths =
                (int)((unsigned)list[i].weight * 10000u / WEIGHT_TOTAL);
            snprintf(all[i].chance, sizeof all[i].chance, "%d.%02d%%",
                     hundredths / 100, hundredths % 100);
        }
        all[i].pool = list[i].pool;
    }

    /* Take the best by weight, then put those in the order the column asks
     * for. Selection sort over at most 64 candidates for at most 20 rows. */
    taken = have < opt_rows ? have : opt_rows;
    for (i = 0; i < taken; i++) {
        int best = -1;
        for (j = 0; j < have; j++) {
            if (all[j].by_weight < 0)
                continue;          /* already taken */
            if (best < 0 || all[j].by_weight > all[best].by_weight)
                best = j;
        }
        if (best < 0)
            break;
        rows[row_count++] = all[best];
        all[best].by_weight = -1;
    }
    for (i = 1; i < row_count; i++) {
        const Row hold = rows[i];
        for (j = i; j > 0 && ahead_of(&hold, &rows[j - 1]); j--)
            rows[j] = rows[j - 1];
        rows[j] = hold;
    }
}

/* ---- the screen ---------------------------------------------------------- */

/* The card under the grid cursor, or 0 when the Library's grid is not what is
 * running. Read straight from the game rather than remembered from a callback,
 * so the panel cannot outlive the screen or blink out under one. */
static int cursor_card(void)
{
    const unsigned mode = D_8009B26C[0];
    int id;

    if ((mode & MAIN_MODE_MASK) != MAIN_MODE_LIBRARY ||
        (mode & MAIN_MODE_ENTERED) == 0)
        return 0;
    if ((D_800EA1E8[0] & 0xF) != LIBRARY_GRID)
        return 0;
    id = (int)Library_GetGridCursorCardId(D_800EA1E8);
    if (id < 1 || id > CARD_COUNT)
        return 0;
    /* Nothing is said about a card the Library itself does not show yet: the
     * panel would be telling the player what is in a cell they cannot see. */
    if ((Library_GetCardFlags(D_800EA1E8, id) & 0x80) == 0)
        return 0;
    return id;
}

static void read_settings(void)
{
    const int was_rows = opt_rows, was_sort = opt_sort;

    opt_rows = host->setting(host, "rows", 3);
    if (opt_rows < 1) opt_rows = 1;
    if (opt_rows > SHOW_MAX) opt_rows = SHOW_MAX;
    opt_sort = host->setting(host, "sort", SORT_WEIGHT);
    if (opt_sort < 0 || opt_sort > SORT_POOL) opt_sort = SORT_WEIGHT;
    opt_x = host->setting(host, "x", 8);
    if (opt_x < 0) opt_x = 0;
    opt_y = host->setting(host, "y", 8);
    if (opt_y < 0) opt_y = 0;

    if (opt_rows != was_rows || opt_sort != was_sort)
        shown_id = 0;              /* the table has to be built again */
}

/* The Library's own frame, then the card it left under the cursor. The disc
 * reads and the table are done here, on the game's frame, rather than in the
 * overlay, which runs while the picture is being presented. */
static void library_frame(void)
{
    void (*original)(void) = (void (*)(void))orig_dispatch;
    int id;

    original();
    read_settings();

    id = cursor_card();
    if (id == 0)
        return;
    ensure_tables();
    if (scanned != 1 || id == shown_id)
        return;
    shown_id = id;
    build_rows(id);
}

/* ---- the panel ----------------------------------------------------------- */

#define COL_BACK     0x0B0E13u
#define COL_TITLE_BG 0x1B2430u
#define COL_BORDER   0x39424Fu
#define COL_BEVEL    0x55616Fu
#define COL_RULE     0x2A3240u
#define COL_STRIPE   0xFFFFFFu
#define COL_TITLE    0xFFFFFFu
#define COL_NUMBER   0x8FA0B4u
#define COL_HEAD     0x7E8A9Au
#define COL_PICKED   0xFFFFFFu
#define COL_NAME     0xDCE3EBu
#define COL_POOL     0x93A0B0u
#define COL_WEIGHT   0xB9C6D6u
#define COL_CHANCE   0x7FC9FFu
#define COL_FOOT     0x76818Fu
#define COL_NONE     0xE8A85Cu

#define HEAD_NAME   "DUELIST"
/* tables.h calls these pools; what the column actually shows is the duel
 * rank each one belongs to, which is what a player earns and recognises. */
#define HEAD_POOL   "RANK"
#define HEAD_WEIGHT "WEIGHT"
#define HEAD_CHANCE "CHANCE"

static int widest(int a, int b) { return a > b ? a : b; }

/* The heading of the column being ordered on, lit so the table says what it is
 * sorted by without a line of its own. */
static unsigned head_colour(int column)
{
    return column == opt_sort ? COL_PICKED : COL_HEAD;
}

static void panel(void)
{
    char number[16], footer[64];
    int width, height, scale, bar;
    int w_name, w_pool, w_weight, w_chance, w_title, w_foot, w_body;
    int pad, gap, line, rule, box_w, box_h, x, y, cy, i;
    int at_name, at_pool, end_weight, end_chance;

    host->overlay_size(host, &width, &height, &scale);
    if (scale < 1)
        scale = 1;
    if (width <= 0 || height <= 0)
        return;

    snprintf(number, sizeof number, "#%d", shown_id);
    if (row_total == 0)
        snprintf(footer, sizeof footer, "No duelist drops this card");
    else if (row_total > row_count)
        snprintf(footer, sizeof footer, "and %d more duelist%s",
                 row_total - row_count, row_total - row_count == 1 ? "" : "s");
    else
        footer[0] = '\0';

    /* Every column is as wide as its heading or its widest cell. */
    w_name = host->text_width(host, HEAD_NAME, scale);
    w_pool = host->text_width(host, HEAD_POOL, scale);
    w_weight = host->text_width(host, HEAD_WEIGHT, scale);
    w_chance = host->text_width(host, HEAD_CHANCE, scale);
    for (i = 0; i < row_count; i++) {
        w_name = widest(w_name, host->text_width(host, rows[i].name, scale));
        w_pool = widest(w_pool, host->text_width(host, POOL_NAME(rows[i].pool), scale));
        w_weight = widest(w_weight, host->text_width(host, rows[i].weight, scale));
        w_chance = widest(w_chance, host->text_width(host, rows[i].chance, scale));
    }

    line = 16 * scale;
    pad = 9 * scale;
    gap = 12 * scale;
    rule = scale;

    w_title = host->text_width(host, shown_name, scale) + gap +
              host->text_width(host, number, scale);
    w_foot = footer[0] ? host->text_width(host, footer, scale) : 0;
    w_body = row_count ? w_name + gap + w_pool + gap + w_weight + gap + w_chance : 0;
    box_w = widest(widest(w_body, w_title), w_foot) + pad * 2;
    box_h = pad + line
          + (row_count ? rule + line + row_count * line : 0)
          + (footer[0] ? rule + line : 0)
          + pad;

    /* x and y are the panel's own corner, in menu units, measured from the top
     * left of the picture -- under the menu bar, which the canvas includes. */
    bar = Menu_Height();
    x = opt_x * scale;
    y = bar + opt_y * scale;
    if (x + box_w > width) x = width - box_w;
    if (y + box_h > height) y = height - box_h;
    if (x < 0) x = 0;
    if (y < bar) y = bar;

    host->fill(host, x, y, box_w, box_h, COL_BACK, PANEL_ALPHA);
    host->fill(host, x, y, box_w, line + pad, COL_TITLE_BG, PANEL_ALPHA);
    host->fill(host, x, y, box_w, rule, COL_BEVEL, 255);
    host->fill(host, x, y + box_h - rule, box_w, rule, COL_BORDER, 255);
    host->fill(host, x, y, rule, box_h, COL_BORDER, 255);
    host->fill(host, x + box_w - rule, y, rule, box_h, COL_BORDER, 255);

    cy = y + pad;
    host->draw_text(host, x + pad, cy + line / 2, shown_name, COL_TITLE, scale);
    host->draw_text(host, x + box_w - pad - host->text_width(host, number, scale),
                    cy + line / 2, number, COL_NUMBER, scale);
    cy += line;

    at_name = x + pad;
    at_pool = at_name + w_name + gap;
    end_weight = at_pool + w_pool + gap + w_weight;
    end_chance = end_weight + gap + w_chance;

    if (row_count) {
        host->fill(host, x + rule, cy, box_w - rule * 2, rule, COL_RULE, 255);
        cy += rule;
        host->draw_text(host, at_name, cy + line / 2, HEAD_NAME,
                        head_colour(SORT_DUELIST), scale);
        host->draw_text(host, at_pool, cy + line / 2, HEAD_POOL,
                        head_colour(SORT_POOL), scale);
        host->draw_text(host, end_weight - host->text_width(host, HEAD_WEIGHT, scale),
                        cy + line / 2, HEAD_WEIGHT, head_colour(SORT_WEIGHT), scale);
        host->draw_text(host, end_chance - host->text_width(host, HEAD_CHANCE, scale),
                        cy + line / 2, HEAD_CHANCE, COL_HEAD, scale);
        cy += line;

        for (i = 0; i < row_count; i++) {
            /* Every other row lifted a little, so the eye keeps its line
             * across four columns without a ruling between them. */
            if (i & 1)
                host->fill(host, x + rule, cy, box_w - rule * 2, line, COL_STRIPE, 10);
            host->draw_text(host, at_name, cy + line / 2, rows[i].name, COL_NAME, scale);
            host->draw_text(host, at_pool, cy + line / 2, POOL_NAME(rows[i].pool),
                            COL_POOL, scale);
            host->draw_text(host,
                            end_weight - host->text_width(host, rows[i].weight, scale),
                            cy + line / 2, rows[i].weight, COL_WEIGHT, scale);
            host->draw_text(host,
                            end_chance - host->text_width(host, rows[i].chance, scale),
                            cy + line / 2, rows[i].chance, COL_CHANCE, scale);
            cy += line;
        }
    }

    if (footer[0]) {
        host->fill(host, x + rule, cy, box_w - rule * 2, rule, COL_RULE, 255);
        cy += rule;
        host->draw_text(host, x + pad, cy + line / 2, footer,
                        row_total == 0 ? COL_NONE : COL_FOOT, scale);
    }
}

static int panel_live(void)
{
    return scanned == 1 && shown_ok && shown_id >= 1 && shown_id <= CARD_COUNT &&
           cursor_card() == shown_id;
}

static void overlay(void)
{
    if (panel_live())
        panel();
}

static unsigned overlay_signature(void)
{
    if (!panel_live())
        return 0;
    return (unsigned)(shown_id * 128 + opt_rows * 8 + opt_sort * 2 + 1) ^
           ((unsigned)opt_x << 20) ^ ((unsigned)opt_y << 26);
}

static void on_state_loaded(void)
{
    shown_id = 0;
    shown_ok = 0;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4)
        return 0;
    host = from;

    if (host->hook(host, (void *)func_8002BAB4, (void *)library_frame,
                   &orig_dispatch) == 0) {
        host->log(host, "drop-odds: failed to hook func_8002BAB4");
        return 0;
    }

    mod->api = MEMORIES_MOD_API;
    mod->reset = on_state_loaded;
    mod->overlay = overlay;
    mod->overlay_signature = overlay_signature;
    return 1;
}
