/* The static recomp's in-duel helper, adapted to native game records and
 * shared mod-aware rules. No fixed 722-card database or guest-pixel canvas. */
#include "fusion_helper.h"
#include "fusion.h"
#include "cards.h"
#include "rules.h"
#include "pc/platform/settings.h"
#include "pc/platform/platform.h"
#include "pc/mods/events.h"
#include "pc/mods/hooks.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "pc/text/overlay_text.h"
#include "game/duel_card.h"
#include "game/duel_hand.h"
#include "game/duel_scene_state.h"
#include "game/duel_card_checks.h"
#include "game/duel_scene_card_placement.h"
#include "game/duel_terrain_boost.h"
#include "game/display_object.h"
#include "game/duel_effect.h"
#include "game/duel_action_lock.h"
#include "game/duel_check_quit_input.h"
#include "game/text_constants.h"
#define D_8009B360_AS_SIDE_ARRAY
#include "game/duel_side_state.h"
#include <stdio.h>
#include <string.h>

extern unsigned char D_8009B26C, D_8009B26E, D_8009B174;
static struct { int x, y, w, h; } viewport;
static struct {
    int visible, unsupported, picked, status;
    FusionLine target, route, current; /* the hand's best; the way to it; the picks' own */
    int card_x[FUSION_HAND], card_y[FUSION_HAND];
    char name[256], current_name[256];
} view;

void FusionHelper_Viewport(int x, int y, int w, int h)
{ viewport.x = x; viewport.y = y; viewport.w = w; viewport.h = h; }

static FusionCard card(int id)
{
    FusionCard result = {0};
    unsigned stats;
    if (!Cards_Valid(id)) return result;
    stats = (unsigned)gDuel_adwCardStats[id - 1];
    result.id = id;
    result.type = (int)(stats >> CARD_STAT_TYPE_SHIFT & CARD_STAT_TYPE_MASK);
    result.attack = (int)(stats & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    result.defense = (int)(stats >> CARD_STAT_DEFENSE_SHIFT & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    result.terrain = Duel_GetTerrainBoost(result.type);
    return result;
}

static void name(int id, char out[256])
{
    const unsigned char *text;
    int base, n = 0, limit = 100;
    if (!Cards_Valid(id)) { out[0] = 0; return; }
    base = Cards_BaseId(id);
    text = Cards_NameText(id);
    if (!text) /* as the duel's text command reads a card name */
        text = Text_Resolve(0x8000 + base, (const unsigned char *)((uintptr_t)D_801D5800 & 0xFFFF0000u) + D_801D5800[base]);
    while (n < 251 && limit-- && *text < 0xF6) {
        int code = *text++;
        uint32_t ch;
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *text++;
        ch = Glyphs_Character(code);
        if (!ch) ch = '?';
        if (ch < 0x80) out[n++] = (char)ch;
        else if (ch < 0x800) { out[n++] = (char)(0xC0 | ch >> 6); out[n++] = (char)(0x80 | (ch & 63)); }
        else if (ch < 0x10000) {
            out[n++] = (char)(0xE0 | ch >> 12); out[n++] = (char)(0x80 | (ch >> 6 & 63)); out[n++] = (char)(0x80 | (ch & 63));
        } else {
            out[n++] = (char)(0xF0 | ch >> 18); out[n++] = (char)(0x80 | (ch >> 12 & 63));
            out[n++] = (char)(0x80 | (ch >> 6 & 63)); out[n++] = (char)(0x80 | (ch & 63));
        }
    }
    out[n] = 0;
    if (!n) snprintf(out, 256, "Card %d", id);
}

static void update(void)
{
    static const FusionRules rules = {card, CardRules_Fusion, CardRules_Equip};
    FusionCard hand[FUSION_HAND] = {{0}};
    int prefix[FUSION_HAND] = {-1, -1, -1, -1, -1};
    int slot, picked = 0, count = 0, side = D_8009B1D5;
    memset(&view, 0, sizeof(view));
    if (!Settings_Get(SET_FUSION_HELPER) || (D_8009B26C & 31) != 3 || D_8009B26E != 0x81 ||
        (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) != 4 ||
        (D_8009B174 & 15) != 1 || side > 1 || D_8009B360[side] >= 0 || gDuel_bEffectState ||
        gDuel_wCardEffectFlags || gDuel_bQuitDialogState) return;
    for (slot = 0; slot < FUSION_HAND; slot++) {
        const DuelHandSlot *pick = &D_800EA030[slot];
        const DisplayObject *object = (const DisplayObject *)pick->object;
        const DuelCardRecord *record;
        int index;
        if (!object) continue;
        /* The object's actual record also works for side 2 and moved hands. */
        index = object->field_6A;
        if (index < 0 || index >= DUEL_CARD_RECORD_COUNT) return;
        record = &D_801A7AD8[index];
        if (!Cards_Valid(record->card_id) || !(record->flags & DUEL_CARD_FLAG_OCCUPIED)) return;
        hand[slot] = (FusionCard){record->card_id, Cards_Type(record->card_id),
            record->attack, record->defense, record->stat_modifier, record->terrain_modifier};
        view.card_x[slot] = (short)object->field_30.h.field_30;
        view.card_y[slot] = (short)object->field_30.h.field_32;
        count++;
        if (pick->child) {
            int order = pick->active_09;
            if (order < 1 || order > FUSION_HAND || prefix[order - 1] >= 0) return;
            prefix[order - 1] = slot;
            picked++;
        }
    }
    if (!count) return;
    for (slot = 0; slot < picked; slot++) if (prefix[slot] < 0) return;
    view.visible = 1;
    /* Arbitrary gameplay callbacks may consume RNG or write state. Never run
     * them while searching, and never claim data-only advice is exact. */
    view.unsupported = Mods_HasSubscribers(MEMORIES_EVENT_FUSION) || Mods_HasSubscribers(MEMORIES_EVENT_EQUIP) ||
        Hooks_IsHooked((const void *)Duel_CheckFusion) || Hooks_IsHooked((const void *)Duel_CheckEquip) ||
        Hooks_IsHooked((const void *)CardRules_Fusion) || Hooks_IsHooked((const void *)CardRules_Equip) ||
        Hooks_IsHooked((const void *)DuelScene_UpdateCardPlacement) || Hooks_IsHooked((const void *)Duel_GetTerrainBoost) ||
        Hooks_IsHooked((const void *)Duel_CalcCardStats);
    if (view.unsupported) return;
    {
        FusionLine none, ahead;
        Fusion_Plan(&rules, hand, NULL, 0, 0, &none, &view.target);
        Fusion_Plan(&rules, hand, prefix, picked, 0, &view.current, &ahead);
    }
    view.picked = picked;
    view.status = Fusion_Toward(&rules, hand, prefix, picked, view.target.card, &view.route);
    name(view.target.card.id, view.name);
    name(view.current.card.id, view.current_name);
}

unsigned FusionHelper_Signature(void)
{
    const unsigned char *bytes = (const unsigned char *)&view;
    unsigned hash = 2166136261u;
    size_t i;
    update();
    for (i = 0; i < sizeof(view); i++) hash = (hash ^ bytes[i]) * 16777619u;
    return view.visible ? hash : 0;
}

static void fill(MenuCanvas *canvas, int x, int y, int w, int h, uint32_t colour, unsigned alpha)
{
    int row, col;
    for (row = y; row < y + h; row++)
        for (col = x; col < x + w; col++) OverlayText_Blend(canvas, col, row, colour, alpha);
}

/* Game picture coordinates (320x240; 2D stays centred when widened) to window pixels. */
enum { FIELD_BOX_LEFT = 13, FIELD_BOX_TOP = 24, HAND_CARD_WIDTH = 47 };
static int screen_x(int x)
{ return viewport.x + viewport.w / 2 + (x - 160) * viewport.w / (Platform_Widescreen() ? 426 : 320); }
static int screen_y(int y) { return viewport.y + y * viewport.h / 240; }

static void describe(char *out, size_t size, const char *label, const char *card_name, FusionCard card)
{
    if (card.type >= 20) snprintf(out, size, "%s%s   Spell / trap", label, card_name);
    else snprintf(out, size, "%s%s   ATK %d  DEF %d", label, card_name, Fusion_Attack(card), Fusion_Defense(card));
}

/* The best card the hand can make and its stats: white before a real
 * choice is made, green when the picks make it, red when they make
 * anything else or cannot get there. Under it, from two picks on, what
 * going ahead now gives. */
void FusionHelper_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    char lines[2][512] = {"", ""};
    uint32_t colour = 0xf2f6f8u;
    int i, rows = 1, font, badge, pad, row, width, height, margin, gap;
    *x = *y = *w = *h = 0;
    update();
    if (!view.visible || viewport.w <= 0 || viewport.h <= 0) return;
    font = badge = viewport.h / 42 < 8 ? 8 : viewport.h / 42 > 40 ? 40 : viewport.h / 42;
    if (view.unsupported)
        snprintf(lines[0], sizeof(lines[0]), "Fusion preview unavailable for custom rule code");
    else if (!view.target.count)
        snprintf(lines[0], sizeof(lines[0]), "No fusions in hand");
    else {
        describe(lines[0], sizeof(lines[0]), "", view.name, view.target.card);
        /* Green only when going ahead now gives it. Two or more picks that
         * give anything else (even on the way) are red; so is a first pick
         * that is not one of its materials. */
        if (view.picked >= 2) colour = view.status == 2 ? 0x8fd6a0u : 0xe8867cu;
        else if (view.picked && !view.status) colour = 0xe8867cu;
        if (view.picked >= 2) describe(lines[rows++], sizeof(lines[1]), "Result: ", view.current_name, view.current.card);
    }
    if (lines[0][0]) {
        /* A see-through strip above the FIELD box (game lines 0-23),
         * left-aligned with it, clear of the zones and the hand. The font
         * shrinks until every row fits there. */
        margin = viewport.h / 240 > 1 ? viewport.h / 240 : 1;
        gap = screen_y(FIELD_BOX_TOP) - viewport.y - 2 * margin;
        if (font > gap * 4 / (rows * 5 + 2)) font = gap * 4 / (rows * 5 + 2);
        if (font < 8) font = 8;
        pad = font / 2;
        row = font + font / 4;
        width = OverlayText_Width(lines[0], font);
        if (rows > 1 && OverlayText_Width(lines[1], font) > width) width = OverlayText_Width(lines[1], font);
        width += 2 * pad;
        height = rows * row + font / 2;
        *x = screen_x(FIELD_BOX_LEFT);
        *y = viewport.y + margin + (gap - height) / 2;
        if (*y < viewport.y) *y = viewport.y;
        if (width > viewport.x + viewport.w - margin - *x) width = viewport.x + viewport.w - margin - *x;
        if (width <= 0) return;
        *w = width; *h = height;
        fill(canvas, *x, *y, width, height, 0x0b0f18u, 150);
        for (i = 0; i < rows; i++)
            OverlayText_Draw(canvas, *x + pad, *y + font / 4 + row * i + row / 2, *x + width - pad, lines[i], font,
                             i ? 0xc4ccd2u : colour);
    }
    if (!view.unsupported && view.status == 1) {
        /* Pick-order badges in each hand card's top-right corner (the game's
         * own selection tags take the top-left one) while the card can still
         * be made. Picks made are skipped. */
        for (i = view.picked; i < view.route.count; i++) {
            char number[8];
            int slot = view.route.slots[i], size = badge + badge / 2;
            int bx = screen_x(view.card_x[slot] + HAND_CARD_WIDTH - 3) - size;
            int by = screen_y(view.card_y[slot] + 5);
            int right = *x + *w, bottom = *y + *h;
            if (!*w) { *x = bx; *y = by; right = bx; bottom = by; }
            snprintf(number, sizeof(number), "%d", i + 1);
            fill(canvas, bx, by, size, size, 0x0b0f18u, 170);
            OverlayText_Draw(canvas, bx + (size - OverlayText_Width(number, badge)) / 2, by + size / 2,
                             bx + size, number, badge, 0xf2f6f8u);
            if (bx < *x) *x = bx;
            if (by < *y) *y = by;
            if (bx + size > right) right = bx + size;
            if (by + size > bottom) bottom = by + size;
            *w = right - *x; *h = bottom - *y;
        }
    }
    /* Bounds are consumed by the platform's partial upload/clear path. */
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*w > canvas->width - *x) *w = canvas->width - *x;
    if (*h > canvas->height - *y) *h = canvas->height - *y;
}
