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
#include "game/duel_effect_init_entry.h"
#include "game/text_box_lifecycle.h"
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
static void *original_init_entry, *original_destroy;

static u8 *card_color;
static u8 *card_color_set;
static int card_count;
static u16 *drop_weights;
static int drops_scanned;
static void scan_drops(void);
static int tier_score(int duelist, int pool, int weight);
static int weight_for(int duelist, int pool, int id)
{
    return drop_weights[((duelist - 1) * RANK_COUNT + pool) * (card_count + 1) + id];
}

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
static u32 original_ramps[COLOR_COUNT * RAMP_ENTRIES / 2];
static int have_original_ramps;

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
    u32 guest_offset; /* Save-safe offset in the fixed two-MiB guest RAM. */
    u8 saved_color;
    u8 override_color;
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
    long whole = 0, frac = 0;
    int digits = 0, any = 0;
    while (isspace((unsigned char)*value)) value++;
    if (*value == '+') value++;
    while (isdigit((unsigned char)*value)) {
        any = 1;
        if (whole < 11) whole = whole * 10 + (*value - '0');
        value++;
    }
    if (*value == '.') {
        value++;
        while (isdigit((unsigned char)*value)) {
            any = 1;
            if (digits < 3) { frac = frac * 10 + (*value - '0'); digits++; }
            value++;
        }
    }
    while (isspace((unsigned char)*value)) value++;
    if (!any || *value) return MULT_ONE;
    while (digits++ < 3) frac *= 10;
    return whole >= 10 ? MULT_MAX : whole * MULT_ONE + frac;
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
            while (isspace((unsigned char)*at)) at++;
            n = strtol(at, &stop, 10);
            if (stop == at || n < 0 || n > 255) return;
            rgb[i] = (u8)n;
            at = stop;
            while (isspace((unsigned char)*at)) at++;
            if (i < 2) { if (*at != ',') return; at++; }
            else if (*at != ')') return;
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
            if (id >= 1 && id <= card_count && color >= 0) {
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
 * retail one. The same walk Cards_NameUtf8 does, which the export table does
 * not carry. UTF-8 for the file; `ascii` for the overlay, whose draw_text
 * takes ASCII and where a glyph outside it becomes '?' rather than a broken
 * byte sequence. */
static int card_text(int id, char *out, size_t size, int ascii)
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
        if (ascii) {
            if (n + 1 < size)
                out[n++] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
        } else if (c < 0x80) {
            if (n + 1 < size) out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 < size) {
                out[n++] = (char)(0xC0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        } else if (c < 0x10000 && n + 3 < size) {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x10000 && c <= 0x10FFFF && n + 4 < size) {
            out[n++] = (char)(0xF0 | (c >> 18));
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = '\0';
    return n != 0;
}

static int card_name(int id, char *out, size_t size)
{
    return card_text(id, out, size, 0);
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
    int i, opp, pool, t;
    if (drops_scanned != 1) return;
    for (i = 1; i <= card_count; i++) {
        int best = 0, droppable = 0;
        for (opp = 1; opp <= duelist_count; opp++) {
            for (pool = 0; pool < RANK_COUNT; pool++) {
                int weight = weight_for(opp, pool, i);
                int score = tier_score(opp, pool, weight);
                if (weight) droppable = 1;
                if (score > best) best = score;
            }
        }
        int chosen = default_color;
        long rarest = -1;

        if (card_color_set[i])          /* [cards] wins */
            continue;
        if (!droppable && undroppable_color >= 0) {
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
            if ((long)best <= tiers[t].threshold &&
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
static int read_original_ramps(void)
{
    u32 sector[512];
    int lba = host->disc_file_start(host, WA_PATH);
    if (lba < 0 || host->disc_read(host, lba + RAMP_SECTOR, 1, sector) <= 0) return 0;
    memcpy(original_ramps, sector, sizeof original_ramps);
    have_original_ramps = 1;
    return 1;
}

static void upload_ramps(void)
{
    u16 base[RAMP_ENTRIES];
    u16 ramp[RAMP_ENTRIES];
    ModRect rect;
    int slot, i, wanted = 0;

    ramps_done = 1;
    for (slot = 0; slot < COLOR_COUNT; slot++)
        wanted += custom_set[slot];
    if (wanted == 0)
        return;

    if (!read_original_ramps()) return;
    memcpy(base, original_ramps, sizeof base); /* slot 0: luminance ramp */

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
        for (i = 0; i < RAMP_ENTRIES; i++)
            if (base[i] && !ramp[i]) ramp[i] = 0x8000;
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
        if (saved_colors[i].active && saved_colors[i].guest_offset == (u32)(uintptr_t)object - 0x80000000u)
            return &saved_colors[i];
    return NULL;
}

static void remember_name_color(DuelEffectChannel *object)
{
    int i;
    if ((uintptr_t)object < 0x80000000u ||
        (uintptr_t)object > 0x80200000u - sizeof(*object)) return;
    if (find_saved_color(object) != NULL)
        return;
    for (i = 0; i < TRACKED_BOXES; i++) {
        if (!saved_colors[i].active) {
            saved_colors[i].guest_offset = (u32)(uintptr_t)object - 0x80000000u;
            saved_colors[i].override_color = card_color[gDuel_wSelectedCardID];
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
    if (object->field_54 == slot->override_color)
        object->field_54 = slot->saved_color;
    slot->active = 0;
    slot->guest_offset = 0;
}

/* A channel can be reused without issuing a description command. Forget its
 * previous name colour at the same boundary that resets the game's channel. */
static DuelEffectChannel *init_entry(s32 index, s32 value, s32 flags)
{
    DuelEffectChannel *object = ((DuelEffectChannel *(*)(s32,s32,s32))original_init_entry)(index, value, flags);
    SavedNameColor *slot = find_saved_color(object);
    if (slot) memset(slot, 0, sizeof(*slot));
    return object;
}
static void destroy_box(DuelEffectChannel *object)
{
    restore_name_color(object);
    ((void (*)(DuelEffectChannel *))original_destroy)(object);
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
    restore_name_color(object);
    original(object);
    if (!host->setting(host, "colors", 1)) return;

    id = gDuel_wSelectedCardID;
    ensure_config();

    if ((command & 0x10) == 0 && (command & 0x20) != 0) {
        /* The original handler has now selected the card-name text. Save the
         * colour the game chose and replace it only for the name. */
        if (id >= 1 && id <= card_count && card_color_set && card_color_set[id]) {
            if (!ramps_done)
                upload_ramps();
            remember_name_color(object);
            if (find_saved_color(object)) object->field_54 = card_color[id];
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
    card_count = gCard_nCount;
    card_color = calloc((size_t)card_count + 1, 1);
    card_color_set = calloc((size_t)card_count + 1, 1);
    if (!card_color || !card_color_set) {
        free(card_color); free(card_color_set);
        card_color = card_color_set = NULL;
        return;
    }
    config_done = 1;
    { int i; for (i = 0; i <= DUELIST_MAX; i++) duelist_mult[i] = -1; }
    {
        const int lba = host->disc_file_start(host, WA_PATH);
        duelist_count = lba >= 0 ? count_duelists(lba) : 0;
    }
    load_ini();
    scan_drops();
    apply_tiers();
}

static void drop_panel_reset(void);
static void restore_boxes(void);
static void colors_removed(void);

/* A save state restores VRAM too, so the ramps have to go back. */
static void on_state_loaded(void)
{
    ensure_config();
    /* A startup load can contain our palette before this process ever drew
     * a name. Restore the disc baseline even when colours are switched off. */
    if (config_done) read_original_ramps();
    colors_removed();
    drop_panel_reset();
}

/* The score a colour tier is chosen on, for one duelist and one pool. The same
 * arithmetic apply_tiers does, so the SCORE column shows the number that
 * picked the colour rather than a second implementation of it. Thousandths,
 * floored at each step, and the config is established here too: the drop panel
 * may be the first thing that asks for it. */
static int tier_score(int duelist, int pool, int weight)
{
    long dm, rm;

    ensure_config();
    if (duelist < 0 || duelist > DUELIST_MAX || pool < 0 || pool >= RANK_COUNT ||
        weight <= 0)
        return 0;
    dm = duelist_mult[duelist] >= 0 ? duelist_mult[duelist] : tier_multiplier("default");
    rm = rank_mult[pool];
    return (int)((((long)weight * dm) / MULT_ONE) * rm / MULT_ONE);
}


/* ==== the drop panel ======================================================
 *
 * The Library tells you a card exists and nothing about how to get one. With
 * "Show drop odds" on, the grid names the duelists who drop the card under the
 * cursor, how often, and what that scores -- the same score the tiers above
 * chose the card's colour by, so the panel shows this mod's working.
 *
 * The weights are the ones apply_tiers already reads: WA_MRG 0xE9B000 +
 * 0x1800*(id-1), three sectors an opponent, four 1460-byte rows summing to
 * 2048. Duel_SelectCardDrop rolls `(rand() & 2047) + 1` against a running sum
 * of the row for the rank earned, so a weight of w IS that card's chance, w
 * out of 2048, once per duel won at that rank.
 *
 * Tables_PoolFor is asked for every pool first, so a mod that edits drops is
 * reflected -- the same call the game's own drop picker makes. Its array is
 * indexed by card id; the disc's row is indexed by id - 1.
 *
 * Main_Loop dispatches on the low five bits of D_8009B26C (main_modes.c), so
 * the Library is the running screen exactly while those bits are
 * MAIN_MODE_LIBRARY and the 0x40 "already entered" bit is set. D_800EA1E8's
 * low nibble is then the screen's own state, and 1 is the card grid. Both are
 * read directly, so nothing depends on how often a callback happens to run:
 * mod->frame is called from GsDrawOt -- once per draw list, not once per game
 * frame -- and a screen that submits several would age a frame counter faster
 * than its own update runs.
 */

#define SHOW_MAX 20                /* the most rows the table will print */
#define WEIGHT_TOTAL 2048          /* DUEL_DROP_WEIGHT_TOTAL */

/* Tables_PoolFor's pool numbering (pc/cards/tables.h): the deck pool, then the
 * three drop pools in Duel_SelectCardDrop's order. */
enum { TABLES_POOL_DECK, TABLES_POOL_POW };

/* main_modes.c's dispatch table, and the bit Main_RunLibraryMenu sets once it
 * has opened the screen. */
#define MAIN_MODE_LIBRARY 4
#define MAIN_MODE_MASK    0x1F
#define MAIN_MODE_ENTERED 0x40
#define LIBRARY_GRID      1        /* func_8002BAB4's state for the card grid */

const unsigned short *Tables_PoolFor(int duelist, int pool,
                                     const unsigned short *retail);
extern u8 D_8009B26C[];
extern u8 D_800EA1E8[];
void func_8002BAB4(void);
s32 Library_GetGridCursorCardId(u8 *state);
/* Bit 0x80 is whether the grid draws the card's cell at all (func_80029EC4),
 * which is the same test the screen makes before it will open one. */
unsigned int Library_GetCardFlags(unsigned char *base, int index);
/* The port's menu bar, which the overlay canvas includes. */
int Menu_Height(void);

static void *orig_dispatch;

enum { SORT_WEIGHT, SORT_SCORE, SORT_DUELIST, SORT_POOL };
static int opt_drops = 1;
static int opt_score = 1;
static int opt_rows = 3;
static int opt_sort = SORT_WEIGHT;
static int opt_x = 8;
static int opt_y = 8;

#define PANEL_ALPHA 205
#define POOL_NAME(p) ((p) == 0 ? "S/A POW" : (p) == 1 ? "B/C/D" : "S/A TEC")

static void scan_drops(void)
{
    static u8 block[DROP_BLOCK];
    static u16 retail[CARD_COUNT];
    const int lba = host->disc_file_start(host, WA_PATH);
    int opp, pool, i;
    drops_scanned = -1;
    if (lba < 0 || duelist_count <= 0) return;
    drop_weights = calloc((size_t)duelist_count * RANK_COUNT * (card_count + 1), sizeof(u16));
    if (!drop_weights) return;
    for (opp = 1; opp <= duelist_count; opp++) {
        if (host->disc_read(host, lba + DROPS_SECTOR + (opp - 1) * DROPS_STRIDE,
                            DROPS_STRIDE, block) <= 0) {
            free(drop_weights); drop_weights = NULL;
            return;
        }
        for (pool = 0; pool < RANK_COUNT; pool++) {
            const u8 *row = block + (pool + 1) * DROP_ROW;
            const u16 *edited;
            for (i = 0; i < CARD_COUNT; i++)
                retail[i] = (u16)(row[i * 2] | ((unsigned)row[i * 2 + 1] << 8));
            edited = Tables_PoolFor(opp, TABLES_POOL_POW + pool, retail);
            for (i = 1; i <= card_count; i++)
                drop_weights[((opp - 1) * RANK_COUNT + pool) * (card_count + 1) + i] =
                    edited ? edited[i] : i <= CARD_COUNT ? retail[i - 1] : 0;
        }
    }
    drops_scanned = 1;
}

static void duelist_name(int duelist, char *out, size_t size)
{
    if (duelist > 0 && duelist < NAMED_DUELISTS)
        snprintf(out, size, "%s", Tables_DuelistNames[duelist]);
    else
        snprintf(out, size, "Duelist %d", duelist);
}

typedef struct {
    char name[32];
    char weight[16];
    char score[16];
    int pool;
    int by_weight;
    int by_score;
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
    case SORT_SCORE:   return a->by_score > b->by_score;
    case SORT_DUELIST: return strcmp(a->name, b->name) < 0;
    case SORT_POOL:    return a->pool < b->pool;
    default:           return a->by_weight > b->by_weight;
    }
}

/* Selection is on the column being ordered when that column is a number, so
 * "by score" really shows the highest scores; ordering by a name or a rank
 * takes the best by weight and then arranges those. */
static int picked_by(const Row *a, const Row *b)
{
    if (opt_sort == SORT_SCORE)
        return a->by_score > b->by_score;
    return a->by_weight > b->by_weight;
}

static void build_rows(int id)
{
    static Row all[DUELIST_MAX];
    int have = 0, opp, pool, i, j, taken;
    row_count = 0;
    shown_ok = card_text(id, shown_name, sizeof shown_name, 1);
    if (!shown_ok) return;
    for (opp = 1; opp <= duelist_count; opp++) {
        int best_weight = 0, best_score = -1, best_pool = 0;
        for (pool = 0; pool < RANK_COUNT; pool++) {
            int weight = weight_for(opp, pool, id);
            int score = tier_score(opp, pool, weight);
            if (weight && (opt_sort == SORT_SCORE ? score > best_score : weight > best_weight)) {
                best_weight = weight; best_score = score; best_pool = pool;
            }
        }
        if (!best_weight) continue;
        duelist_name(opp, all[have].name, sizeof all[have].name);
        snprintf(all[have].weight, sizeof all[have].weight, "%d/2048", best_weight);
        all[have].by_weight = best_weight;
        all[have].by_score = best_score;
        snprintf(all[have].score, sizeof all[have].score, "%d", best_score);
        all[have++].pool = best_pool;
    }
    row_total = have;

    /* Take the best few, then put those in the order the column asks for.
     * Selection sort over at most 64 candidates for at most 20 rows. */
    taken = have < opt_rows ? have : opt_rows;
    for (i = 0; i < taken; i++) {
        int best = -1;
        for (j = 0; j < have; j++) {
            if (all[j].by_weight < 0)
                continue;          /* already taken */
            if (best < 0 || picked_by(&all[j], &all[best]))
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

/* The card under the grid cursor, or 0 when the Library's grid is not what is
 * running. Read straight from the game rather than remembered from a callback,
 * so the panel cannot outlive the screen or blink out under one. */
static int cursor_card(void)
{
    const unsigned mode = D_8009B26C[0];
    int id;

    if (!opt_drops)
        return 0;
    if ((mode & MAIN_MODE_MASK) != MAIN_MODE_LIBRARY ||
        (mode & MAIN_MODE_ENTERED) == 0)
        return 0;
    if ((D_800EA1E8[0] & 0xF) != LIBRARY_GRID)
        return 0;
    id = (int)Library_GetGridCursorCardId(D_800EA1E8);
    if (id < 1 || id > gCard_nCount)
        return 0;
    /* Nothing is said about a card the Library itself does not show yet: the
     * panel would be telling the player what is in a cell they cannot see. */
    if ((Library_GetCardFlags(D_800EA1E8, id) & 0x80) == 0)
        return 0;
    return id;
}

static void read_panel_settings(void)
{
    const int was_rows = opt_rows, was_sort = opt_sort;

    opt_score = host->setting(host, "show_score", 1);
    opt_drops = host->setting(host, "drops", 1);
    opt_rows = host->setting(host, "drop_rows", 3);
    if (opt_rows < 1) opt_rows = 1;
    if (opt_rows > SHOW_MAX) opt_rows = SHOW_MAX;
    opt_sort = host->setting(host, "drop_sort", SORT_WEIGHT);
    if (opt_sort < 0 || opt_sort > SORT_POOL) opt_sort = SORT_WEIGHT;
    opt_x = host->setting(host, "drop_x", 8);
    if (opt_x < 0) opt_x = 0;
    if (opt_x > 1000) opt_x = 1000;
    opt_y = host->setting(host, "drop_y", 8);
    if (opt_y < 0) opt_y = 0;
    if (opt_y > 800) opt_y = 800;

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
    read_panel_settings();

    id = cursor_card();
    if (id == 0)
        return;
    ensure_config();               /* the multipliers the score needs */
    if (drops_scanned != 1 || id == shown_id)
        return;
    shown_id = id;
    build_rows(id);
}

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
#define COL_SCORE    0x7FC9FFu
#define COL_FOOT     0x76818Fu
#define COL_NONE     0xE8A85Cu

#define HEAD_NAME   "DUELIST"
/* tables.h calls these pools; what the column actually shows is the duel
 * rank each one belongs to, which is what a player earns and recognises. */
#define HEAD_POOL   "RANK"
#define HEAD_WEIGHT "WEIGHT"
#define HEAD_SCORE  "SCORE"

static int widest(int a, int b) { return a > b ? a : b; }

/* The heading of the column being ordered on, lit so the table says what it is
 * sorted by without a line of its own. */
static unsigned head_colour(int column)
{
    return column == opt_sort ? COL_PICKED : COL_HEAD;
}

static void panel(void)
{
    char number[16], footer[64], title[64];
    int width, height, scale, bar;
    int w_name, w_pool, w_weight, w_score, w_title, w_foot, w_body;
    int pad, gap, line, rule, box_w, box_h, x, y, cy, i;
    int at_name, at_pool, end_weight, end_score, visible;

    host->overlay_size(host, &width, &height, &scale);
    if (scale < 1)
        scale = 1;
    if (width <= 0 || height <= 0)
        return;

    bar = Menu_Height();
    while (scale > 1 && (width < 520 * scale || height - bar < 140 * scale)) scale--;
    if (width < 300 * scale || height - bar < 100 * scale) return;
    visible = (height - bar - 85 * scale) / (16 * scale);
    if (visible > row_count) visible = row_count;
    if (visible < 0) visible = 0;
    snprintf(title, sizeof title, "%s", shown_name);
    while (title[0] && host->text_width(host, title, scale) > width - 90 * scale)
        title[strlen(title) - 1] = 0;
    snprintf(number, sizeof number, "#%d", shown_id);
    if (row_total == 0)
        snprintf(footer, sizeof footer, "No duelist drops this card");
    else if (row_total > visible)
        snprintf(footer, sizeof footer, "and %d more duelist%s",
                 row_total - visible, row_total - visible == 1 ? "" : "s");
    else
        footer[0] = '\0';

    /* Every column is as wide as its heading or its widest cell. */
    w_name = host->text_width(host, HEAD_NAME, scale);
    w_pool = host->text_width(host, HEAD_POOL, scale);
    w_weight = host->text_width(host, HEAD_WEIGHT, scale);
    w_score = host->text_width(host, HEAD_SCORE, scale);
    for (i = 0; i < visible; i++) {
        w_name = widest(w_name, host->text_width(host, rows[i].name, scale));
        w_pool = widest(w_pool, host->text_width(host, POOL_NAME(rows[i].pool), scale));
        w_weight = widest(w_weight, host->text_width(host, rows[i].weight, scale));
        w_score = widest(w_score, host->text_width(host, rows[i].score, scale));
    }

    line = 16 * scale;
    pad = 9 * scale;
    gap = 12 * scale;
    rule = scale;

    w_title = host->text_width(host, title, scale) + gap +
              host->text_width(host, number, scale);
    w_foot = footer[0] ? host->text_width(host, footer, scale) : 0;
    w_body = visible ? w_name + gap + w_pool + gap + w_weight + (opt_score ? gap + w_score : 0) : 0;
    box_w = widest(widest(w_body, w_title), w_foot) + pad * 2;
    if (box_w > width) return;
    box_h = pad + line
          + (visible ? rule + line + visible * line : 0)
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
    host->draw_text(host, x + pad, cy + line / 2, title, COL_TITLE, scale);
    host->draw_text(host, x + box_w - pad - host->text_width(host, number, scale),
                    cy + line / 2, number, COL_NUMBER, scale);
    cy += line;

    at_name = x + pad;
    at_pool = at_name + w_name + gap;
    end_weight = at_pool + w_pool + gap + w_weight;
    end_score = end_weight + gap + w_score;

    if (visible) {
        host->fill(host, x + rule, cy, box_w - rule * 2, rule, COL_RULE, 255);
        cy += rule;
        host->draw_text(host, at_name, cy + line / 2, HEAD_NAME,
                        head_colour(SORT_DUELIST), scale);
        host->draw_text(host, at_pool, cy + line / 2, HEAD_POOL,
                        head_colour(SORT_POOL), scale);
        host->draw_text(host, end_weight - host->text_width(host, HEAD_WEIGHT, scale),
                        cy + line / 2, HEAD_WEIGHT, head_colour(SORT_WEIGHT), scale);
        if (opt_score) host->draw_text(host, end_score - host->text_width(host, HEAD_SCORE, scale),
                        cy + line / 2, HEAD_SCORE, head_colour(SORT_SCORE), scale);
        cy += line;

        for (i = 0; i < visible; i++) {
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
            if (opt_score) host->draw_text(host,
                            end_score - host->text_width(host, rows[i].score, scale),
                            cy + line / 2, rows[i].score, COL_SCORE, scale);
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

static void drop_panel_reset(void)
{
    shown_id = 0;
    shown_ok = 0;
}

static int panel_live(void)
{
    return opt_drops && drops_scanned == 1 && shown_ok &&
           shown_id >= 1 && shown_id <= card_count && cursor_card() == shown_id;
}

static void drop_overlay(void)
{
    if (panel_live())
        panel();
}

static unsigned drop_overlay_signature(void)
{
    if (!panel_live())
        return 0;
    {
        unsigned h = 2166136261u;
        h = (h ^ (unsigned)shown_id) * 16777619u;
        h = (h ^ (unsigned)opt_rows) * 16777619u;
        h = (h ^ (unsigned)opt_sort) * 16777619u;
        h = (h ^ (unsigned)opt_score) * 16777619u;
        h = (h ^ (unsigned)opt_x) * 16777619u;
        return (h ^ (unsigned)opt_y) * 16777619u;
    }
}

static void restore_boxes(void)
{
    int i;
    for (i = 0; i < TRACKED_BOXES; i++) {
        SavedNameColor *slot = &saved_colors[i];
        if (slot->active && slot->guest_offset <= 0x200000u - sizeof(DuelEffectChannel))
            restore_name_color((DuelEffectChannel *)(uintptr_t)(0x80000000u + slot->guest_offset));
    }
    memset(saved_colors, 0, sizeof saved_colors);
}
static void colors_removed(void)
{
    ModRect rect = {RAMP_VX, RAMP_VY, RAMP_ENTRIES, COLOR_COUNT};
    restore_boxes();
    if (have_original_ramps) {
        int slot;
        rect.h = 1;
        for (slot = 0; slot < COLOR_COUNT; slot++) {
            if (!custom_set[slot]) continue;
            rect.y = (s16)(RAMP_VY + slot);
            LoadImage(&rect, original_ramps + slot * RAMP_ENTRIES / 2);
        }
        DrawSync(0);
    }
    ramps_done = 0;
    have_original_ramps = 0;
}
static void colors_frame(void)
{
    if (!host->setting(host, "colors", 1)) colors_removed();
}
static void colors_applied(int on)
{
    if (!on) { colors_removed(); drop_panel_reset(); }
}

int YamyiColors_Init(const MemoriesModHost *from, MemoriesMod *mod)
{
    int token;

    if (from->api < 4)
        return 0;

    host = from;
    memset(saved_colors, 0, sizeof saved_colors);
    if (!host->register_state(host, saved_colors, sizeof saved_colors, 1)) return 0;

    token = host->hook(host, (void *)func_80037DA4,
                       (void *)process_text_command, &original_func_80037DA4);
    if (!token) {
        host->log(host, "card-name-color: failed to hook func_80037DA4");
        return 0;
    }

    if (!host->hook(host, (void *)DuelEffect_InitEntry, (void *)init_entry, &original_init_entry) ||
        !host->hook(host, (void *)TextBox_Destroy, (void *)destroy_box, &original_destroy)) return 0;

    /* A package requires all its advertised features to load. */
    if (host->hook(host, (void *)func_8002BAB4, (void *)library_frame,
                   &orig_dispatch) == 0)
        return 0;

    mod->api = MEMORIES_MOD_API;
    mod->reset = on_state_loaded;
    mod->frame = colors_frame;
    mod->applied = colors_applied;
    mod->overlay = drop_overlay;
    mod->overlay_signature = drop_overlay_signature;
    return 1;
}
