/* Synthetic drop tables verify that colours and the Library agree after
 * other mods change pools, including added cards and unequal rank scores. */
#include "../../mods/yamyi-mods/card_name_color.c"
#include <assert.h>

int gCard_nCount = 723;
s16 gDuel_wSelectedCardID;
u8 D_8009B26C[1], D_800EA1E8[1];
const char *const Tables_DuelistNames[40] = {"", "Opponent"};
static u16 edited[724];
static int uploads;
int Cards_Valid(int id) { return id > 0 && id <= gCard_nCount; }
int Cards_Named(const char *s) { (void)s; return 723; }
int Cards_BaseId(int id) { return id; }
const unsigned char *Cards_NameText(int id) { static const u8 name[] = {1, 0xff}; (void)id; return name; }
const unsigned char *Text_Resolve(int id, const unsigned char *p) { (void)id; return p; }
unsigned Glyphs_Character(int code) { (void)code; return 'A'; }
const unsigned short *Tables_PoolFor(int duelist, int pool, const unsigned short *retail)
{ assert(duelist == 1 && retail[0] == 2048); return pool == 2 ? edited : NULL; }
int LoadImage(ModRect *r, u32 *p) { (void)r; (void)p; uploads++; return 0; }
int DrawSync(int mode) { (void)mode; return 0; }
int Menu_Height(void) { return 26; }
static int disc_start(const MemoriesModHost *h, const char *s) { (void)h; (void)s; return 0; }
static int disc_read(const MemoriesModHost *h, int lba, int count, void *out)
{
    int row; u8 *p = out; (void)h;
    assert(lba == DROPS_SECTOR && count == 3);
    memset(out, 0, DROP_BLOCK);
    for (row = 0; row < 4; row++) p[row * DROP_ROW + 1] = 8;
    return count;
}
static void log_message(const MemoriesModHost *h, const char *fmt, ...) { (void)h; (void)fmt; }
static FILE *open_data(const MemoriesModHost *h, const char *path, const char *mode)
{ (void)h; (void)path; (void)mode; assert(0); return NULL; }
static MemoriesModHost test_host = {.disc_file_start=disc_start, .disc_read=disc_read,
                                    .log=log_message, .open_data=open_data};
int main(void)
{
    int i;
    assert(read_multiplier("1.25") == 1250);
    assert(read_multiplier("0") == 0);
    assert(read_multiplier(".5") == 500);
    assert(read_multiplier("-.5") == MULT_ONE);
    assert(read_multiplier("junk") == MULT_ONE);
    assert(read_multiplier("2oops") == MULT_ONE);
    assert(read_multiplier("99999999999999999999999999999999999") == MULT_MAX);
    read_color_line("bad", "7 (-1,20,30)"); assert(!custom_set[7]);
    read_color_line("good", "7 (0,20,30)"); assert(custom_set[7] && custom_rgb[7][1] == 20);
    host = &test_host; config_done = 1; card_count = gCard_nCount; duelist_count = 1;
    card_color = calloc(card_count + 1, 1); card_color_set = calloc(card_count + 1, 1);
    for (i = 0; i <= DUELIST_MAX; i++) duelist_mult[i] = -1;
    edited[1] = 2028; edited[723] = 20;
    scan_drops();
    assert(drops_scanned == 1 && weight_for(1, 1, 723) == 20);
    assert(weight_for(1, 0, 723) == 0);
    tier_count = 1; tiers[0] = (Tier){.threshold=20,.color=2,.has_threshold=1,.has_color=1};
    default_color = 0; undroppable_color = 6;
    apply_tiers();
    assert(card_color[723] == 2 && card_color[722] == 6 && card_color[1] == 0);
    /* The lower raw weight wins by score. Preserve that pool in the panel. */
    rank_mult[1] = 2000; opt_sort = SORT_SCORE; build_rows(1);
    assert(row_count == 1 && rows[0].pool == 1 && rows[0].by_score == 4056);
    opt_sort = SORT_WEIGHT; build_rows(1);
    assert(rows[0].pool == 0 && rows[0].by_weight == 2048);
    duelist_mult[1] = 0;
    assert(tier_score(1, 1, 20) == 0);
    memset(card_color_set, 0, card_count + 1); apply_tiers();
    assert(card_color[723] == 2); /* Zero score is still droppable. */
    custom_set[7] = 1; have_original_ramps = 1; ramps_done = 1; colors_removed();
    assert(uploads == 1 && !ramps_done && !have_original_ramps);
    colors_removed(); assert(uploads == 1);
    free(drop_weights); free(card_color); free(card_color_set);
    puts("Yamyi Mods: parsing, edited/added-card drops, score sorting and palette removal passed");
    return 0;
}
