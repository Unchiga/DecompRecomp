/* Searchable mod library, details, staged settings and named profiles.
 * The same bounded layout drives painting and input on SDL and X11. */
#include "mods_window.h"
#include "../../types.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "platform.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BG 0x181b20u
#define PANEL 0x22262du
#define EDGE 0x363d49u
#define TEXT 0xe9edf3u
#define DIM 0x9aa8bau
#define BLUE 0x639cffu
#define ACCENT 0x356dd1u
#define GREEN 0x77d6a0u
#define WARN 0xf4bd6au
#define RED 0xff9292u

typedef struct {
    int x, y, w, h;
} Rect;
typedef struct {
    Rect search, filter, list, list_bar, detail, toggle, tabs[3], view, bar, apply, close, profile, save, load,
        order[2], defaults, folder;
} Layout;
static int width, height, unit, selected, scroll, detail_scroll, tab, filter, focus, pending;
static int wanted[MODS_MAX], ranks[MODS_MAX], *values[MODS_MAX], counts[MODS_MAX];
static int slider_drag = -1, bar_drag, bar_grab; /* bar_drag: 1 the list's scrollbar, 2 the details' */
static char query[96], profile[65] = "Default", status[512];
static const char *filters[] = {"All mods", "Enabled", "Disabled", "Issues"};
static const char *pad_names[] = {"Select", "L3", "R3", "Start", "Up",       "Right",  "Down",  "Left",
                                  "L2",     "R2", "L1", "R1",    "Triangle", "Circle", "Cross", "Square"};
#define BAR (10 * unit)
#define LINE (22 * unit)
static int width_text(const char *s) { return Menu_TextWidthScaled(s, unit); }
static Rect rect(int x, int y, int w, int h)
{
    Rect r = {x, y, w, h};
    return r;
}
static int inside(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static void layout(Layout *l)
{
    int p = 20 * unit, gap = 16 * unit, left = width * 36 / 100, footer = height - 72 * unit, top = 142 * unit, right,
        view_top;
    l->search = rect(p, 64 * unit, left - p, 32 * unit);
    l->filter = rect(p, 104 * unit, left - p, 28 * unit);
    l->list = rect(p, top, left - p, footer - top - 12 * unit);
    l->list_bar = rect(l->list.x + l->list.w - BAR - 3 * unit, l->list.y + 4 * unit, BAR, l->list.h - 8 * unit);
    l->detail = rect(left + gap, top, width - left - gap - p, footer - top - 12 * unit);
    l->profile = rect(left + gap, 64 * unit, max(80 * unit, width - left - gap - p - 160 * unit), 32 * unit);
    l->save = rect(width - p - 152 * unit, 64 * unit, 72 * unit, 32 * unit);
    l->load = rect(width - p - 72 * unit, 64 * unit, 72 * unit, 32 * unit);
    /* Name and metadata on the left of the details, switch and order on the right. */
    right = l->detail.x + l->detail.w - 16 * unit;
    l->toggle = rect(right - 120 * unit, l->detail.y + 10 * unit, 120 * unit, 28 * unit);
    l->order[0] = rect(right - 120 * unit, l->detail.y + 44 * unit, 28 * unit, 26 * unit);
    l->order[1] = rect(right - 28 * unit, l->detail.y + 44 * unit, 28 * unit, 26 * unit);
    for (int i = 0; i < 3; i++)
        l->tabs[i] = rect(l->detail.x + i * l->detail.w / 3, l->detail.y + 80 * unit,
                          (i + 1) * l->detail.w / 3 - i * l->detail.w / 3, 32 * unit);
    view_top = l->detail.y + 124 * unit;
    l->defaults = rect(right - 150 * unit, view_top, 150 * unit, 28 * unit);
    if (tab == 1 && selected >= 0 && counts[selected])
        view_top += 40 * unit; /* the settings' toolbar stays put above the scrolling list */
    l->view = rect(l->detail.x + 16 * unit, view_top, l->detail.w - 22 * unit,
                   max(0, l->detail.y + l->detail.h - 8 * unit - view_top));
    l->bar = rect(l->view.x + l->view.w - BAR, l->view.y, BAR, l->view.h);
    l->apply = rect(width - p - 150 * unit, height - 48 * unit, 150 * unit, 30 * unit);
    l->close = rect(width - p - 252 * unit, height - 48 * unit, 94 * unit, 30 * unit);
    l->folder = rect(width - p - 418 * unit, height - 48 * unit, 158 * unit, 30 * unit);
}
static const char *str(const JsonValue *v, const char *key, const char *fallback)
{
    return Json_String(Json_Member(v, key), fallback);
}
static int num(const JsonValue *v, const char *key, int fallback)
{
    return (int)Json_Number(Json_Member(v, key), fallback);
}
static int contains(const char *text, const char *part)
{
    size_t i, n = strlen(part);
    if (!n)
        return 1;
    for (; *text; text++) {
        for (i = 0; i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)part[i]); i++) {
        }
        if (i == n)
            return 1;
    }
    return 0;
}
static int visible(int mod)
{
    if (filter == 1 && !wanted[mod])
        return 0;
    if (filter == 2 && wanted[mod])
        return 0;
    if (filter == 3 && !Mods_Status(mod)[0])
        return 0;
    return contains(Mods_Name(mod), query) || contains(Mods_Id(mod), query) ||
           contains(Mods_Metadata(mod, "author"), query);
}
static int shown(int row)
{
    for (int i = 0; i < Mods_Count(); i++)
        if (visible(i) && row-- == 0)
            return i;
    return -1;
}
static int rows(void)
{
    Layout l;
    layout(&l);
    return max(1, l.list.h / (58 * unit));
}
static int shown_count(void)
{
    int n = 0;
    for (int i = 0; i < Mods_Count(); i++)
        n += visible(i);
    return n;
}
static int changed(void)
{
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (wanted[i] != Mods_Enabled(i) || ranks[i] != Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0)))
            return 1;
        for (int j = 0; j < counts[i]; j++)
            if (values[i][j] != Mods_OptionValue(i, j))
                return 1;
    }
    return 0;
}
void ModsWindow_Init(void)
{
    unit = Menu_Scale();
    width = 920 * unit;
    height = 640 * unit;
    slider_drag = -1;
    bar_drag = 0;
    selected = Mods_Count() ? 0 : -1;
    scroll = detail_scroll = tab = filter = focus = pending = 0;
    query[0] = status[0] = 0;
    for (int i = 0; i < MODS_MAX; i++) {
        free(values[i]);
        values[i] = NULL;
        counts[i] = 0;
    }
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        wanted[i] = Mods_Enabled(i);
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        ranks[i] = Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0));
        counts[i] = Mods_OptionCount(i);
        values[i] = calloc((size_t)max(1, counts[i]), sizeof(int));
        if (!values[i]) {
            counts[i] = 0;
            snprintf(status, sizeof(status), "Could not allocate mod settings");
            continue;
        }
        for (int j = 0; j < counts[i]; j++)
            values[i][j] = Mods_OptionValue(i, j);
    }
}
void ModsWindow_Resize(int w, int h)
{
    width = max(480, w);
    height = max(360, h);
    unit = Menu_Scale();
    while (unit > 1 && (width < 700 * unit || height < 460 * unit))
        unit--;
}
void ModsWindow_Size(int *w, int *h)
{
    *w = width;
    *h = height;
}
static void fill(MenuCanvas *c, Rect r, unsigned colour)
{
    int x0 = max(0, r.x), y0 = max(0, r.y), x1 = r.x + r.w, y1 = r.y + r.h;
    if (!c)
        return;
    if (x1 > c->width)
        x1 = c->width;
    if (y1 > c->height)
        y1 = c->height;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            c->pixels[y * c->stride + x] = 0xff000000u | colour;
}
static void text(MenuCanvas *c, int x, int y, int w, const char *s, unsigned colour)
{
    char line[512];
    size_t n = strlen(s);
    if (!c || w <= 0)
        return;
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    memcpy(line, s, n);
    line[n] = 0;
    if (width_text(line) > w) {
        while (n && width_text(line) + width_text("...") > w)
            line[--n] = 0;
        if (n + 3 < sizeof(line))
            strcat(line, "...");
    }
    Menu_DrawTextScaled(c, x, y, line, colour, unit);
}
static void button(MenuCanvas *c, Rect r, const char *label, int accent)
{
    fill(c, r, accent ? ACCENT : EDGE);
    text(c, r.x + 10 * unit, r.y + r.h / 2, r.w - 20 * unit, label, TEXT);
}
static void centred(MenuCanvas *c, Rect r, const char *s, unsigned colour)
{
    text(c, r.x + max(6 * unit, (r.w - width_text(s)) / 2), r.y + r.h / 2, r.w - 12 * unit, s, colour);
}
/* Word-wraps `s` into `w` pixels, its first line's top at `y`, and returns the
 * top of the line after it. Without a canvas it only measures. */
static int wrap(MenuCanvas *c, int x, int y, int w, const char *s, unsigned colour)
{
    char line[512];
    int n = 0;
    while (*s) {
        line[n++] = *s++;
        line[n] = 0;
        if (n >= 500 || *s == '\n' || !*s || width_text(line) > w) {
            if (width_text(line) > w && n > 1) {
                int split = n - 1;
                while (split > 0 && line[split] != ' ')
                    split--;
                if (!split)
                    split = n - 1;
                s -= n - split;
                n = split;
                line[n] = 0;
            }
            text(c, x, y + LINE / 2, w, line, colour);
            y += LINE;
            n = 0;
            while (*s == ' ' || *s == '\n')
                s++;
        }
    }
    return y;
}
static void option_label(int mod, int index, char *out, size_t size)
{
    const JsonValue *spec = Mods_Option(mod, index);
    const char *type = str(spec, "type", "int");
    int value = values[mod][index];
    if (!strcmp(type, "bool"))
        snprintf(out, size, "%s", value ? "On" : "Off");
    else if (!strcmp(type, "choice"))
        snprintf(out, size, "%s", Json_String(Json_At(Json_Member(spec, "choices"), value), "Invalid choice"));
    else if (!strcmp(type, "key")) {
        int bit = 0;
        while (bit < 16 && value != (1 << bit))
            bit++;
        snprintf(out, size, "%s", bit < 16 ? pad_names[bit] : "None");
    } else
        snprintf(out, size, "%d%s", value, str(spec, "suffix", ""));
}
/* Wide enough for every value the setting can take, so none is cut off. */
static int value_width(int option, int w)
{
    const JsonValue *spec = Mods_Option(selected, option);
    const char *type = str(spec, "type", "int");
    char s[96];
    int widest = width_text("Off");
    if (!strcmp(type, "choice")) {
        const JsonValue *choices = Json_Member(spec, "choices");
        for (int i = 0; i < Json_Count(choices); i++)
            widest = max(widest, width_text(Json_String(Json_At(choices, i), "")));
    } else if (!strcmp(type, "key")) {
        for (int i = 0; i < 16; i++)
            widest = max(widest, width_text(pad_names[i]));
    } else if (strcmp(type, "bool")) {
        snprintf(s, sizeof(s), "%d%s", num(spec, "min", 0), str(spec, "suffix", ""));
        widest = max(widest, width_text(s));
        snprintf(s, sizeof(s), "%d%s", num(spec, "max", 100), str(spec, "suffix", ""));
        widest = max(widest, width_text(s));
    }
    return min(max(widest + 24 * unit, 64 * unit), w * 2 / 5);
}
static int is_int(int option) { return !strcmp(str(Mods_Option(selected, option), "type", "int"), "int"); }
typedef struct {
    Rect minus, value, plus, slider;
    int height;
} OptionBox;
/* One setting of the selected mod, `w` wide with its top at `y`: the label
 * wraps beside - value +, the description wraps under both, then an int's
 * slider. Fills `o` in the same coordinates; draws only with a canvas. */
static void option(MenuCanvas *c, int index, int w, int y, OptionBox *o)
{
    const JsonValue *spec = Mods_Option(selected, index);
    const char *about = str(spec, "description", "");
    char value[160];
    int box = value_width(index, w), bottom;
    o->plus = rect(w - 28 * unit, y, 28 * unit, 28 * unit);
    o->value = rect(o->plus.x - 4 * unit - box, y, box, 28 * unit);
    o->minus = rect(o->value.x - 32 * unit, y, 28 * unit, 28 * unit);
    bottom = wrap(c, 0, y + 3 * unit, o->minus.x - 16 * unit, str(spec, "label", str(spec, "key", "Setting")), TEXT);
    bottom = max(y + 28 * unit, bottom);
    if (*about)
        bottom = wrap(c, 0, bottom + 4 * unit, w, about, DIM);
    if (Json_Bool(Json_Member(spec, "restart"), 0))
        bottom = wrap(c, 0, bottom + (*about ? 0 : 4 * unit), w, "Requires a restart", WARN);
    o->slider = rect(0, bottom + 2 * unit, w, is_int(index) ? 22 * unit : 0);
    o->height = o->slider.y + o->slider.h + 14 * unit - y;
    if (!c)
        return;
    option_label(selected, index, value, sizeof(value));
    button(c, o->minus, "-", 0);
    fill(c, o->value, BG);
    centred(c, o->value, value,
            !strcmp(str(spec, "type", "int"), "bool") ? (values[selected][index] ? GREEN : DIM) : BLUE);
    button(c, o->plus, "+", 0);
    if (o->slider.h) {
        int low = num(spec, "min", 0), high = num(spec, "max", 100), span = w - 10 * unit;
        int mid = o->slider.y + o->slider.h / 2;
        int at = high > low ? (int)(((int64_t)values[selected][index] - low) * span / ((int64_t)high - low)) : 0;
        at = min(max(at, 0), span);
        fill(c, rect(0, mid - 2 * unit, w, 4 * unit), EDGE);
        fill(c, rect(0, mid - 2 * unit, at, 4 * unit), ACCENT);
        fill(c, rect(at, mid - 7 * unit, 10 * unit, 14 * unit), BLUE);
    }
    fill(c, rect(0, y + o->height - 1, w, 1), EDGE);
}
/* The selected tab's contents, `w` wide from `y`; returns where they end. */
static int body(MenuCanvas *c, int w, int y)
{
    char line[512];
    if (tab == 0) {
        y = wrap(c, 0, y, w, Mods_Metadata(selected, "description"), TEXT) + 16 * unit;
        snprintf(line, sizeof(line), "ID: %s", Mods_Id(selected));
        y = wrap(c, 0, y, w, line, DIM);
        y = wrap(c, 0, y, w, Mods_Directory(selected), DIM) + 16 * unit;
        y = wrap(c, 0, y, w,
                 *Mods_Metadata(selected, "library") ? "Native code mod: runs game code from this author."
                                                     : "Content mod: assets, cards or data patches.",
                 DIM);
        if (Mods_Status(selected)[0])
            y = wrap(c, 0, y + 16 * unit, w, Mods_Status(selected), Mods_Failed(selected) ? RED : WARN);
    } else if (tab == 1) {
        if (!counts[selected])
            return wrap(c, 0, y, w, "This mod does not declare configurable settings.", DIM);
        for (int j = 0; j < counts[selected]; j++) {
            OptionBox o;
            option(c, j, w, y + 10 * unit, &o);
            y += 10 * unit + o.height;
        }
    } else {
        const char *keys[] = {"requires", "after", "conflicts"};
        const char *labels[] = {"Requires: ", "Load after: ", "Conflicts: "};
        y = wrap(c, 0, y, w,
                 Mods_RequiresRestart(selected) ? "Changes require a restart." : "This mod supports live changes.",
                 DIM) +
            12 * unit;
        for (int k = 0; k < 3; k++) {
            const JsonValue *list = Json_Member(Mods_Manifest(selected), keys[k]);
            for (int j = 0; j < Json_Count(list); j++) {
                const JsonValue *v = Json_At(list, j);
                snprintf(line, sizeof(line), "%s%s", labels[k], Json_String(v, str(v, "id", "")));
                y = wrap(c, 0, y, w, line, TEXT);
            }
        }
        if (!Mods_Validate(wanted, line, sizeof(line)))
            y = wrap(c, 0, y + 12 * unit, w, line, WARN);
        else
            y = wrap(c, 0, y + 12 * unit, w, "Dependencies and declared conflicts are satisfied.", GREEN);
        if (Mods_ConflictText(selected, line, sizeof(line)))
            y = wrap(c, 0, y + 12 * unit, w, line, WARN);
    }
    return y;
}
/* The details scroll by pixels inside `view`, left of their scrollbar. */
static int body_width(const Layout *l) { return l->view.w - BAR - 12 * unit; }
static int body_height(const Layout *l) { return selected < 0 ? 0 : body(NULL, body_width(l), 0) + 12 * unit; }
static int detail_limit(const Layout *l) { return max(0, body_height(l) - l->view.h); }
static Rect thumb(Rect track, int total, int shown, int at)
{
    int h = min(track.h, max(28 * unit, (int)((int64_t)track.h * shown / max(1, total))));
    return rect(track.x, track.y + (int)((int64_t)(track.h - h) * at / max(1, total - shown)), track.w, h);
}
/* The scroll position that puts the thumb's top at `y`. */
static int thumb_to(Rect track, int total, int shown, int y)
{
    int travel = track.h - thumb(track, total, shown, 0).h;
    if (travel <= 0)
        return 0;
    y = min(max(y - track.y, 0), travel);
    return (int)(((int64_t)y * (total - shown) + travel / 2) / travel);
}
static void scrollbar(MenuCanvas *c, Rect track, int total, int shown, int at, int held)
{
    if (total <= shown)
        return;
    fill(c, track, BG);
    fill(c, thumb(track, total, shown, at), held ? BLUE : 0x5d6b80u);
}
void ModsWindow_Draw(MenuCanvas *c)
{
    Layout l;
    char line[512];
    int enabled = 0, list_bar;
    layout(&l);
    fill(c, rect(0, 0, c->width, c->height), BG);
    for (int i = 0; i < Mods_Count(); i++)
        enabled += !!wanted[i];
    text(c, 20 * unit, 29 * unit, width - 40 * unit, "Mod library", TEXT);
    snprintf(line, sizeof(line), "%d installed  /  %d enabled%s", Mods_Count(), enabled,
             changed() ? "  /  Unsaved changes" : "");
    text(c, width / 2, 29 * unit, width / 2 - 20 * unit, line, DIM);
    fill(c, l.search, focus == 1 ? EDGE : PANEL);
    text(c, l.search.x + 10 * unit, l.search.y + 16 * unit, l.search.w - 20 * unit,
         *query ? query : "Search mods, IDs or authors...", *query ? TEXT : DIM);
    button(c, l.filter, filters[filter], 0);
    fill(c, l.profile, focus == 2 ? EDGE : PANEL);
    text(c, l.profile.x + 10 * unit, l.profile.y + 16 * unit, l.profile.w - 20 * unit, profile, TEXT);
    button(c, l.save, "Save", 0);
    button(c, l.load, "Load", 0);
    text(c, l.profile.x, 118 * unit, width - l.profile.x - 20 * unit, "Named profile  /  Save uses applied settings",
         DIM);
    fill(c, l.list, PANEL);
    fill(c, l.detail, PANEL);
    list_bar = shown_count() > rows();
    for (int r = 0; r < rows(); r++) {
        int mod = shown(scroll + r), w = l.list.w - 54 * unit - (list_bar ? BAR + 6 * unit : 0);
        Rect row = rect(l.list.x, l.list.y + r * 58 * unit, l.list.w, 56 * unit);
        if (mod < 0)
            break;
        if (mod == selected)
            fill(c, row, 0x293e60u);
        text(c, row.x + 12 * unit, row.y + 18 * unit, 24 * unit, wanted[mod] ? "[x]" : "[ ]",
             wanted[mod] ? GREEN : DIM);
        text(c, row.x + 44 * unit, row.y + 18 * unit, w, Mods_Name(mod), TEXT);
        snprintf(line, sizeof(line), "%s%s%s",
                 Mods_Failed(mod)      ? "Error"
                 : Mods_Status(mod)[0] ? "Warning"
                 : Mods_Active(mod)    ? "Active"
                                       : "Inactive",
                 wanted[mod] != Mods_Active(mod) ? " / pending" : "", Mods_RequiresRestart(mod) ? " / restart" : "");
        text(c, row.x + 44 * unit, row.y + 39 * unit, w, line, Mods_Failed(mod) ? RED : DIM);
    }
    if (!shown_count())
        text(c, l.list.x + 16 * unit, l.list.y + 32 * unit, l.list.w - 32 * unit, "No matching mods", DIM);
    scrollbar(c, l.list_bar, shown_count(), rows(), scroll, bar_drag == 1);
    if (selected >= 0) {
        int name_x = l.detail.x + 16 * unit, total = body_height(&l), order_x;
        text(c, name_x, l.detail.y + 24 * unit, l.toggle.x - 12 * unit - name_x, Mods_Name(selected), TEXT);
        button(c, l.toggle, wanted[selected] ? "Enabled" : "Disabled", wanted[selected]);
        order_x = l.order[0].x - 10 * unit - width_text("Load order");
        snprintf(line, sizeof(line), "%s  /  %s  /  %s", Mods_Metadata(selected, "version"),
                 Mods_Metadata(selected, "author"), Mods_Origin(selected));
        text(c, name_x, l.detail.y + 57 * unit, order_x - 12 * unit - name_x, line, DIM);
        text(c, order_x, l.detail.y + 57 * unit, width_text("Load order") + unit, "Load order", DIM);
        button(c, l.order[0], "-", 0);
        snprintf(line, sizeof(line), "%d", ranks[selected]);
        centred(c, rect(l.order[0].x + l.order[0].w, l.order[0].y, l.order[1].x - l.order[0].x - l.order[0].w,
                        l.order[0].h),
                line, TEXT);
        button(c, l.order[1], "+", 0);
        button(c, l.tabs[0], "About", tab == 0);
        button(c, l.tabs[1], "Settings", tab == 1);
        button(c, l.tabs[2], "Compatibility", tab == 2);
        if (tab == 1 && counts[selected]) {
            snprintf(line, sizeof(line), "%d setting%s", counts[selected], counts[selected] == 1 ? "" : "s");
            text(c, l.view.x, l.defaults.y + l.defaults.h / 2, l.defaults.x - l.view.x - 12 * unit, line, DIM);
            button(c, l.defaults, "Restore defaults", 0);
            fill(c, rect(l.view.x, l.view.y - 7 * unit, l.view.w, 1), EDGE);
        }
        detail_scroll = min(detail_scroll, max(0, total - l.view.h));
        if (l.view.h > 0 && l.view.x + l.view.w <= c->width && l.view.y + l.view.h <= c->height) {
            /* A canvas over just the viewport clips the scrolled contents. */
            MenuCanvas view = *c;
            view.pixels = c->pixels + (size_t)l.view.y * c->stride + l.view.x;
            view.width = body_width(&l);
            view.height = l.view.h;
            body(&view, view.width, -detail_scroll);
        }
        scrollbar(c, l.bar, total, l.view.h, detail_scroll, bar_drag == 2);
    } else
        text(c, l.detail.x + 20 * unit, l.detail.y + 30 * unit, l.detail.w - 40 * unit,
             "Select a mod to view its details", DIM);
    fill(c, rect(0, height - 72 * unit, width, 1), EDGE);
    text(c, 20 * unit, height - 59 * unit, width - 40 * unit,
         *status ? status : "Changes are staged. Apply once when you are ready.", pending ? WARN : DIM);
    text(c, 20 * unit, height - 32 * unit, l.folder.x - 28 * unit,
         "Arrow keys: select / toggle   Tab: search   Mouse wheel: scroll", DIM);
    button(c, l.folder, "Open mods folder", 0);
    button(c, l.close, pending ? "Cancel" : "Close", 0);
    button(c, l.apply,
           pending == 2   ? "Discard changes"
           : pending == 1 ? "Apply & restart"
                          : "Apply changes",
           changed() || pending);
}
static int needs_restart(void)
{
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (ranks[i] != Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0)))
            return 1;
        /* A live mod whose requirement is applied at the next launch waits
         * for that launch too (Mods_WaitsForRestart). */
        if (wanted[i] != Mods_Enabled(i) &&
            (Mods_RequiresRestart(i) || (wanted[i] && Mods_WaitsForRestart(i, wanted) >= 0)))
            return 1;
        for (int j = 0; j < counts[i]; j++)
            if (values[i][j] != Mods_OptionValue(i, j) &&
                (Mods_RequiresRestart(i) || Json_Bool(Json_Member(Mods_Option(i, j), "restart"), 0)))
                return 1;
    }
    return 0;
}
static void apply(void)
{
    int restart = needs_restart(), old_ranks[MODS_MAX], *old_values[MODS_MAX] = {0};
    if (!changed()) {
        snprintf(status, sizeof(status), "No pending changes");
        return;
    }
    if (restart && pending != 1) {
        pending = 1;
        snprintf(status, sizeof(status),
                 "Applying these changes restarts the game. Unsaved game progress "
                 "will be lost.");
        return;
    }
    pending = 0;
    if (!Mods_Validate(wanted, status, sizeof(status)))
        return;
    for (int i = 0; i < Mods_Count(); i++)
        for (int j = 0; j < counts[i]; j++) {
            if (!Mods_Failed(i) && !Mods_OptionValid(i, j, values[i][j])) {
                snprintf(status, sizeof(status), "Invalid setting: %s / %s", Mods_Name(i),
                         str(Mods_Option(i, j), "key", ""));
                return;
            }
        }
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        old_ranks[i] = Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0));
        old_values[i] = calloc((size_t)max(1, counts[i]), sizeof(int));
        if (!old_values[i]) {
            for (int j = 0; j < i; j++)
                free(old_values[j]);
            snprintf(status, sizeof(status), "Could not prepare settings");
            return;
        }
        for (int j = 0; j < counts[i]; j++)
            old_values[i][j] = Mods_OptionValue(i, j);
    }
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (ranks[i] != old_ranks[i]) /* untouched mods keep following their manifest */
            Settings_SetNamed(key, ranks[i]);
        for (int j = 0; j < counts[i]; j++)
            Mods_OptionSet(i, j, values[i][j]);
    }
    if (!Mods_Apply(wanted, status, sizeof(status))) {
        for (int i = 0; i < Mods_Count(); i++) {
            char key[160];
            snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
            if (ranks[i] != old_ranks[i])
                Settings_SetNamed(key, old_ranks[i]);
            for (int j = 0; j < counts[i]; j++)
                Mods_OptionSet(i, j, old_values[i][j]);
        }
        Settings_Save(); /* Also restore options if activation failed after persistence. */
    } else {
        for (int i = 0; i < Mods_Count(); i++)
            for (int j = 0; j < counts[i]; j++)
                if (values[i][j] != old_values[i][j]) {
                    MemoriesModEvent event = {MEMORIES_EVENT_SETTINGS, MEMORIES_AFTER, i, j, values[i][j], 0, 0};
                    Mods_Dispatch(&event);
                }
        snprintf(status, sizeof(status), "Changes applied.");
        if (restart && Platform_RestartGame() < 0)
            snprintf(status, sizeof(status),
                     "Settings saved. Restart failed; relaunch the game to finish "
                     "applying them.");
    }
    for (int i = 0; i < Mods_Count(); i++)
        free(old_values[i]);
}
static void select_step(int step)
{
    int at = -1, n = shown_count();
    if (!n)
        return;
    for (int i = 0; i < n; i++)
        if (shown(i) == selected)
            at = i;
    at = (at + step + n) % n;
    selected = shown(at);
    detail_scroll = 0;
    slider_drag = -1; /* a held slider belonged to the previous mod */
    if (at < scroll)
        scroll = at;
    if (at >= scroll + rows())
        scroll = at - rows() + 1;
}
static void adjust(int option, int direction)
{
    const JsonValue *spec = Mods_Option(selected, option);
    const char *type = str(spec, "type", "int");
    int low = num(spec, "min", 0), high = num(spec, "max", 100), step = max(1, num(spec, "step", 1)),
        value = values[selected][option];
    if (!strcmp(type, "bool"))
        value = !value;
    else if (!strcmp(type, "key")) {
        int bit = 0;
        while (bit < 16 && value != (1 << bit))
            bit++;
        bit = (bit + direction + 17) % 17;
        value = bit == 16 ? 0 : 1 << bit;
    } else {
        if (!strcmp(type, "choice")) {
            low = 0;
            high = Json_Count(Json_Member(spec, "choices")) - 1;
            step = 1;
        }
        int64_t next = (int64_t)value + (direction > 0 ? step : -step);
        value = next > high ? high : next < low ? low : (int)next;
    }
    values[selected][option] = value;
}
static void slider_value(int option, int x)
{
    Layout l;
    layout(&l);
    const JsonValue *spec = Mods_Option(selected, option);
    int low = num(spec, "min", 0), high = num(spec, "max", 100), step = max(1, num(spec, "step", 1));
    int span = body_width(&l) - 10 * unit, at = x - l.view.x - 5 * unit;
    if (span <= 0 || high <= low)
        return;
    at = min(max(at, 0), span);
    int64_t value = low + ((int64_t)high - low) * at / span;
    value = low + ((value - low + step / 2) / step) * step;
    if (value > high)
        value = high;
    values[selected][option] = (int)value;
}
/* Pointer at `y` on a held scrollbar: the thumb follows it. */
static void bar_move(const Layout *l, int y)
{
    if (bar_drag == 1)
        scroll = thumb_to(l->list_bar, shown_count(), rows(), y - bar_grab);
    else if (bar_drag == 2)
        detail_scroll = thumb_to(l->bar, body_height(l), l->view.h, y - bar_grab);
}
/* Pressing a scrollbar grabs its thumb, or centres the thumb on the pointer. */
static int bar_press(const Layout *l, int which, int x, int y)
{
    Rect track = which == 1 ? l->list_bar : l->bar, t;
    int total = which == 1 ? shown_count() : body_height(l), shown = which == 1 ? rows() : l->view.h,
        at = which == 1 ? scroll : detail_scroll;
    if (total <= shown || !inside(rect(track.x - 3 * unit, track.y, track.w + 6 * unit, track.h), x, y))
        return 0;
    t = thumb(track, total, shown, at);
    bar_drag = which;
    bar_grab = inside(rect(track.x - 3 * unit, t.y, track.w + 6 * unit, t.h), x, y) ? y - t.y : t.h / 2;
    bar_move(l, y);
    return 1;
}
/* A press on the settings list: the - value + buttons or an int's slider. */
static void option_press(const Layout *l, int x, int y)
{
    int w = body_width(l), top = -detail_scroll;
    x -= l->view.x;
    y -= l->view.y;
    for (int j = 0; j < counts[selected]; j++) {
        OptionBox o;
        option(NULL, j, w, top + 10 * unit, &o);
        top += 10 * unit + o.height;
        if (y >= top)
            continue;
        if (inside(o.minus, x, y))
            adjust(j, -1);
        else if (inside(o.plus, x, y) || inside(o.value, x, y))
            adjust(j, 1);
        else if (o.slider.h &&
                 inside(rect(o.slider.x, o.slider.y - 4 * unit, o.slider.w, o.slider.h + 8 * unit), x, y)) {
            slider_drag = j;
            slider_value(j, x + l->view.x);
        }
        return;
    }
}
int ModsWindow_Redraws(const MenuEvent *e)
{
    return e->type != MENU_EVENT_MOTION || slider_drag >= 0 || bar_drag;
}
int ModsWindow_RequestClose(void)
{
    focus = 0;
    slider_drag = -1;
    bar_drag = 0;
    if (pending == 2 || !changed())
        return 1;
    pending = 2;
    snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
    return 0;
}
int ModsWindow_Event(const MenuEvent *e)
{
    Layout l;
    layout(&l);
    if (e->type == MENU_EVENT_BUTTON_UP || e->type == MENU_EVENT_LEAVE)
        slider_drag = -1, bar_drag = 0;
    if (e->type == MENU_EVENT_MOTION && bar_drag) {
        bar_move(&l, e->y);
        return 0;
    }
    if (e->type == MENU_EVENT_MOTION && slider_drag >= 0 && selected >= 0 && !pending && tab == 1 &&
        slider_drag < counts[selected]) {
        slider_value(slider_drag, e->x);
        return 0;
    }
    if (e->type == MENU_EVENT_KEY_DOWN) {
        if (e->key == MENU_KEY_ESCAPE) {
            if (pending) {
                pending = 0;
                status[0] = 0;
                return 0;
            }
            if (focus) {
                focus = 0;
                return 0;
            }
            if (changed()) {
                pending = 2;
                snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
                return 0;
            }
            return 1;
        }
        if (e->key == MENU_KEY_TAB) {
            focus = (focus + 1) % 3;
            return 0;
        }
        if (focus) {
            char *target = focus == 1 ? query : profile;
            size_t cap = focus == 1 ? sizeof(query) : sizeof(profile), n = strlen(target);
            if (e->key == MENU_KEY_BACKSPACE && n)
                target[n - 1] = 0;
            if (e->text[0] && n + strlen(e->text) < cap)
                strcat(target, e->text);
            scroll = 0;
            return 0;
        }
        if (pending) {
            if (e->key == MENU_KEY_ENTER) {
                if (pending == 2)
                    return 1;
                apply();
            }
            return 0;
        }
        if (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN)
            select_step(e->key == MENU_KEY_DOWN ? 1 : -1);
        if (selected >= 0 && (e->key == MENU_KEY_LEFT || e->key == MENU_KEY_RIGHT || e->key == MENU_KEY_ENTER))
            wanted[selected] = e->key == MENU_KEY_LEFT ? 0 : e->key == MENU_KEY_RIGHT ? 1 : !wanted[selected];
    } else if (e->type == MENU_EVENT_TEXT && focus) {
        char *target = focus == 1 ? query : profile;
        size_t cap = focus == 1 ? sizeof(query) : sizeof(profile);
        if (strlen(target) + strlen(e->text) < cap)
            strcat(target, e->text);
        scroll = 0;
    } else if (e->type == MENU_EVENT_WHEEL && !pending) {
        if (inside(l.list, e->x, e->y)) {
            scroll -= e->wheel * 3;
            scroll = max(0, scroll);
            if (scroll > max(0, shown_count() - rows()))
                scroll = max(0, shown_count() - rows());
        } else if (inside(l.detail, e->x, e->y) && selected >= 0)
            detail_scroll = min(max(0, detail_scroll - e->wheel * 3 * LINE), detail_limit(&l));
    } else if (e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1) {
        if (inside(l.close, e->x, e->y)) {
            if (pending) {
                pending = 0;
                status[0] = 0;
            } else if (changed()) {
                pending = 2;
                snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
            } else
                return 1;
        } else if (inside(l.apply, e->x, e->y)) {
            if (pending == 2)
                return 1;
            apply();
        } else if (!pending && inside(l.folder, e->x, e->y)) {
            char path[1024];
            if (Mods_InstallDirectory(path, sizeof(path)))
                snprintf(status, sizeof(status), "Could not create the mods folder.");
            else if (Platform_OpenFolder(path))
                snprintf(status, sizeof(status), "Could not open %.400s", path);
            else
                snprintf(status, sizeof(status), "Opened %.400s. New mods appear after a restart.", path);
        } else if (!pending) {
            focus = inside(l.search, e->x, e->y) ? 1 : inside(l.profile, e->x, e->y) ? 2 : 0;
            if (inside(l.filter, e->x, e->y)) {
                filter = (filter + 1) % 4;
                scroll = 0;
            }
            if (bar_press(&l, 1, e->x, e->y) || (selected >= 0 && bar_press(&l, 2, e->x, e->y)))
                return 0;
            if (inside(l.list, e->x, e->y) && (e->y - l.list.y) / (58 * unit) < rows()) {
                int mod = shown(scroll + (e->y - l.list.y) / (58 * unit));
                if (mod >= 0) {
                    selected = mod;
                    detail_scroll = 0;
                    if (e->x < l.list.x + 40 * unit)
                        wanted[mod] = !wanted[mod];
                }
            }
            if (inside(l.save, e->x, e->y))
                snprintf(status, sizeof(status), "%s",
                         changed()                   ? "Apply your changes before saving a profile."
                         : Mods_ProfileSave(profile) ? "Profile saved."
                                                     : "Could not save profile; use a simple name.");
            if (inside(l.load, e->x, e->y)) {
                if (Mods_ProfileRead(profile, wanted)) {
                    for (int i = 0; i < Mods_Count(); i++) {
                        char key[256];
                        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
                        ranks[i] = Mods_ProfileValue(profile, key, num(Mods_Manifest(i), "priority", 0));
                        for (int j = 0; j < counts[i]; j++) {
                            snprintf(key, sizeof(key), "mod.%s.%s", Mods_Id(i), str(Mods_Option(i, j), "key", ""));
                            values[i][j] = Mods_ProfileValue(profile, key, num(Mods_Option(i, j), "default", 0));
                        }
                    }
                    snprintf(status, sizeof(status), "Profile loaded for review. Apply changes when ready.");
                } else
                    snprintf(status, sizeof(status), "Profile missing or references unavailable mods.");
            }
            if (selected >= 0) {
                if (inside(l.toggle, e->x, e->y))
                    wanted[selected] = !wanted[selected];
                for (int i = 0; i < 3; i++)
                    if (inside(l.tabs[i], e->x, e->y)) {
                        tab = i;
                        detail_scroll = 0;
                    }
                if (inside(l.order[0], e->x, e->y) && ranks[selected] > -100000)
                    ranks[selected]--;
                if (inside(l.order[1], e->x, e->y) && ranks[selected] < 100000)
                    ranks[selected]++;
                if (tab == 1 && counts[selected]) {
                    if (inside(l.defaults, e->x, e->y))
                        for (int j = 0; j < counts[selected]; j++)
                            values[selected][j] = num(Mods_Option(selected, j), "default", 0);
                    else if (inside(l.view, e->x, e->y))
                        option_press(&l, e->x, e->y);
                }
            }
        }
    }
    return 0;
}
