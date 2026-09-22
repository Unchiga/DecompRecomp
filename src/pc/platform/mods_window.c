/* The mods window: two lists, Available and Applied, with arrow buttons
 * between them. Everything is laid out from `unit` (the menu scale) by
 * layout(), which both drawing and hit-testing share; colours follow the
 * menu bar's palette in menu.c so the two look like one piece of UI. */
#include "mods_window.h"
#include "platform.h"
#include "settings.h"
#include "pc/mods/mods.h"
#include <string.h>

#define C_BG 0x1e1f22u
#define C_PANEL 0x27282cu
#define C_EDGE 0x3f4147u
#define C_HOVER 0x33353au
#define C_BUTTON 0x33353au
#define C_BUTTON_HOVER 0x3f4147u
#define C_ACCENT 0x3b82f6u
#define C_ACCENT_HOVER 0x2f6fdbu
#define C_TEXT 0xe8e8eau
#define C_TEXT_DIM 0x8b8d93u
#define C_TEXT_FAINT 0x5c5e66u
#define C_TEXT_ON_ACCENT 0xffffffu
#define C_TAG_ON_ACCENT 0xdbeafeu
#define C_WARN 0xf5b642u

typedef struct { int x, y, w, h; } Rect;
typedef struct {
    int w, h;
    Rect panel[2];  /* 0: Available, 1: Applied */
    Rect arrow[2];  /* 0: apply (right), 1: remove (left) */
    Rect close, cancel, primary; /* the footer; cancel/primary only while pending */
    int footer_middle;
} Layout;

static int selected = -1, dragging = -1, pending = -1, pending_value, drag_moved;
static int unit, column_width, pointer_x, pointer_y, pointer_inside;
static Rect drag_origin; /* the row the drag started on; leaving it starts the drag */
static const char *status;

#define PAD (16 * unit)
#define ROW (28 * unit)
#define PANEL_PAD (4 * unit)
#define PANEL_TOP (96 * unit)
#define GUTTER (56 * unit)
#define BUTTON_H (28 * unit)
#define TITLE_Y (24 * unit)
#define HINT_Y (46 * unit)
#define HEADER_SEP_Y (62 * unit)
#define LABEL_Y (82 * unit)
#define RESTART_TAG "restart"
#define ERROR_TAG "error"
#define MSG_CONFIRM "Needs a restart. Unsaved progress will be lost."
#define MSG_RESTART_FAILED "Restart failed. Relaunch the game to apply it."
#define MSG_SAVE_FAILED "Could not save settings. Change cancelled."

static int visible_rows(void) { return Mods_Count() > 3 ? Mods_Count() : 3; }

/* The tag at the right of a row, or null: a mod that could not load says so
 * before it says it wants a restart. */
static const char *row_tag(int mod)
{
    if (Mods_Status(mod)[0]) return ERROR_TAG;
    return Mods_RequiresRestart(mod) ? RESTART_TAG : 0;
}

static int inside(const Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static Rect rect(int x, int y, int w, int h) { Rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }

static int button_width(const char *label) { return Menu_TextWidth(label) + 28 * unit; }

static void layout(Layout *l)
{
    int panel_h = visible_rows() * ROW + 2 * PANEL_PAD, centre, sep, right;
    l->w = 2 * PAD + 2 * column_width + GUTTER;
    l->panel[0] = rect(PAD, PANEL_TOP, column_width, panel_h);
    l->panel[1] = rect(PAD + column_width + GUTTER, PANEL_TOP, column_width, panel_h);
    centre = PANEL_TOP + panel_h / 2;
    l->arrow[0] = rect(PAD + column_width + (GUTTER - 32 * unit) / 2, centre - BUTTON_H - 4 * unit, 32 * unit, BUTTON_H);
    l->arrow[1] = rect(l->arrow[0].x, centre + 4 * unit, 32 * unit, BUTTON_H);
    sep = PANEL_TOP + panel_h + PAD;
    l->footer_middle = sep + PAD + BUTTON_H / 2;
    l->h = sep + PAD + BUTTON_H + PAD;
    right = l->w - PAD;
    l->primary = rect(right - button_width("Apply and restart"), sep + PAD, button_width("Apply and restart"), BUTTON_H);
    l->cancel = rect(l->primary.x - 8 * unit - button_width("Cancel"), sep + PAD, button_width("Cancel"), BUTTON_H);
    l->close = rect(right - button_width("Close"), sep + PAD, button_width("Close"), BUTTON_H);
}

/* The footer must hold its longest message beside its widest button pair. */
static int footer_width(void)
{
    int pending_row = Menu_TextWidth(MSG_CONFIRM) + button_width("Cancel") + 8 * unit + button_width("Apply and restart");
    int failed_row = Menu_TextWidth(MSG_RESTART_FAILED) + button_width("Close");
    int saved_row = Menu_TextWidth(MSG_SAVE_FAILED) + button_width("Close");
    int widest = pending_row > failed_row ? pending_row : failed_row;
    if (saved_row > widest) widest = saved_row;
    return 2 * PAD + widest + PAD;
}

void ModsWindow_Init(void)
{
    int i, needed;
    unit = Menu_Scale();
    column_width = 220 * unit;
    for (i = 0; i < Mods_Count(); ++i) {
        const char *tag = row_tag(i);
        int w = Menu_TextWidth(Mods_Name(i)) + 2 * PANEL_PAD + 24 * unit +
                (tag ? Menu_TextWidth(tag) + 12 * unit : 0);
        if (w > column_width) column_width = w;
    }
    needed = (footer_width() - 2 * PAD - GUTTER + 1) / 2;
    if (needed > column_width) column_width = needed;
    selected = dragging = pending = -1;
    drag_moved = pointer_inside = 0;
    status = "";
}

void ModsWindow_Size(int *w, int *h)
{
    Layout l;
    layout(&l);
    *w = l.w;
    *h = l.h;
}

/* --- drawing --------------------------------------------------------- */

static void fill(MenuCanvas *c, int x, int y, int w, int h, uint32_t colour)
{
    int xx, yy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width) w = c->width - x;
    if (y + h > c->height) h = c->height - y;
    for (yy = y; yy < y + h; ++yy)
        for (xx = x; xx < x + w; ++xx) c->pixels[yy * c->stride + xx] = 0xff000000u | colour;
}

static void fill_rect(MenuCanvas *c, const Rect *r, uint32_t colour) { fill(c, r->x, r->y, r->w, r->h, colour); }

static void outline(MenuCanvas *c, const Rect *r, int thickness, uint32_t colour)
{
    fill(c, r->x, r->y, r->w, thickness, colour);
    fill(c, r->x, r->y + r->h - thickness, r->w, thickness, colour);
    fill(c, r->x, r->y, thickness, r->h, colour);
    fill(c, r->x + r->w - thickness, r->y, thickness, r->h, colour);
}

/* A triangle with its tip at tip_x, pointing right (direction 1) or left (-1). */
static void arrow(MenuCanvas *c, int tip_x, int middle, int direction, uint32_t colour)
{
    int span = 5 * unit, i;
    for (i = 0; i < span; ++i) {
        int half = (i + 1) * 5 * unit / span;
        fill(c, tip_x - direction * (i + 1), middle - half, 1, 2 * half + 1, colour);
    }
}

static void button(MenuCanvas *c, const Rect *r, const char *label, int primary, int enabled)
{
    int hover = enabled && pointer_inside && inside(r, pointer_x, pointer_y);
    uint32_t bg = primary ? (hover ? C_ACCENT_HOVER : C_ACCENT) : hover ? C_BUTTON_HOVER : C_BUTTON;
    fill_rect(c, r, bg);
    if (!primary) outline(c, r, unit, C_EDGE);
    Menu_DrawText(c, r->x + (r->w - Menu_TextWidth(label)) / 2, r->y + r->h / 2, label,
                  !enabled ? C_TEXT_FAINT : primary ? C_TEXT_ON_ACCENT : C_TEXT);
}

static void arrow_button(MenuCanvas *c, const Rect *r, int direction, int enabled)
{
    int hover = enabled && pointer_inside && inside(r, pointer_x, pointer_y);
    if (enabled) fill_rect(c, r, hover ? C_ACCENT_HOVER : C_ACCENT);
    else { fill_rect(c, r, C_PANEL); outline(c, r, unit, C_EDGE); }
    arrow(c, r->x + r->w / 2 + direction * 3 * unit, r->y + r->h / 2, direction,
          enabled ? C_TEXT_ON_ACCENT : C_TEXT_FAINT);
}

/* The mods in the order the window shows them: Available, then Applied. */
static int nth_shown(int side, int row)
{
    int i, n = 0;
    for (i = 0; i < Mods_Count(); ++i)
        if (!!Mods_Enabled(i) == side && n++ == row) return i;
    return -1;
}

static Rect row_rect(const Layout *l, int side, int row)
{
    const Rect *p = &l->panel[side];
    return rect(p->x + PANEL_PAD, p->y + PANEL_PAD + row * ROW, p->w - 2 * PANEL_PAD, ROW);
}

static int drop_target(const Layout *l)
{
    int side;
    if (dragging < 0 || pending >= 0 || !pointer_inside) return -1;
    side = inside(&l->panel[1], pointer_x, pointer_y) ? 1 : inside(&l->panel[0], pointer_x, pointer_y) ? 0 : -1;
    return side >= 0 && side != !!Mods_Enabled(dragging) ? side : -1;
}

static void draw_panel(MenuCanvas *c, const Layout *l, int side)
{
    const Rect *p = &l->panel[side];
    int row, n = 0, target = drop_target(l) == side;
    Menu_DrawText(c, p->x, LABEL_Y, side ? "Applied" : "Available", C_TEXT_DIM);
    fill_rect(c, p, C_PANEL);
    outline(c, p, target ? 2 * unit : unit, target ? C_ACCENT : C_EDGE);
    for (row = 0; row < visible_rows(); ++row) {
        int mod = nth_shown(side, row), on_accent;
        Rect r;
        if (mod < 0) break;
        r = row_rect(l, side, row);
        on_accent = mod == selected && dragging != mod;
        if (on_accent) fill_rect(c, &r, C_ACCENT);
        else if (dragging == mod && drag_moved) outline(c, &r, unit, C_EDGE);
        else if (dragging < 0 && pending < 0 && pointer_inside && inside(&r, pointer_x, pointer_y)) fill_rect(c, &r, C_HOVER);
        Menu_DrawText(c, r.x + 12 * unit, r.y + ROW / 2, Mods_Name(mod), on_accent ? C_TEXT_ON_ACCENT : C_TEXT);
        if (row_tag(mod))
            Menu_DrawText(c, r.x + r.w - 12 * unit - Menu_TextWidth(row_tag(mod)), r.y + ROW / 2, row_tag(mod),
                          on_accent ? C_TAG_ON_ACCENT : C_TEXT_FAINT);
        ++n;
    }
    if (!n) {
        const char *empty = !Mods_Count() ? "No mods found" : side ? "No mods applied" : "All mods applied";
        Menu_DrawText(c, p->x + (p->w - Menu_TextWidth(empty)) / 2, p->y + p->h / 2, empty, C_TEXT_FAINT);
    }
}

void ModsWindow_Draw(MenuCanvas *c)
{
    Layout l;
    const char *hint = "Drag a mod between the lists, or select one and use the arrows.";
    layout(&l);
    fill(c, 0, 0, c->width, c->height, C_BG);
    Menu_DrawText(c, PAD, TITLE_Y, "Mods", C_TEXT);
    Menu_DrawText(c, PAD, HINT_Y, hint, C_TEXT_DIM);
    fill(c, 0, HEADER_SEP_Y, l.w, unit, C_EDGE);
    draw_panel(c, &l, 0);
    draw_panel(c, &l, 1);
    arrow_button(c, &l.arrow[0], 1, pending < 0 && selected >= 0 && !Mods_Enabled(selected));
    arrow_button(c, &l.arrow[1], -1, pending < 0 && selected >= 0 && Mods_Enabled(selected));
    fill(c, 0, l.panel[0].y + l.panel[0].h + PAD, l.w, unit, C_EDGE);
    if (pending >= 0) {
        Menu_DrawText(c, PAD, l.footer_middle, status, C_WARN);
        button(c, &l.cancel, "Cancel", 0, 1);
        button(c, &l.primary, "Apply and restart", 1, 1);
    } else {
        Menu_DrawText(c, PAD, l.footer_middle, status, C_TEXT_DIM);
        button(c, &l.close, "Close", 0, 1);
    }
    if (dragging >= 0 && drag_moved && pointer_inside && pending < 0) {
        Rect ghost = rect(pointer_x + 12 * unit, pointer_y - ROW / 2, Menu_TextWidth(Mods_Name(dragging)) + 24 * unit, ROW);
        if (ghost.x + ghost.w > l.w) ghost.x = pointer_x - 12 * unit - ghost.w;
        fill_rect(c, &ghost, C_HOVER);
        outline(c, &ghost, unit, C_ACCENT);
        Menu_DrawText(c, ghost.x + 12 * unit, ghost.y + ROW / 2, Mods_Name(dragging), C_TEXT);
    }
}

/* --- behaviour ------------------------------------------------------- */

static void apply(int mod, int value, int confirmed)
{
    if (mod < 0 || mod >= Mods_Count()) return;
    if (value && Mods_Status(mod)[0]) {   /* it cannot load; removing it still can */
        status = Mods_Status(mod);
        return;
    }
    if (Mods_RequiresRestart(mod) && !confirmed) {
        pending = mod;
        pending_value = value;
        status = MSG_CONFIRM;
        return;
    }
    {
        int old = Mods_Enabled(mod);
        Mods_SetEnabled(mod, value);
        if (!Settings_Save()) {
            Mods_SetEnabled(mod, old);
            pending = -1;
            status = MSG_SAVE_FAILED;
            return;
        }
    }
    if (Mods_Status(mod)[0]) {   /* it was applied, and would not load */
        status = Mods_Status(mod);
        pending = -1;
        return;
    }
    if (Mods_RequiresRestart(mod)) {
        if (Platform_RestartGame() < 0) status = MSG_RESTART_FAILED;
    } else status = value ? "Mod applied." : "Mod removed.";
    pending = -1;
}

static int row_at(const Layout *l, int x, int y)
{
    int side, row;
    for (side = 0; side < 2; ++side) {
        const Rect *p = &l->panel[side];
        if (!inside(p, x, y) || y < p->y + PANEL_PAD) continue;
        row = (y - p->y - PANEL_PAD) / ROW;
        return nth_shown(side, row);
    }
    return -1;
}

/* Up/Down walk the lists in the order they are shown. */
static void step_selection(int direction)
{
    int order[MODS_MAX], count = 0, side, i, at = -1;
    for (side = 0; side < 2; ++side)
        for (i = 0; i < Mods_Count(); ++i)
            if (!!Mods_Enabled(i) == side) order[count++] = i;
    if (!count) return;
    for (i = 0; i < count; ++i)
        if (order[i] == selected) at = i;
    selected = order[(at + direction + count) % count];
}

static void cancel_pending(void)
{
    pending = -1;
    status = "Change cancelled.";
}

int ModsWindow_Event(const MenuEvent *e)
{
    Layout l;
    layout(&l);
    if (e->type == MENU_EVENT_MOTION || e->type == MENU_EVENT_BUTTON_DOWN || e->type == MENU_EVENT_BUTTON_UP) {
        if (dragging >= 0 && !inside(&drag_origin, e->x, e->y)) drag_moved = 1;
        pointer_x = e->x; pointer_y = e->y; pointer_inside = 1;
    }
    if (e->type == MENU_EVENT_KEY_DOWN) {
        if (e->key == MENU_KEY_ESCAPE) {
            if (pending < 0) return 1;
            cancel_pending();
        } else if (e->key == MENU_KEY_ENTER) {
            if (pending >= 0) apply(pending, pending_value, 1);
            else if (selected >= 0) apply(selected, !Mods_Enabled(selected), 0);
        } else if (pending < 0 && (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN)) {
            step_selection(e->key == MENU_KEY_DOWN ? 1 : -1);
        } else if (pending < 0 && selected >= 0 && (e->key == MENU_KEY_RIGHT || e->key == MENU_KEY_LEFT)) {
            int want = e->key == MENU_KEY_RIGHT;
            if (want != !!Mods_Enabled(selected)) apply(selected, want, 0);
        }
    } else if (e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1) {
        if (pending >= 0) {
            if (inside(&l.cancel, e->x, e->y)) cancel_pending();
            else if (inside(&l.primary, e->x, e->y)) apply(pending, pending_value, 1);
        } else if (inside(&l.close, e->x, e->y)) {
            return 1;
        } else if (inside(&l.arrow[0], e->x, e->y)) {
            if (selected >= 0 && !Mods_Enabled(selected)) apply(selected, 1, 0);
        } else if (inside(&l.arrow[1], e->x, e->y)) {
            if (selected >= 0 && Mods_Enabled(selected)) apply(selected, 0, 0);
        } else {
            int hit = row_at(&l, e->x, e->y), row;
            if (hit >= 0 || inside(&l.panel[0], e->x, e->y) || inside(&l.panel[1], e->x, e->y)) selected = hit;
            dragging = hit;
            drag_moved = 0;
            if (hit >= 0) {
                for (row = 0; nth_shown(!!Mods_Enabled(hit), row) != hit; ++row) {}
                drag_origin = row_rect(&l, !!Mods_Enabled(hit), row);
            }
        }
    } else if (e->type == MENU_EVENT_BUTTON_UP && e->button == 1) {
        int target = drop_target(&l);
        if (target >= 0 && drag_moved) apply(dragging, target, 0);
        dragging = -1;
        drag_moved = 0;
    } else if (e->type == MENU_EVENT_LEAVE) {
        dragging = -1;
        drag_moved = 0;
        pointer_inside = 0;
    }
    return 0;
}
