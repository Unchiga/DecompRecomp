/* Card Name Color - API 4 mod
 *
 * Card names and card descriptions are built by the same text box. The
 * F8 text command handled by func_80037DA4 distinguishes them:
 *
 *   bit 0x20 = card name
 *   bit 0x40 = card description
 *
 * Hooking func_80039A14() is too late: by then the name and description share
 * the same field_54 colour. This hooks func_80037DA4(), changes field_54 only
 * after a name command, and restores the colour that was in effect before the
 * name when the following description command is encountered.
 *
 * WHAT DECIDES A CARD'S COLOUR
 *
 *   [cards]   pins one card outright, and wins over everything else
 *   [tiers]   colours the rest by how rare the card is to win
 *   neither   the card keeps the colour the game gives it
 *
 * Rarity is the best chance any opponent gives the card: its weight out of
 * 2048 in one of that opponent's three drop pools, times that opponent's tier
 * multiplier and that pool's rank multiplier. A generous opponent anywhere
 * makes a card common, so a LOW score is rare.
 *
 * The weights come off the disc. WA_MRG 0xE9B000 + 0x1800*(id-1) holds 39
 * blocks of three sectors, each four 1460-byte rows of 722 halfword weights
 * summing to 2048 -- deck pool, S/A-POW, B/C/D, S/A-TEC -- then a 304-byte
 * rank table. Verified against a real disc: every row of opponents 1, 2 and 39
 * sums to exactly 2048. Only the three drop rows count here; the deck pool is
 * what an opponent plays with, not what it gives away.
 *
 * COLOURS
 *
 * The game ships seven text ramps and an empty eighth: boot stage 2 uploads
 * 0x100 bytes from WA 0xB61000 as eight 16-colour rows at VRAM (640,232), and
 * rows 232..238 are filled while row 239 is entirely zero. Slot 7 was always
 * addressable and simply drew nothing, so filling it costs nothing else.
 *
 * A slot given an RGB is built from the game's own white ramp -- slot 0 is a
 * pure luminance ramp -- scaled to that colour, so it keeps the dark outline
 * the game draws round every glyph.
 *
 * The upload happens at first use, NOT at load: MemoriesModInit runs at frame
 * 0 and the boot sequence writes those rows afterwards, so anything put there
 * at load is overwritten -- and zero at 4bpp is transparent, not black.
 */
#include "types.h"
#include "game/duel_effect.h"
#include "game/duel_effect_command.h"
#include "pc/mods/modapi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARD_COUNT 722
#define COLOR_COUNT 8
#define TRACKED_BOXES 16

/* the ramps */
#define RAMP_VX 640
#define RAMP_VY 232
#define RAMP_ENTRIES 16
#define WA_PATH "\\DATA\\WA_MRG.MRG;1"
#define RAMP_SECTOR 5826           /* 0xB61000 / 2048 */

/* the drop tables */
#define DROPS_SECTOR 7478          /* 0xE9B000 / 2048 */
#define DROPS_STRIDE 3             /* sectors per opponent */
#define DUELIST_MAX 64             /* room for a build with more of them */
#define NAMED_DUELISTS 40          /* what Tables_DuelistNames holds today */
#define DROP_ROW 1460
#define DROP_BLOCK (DROPS_STRIDE * 2048)
#define RANK_COUNT 3               /* S/A-POW, B/C/D, S/A-TEC: rows 1, 2, 3 */

#define MULT_ONE 1000              /* multipliers held in thousandths */
#define MULT_MAX (10 * MULT_ONE)   /* keeps weight * multiplier in 32 bits */
#define MAX_NAMES 24
#define NAME_MAX 28
#define INI_NAME "card_name_color.ini"

typedef struct { s16 x, y, w, h; } ModRect;   /* psyq/libgpu.h RECT */
int LoadImage(ModRect *rect, u32 *pixels);
int DrawSync(int mode);

extern const char *const Tables_DuelistNames[];
extern int gCard_nCount;
int Cards_Named(const char *text);
int Cards_Valid(int id);
int Cards_BaseId(int id);
const unsigned char *Cards_NameText(int id);
const unsigned char *Text_Resolve(int id, const unsigned char *retail);
unsigned Glyphs_Character(int code);

/* The retail name table, as cards.c reads it. */
#define RETAIL_NAME_OFFSETS 0x801D5800u
#define TEXT_BANK           0x801D0000u

static const MemoriesModHost *host;
static void *original_func_80037DA4;

static u8 card_color[CARD_COUNT + 1];
static u8 card_color_set[CARD_COUNT + 1];

/* colours by name, from [Colors] */
typedef struct {
    char name[NAME_MAX];
    int slot;
} ColorName;
static ColorName color_names[MAX_NAMES];
static int color_name_count;
static u8 custom_rgb[COLOR_COUNT][3];
static u8 custom_set[COLOR_COUNT];
static int ramps_done;

/* rarity tiers, from [tiers] */
typedef struct {
    char name[NAME_MAX];
    long threshold;
    int color;
    int has_threshold;
    int has_color;
} Tier;
static Tier tiers[MAX_NAMES];
static int tier_count;
static int default_color = -1;
static int undroppable_color = -1;

/* multipliers */
typedef struct {
    char name[NAME_MAX];
    long mult;
} NamedMult;
static NamedMult tier_mult[MAX_NAMES];
static int tier_mult_count;
static long duelist_mult[DUELIST_MAX + 1];
static int duelist_count;   /* counted off the disc, not assumed */
static long rank_mult[RANK_COUNT] = { MULT_ONE, MULT_ONE, MULT_ONE };

typedef struct {
    DuelEffectChannel *object;
    u8 saved_color;
    u8 active;
} SavedNameColor;
static SavedNameColor saved_colors[TRACKED_BOXES];

/* ---- small helpers ------------------------------------------------------- */

/* Compare letters and digits only, ignoring case, spaces and punctuation, so
 * "Villager1" finds "Villager 1" and "s_a_pow" finds "S/A-POW". The port's own
 * duelist lookup matches the same way. */
static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
}

static void trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
}

/* "1", "1.5", "0.75" -> thousandths, so scoring stays in integers. */
static long read_multiplier(const char *value)
{
    char *end;
    long whole, frac = 0;
    int digits = 0;

    while (isspace((unsigned char)*value)) value++;
    whole = strtol(value, &end, 10);
    if (*end == '.') {
        end++;
        while (*end >= '0' && *end <= '9' && digits < 3) {
            frac = frac * 10 + (*end - '0');
            end++;
            digits++;
        }
    }
    while (digits++ < 3) frac *= 10;
    if (whole < 0) return MULT_ONE;
    whole = whole * MULT_ONE + frac;
    return whole > MULT_MAX ? MULT_MAX : whole;
}

/* A colour by name, or by its slot number. */
static int color_by_name(const char *value)
{
    int i;
    char buf[NAME_MAX];

    while (isspace((unsigned char)*value)) value++;
    if (*value == '\0') return -1;
    if (isdigit((unsigned char)*value)) {
        const long n = strtol(value, NULL, 10);
        return n >= 0 && n < COLOR_COUNT ? (int)n : -1;
    }
    strncpy(buf, value, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    trim(buf);
    for (i = 0; i < color_name_count; i++)
        if (same_letters(color_names[i].name, buf))
            return color_names[i].slot;
    return -1;
}

/* ---- [Colors] ------------------------------------------------------------
 * "magenta = 7 (255,0,255)" names slot 7 and gives it a colour of its own.
 * "white = 0" only puts a name to a slot the game already fills. */
static void read_color_line(const char *key, const char *value)
{
    const char *open;
    long slot;
    char *end;

    while (isspace((unsigned char)*value)) value++;
    slot = strtol(value, &end, 10);
    if (end == value || slot < 0 || slot >= COLOR_COUNT)
        return;

    if (color_name_count < MAX_NAMES) {
        strncpy(color_names[color_name_count].name, key, NAME_MAX - 1);
        color_names[color_name_count].name[NAME_MAX - 1] = '\0';
        color_names[color_name_count].slot = (int)slot;
        color_name_count++;
    }

    open = strchr(end, '(');
    if (open != NULL) {
        const char *at = open + 1;
        u8 rgb[3];
        int i;
        for (i = 0; i < 3; i++) {
            char *stop;
            long n;
            while (*at && !isdigit((unsigned char)*at)) at++;
            n = strtol(at, &stop, 10);
            if (stop == at || n < 0 || n > 255) return;
            rgb[i] = (u8)n;
            at = stop;
        }
        custom_rgb[slot][0] = rgb[0];
        custom_rgb[slot][1] = rgb[1];
        custom_rgb[slot][2] = rgb[2];
        custom_set[slot] = 1;
    }
}

/* ---- [tiers] ------------------------------------------------------------- */
static Tier *tier_named(const char *name)
{
    int i;
    for (i = 0; i < tier_count; i++)
        if (same_letters(tiers[i].name, name))
            return &tiers[i];
    if (tier_count >= MAX_NAMES) return NULL;
    memset(&tiers[tier_count], 0, sizeof tiers[0]);
    strncpy(tiers[tier_count].name, name, NAME_MAX - 1);
    return &tiers[tier_count++];
}

/* "legendary_threshold" and "legendary_color": the tier's name is whatever
 * comes before the suffix, so the names themselves carry no meaning and the
 * thresholds alone decide the order. */
static void read_tier_line(char *key, const char *value)
{
    static const char *const SUFFIX[] = { "_threshold", "_color", "_colour" };
    const size_t n = strlen(key);
    int which;

    if (same_letters(key, "default_color") || same_letters(key, "default_colour")) {
        default_color = color_by_name(value);
        return;
    }
    if (same_letters(key, "undroppable_color") ||
        same_letters(key, "undroppable_colour")) {
        undroppable_color = color_by_name(value);
        return;
    }
    for (which = 0; which < 3; which++) {
        const size_t len = strlen(SUFFIX[which]);
        Tier *tier;
        if (n <= len || !same_letters(key + n - len, SUFFIX[which]))
            continue;
        key[n - len] = '\0';
        tier = tier_named(key);
        if (tier == NULL) return;
        if (which == 0) {
            tier->threshold = strtol(value, NULL, 10);
            tier->has_threshold = 1;
        } else {
            tier->color = color_by_name(value);
            tier->has_color = tier->color >= 0;
        }
        return;
    }
}

/* ---- the file ------------------------------------------------------------ */
enum { S_NONE, S_COLORS, S_TIERS, S_TIER_MULT, S_DUELIST_TIERS, S_RANK_MULT, S_CARDS };

static int section_of(const char *name)
{
    if (same_letters(name, "Colors") || same_letters(name, "Colours")) return S_COLORS;
    if (same_letters(name, "tiers")) return S_TIERS;
    if (same_letters(name, "duelist_tier_multipliers")) return S_TIER_MULT;
    if (same_letters(name, "duelist_tiers")) return S_DUELIST_TIERS;
    if (same_letters(name, "rank_multipliers")) return S_RANK_MULT;
    if (same_letters(name, "cards")) return S_CARDS;
    return S_NONE;
}

static long tier_multiplier(const char *name)
{
    int i;
    for (i = 0; i < tier_mult_count; i++)
        if (same_letters(tier_mult[i].name, name))
            return tier_mult[i].mult;
    return MULT_ONE;      /* blank, invalid or undefined: no weighting */
}

static int duelist_by_name(const char *name)
{
    int id;
    for (id = 1; id < NAMED_DUELISTS; id++)
        if (same_letters(name, Tables_DuelistNames[id]))
            return id;
    return -1;
}

/* Two passes. [Colors] has to be complete before [tiers] or [cards] can name a
 * colour, and [duelist_tier_multipliers] before [duelist_tiers] can name a
 * tier -- and the file is free to put its sections in any order. */
static void read_file(FILE *file, int pass)
{
    char line[160];
    int section = S_NONE;

    while (fgets(line, (int)sizeof line, file) != NULL) {
        char *eq, *value;

        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close != NULL) {
                *close = '\0';
                section = section_of(line + 1);
            }
            continue;
        }

        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        value = eq + 1;
        trim(line);
        trim(value);
        if (line[0] == '\0')
            continue;

        if (pass == 0) {
            if (section == S_COLORS) {
                read_color_line(line, value);
            } else if (section == S_TIER_MULT && tier_mult_count < MAX_NAMES) {
                strncpy(tier_mult[tier_mult_count].name, line, NAME_MAX - 1);
                tier_mult[tier_mult_count].name[NAME_MAX - 1] = '\0';
                tier_mult[tier_mult_count].mult = read_multiplier(value);
                tier_mult_count++;
            }
            continue;
        }

        switch (section) {
        case S_TIERS:
            read_tier_line(line, value);
            break;
        case S_DUELIST_TIERS: {
                const int id = duelist_by_name(line);
            if (id > 0)
                duelist_mult[id] = tier_multiplier(value);
            break;
        }
        case S_RANK_MULT:
            if (same_letters(line, "s_a_pow")) rank_mult[0] = read_multiplier(value);
            else if (same_letters(line, "b_c_d")) rank_mult[1] = read_multiplier(value);
            else if (same_letters(line, "s_a_tec")) rank_mult[2] = read_multiplier(value);
            break;
        case S_CARDS: {
            /* An id or a name: "123 = blue" and "Blue-eyes White Dragon =
             * blue" both work. Ids survive a translation mod, which renames
             * every card; names are easier to read. Cards_Named is the same
             * lookup the game's own manifests use for "fusions" and "drops". */
            char *end;
            long id = strtol(line, &end, 10);
            const int color = color_by_name(value);

            if (end == line || *end != '\0')
                id = Cards_Named(line);
            if (id >= 1 && id <= CARD_COUNT && color >= 0) {
                card_color[id] = (u8)color;
                card_color_set[id] = 1;
            } else if (color >= 0) {
                /* Worth saying out loud: a mistyped name looks exactly like a
                 * card that was simply left alone. */
                host->log(host, "card-name-color: no card called \"%s\"", line);
            }
            break;
        }
        default:
            break;
        }
    }
}

static void write_default_ini(void);

/* The file lives in the player's own directory, so it survives replacing the
 * build. When it is not there yet it is written from what this build actually
 * has -- every duelist the disc carries, under the name the game gives it, and
 * every card this run knows, including any a mod added. */
static void load_ini(void)
{
    FILE *file = host->open_data(host, INI_NAME, "r");

    if (file == NULL) {
        write_default_ini();
        file = host->open_data(host, INI_NAME, "r");
        if (file == NULL)
            return;
    }
    read_file(file, 0);
    fseek(file, 0, SEEK_SET);
    read_file(file, 1);
    fclose(file);
}


/* ---- writing the file the first time -------------------------------------
 *
 * Nothing about the game is baked in here. The duelist list comes from the
 * game's own name table, the card list from the game's own names, and the
 * number of opponents is counted off the disc -- so a renamed duelist, a
 * translation, or a mod that adds cards all show up in a freshly written file
 * without this mod being touched.
 */
static void wr(FILE *f, const char *text)
{
    fwrite(text, 1, strlen(text), f);
}

/* How many opponents the disc actually carries: every real block's four rows
 * each add up to 2048, so the first block that does not is one past the end.
 * Counted rather than assumed, because a later build may have more. */
static int count_duelists(int lba)
{
    static u8 block[DROP_BLOCK];
    int opp;

    for (opp = 1; opp <= DUELIST_MAX; opp++) {
        int row, ok = 1;
        if (host->disc_read(host, lba + DROPS_SECTOR + (opp - 1) * DROPS_STRIDE,
                            DROPS_STRIDE, block) <= 0)
            break;
        for (row = 0; row < 4 && ok; row++) {
            unsigned long sum = 0;
            int i;
            for (i = 0; i < CARD_COUNT; i++)
                sum += (unsigned)(block[row * DROP_ROW + i * 2] |
                                  ((unsigned)block[row * DROP_ROW + i * 2 + 1] << 8));
            if (sum != 2048u) ok = 0;
        }
        if (!ok) break;
    }
    return opp - 1;
}

/* A card's name as the game shows it now -- a mod's, a translation's, or the
 * retail one -- in UTF-8. The same walk Cards_NameUtf8 does, which the export
 * table does not carry. */
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
    while (name != NULL && *name < 0xF6) {
        int code = *name++;
        unsigned c;
        if (code >= 0xF0)
            code = ((code - 0xF0) << 8) | *name++;
        c = code ? Glyphs_Character(code) : ' ';
        if (c == 0) c = '?';
        if (c < 0x80) {
            if (n + 1 < size) out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 < size) {
                out[n++] = (char)(0xC0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        } else if (n + 3 < size) {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = '\0';
    return n != 0;
}

/* The tier each opponent starts in, by duelist id. Only the tier is fixed --
 * the NAME beside it is whatever the game calls that opponent now. */
static const char *const START_TIER[NAMED_DUELISTS] = {
    "", "tutorial", "tutorial", "tutorial", "tutorial", "tutorial", "tutorial",
    "normal", "boss", "normal", "normal", "normal", "normal", "tough", "tough",
    "boss", "boss", "boss", "rookie", "normal", "normal", "tough", "tough",
    "tough", "tough", "tough", "tough", "tough", "tough", "tough", "tough",
    "boss", "boss", "boss", "boss", "boss", "superboss", "superboss",
    "superboss", "superboss"
};

static void write_default_ini(void)
{
    char line[160];
    char name[96];
    FILE *f;
    int i;

    f = host->open_data(host, INI_NAME, "w");
    if (f == NULL) {
        host->log(host, "card-name-color: could not write %s", INI_NAME);
        return;
    }

    wr(f,
       "# CARD NAME COLOR - tints each card's name by drop rarity.\n"
       "# Edit and restart to apply. Delete this file to reset.\n"
       "\n"
       "[tiers]\n"
       "# A card gets the rarest tier whose threshold it does not exceed.\n"
       "# Thresholds decide the order, not the tier names.\n"
       "#\n"
       "# threshold = best rarity score any duelist gives the card:\n"
       "# weight (out of 2048) times the multipliers below.\n"
       "# color = any name from [Colors], or a slot number.\n"
       "# undroppable_color applies to cards no duelist drops at all.\n"
       "undroppable_color     = magenta\n"
       "legendary_threshold   = 2\n"
       "legendary_color       = blue\n"
       "ultra_rare_threshold  = 5\n"
       "ultra_rare_color      = red\n"
       "super_rare_threshold  = 7\n"
       "super_rare_color      = orange\n"
       "rare_threshold        = 11\n"
       "rare_color            = yellow\n"
       "uncommon_threshold    = 16\n"
       "uncommon_color        = green\n"
       "default_color         = white\n"
       "\n"
       "[duelist_tier_multipliers]\n"
       "# Name a duelist tier and give it a multiplier, applied to every\n"
       "# card that tier's duelists can drop. \"default\" always exists.\n"
       "default   = 1\n"
       "tutorial  = 2\n"
       "rookie    = 1.5\n"
       "normal    = 1\n"
       "tough     = 0.75\n"
       "boss      = 0.5\n"
       "superboss = 0.25\n"
       "\n"
       "[duelist_tiers]\n"
       "# DUELIST NAME = tier name (must be defined above). Blank,\n"
       "# invalid or undefined tiers fall back to a multiplier of 1.\n");

    for (i = 1; i <= duelist_count; i++) {
        const char *who = i < NAMED_DUELISTS ? Tables_DuelistNames[i] : NULL;
        const char *tier = i < NAMED_DUELISTS ? START_TIER[i] : "normal";
        if (who == NULL || *who == '\0')
            continue;
        snprintf(line, sizeof line, "%s = %s\n", who, tier);
        wr(f, line);
    }

    wr(f,
       "\n"
       "[rank_multipliers]\n"
       "# Multiplier for each rank band: s_a_pow, b_c_d, s_a_tec.\n"
       "s_a_pow = 0.75\n"
       "b_c_d   = 1\n"
       "s_a_tec = 0.5\n"
       "\n"
       "[Colors]\n"
       "; name = slot, and optionally (red,green,blue) to build that slot.\n"
       "; Slots 0-6 are the game's own and are shared with every other screen\n"
       "; that prints text; slot 7 is empty in the stock game and free.\n"
       "white = 0\n"
       "yellow = 1\n"
       "blue = 2\n"
       "green = 3\n"
       "grey = 4\n"
       "orange = 5\n"
       "red = 6\n"
       "magenta = 7 (255,0,255)\n"
       "\n"
       "[cards]\n"
       "# NAME = color, or ID = color. Overrides the tier for that card.\n"
       "# A name that matches no card is reported on the mods log channel.\n"
       "# Ids survive a translation, which renames every card.\n"
       "#\n"
       "# Every card this build has is listed below, commented out.\n");

    for (i = 1; i <= gCard_nCount; i++) {
        if (!card_name(i, name, sizeof name))
            continue;
        snprintf(line, sizeof line, "#%s = white\n", name);
        wr(f, line);
    }

    fclose(f);
    host->log(host, "card-name-color: wrote %s (%d cards, %d duelists)",
              INI_NAME, gCard_nCount, duelist_count);
}

/* ---- rarity -------------------------------------------------------------- */
static void apply_tiers(void)
{
    static u8 block[DROP_BLOCK];
    static u32 best[CARD_COUNT + 1];
    /* Whether ANY pool lists the card at all, taken from the raw weight before
     * the multipliers. A score of zero does not mean the same thing: a weight
     * of 1 through a 0.25 opponent and a 0.5 pool floors to zero in integer
     * maths, and that card is still winnable. */
    static u8 droppable[CARD_COUNT + 1];
    const long all = tier_multiplier("default");
    int lba, opp, pool, i, t;

    if (tier_count == 0 && default_color < 0 && undroppable_color < 0)
        return;

    lba = host->disc_file_start(host, WA_PATH);
    if (lba < 0) {
        host->log(host, "card-name-color: no %s on the disc", WA_PATH);
        return;
    }

    memset(best, 0, sizeof best);
    memset(droppable, 0, sizeof droppable);
    for (opp = 1; opp <= duelist_count; opp++) {
        const u32 dm = (u32)(duelist_mult[opp] != 0 ? duelist_mult[opp] : all);
        if (host->disc_read(host,
                            lba + DROPS_SECTOR + (opp - 1) * DROPS_STRIDE,
                            DROPS_STRIDE, block) <= 0) {
            host->log(host, "card-name-color: could not read opponent %d's drops", opp);
            return;
        }
        for (pool = 0; pool < RANK_COUNT; pool++) {
            const u8 *row = block + (size_t)(pool + 1) * DROP_ROW;
            const u32 rm = (u32)rank_mult[pool];
            for (i = 1; i <= CARD_COUNT; i++) {
                const u32 w = (u32)(row[(i - 1) * 2] |
                                    ((u32)row[(i - 1) * 2 + 1] << 8));
                const u32 score = ((w * dm) / MULT_ONE) * rm / MULT_ONE;
                if (w != 0)
                    droppable[i] = 1;
                if (score > best[i])
                    best[i] = score;
            }
        }
    }

    for (i = 1; i <= CARD_COUNT; i++) {
        int chosen = default_color;
        long rarest = -1;

        if (card_color_set[i])          /* [cards] wins */
            continue;
        if (!droppable[i] && undroppable_color >= 0) {
            /* No opponent lists it in any pool: it cannot be won at all, so
             * it is given its own colour rather than falling into the rarest
             * tier and crowding it. */
            card_color[i] = (u8)undroppable_color;
            card_color_set[i] = 1;
            continue;
        }
        for (t = 0; t < tier_count; t++) {
            if (!tiers[t].has_threshold || !tiers[t].has_color)
                continue;
            if ((long)best[i] <= tiers[t].threshold &&
                (rarest < 0 || tiers[t].threshold < rarest)) {
                rarest = tiers[t].threshold;
                chosen = tiers[t].color;
            }
        }
        if (chosen >= 0) {
            card_color[i] = (u8)chosen;
            card_color_set[i] = 1;
        }
    }
    host->log(host, "card-name-color: %d tiers over %d opponents", tier_count,
              duelist_count);
}

/* ---- the ramps ----------------------------------------------------------- */
static void upload_ramps(void)
{
    static u32 sector[512];
    u16 base[RAMP_ENTRIES];
    u16 ramp[RAMP_ENTRIES];
    ModRect rect;
    int lba, slot, i, wanted = 0;

    ramps_done = 1;
    for (slot = 0; slot < COLOR_COUNT; slot++)
        wanted += custom_set[slot];
    if (wanted == 0)
        return;

    lba = host->disc_file_start(host, WA_PATH);
    if (lba < 0)
        return;
    if (host->disc_read(host, lba + RAMP_SECTOR, 1, sector) <= 0) {
        host->log(host, "card-name-color: could not read the text ramps");
        return;
    }
    memcpy(base, sector, sizeof base);   /* slot 0: the luminance ramp */

    for (slot = 0; slot < COLOR_COUNT; slot++) {
        if (!custom_set[slot])
            continue;
        for (i = 0; i < RAMP_ENTRIES; i++) {
            const unsigned lum = base[i] & 31u;
            if (base[i] == 0) {
                ramp[i] = 0;             /* index 0 is transparent, not black */
                continue;
            }
            ramp[i] = (u16)((((lum * custom_rgb[slot][2]) / 255u) << 10) |
                            (((lum * custom_rgb[slot][1]) / 255u) << 5) |
                             ((lum * custom_rgb[slot][0]) / 255u));
        }
        rect.x = RAMP_VX;
        rect.y = (s16)(RAMP_VY + slot);
        rect.w = RAMP_ENTRIES;
        rect.h = 1;
        LoadImage(&rect, (u32 *)ramp);
        DrawSync(0);
        host->log(host, "card-name-color: slot %d = %u,%u,%u", slot,
                  custom_rgb[slot][0], custom_rgb[slot][1], custom_rgb[slot][2]);
    }
}

/* ---- the hook ------------------------------------------------------------ */
static SavedNameColor *find_saved_color(DuelEffectChannel *object)
{
    int i;
    for (i = 0; i < TRACKED_BOXES; i++)
        if (saved_colors[i].active && saved_colors[i].object == object)
            return &saved_colors[i];
    return NULL;
}

static void remember_name_color(DuelEffectChannel *object)
{
    int i;
    if (find_saved_color(object) != NULL)
        return;
    for (i = 0; i < TRACKED_BOXES; i++) {
        if (!saved_colors[i].active) {
            saved_colors[i].object = object;
            saved_colors[i].saved_color = object->field_54;
            saved_colors[i].active = 1;
            return;
        }
    }
}

static void restore_name_color(DuelEffectChannel *object)
{
    SavedNameColor *slot = find_saved_color(object);
    if (slot == NULL)
        return;
    object->field_54 = slot->saved_color;
    slot->active = 0;
    slot->object = NULL;
}

static void ensure_config(void);

static void process_text_command(DuelEffectChannel *object)
{
    TextStreamOwner *owner = (TextStreamOwner *)object;
    u8 *current;
    u8 command;
    s16 id;
    void (*original)(DuelEffectChannel *);

    current = owner->streams[(s8)object->stream_58];
    command = *current;

    original = (void (*)(DuelEffectChannel *))original_func_80037DA4;
    original(object);

    id = gDuel_wSelectedCardID;
    ensure_config();

    if ((command & 0x20) != 0) {
        /* The original handler has now selected the card-name text. Save the
         * colour the game chose and replace it only for the name. */
        if (id >= 1 && id <= CARD_COUNT && card_color_set[id]) {
            if (!ramps_done)
                upload_ramps();
            remember_name_color(object);
            object->field_54 = card_color[id];
        }
    } else if ((command & 0x40) != 0) {
        /* The same box is switching to the description: put back whatever
         * colour was active before the override. */
        restore_name_color(object);
    }
}

/* Everything the file needs waits for the game to be ready.
 *
 * MemoriesModInit runs at frame 0, before Cards_Build: gCard_nCount is still
 * 0 and Cards_Valid is false for every id, so a file written there came out
 * with no cards in it at all. The drop tables and the duelist names are
 * readable that early, which is exactly why the duelist half looked right and
 * hid the problem.
 *
 * So the config is built the first time a card name is actually drawn, by
 * which point the card tables certainly exist. Costs one comparison per call
 * afterwards. */
static int config_done;

static void ensure_config(void)
{
    if (config_done)
        return;
    if (gCard_nCount <= 0)
        return;                     /* cards not built yet; try again later */
    config_done = 1;

    {
        const int lba = host->disc_file_start(host, WA_PATH);
        duelist_count = lba >= 0 ? count_duelists(lba) : 0;
    }
    load_ini();
    apply_tiers();
}

/* A save state restores VRAM too, so the ramps have to go back. */
static void on_state_loaded(void)
{
    ramps_done = 0;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    int token;

    if (from->api < 4)
        return 0;

    host = from;
    memset(saved_colors, 0, sizeof saved_colors);

    token = host->hook(host, (void *)func_80037DA4,
                       (void *)process_text_command, &original_func_80037DA4);
    if (!token) {
        host->log(host, "card-name-color: failed to hook func_80037DA4");
        return 0;
    }

    mod->api = MEMORIES_MOD_API;
    mod->reset = on_state_loaded;
    return 1;
}
