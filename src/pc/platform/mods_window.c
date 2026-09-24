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
    Rect search, filter, list, detail, toggle, tabs[3], apply, close, profile, save, load, order[2], defaults;
} Layout;
static int width, height, unit, selected, scroll, detail_scroll, tab, filter, focus, pending;
static int wanted[MODS_MAX], ranks[MODS_MAX], *values[MODS_MAX], counts[MODS_MAX];
static int slider_drag = -1;
static char query[96], profile[65] = "Default", status[512];
static const char *filters[] = {"All mods", "Enabled", "Disabled", "Issues"};
static const char *pad_names[] = {"Select", "L3", "R3", "Start", "Up",       "Right",  "Down",  "Left",
                                  "L2",     "R2", "L1", "R1",    "Triangle", "Circle", "Cross", "Square"};
static int width_text(const char *s) { return Menu_TextWidthScaled(s, unit); }
static Rect rect(int x, int y, int w, int h)
{
    Rect r = {x, y, w, h};
    return r;
}
static int inside(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int max(int a, int b) { return a > b ? a : b; }
static void layout(Layout *l)
{
    int p = 20 * unit, gap = 16 * unit, left = width * 36 / 100, footer = height - 72 * unit, top = 142 * unit;
    l->search = rect(p, 64 * unit, left - p, 32 * unit);
    l->filter = rect(p, 104 * unit, left - p, 28 * unit);
    l->list = rect(p, top, left - p, footer - top - 12 * unit);
    l->detail = rect(left + gap, top, width - left - gap - p, footer - top - 12 * unit);
    l->profile = rect(left + gap, 64 * unit, max(80 * unit, width - left - gap - p - 160 * unit), 32 * unit);
    l->save = rect(width - p - 152 * unit, 64 * unit, 72 * unit, 32 * unit);
    l->load = rect(width - p - 72 * unit, 64 * unit, 72 * unit, 32 * unit);
    l->toggle = rect(l->detail.x + 16 * unit, top + 74 * unit, 130 * unit, 30 * unit);
    for (int i = 0; i < 3; i++)
        l->tabs[i] = rect(l->detail.x + i * l->detail.w / 3, top + 120 * unit, l->detail.w / 3, 32 * unit);
    l->apply = rect(width - p - 150 * unit, height - 48 * unit, 150 * unit, 30 * unit);
    l->close = rect(width - p - 252 * unit, height - 48 * unit, 94 * unit, 30 * unit);
    l->order[0] = rect(l->detail.x + l->detail.w - 88 * unit, top + 74 * unit, 30 * unit, 30 * unit);
    l->order[1] = rect(l->detail.x + l->detail.w - 48 * unit, top + 74 * unit, 30 * unit, 30 * unit);
    l->defaults = rect(l->detail.x + 16 * unit, top + 164 * unit, l->detail.w - 32 * unit, 28 * unit);
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
    if (w <= 0)
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
static int wrap(MenuCanvas *c, Rect bounds, int y, const char *s, unsigned colour)
{
    char line[512];
    int n = 0;
    while (*s) {
        line[n++] = *s++;
        line[n] = 0;
        if (n >= 500 || *s == '\n' || !*s || width_text(line) > bounds.w - 16 * unit) {
            if (width_text(line) > bounds.w - 16 * unit && n > 1) {
                int split = n - 1;
                while (split > 0 && line[split] != ' ')
                    split--;
                if (!split)
                    split = n - 1;
                s -= n - split;
                n = split;
                line[n] = 0;
            }
            if (y >= bounds.y + 8 * unit && y < bounds.y + bounds.h - 8 * unit)
                text(c, bounds.x + 8 * unit, y, bounds.w - 16 * unit, line, colour);
            y += 22 * unit;
            n = 0;
            while (*s == ' ' || *s == '\n')
                s++;
        }
    }
    return y;
}
static Rect content(const Layout *l)
{
    return rect(l->detail.x + 8 * unit, l->detail.y + 158 * unit, l->detail.w - 16 * unit, l->detail.h - 166 * unit);
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
void ModsWindow_Draw(MenuCanvas *c)
{
    Layout l;
    char line[512];
    int enabled = 0;
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
    for (int r = 0; r < rows(); r++) {
        int mod = shown(scroll + r);
        Rect row = rect(l.list.x, l.list.y + r * 58 * unit, l.list.w, 56 * unit);
        if (mod < 0)
            break;
        if (mod == selected)
            fill(c, row, 0x293e60u);
        text(c, row.x + 12 * unit, row.y + 18 * unit, 24 * unit, wanted[mod] ? "[x]" : "[ ]",
             wanted[mod] ? GREEN : DIM);
        text(c, row.x + 44 * unit, row.y + 18 * unit, row.w - 54 * unit, Mods_Name(mod), TEXT);
        snprintf(line, sizeof(line), "%s%s%s",
                 Mods_Failed(mod)      ? "Error"
                 : Mods_Status(mod)[0] ? "Warning"
                 : Mods_Active(mod)    ? "Active"
                                       : "Inactive",
                 wanted[mod] != Mods_Active(mod) ? " / pending" : "", Mods_RequiresRestart(mod) ? " / restart" : "");
        text(c, row.x + 44 * unit, row.y + 39 * unit, row.w - 54 * unit, line, Mods_Failed(mod) ? RED : DIM);
    }
    if (!shown_count())
        text(c, l.list.x + 16 * unit, l.list.y + 32 * unit, l.list.w - 32 * unit, "No matching mods", DIM);
    if (shown_count() > rows()) {
        int track = l.list.h, thumb = max(16 * unit, track * rows() / shown_count());
        fill(c,
             rect(l.list.x + l.list.w - 4 * unit, l.list.y + scroll * (track - thumb) / max(1, shown_count() - rows()),
                  3 * unit, thumb),
             ACCENT);
    }
    if (selected >= 0) {
        Rect body = content(&l);
        int y = body.y + 18 * unit - detail_scroll * 22 * unit;
        text(c, l.detail.x + 16 * unit, l.detail.y + 24 * unit, l.detail.w - 32 * unit, Mods_Name(selected), TEXT);
        snprintf(line, sizeof(line), "%s  /  %s  /  %s", Mods_Metadata(selected, "version"),
                 Mods_Metadata(selected, "author"), Mods_Origin(selected));
        text(c, l.detail.x + 16 * unit, l.detail.y + 49 * unit, l.detail.w - 32 * unit, line, DIM);
        button(c, l.toggle, wanted[selected] ? "Enabled" : "Disabled", wanted[selected]);
        snprintf(line, sizeof(line), "Order %d", ranks[selected]);
        text(c, l.toggle.x + l.toggle.w + 12 * unit, l.toggle.y + 15 * unit,
             l.order[0].x - l.toggle.x - l.toggle.w - 20 * unit, line, DIM);
        button(c, l.order[0], "-", 0);
        button(c, l.order[1], "+", 0);
        button(c, l.tabs[0], "About", tab == 0);
        button(c, l.tabs[1], "Settings", tab == 1);
        button(c, l.tabs[2], "Compatibility", tab == 2);
        if (tab == 0) {
            y = wrap(c, body, y, Mods_Metadata(selected, "description"), TEXT) + 16 * unit;
            snprintf(line, sizeof(line), "ID: %s", Mods_Id(selected));
            y = wrap(c, body, y, line, DIM);
            y = wrap(c, body, y, Mods_Directory(selected), DIM) + 16 * unit;
            y = wrap(c, body, y,
                     *Mods_Metadata(selected, "library") ? "Native code mod: runs game code from this author."
                                                         : "Content mod: assets, cards or data patches.",
                     DIM);
            if (Mods_Status(selected)[0])
                wrap(c, body, y + 16 * unit, Mods_Status(selected), Mods_Failed(selected) ? RED : WARN);
        } else if (tab == 1) {
            int first = detail_scroll, visible_rows = max(1, (body.h - 44 * unit) / (66 * unit));
            button(c, l.defaults, "Restore defaults", 0);
            for (int r = 0; r < visible_rows; r++) {
                int option = first + r;
                const JsonValue *spec;
                Rect row;
                if (option >= counts[selected])
                    break;
                spec = Mods_Option(selected, option);
                row = rect(body.x + 8 * unit, body.y + 44 * unit + r * 66 * unit, body.w - 16 * unit, 60 * unit);
                text(c, row.x, row.y + 10 * unit, row.w - 150 * unit, str(spec, "label", str(spec, "key", "Setting")),
                     TEXT);
                option_label(selected, option, line, sizeof(line));
                button(c, rect(row.x + row.w - 146 * unit, row.y, 28 * unit, 26 * unit), "-", 0);
                text(c, row.x + row.w - 110 * unit, row.y + 13 * unit, 76 * unit, line, BLUE);
                button(c, rect(row.x + row.w - 28 * unit, row.y, 28 * unit, 26 * unit), "+", 0);
                text(c, row.x, row.y + 36 * unit, row.w,
                     str(spec, "description",
                         Json_Bool(Json_Member(spec, "restart"), 0) ? "Requires restart"
                                                                    : "Applies when changes are saved"),
                     DIM);
                if (!strcmp(str(spec, "type", "int"), "int")) {
                    int low = num(spec, "min", 0), high = num(spec, "max", 100);
                    int position = high > low ? (int)(((int64_t)values[selected][option] - low) * (row.w - 8 * unit) /
                                                      ((int64_t)high - low))
                                              : 0;
                    if (position < 0)
                        position = 0;
                    if (position > row.w - 8 * unit)
                        position = row.w - 8 * unit;
                    fill(c, rect(row.x, row.y + 51 * unit, row.w, 3 * unit), EDGE);
                    fill(c, rect(row.x, row.y + 51 * unit, position, 3 * unit), ACCENT);
                    fill(c, rect(row.x + position, row.y + 48 * unit, 8 * unit, 9 * unit), BLUE);
                } else
                    fill(c, rect(row.x, row.y + 56 * unit, row.w, 1), EDGE);
            }
            if (!counts[selected])
                wrap(c, body, body.y + 70 * unit, "This mod does not declare configurable settings.", DIM);
        } else {
            const char *keys[] = {"requires", "after", "conflicts"};
            const char *labels[] = {"Requires: ", "Load after: ", "Conflicts: "};
            y = wrap(c, body, y,
                     Mods_RequiresRestart(selected) ? "Changes require a restart." : "This mod supports live changes.",
                     DIM) +
                12 * unit;
            for (int k = 0; k < 3; k++) {
                const JsonValue *list = Json_Member(Mods_Manifest(selected), keys[k]);
                for (int j = 0; j < Json_Count(list); j++) {
                    const JsonValue *v = Json_At(list, j);
                    snprintf(line, sizeof(line), "%s%s", labels[k], Json_String(v, str(v, "id", "")));
                    y = wrap(c, body, y, line, TEXT);
                }
            }
            if (!Mods_Validate(wanted, line, sizeof(line)))
                y = wrap(c, body, y + 12 * unit, line, WARN);
            else
                y = wrap(c, body, y + 12 * unit, "Dependencies and declared conflicts are satisfied.", GREEN);
            if (Mods_ConflictText(selected, line, sizeof(line)))
                wrap(c, body, y + 12 * unit, line, WARN);
        }
    } else
        text(c, l.detail.x + 20 * unit, l.detail.y + 30 * unit, l.detail.w - 40 * unit,
             "Select a mod to view its details", DIM);
    fill(c, rect(0, height - 72 * unit, width, 1), EDGE);
    text(c, 20 * unit, height - 59 * unit, width - 40 * unit,
         *status ? status : "Changes are staged. Apply once when you are ready.", pending ? WARN : DIM);
    text(c, 20 * unit, height - 32 * unit, width - 320 * unit,
         "Arrow keys: select / toggle   Tab: search   Mouse wheel: scroll", DIM);
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
        if (wanted[i] != Mods_Enabled(i) && Mods_RequiresRestart(i))
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
        Settings_SetNamed(key, ranks[i]);
        for (int j = 0; j < counts[i]; j++)
            Mods_OptionSet(i, j, values[i][j]);
    }
    if (!Mods_Apply(wanted, status, sizeof(status))) {
        for (int i = 0; i < Mods_Count(); i++) {
            char key[160];
            snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
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
    Rect body = content(&l);
    const JsonValue *spec = Mods_Option(selected, option);
    int low = num(spec, "min", 0), high = num(spec, "max", 100), step = max(1, num(spec, "step", 1));
    int span = body.w - 24 * unit, at = x - body.x - 8 * unit;
    if (span <= 0 || high <= low)
        return;
    if (at < 0)
        at = 0;
    if (at > span)
        at = span;
    int64_t value = low + ((int64_t)high - low) * at / span;
    value = low + ((value - low + step / 2) / step) * step;
    if (value > high)
        value = high;
    values[selected][option] = (int)value;
}
int ModsWindow_Event(const MenuEvent *e)
{
    Layout l;
    layout(&l);
    if (e->type == MENU_EVENT_BUTTON_UP || e->type == MENU_EVENT_LEAVE)
        slider_drag = -1;
    if (e->type == MENU_EVENT_MOTION && slider_drag >= 0 && selected >= 0 && !pending) {
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
        } else if (inside(l.detail, e->x, e->y)) {
            detail_scroll = max(0, detail_scroll - e->wheel);
            if (tab == 1 && selected >= 0 && detail_scroll >= counts[selected])
                detail_scroll = max(0, counts[selected] - 1);
            if (detail_scroll > 100)
                detail_scroll = 100;
        }
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
        } else if (!pending) {
            focus = inside(l.search, e->x, e->y) ? 1 : inside(l.profile, e->x, e->y) ? 2 : 0;
            if (inside(l.filter, e->x, e->y)) {
                filter = (filter + 1) % 4;
                scroll = 0;
            }
            if (inside(l.list, e->x, e->y)) {
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
                Rect body = content(&l);
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
                if (tab == 1) {
                    if (inside(l.defaults, e->x, e->y))
                        for (int j = 0; j < counts[selected]; j++)
                            values[selected][j] = num(Mods_Option(selected, j), "default", 0);
                    else if (inside(body, e->x, e->y) && e->y >= body.y + 44 * unit) {
                        int row = (e->y - body.y - 44 * unit) / (66 * unit), option = detail_scroll + row;
                        if (option < counts[selected]) {
                            int y = (e->y - body.y - 44 * unit) % (66 * unit);
                            if (y >= 46 * unit && !strcmp(str(Mods_Option(selected, option), "type", "int"), "int")) {
                                slider_drag = option;
                                slider_value(option, e->x);
                            } else if (y < 28 * unit && e->x >= body.x + body.w - 154 * unit)
                                adjust(option, e->x < body.x + body.w - 100 * unit ? -1 : 1);
                        }
                    }
                }
            }
        }
    }
    return 0;
}
