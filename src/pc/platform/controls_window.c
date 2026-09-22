/* The Controls window: a header with the player and the Keyboard/Controller
 * tabs, the device picker, a PlayStation pad picture beside the binding
 * table, and the Restore/Cancel/Apply/OK footer.
 *
 * Drawing and hit-testing are one pass: every widget paints itself and
 * registers its rectangle, so the two can never disagree. Coordinates are
 * logical units multiplied by `scale`, which drops towards 1 when the window
 * is too small for the chosen menu scale; the window is resizable, so the
 * layout also reflows (the picture drops out, the table scrolls) rather than
 * assuming its default size. Colours follow menu.c so the game menu, the Mods
 * window and this one look like one piece of UI.
 *
 * The table lists the 16 PlayStation pad destinations in a reading order
 * (D-pad, face buttons, shoulders, stick clicks, system) with group headings;
 * that order is presentation only. Destination indices, the stored file and
 * every id below still use the wire-bit order of Controls_Actions.
 * Main thread only. */
#include "controls_window.h"
#include "controls_art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Short names for the public widget ids (controls_window.h). */
enum {
    PLAYER = CONTROLS_UI_PLAYER_1,
    PLAYER_2 = CONTROLS_UI_PLAYER_2,
    DEVICE = CONTROLS_UI_DEVICE,
    KEYBOARD = CONTROLS_UI_KEYBOARD,
    CONTROLLER = CONTROLS_UI_CONTROLLER,
    REBIND = CONTROLS_UI_REBIND,
    CLEAR = CONTROLS_UI_CLEAR,
    DEFAULTS = CONTROLS_UI_DEFAULTS,
    CANCEL = CONTROLS_UI_CANCEL,
    APPLY = CONTROLS_UI_APPLY,
    OK = CONTROLS_UI_OK,
    YES = CONTROLS_UI_YES,
    NO = CONTROLS_UI_NO,
    KEEP = CONTROLS_UI_KEEP,
    ROW = CONTROLS_UI_BINDING,  /* ROW + destination * 2 + slot */
    DIAGRAM = CONTROLS_UI_PAD,  /* DIAGRAM + destination */
    CHOICE = CONTROLS_UI_CHOICE /* CHOICE + device-list entry */
};

/* Logical metrics (before `scale`). */
enum {
    PAD = 18,
    GAP = 16,
    HEADER_H = 74,
    ROW_H = 22,
    HEAD_H = 24,
    BTN_H = 30,
    FOOTER_H = 54,
    NAME_W = 106,
    SLOT_W = 172,
    KEY_W = 224,
    POPUP_ROW = 26,
    POPUP_ROWS = 8,
    DIAGRAM_MIN = 250,
    DIAGRAM_MAX = 520,
    MIN_W = 560,
    MIN_H = 440,
    SIZE_W = 940,
    SIZE_H = 780
};

/* Status severity: picks the colour of the message line. */
enum { SAY_INFO, SAY_DONE, SAY_WARN, SAY_BUSY };

typedef struct {
    int x, y, w, h;
} Rect;
typedef struct {
    int x, y, w, h, id;
} Hit;

static struct {
    ControlsConfig draft;
    int player, tab, row, slot, scroll, scroll_max, lines_shown, focus, hover, close, modal, popup,
        popup_scroll, popup_max, width, height;
    int count, mods, selected_device, draw_scale, say;
    int click_id, click_x, click_y;
    uint64_t click_time;
    Hit hits[320];
    ControlCapture capture;
    ControlsEvaluator preview;
    char status[256];
} ui;
static MenuCanvas *canvas;
static int scale;
/* Where the last draw put each pad button (the drawn shape, not its larger
 * mouse target); ControlsWindow_Locate reports these for the picture. */
static Rect pad_rect[CTRL_DEST_COUNT];
static Rect device_drop; /* set while drawing; the device list hangs off it */

static const uint32_t bg = 0xff1e1f22, panel = 0xff27282c, raised = 0xff34363b, hot = 0xff42454d,
                      sunken = 0xff232427, edge = 0xff4a4d55, edge_off = 0xff2f3136, text = 0xffe8e8ea,
                      dim = 0xff9a9ca3, faint = 0xff6c6e76, off = 0xff55575e,
                      accent = 0xff3b82f6, accent_hot = 0xff5a97f8, on_accent = 0xffffffff,
                      held = 0xff2f7d55, held_cell = 0xff26603f, on_held = 0xffeafff3,
                      good = 0xff4cc38a, warn = 0xfff5b642,
                      /* The pad picture's selection ring: bright grey-green,
                       * clear of both the cyan artwork and the pressed fill. */
                      marker = 0xff8ccda0;

/* ------------------------------------------------------------------ */
/*  Reading order for the table                                        */
/* ------------------------------------------------------------------ */

static const int order[CTRL_DEST_COUNT] = {4,  6,  7,  5,  /* Up Down Left Right */
                                           12, 13, 14, 15, /* Triangle Circle Cross Square */
                                           10, 11, 8,  9,  /* L1 R1 L2 R2 */
                                           1,  2,          /* L3 R3 */
                                           3,  0};         /* Start Select */
static const struct {
    const char *title;
    int first, count;
} groups[] = {{"D-pad", 0, 4},
              {"Face buttons", 4, 4},
              {"Shoulders and triggers", 8, 4},
              {"Stick clicks", 12, 2},
              {"System", 14, 2}};
#define GROUP_COUNT ((int)(sizeof(groups) / sizeof(groups[0])))
#define LINE_COUNT (CTRL_DEST_COUNT + GROUP_COUNT)

/* Table line -> destination, or -1 for a group heading. */
static int line_action(int line, int *group_out)
{
    int at = 0;
    for (int g = 0; g < GROUP_COUNT; g++) {
        if (line == at) {
            if (group_out)
                *group_out = g;
            return -1;
        }
        at++;
        if (line < at + groups[g].count) {
            if (group_out)
                *group_out = g;
            return order[groups[g].first + line - at];
        }
        at += groups[g].count;
    }
    if (group_out)
        *group_out = 0;
    return -1;
}
static int action_line(int action)
{
    for (int line = 0; line < LINE_COUNT; line++)
        if (line_action(line, NULL) == action)
            return line;
    return 0;
}
static const char *action_group(int action)
{
    int group = 0;
    line_action(action_line(action), &group);
    return groups[group].title;
}

/* ------------------------------------------------------------------ */
/*  State helpers                                                      */
/* ------------------------------------------------------------------ */

static int dirty(void) { return !Controls_Equal(&ui.draft, ControlsRuntime_Config()); }
static ControlsProfile *profile(int create)
{
    return ui.tab ? ControlsRuntime_Profile(&ui.draft, ui.player, create) : &ui.draft.kb;
}
static int current_device(void) { return ControlsRuntime_Assigned(&ui.draft, ui.player); }
static void say(int kind, const char *s)
{
    ui.say = kind;
    snprintf(ui.status, sizeof(ui.status), "%s", s);
}

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

static Rect rect(int x, int y, int w, int h)
{
    Rect r = {x, y, w, h};
    return r;
}
static void fill(int x, int y, int w, int h, uint32_t color)
{
    x *= scale;
    y *= scale;
    w *= scale;
    h *= scale;
    for (int j = y < 0 ? 0 : y; j < y + h && j < canvas->height; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < canvas->width; i++)
            canvas->pixels[j * canvas->stride + i] = color;
}
static void box(int x, int y, int w, int h, uint32_t color)
{
    fill(x, y, w, 1, color);
    fill(x, y + h - 1, w, 1, color);
    fill(x, y, 1, h, color);
    fill(x + w - 1, y, 1, h, color);
}
static void frame(Rect r, uint32_t face, uint32_t border)
{
    fill(r.x, r.y, r.w, r.h, face);
    box(r.x, r.y, r.w, r.h, border);
}
/* The keyboard-focus ring sits just outside the widget it belongs to. */
static void ring(Rect r)
{
    box(r.x - 1, r.y - 1, r.w + 2, r.h + 2, accent);
    box(r.x - 2, r.y - 2, r.w + 4, r.h + 4, accent);
}
/* Darken what is already on the canvas: the scrim behind a dialog. */
static void shade(int x, int y, int w, int h)
{
    x *= scale;
    y *= scale;
    w *= scale;
    h *= scale;
    for (int j = y < 0 ? 0 : y; j < y + h && j < canvas->height; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < canvas->width; i++) {
            uint32_t p = canvas->pixels[j * canvas->stride + i];
            canvas->pixels[j * canvas->stride + i] = 0xff000000u | ((p >> 16 & 255) * 2 / 5) << 16 |
                                                     ((p >> 8 & 255) * 2 / 5) << 8 | (p & 255) * 2 / 5;
        }
}
static void ellipse(int cx, int cy, int rx, int ry, uint32_t color)
{
    if (rx < 1 || ry < 1)
        return;
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++)
            if ((long)x * x * ry * ry + (long)y * y * rx * rx <= (long)rx * rx * ry * ry)
                fill(cx + x, cy + y, 1, 1, color);
}
/* A dropdown's downward triangle; the font has no arrow glyphs. */
static void caret(int cx, int cy, uint32_t color)
{
    for (int i = 0; i < 4; i++)
        fill(cx - 3 + i, cy - 2 + i, 7 - 2 * i, 1, color);
}

static int text_w(const char *s) { return (Menu_TextWidthScaled(s, scale) + scale - 1) / scale; }
static void text_at(int x, int middle, const char *s, uint32_t color)
{
    Menu_DrawTextScaled(canvas, x * scale, middle * scale, s, color, scale);
}
static void text_clip(int x, int middle, const char *s, int width, uint32_t color)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    size_t n = strlen(buf);
    while (n && text_w(buf) > width)
        buf[--n] = 0;
    if (n < strlen(s) && n > 3)
        memcpy(buf + n - 3, "...", 3);
    text_at(x, middle, buf, color);
}
static void text_right(int right, int middle, const char *s, uint32_t color)
{
    text_at(right - text_w(s), middle, s, color);
}
static void text_right_clip(int right, int middle, const char *s, int width, uint32_t color)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    size_t n = strlen(buf);
    while (n && text_w(buf) > width)
        buf[--n] = 0;
    if (n < strlen(s) && n > 3)
        memcpy(buf + n - 3, "...", 3);
    text_right(right, middle, buf, color);
}
/* Word-wrapped body text for the dialogs. With `draw` clear it only counts
 * the lines, so a dialog can size itself before it paints. */
static int text_wrap(int x, int middle, const char *s, int width, uint32_t color, int max_lines, int draw)
{
    char line[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        size_t best = 0, at = 0;
        for (;;) {
            size_t end = at;
            while (s[end] == ' ')
                end++;
            while (s[end] && s[end] != ' ')
                end++;
            if (end == at || end >= sizeof(line))
                break;
            memcpy(line, s, end);
            line[end] = 0;
            if (best && text_w(line) > width)
                break;
            best = at = end;
            if (!s[end])
                break;
        }
        if (!best)
            best = strlen(s) < sizeof(line) - 1 ? strlen(s) : sizeof(line) - 1;
        memcpy(line, s, best);
        line[best] = 0;
        if (draw)
            text_clip(x, middle + lines * 18, line, width, color);
        s += best;
        while (*s == ' ')
            s++;
        lines++;
    }
    return lines;
}

static void hit(int id, Rect r)
{
    if (ui.count < (int)(sizeof(ui.hits) / sizeof(ui.hits[0])))
        ui.hits[ui.count++] = (Hit){r.x, r.y, r.w, r.h, id};
}

/* BTN_READY is a normal button with something waiting behind it: Apply while
 * the draft differs from the saved configuration. A disabled button loses its
 * raised face as well as its contrast, so the two cannot be confused. */
enum { BTN_NORMAL, BTN_PRIMARY, BTN_READY, BTN_QUIET };
static int button_w(const char *label) { return text_w(label) + 28; }
static void button(int id, Rect r, const char *label, int style)
{
    int hovered = ui.hover == id;
    uint32_t face = style == BTN_PRIMARY ? (hovered ? accent_hot : accent)
                    : style == BTN_QUIET ? sunken
                    : hovered             ? hot
                                          : raised;
    uint32_t ink = style == BTN_PRIMARY ? on_accent : style == BTN_QUIET ? off : text;
    frame(r, face,
          style == BTN_PRIMARY  ? accent_hot
          : style == BTN_READY  ? accent
          : style == BTN_QUIET  ? edge_off
                                : edge);
    text_clip(r.x + (r.w - text_w(label)) / 2, r.y + r.h / 2, label, r.w - 12, ink);
    if (ui.focus == id)
        ring(r);
    hit(id, r);
}

/* ------------------------------------------------------------------ */
/*  Capture and actions                                                */
/* ------------------------------------------------------------------ */

static void cancel_capture(void)
{
    ui.capture.state = CAP_IDLE;
    say(SAY_INFO, "Binding unchanged.");
}
static void apply(int close)
{
    if (ControlsRuntime_Apply(&ui.draft, ui.status, sizeof(ui.status))) {
        say(SAY_DONE, "Controls saved.");
        ui.modal = 0;
        ui.close = close;
    } else
        ui.say = SAY_WARN;
}
void ControlsWindow_RequestClose(void)
{
    if (ui.capture.state) {
        cancel_capture();
        return;
    }
    if (dirty()) {
        ui.modal = 1;
        ui.focus = YES;
    } else
        ui.close = 1;
}
void ControlsWindow_Init(void)
{
    memset(&ui, 0, sizeof(ui));
    ui.draft = *ControlsRuntime_Config();
    ui.focus = KEYBOARD;
    ui.row = order[0];
    ui.selected_device = -1;
    /* The message line starts empty: it reports what happened, and the hint
     * line above it already says how to edit a binding. */
    if (ControlsRuntime_Error()[0])
        say(SAY_WARN, ControlsRuntime_Error());
    ControlsRuntime_Block(1);
}
void ControlsWindow_Size(int *w, int *h)
{
    *w = SIZE_W * Menu_Scale();
    *h = SIZE_H * Menu_Scale();
}
void ControlsWindow_MinSize(int *w, int *h)
{
    *w = MIN_W;
    *h = MIN_H;
}
int ControlsWindow_ShouldClose(void) { return ui.close; }
int ControlsWindow_Locate(int id, int *x, int *y)
{
    int sc = ui.draw_scale ? ui.draw_scale : 1;
    if (id >= DIAGRAM && id < CHOICE && id - DIAGRAM < CTRL_DEST_COUNT) {
        Rect *r = &pad_rect[id - DIAGRAM];
        if (!r->w)
            return 0;
        *x = (r->x + r->w / 2) * sc;
        *y = (r->y + r->h / 2) * sc;
        return 1;
    }
    for (int i = ui.count - 1; i >= 0; i--) {
        if (ui.hits[i].id != id)
            continue;
        *x = (ui.hits[i].x + ui.hits[i].w / 2) * sc;
        *y = (ui.hits[i].y + ui.hits[i].h / 2) * sc;
        return 1;
    }
    return 0;
}
void ControlsWindow_FocusLost(void)
{
    ui.click_id = 0;
    ui.hover = 0;
    if (ui.capture.state)
        cancel_capture();
    ControlsRuntime_ResetKeys();
}
static void start_capture(void)
{
    ui.click_id = 0;
    if (!ui.tab && ui.player) {
        say(SAY_WARN, "The keyboard always plays as Player 1.");
        return;
    }
    if (ui.tab && ui.draft.profile_count == CTRL_PROFILE_MAX) {
        int found = 0, dev = current_device();
        for (int i = 0; i < ui.draft.profile_count; i++)
            if (dev >= 0 && !strcmp(ui.draft.profiles[i].identity, ControlsRuntime_Device(dev)->identity))
                found = 1;
        if (!found) {
            say(SAY_WARN, "Profile limit reached. Remove old profiles from controls.txt before adding more.");
            return;
        }
    }
    if (ui.tab && current_device() < 0) {
        say(SAY_WARN, "Connect a controller and choose it above before rebinding.");
        return;
    }
    ui.modal = 0;
    ui.popup = 0;
    Controls_CaptureBegin(&ui.capture, ui.row, ui.slot, ControlsRuntime_Now());
    say(SAY_BUSY, "Release everything you are holding, then press the input you want.");
}
/* Keep the selected line, and its heading where possible, inside the view. */
static void reveal_row(void)
{
    int line = action_line(ui.row);
    if (!ui.lines_shown)
        return;
    if (line - 1 < ui.scroll)
        ui.scroll = line - 1 < 0 ? 0 : line - 1;
    if (line >= ui.scroll + ui.lines_shown)
        ui.scroll = line - ui.lines_shown + 1;
}
static void select_row(int action, int slot)
{
    ui.row = action;
    ui.slot = slot;
    ui.focus = ROW + action * 2 + slot;
    reveal_row();
}
static void move_row(int delta)
{
    int at = 0;
    for (int i = 0; i < CTRL_DEST_COUNT; i++)
        if (order[i] == ui.row)
            at = i;
    select_row(order[(at + delta + CTRL_DEST_COUNT) % CTRL_DEST_COUNT], ui.slot);
}
static void activate(int id)
{
    if (id >= CHOICE) {
        int choice = id - CHOICE;
        if (choice == CONTROLS_DEVICES + 2) {
            ui.popup = 0;
            return;
        }
        if (ui.popup == DEVICE) {
            ControlsPort *p = &ui.draft.port[ui.player];
            if (choice < 2) {
                p->mode = choice == 0 ? 1 : 0;
                p->identity[0] = 0;
                say(SAY_INFO, choice == 0 ? "This player uses the first free controller."
                                          : "This player's controller is switched off.");
            } else {
                ControllerDevice *d = ControlsRuntime_Device(choice - 2);
                if (!d || !d->connected)
                    return;
                if (d->ambiguous)
                    say(SAY_WARN, "Two identical controllers: choose this one again after restarting the game.");
                ControlsPort *other = &ui.draft.port[1 - ui.player];
                if (other->mode == 2 && !strcmp(other->identity, d->identity)) {
                    say(SAY_WARN, "That controller belongs to the other player. Release it there first.");
                    ui.popup = 0;
                    return;
                }
                p->mode = 2;
                snprintf(p->identity, sizeof(p->identity), "%s", d->identity);
            }
        }
        ui.popup = 0;
        ui.capture.state = CAP_IDLE;
        memset(&ui.preview, 0, sizeof(ui.preview));
        return;
    }
    if (id >= ROW && id < DIAGRAM) {
        select_row((id - ROW) / 2, (id - ROW) % 2);
        return;
    }
    switch (id) {
    case PLAYER:
    case PLAYER_2: {
        int player = id == PLAYER ? 0 : 1;
        if (player == ui.player)
            break;
        ui.player = player;
        if (ui.player)
            ui.tab = 1;
        ui.capture.state = CAP_IDLE;
        ui.popup = 0;
        memset(&ui.preview, 0, sizeof(ui.preview));
        break;
    }
    case DEVICE:
        ui.popup = ui.popup == id ? 0 : id;
        ui.popup_scroll = 0;
        break;
    case KEYBOARD:
        if (ui.player) {
            say(SAY_WARN, "The keyboard always plays as Player 1. Switch to Player 1 to edit it.");
            break;
        }
        ui.tab = 0;
        ui.slot = 0;
        ui.capture.state = CAP_IDLE;
        break;
    case CONTROLLER:
        ui.tab = 1;
        ui.capture.state = CAP_IDLE;
        break;
    case REBIND:
        if (ui.capture.state)
            cancel_capture();
        else
            start_capture();
        break;
    case CLEAR:
        if (profile(0)->src[ui.row][ui.slot].kind == CTRL_SRC_UNBOUND) {
            say(SAY_INFO, "That slot is already empty.");
            break;
        }
        Controls_ClearSlot(profile(1), ui.row, ui.slot);
        say(SAY_INFO, "Binding cleared. Apply to save.");
        break;
    case DEFAULTS:
        ui.modal = 2;
        ui.focus = YES;
        break;
    case CANCEL:
        ui.close = 1;
        break;
    case APPLY:
        if (!dirty()) {
            say(SAY_INFO, "Nothing to save.");
            break;
        }
        apply(0);
        break;
    case OK:
        if (dirty())
            apply(1);
        else
            ui.close = 1;
        break;
    case YES:
        if (ui.modal == 1)
            apply(1);
        else if (ui.modal == 2) {
            ControlsConfig defaults;
            Controls_InitDefaults(&defaults);
            *profile(1) = ui.tab ? defaults.ctrl[ui.player] : defaults.kb;
            ui.modal = 0;
            say(SAY_INFO, "Defaults restored. Apply to save.");
        } else if (ui.modal == 3) {
            Controls_MoveSource(profile(1), ui.row, ui.slot, &ui.capture.pending);
            ui.capture.state = CAP_IDLE;
            ui.modal = 0;
            say(SAY_INFO, "Binding moved. Apply to save.");
        }
        break;
    case NO:
        if (ui.modal == 1)
            ui.close = 1;
        else {
            ui.modal = 0;
            cancel_capture();
        }
        break;
    case KEEP:
        ui.modal = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Events                                                             */
/* ------------------------------------------------------------------ */

static int hit_at(int x, int y)
{
    for (int i = ui.count - 1; i >= 0; i--) {
        Hit *h = &ui.hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
            return i;
    }
    return -1;
}
void ControlsWindow_Event(const MenuEvent *e)
{
    int sc = ui.draw_scale ? ui.draw_scale : 1;
    if (e->type == MENU_EVENT_LEAVE) {
        ui.hover = 0;
        return;
    }
    if (e->type == MENU_EVENT_MOTION) {
        int at = hit_at(e->x / sc, e->y / sc);
        ui.hover = at < 0 ? 0 : ui.hits[at].id;
        return;
    }
    if (e->type == MENU_EVENT_WHEEL) {
        ui.click_id = 0;
        int *offset = ui.popup ? &ui.popup_scroll : &ui.scroll;
        int maximum = ui.popup ? ui.popup_max : ui.scroll_max;
        *offset -= e->wheel;
        if (*offset > maximum)
            *offset = maximum;
        if (*offset < 0)
            *offset = 0;
        return;
    }
    if (e->type != MENU_EVENT_BUTTON_DOWN || e->button != 1)
        return;
    int x = e->x / sc, y = e->y / sc, at = hit_at(x, y);
    if (ui.capture.state && !ui.modal) {
        /* Any click during capture means "stop listening", hit or miss. */
        ui.click_id = 0;
        cancel_capture();
        return;
    }
    if (at < 0) {
        ui.click_id = 0;
        ui.popup = 0;
        return;
    }
    Hit *h = &ui.hits[at];
    uint64_t now = ControlsRuntime_Now();
    int control = h->id >= ROW && h->id < CHOICE;
    int double_click = control && ui.click_id == h->id && now - ui.click_time <= 400000 &&
                       abs(x - ui.click_x) <= 5 && abs(y - ui.click_y) <= 5;
    ui.click_id = control ? h->id : 0;
    ui.click_time = now;
    ui.click_x = x;
    ui.click_y = y;
    ui.focus = h->id;
    if (h->id >= DIAGRAM && h->id < CHOICE) {
        select_row(h->id - DIAGRAM, 0);
        say(SAY_INFO, Controls_Actions[ui.row].name);
        if (double_click)
            start_capture();
        return;
    }
    activate(h->id);
    if (double_click)
        start_capture();
}
/* Tab order: everything that was drawn, except the pad picture, which is a
 * mouse shortcut into the table rather than a separate control. */
static void focus_step(int delta)
{
    int at = -1;
    for (int i = 0; i < ui.count; i++)
        if (ui.hits[i].id == ui.focus) {
            at = i;
            break;
        }
    if (at < 0)
        at = delta > 0 ? -1 : 0;
    for (int i = 0; i < ui.count; i++) {
        at = (at + delta + ui.count) % ui.count;
        int id = ui.hits[at].id;
        if (id == ui.focus || (id >= DIAGRAM && id < CHOICE))
            continue;
        ui.focus = id;
        break;
    }
    if (ui.focus >= ROW && ui.focus < DIAGRAM)
        select_row((ui.focus - ROW) / 2, (ui.focus - ROW) % 2);
}
void ControlsWindow_Key(int key, int down, int repeat, int modifiers)
{
    ui.mods = modifiers;
    if (!down || repeat)
        return;
    ui.click_id = 0;
    if (key == CTRL_KEY_ESCAPE) {
        if (ui.modal) {
            ui.modal = 0;
            cancel_capture();
        } else if (ui.popup)
            ui.popup = 0;
        else
            ControlsWindow_RequestClose();
        return;
    }
    if (ui.capture.state) {
        if (!key)
            say(SAY_WARN, "That key is not supported. Choose a standard keyboard key.");
        return;
    }
    if (key == CTRL_KEY_TAB) {
        focus_step(modifiers & 1 ? -1 : 1);
    } else if (key == CTRL_KEY_ENTER || key == CTRL_KEY_SPACE) {
        if (ui.focus >= ROW && ui.focus < DIAGRAM)
            start_capture();
        else
            activate(ui.focus);
    } else if ((key == CTRL_KEY_DELETE || key == CTRL_KEY_BACKSPACE) && !ui.modal && !ui.popup &&
               ui.focus >= ROW && ui.focus < DIAGRAM) {
        activate(CLEAR);
    } else if (key == CTRL_KEY_PAGE_UP || key == CTRL_KEY_PAGE_DOWN) {
        if (!ui.modal && !ui.popup) {
            int step = ui.lines_shown > 1 ? ui.lines_shown - 1 : 1;
            ui.scroll += key == CTRL_KEY_PAGE_DOWN ? step : -step;
            if (ui.scroll > ui.scroll_max)
                ui.scroll = ui.scroll_max;
            if (ui.scroll < 0)
                ui.scroll = 0;
        }
    } else if (key == CTRL_KEY_ARROW_DOWN || key == CTRL_KEY_ARROW_UP) {
        int delta = key == CTRL_KEY_ARROW_DOWN ? 1 : -1;
        if (ui.modal)
            focus_step(delta);
        else if (ui.popup) {
            int choices[CONTROLS_DEVICES + 3], n = 0;
            choices[n++] = 0;
            choices[n++] = 1;
            if (ui.draft.port[ui.player].mode == 2 && current_device() < 0)
                choices[n++] = CONTROLS_DEVICES + 2;
            for (int i = 0; i < CONTROLS_DEVICES; i++)
                if (ControlsRuntime_Device(i)->connected)
                    choices[n++] = i + 2;
            int at = delta > 0 ? -1 : 0;
            for (int i = 0; i < n; i++)
                if (ui.focus == CHOICE + choices[i])
                    at = i;
            at = (at + delta + n) % n;
            ui.focus = CHOICE + choices[at];
            ui.popup_scroll = at > POPUP_ROWS - 1 ? at - (POPUP_ROWS - 1) : 0;
        } else
            move_row(delta);
    } else if ((key == CTRL_KEY_ARROW_LEFT || key == CTRL_KEY_ARROW_RIGHT) && !ui.modal && !ui.popup) {
        if (ui.tab)
            select_row(ui.row, 1 - ui.slot);
    }
}
void ControlsWindow_Tick(void)
{
    ControlSource keys[CTRL_KEY_COUNT], sources[64];
    uint64_t now = ControlsRuntime_Now();
    int device = current_device(), n;
    if (device != ui.selected_device) {
        ui.click_id = 0;
        if (ui.selected_device >= 0 && ui.capture.state)
            cancel_capture();
        ui.selected_device = device;
        memset(&ui.preview, 0, sizeof(ui.preview));
    }
    ControllerDevice *d = ControlsRuntime_Device(device);
    if (ui.capture.state && ui.modal != 3) {
        if (ui.tab && (!d || !d->connected)) {
            cancel_capture();
            say(SAY_WARN, "Controller disconnected; binding unchanged.");
            return;
        }
        n = ui.tab ? ControlsRuntime_Sources(d, sources, 64, ui.capture.state == CAP_WAIT_NEUTRAL)
                   : ControlsRuntime_Keys(keys);
        ControlSource *src = n ? (ui.tab ? sources : keys) : NULL;
        if ((ui.mods || n > 1) && !ui.tab && ui.capture.state == CAP_AWAIT_INPUT) {
            say(SAY_WARN, "Key combinations cannot be bound. Release the keys and try again.");
            if (now >= ui.capture.deadline_us)
                cancel_capture();
            return;
        }
        int result = Controls_CaptureStep(&ui.capture, profile(0), src, ui.tab && device >= 0, now);
        if (result == CAPTURE_DONE) {
            Controls_MoveSource(profile(1), ui.row, ui.slot, &ui.capture.pending);
            say(SAY_DONE, "Binding updated. Apply to save.");
        } else if (result == CAPTURE_CONFLICT) {
            ui.modal = 3;
            ui.focus = YES;
        } else if (result == CAPTURE_REJECT_RESERVED)
            say(SAY_WARN, Controls_ReservedReason(src->code, Controls_IsModifier(src->code)));
        else if (result == CAPTURE_RESET)
            say(SAY_WARN, "Nothing was pressed in time; binding unchanged.");
    }
}

/* ------------------------------------------------------------------ */
/*  The pad picture                                                    */
/* ------------------------------------------------------------------ */

/* Button regions in image coordinates normalized to a width of 1000, measured
 * from ps1-controller.png itself.
 *
 * Each button has two rectangles. `bx, by, bw, bh` is the button as it is
 * drawn: what the selection ring encloses and what a press fills. `x, y, w, h`
 * is where the mouse has to be, and those tile the picture instead of hugging
 * the artwork, because the drawn buttons are small, some (the L2/R2 strips)
 * only a few pixels tall, and neighbours would be a pixel apart. The D-pad
 * and the face cluster are each split four ways along the lines between their
 * buttons, the shoulders split just under the L2/R2 strip, and no two
 * rectangles overlap, so every click lands on exactly one button.
 *
 * The supplied digital pad has no stick clicks: L3/R3 stay in the list. */
static const struct {
    int destination, x, y, w, h, bx, by, bw, bh;
} controller_regions[] = {
    {8, 165, 55, 165, 45, 192, 80, 110, 17},        /* L2, the upper strip */
    {9, 668, 55, 165, 45, 695, 80, 110, 17},        /* R2 */
    {10, 165, 100, 165, 59, 165, 97, 165, 62},      /* L1 */
    {11, 668, 100, 165, 59, 668, 97, 165, 62},      /* R1 */
    {4, 130, 162, 234, 98, 218, 222, 58, 64},       /* Up: the D-pad's top */
    {5, 247, 260, 117, 81, 261, 272, 65, 57},       /* Right */
    {6, 130, 341, 234, 98, 218, 315, 58, 64},       /* Down */
    {7, 130, 260, 117, 81, 168, 272, 64, 57},       /* Left */
    {12, 620, 162, 261, 104, 714, 194, 73, 73},     /* Triangle: the top */
    {13, 750, 266, 131, 70, 785, 264, 73, 73},      /* Circle */
    {14, 620, 336, 261, 103, 714, 334, 73, 73},     /* Cross */
    {15, 620, 266, 130, 70, 642, 264, 73, 73},      /* Square */
    {0, 400, 320, 99, 70, 418, 337, 52, 36},        /* Select */
    {3, 500, 320, 99, 70, 526, 336, 58, 38}};       /* Start */
#define REGION_COUNT ((unsigned)(sizeof(controller_regions) / sizeof(controller_regions[0])))

/* The two rectangles of a region, placed on a picture drawn at (x, y, w). */
static Rect region_hit(unsigned i, int x, int y, int w)
{
    return rect(x + controller_regions[i].x * w / 1000, y + controller_regions[i].y * w / 1000,
                controller_regions[i].w * w / 1000, controller_regions[i].h * w / 1000);
}
static Rect region_button(unsigned i, int x, int y, int w)
{
    return rect(x + controller_regions[i].bx * w / 1000, y + controller_regions[i].by * w / 1000,
                controller_regions[i].bw * w / 1000, controller_regions[i].bh * w / 1000);
}

static void controller(int x, int y, int w, uint16_t bits)
{
    /* Pressed buttons are filled first so the artwork's own outlines stay on
     * top of the fill; the selection ring is drawn last, over everything. */
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        int dest = controller_regions[i].destination;
        Rect b = region_button(i, x, y, w);
        pad_rect[dest] = b;
        if (bits & Controls_Actions[dest].bit)
            ellipse(b.x + b.w / 2, b.y + b.h / 2, b.w / 2, b.h / 2, held);
        hit(DIAGRAM + dest, region_hit(i, x, y, w));
    }
    if (!ControlsArt_Draw(canvas, x * scale, y * scale, w * scale)) {
        text_at(x + 20, y + 40, "Controller picture unavailable", dim);
        return;
    }
    /* Tiny PS1 face symbols retain the original line drawing's open buttons. */
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        int dest = controller_regions[i].destination;
        Rect b = region_button(i, x, y, w);
        int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (dest == 14)
            /* The cross and the triangle read a pixel low and right of the
             * circle they sit in; nudge both back. */
            for (int j = -4; j <= 4; j++) {
                fill(cx - 1 + j, cy - 1 + j, 1, 1, text);
                fill(cx - 1 + j, cy - 1 - j, 1, 1, text);
            }
        if (dest == 15)
            box(cx - 4, cy - 4, 9, 9, text);
        if (dest == 12) {
            for (int j = 0; j < 8; j++) {
                fill(cx - 1 - j / 2, cy - 5 + j, 1, 1, text);
                fill(cx - 1 + j / 2, cy - 5 + j, 1, 1, text);
            }
            fill(cx - 5, cy + 3, 9, 1, text);
        }
        if (dest == 13)
            for (int j = -5; j <= 5; j++)
                for (int k = -5; k <= 5; k++)
                    if (j * j + k * k >= 14 && j * j + k * k <= 23)
                        fill(cx + j, cy + k, 1, 1, text);
        /* L1/R1 are labelled inside their button; the L2/R2 strips are too
         * thin, so their labels sit just above them, and both are dropped
         * when the picture is too small to keep them apart. */
        if (dest >= 8 && dest <= 11 && b.h >= 10 && text_w(Controls_Actions[dest].name) + 4 <= b.w)
            text_at(cx - text_w(Controls_Actions[dest].name) / 2, dest <= 9 ? cy - 11 : cy,
                    Controls_Actions[dest].name, dim);
    }
    /* Selection and hover, over the artwork so nothing hides them. */
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        int dest = controller_regions[i].destination;
        Rect b = region_button(i, x, y, w);
        if (ui.row == dest)
            for (int ring_px = 1; ring_px <= 3; ring_px++)
                box(b.x - ring_px, b.y - ring_px, b.w + 2 * ring_px, b.h + 2 * ring_px, marker);
        else if (ui.hover == DIAGRAM + dest)
            box(b.x - 2, b.y - 2, b.w + 4, b.h + 4, edge);
    }
}

/* ------------------------------------------------------------------ */
/*  Panels                                                             */
/* ------------------------------------------------------------------ */

static void held_summary(char *out, size_t size, uint16_t bits)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < CTRL_DEST_COUNT && at + 1 < size; i++) {
        int action = order[i];
        if (!(bits & Controls_Actions[action].bit))
            continue;
        int wrote = snprintf(out + at, size - at, "%s%s", at ? ", " : "", Controls_Actions[action].name);
        if (wrote < 0 || (size_t)wrote >= size - at)
            break;
        at += (size_t)wrote;
    }
}

static void draw_diagram(Rect r, uint16_t bits)
{
    char live[160];
    frame(r, panel, edge);
    int title_mid = r.y + 18, title_w = text_w("PlayStation pad");
    text_clip(r.x + 12, title_mid, "PlayStation pad", r.w - 24, dim);
    held_summary(live, sizeof(live), bits);
    int room = r.w - 36 - title_w;
    if (room > 40)
        text_right_clip(r.x + r.w - 12, title_mid, live[0] ? live : "nothing pressed", room,
                        live[0] ? on_held : faint);

    int legend_lines = r.w >= 330 ? 3 : 2;
    int legend_h = legend_lines * 18 + 10;
    int art_top = r.y + 34, art_w = r.w - 24;
    int art_room = r.h - (art_top - r.y) - legend_h;
    if (ControlsArt_Height(art_w) > art_room)
        art_w = art_room * 1474 / 1067;
    if (art_w > 8)
        controller(r.x + (r.w - art_w) / 2, art_top + (art_room - ControlsArt_Height(art_w)) / 2, art_w,
                   bits);

    int y = r.y + r.h - legend_h + 4, swatch = 10;
    fill(r.x + 12, y + 3, swatch, swatch, held);
    text_at(r.x + 12 + swatch + 6, y + 8, "pressed now", faint);
    int x = r.x + 12 + swatch + 6 + text_w("pressed now") + 18;
    box(x, y + 3, swatch, swatch, marker);
    box(x + 1, y + 4, swatch - 2, swatch - 2, marker);
    text_at(x + swatch + 6, y + 8, "selected", faint);
    text_clip(r.x + 12, y + 26, "Click a button here to jump to its row.", r.w - 24, faint);
    if (legend_lines > 2)
        text_clip(r.x + 12, y + 44,
                  ui.tab ? "Stick clicks (L3/R3) are in the list only."
                         : "The picture always shows a PlayStation pad.",
                  r.w - 24, faint);
}

/* How many table lines a panel of this height shows, and the height that
 * exactly holds them: both panels are sized from this so they end level. */
static int table_lines(int height)
{
    int shown = (height - HEAD_H - 11) / ROW_H;
    if (shown < 1)
        shown = 1;
    return shown > LINE_COUNT ? LINE_COUNT : shown;
}
static int table_height(int height) { return HEAD_H + 11 + table_lines(height) * ROW_H; }

static void draw_table(Rect r, uint16_t bits)
{
    const ControlsProfile *p = profile(0);
    int slots = ui.tab ? 2 : 1;
    frame(r, panel, edge);
    fill(r.x + 1, r.y + 1, r.w - 2, HEAD_H - 1, raised);
    fill(r.x + 1, r.y + HEAD_H, r.w - 2, 1, edge);

    int top = r.y + HEAD_H + 5;
    int shown = table_lines(r.h);
    ui.lines_shown = shown;
    ui.scroll_max = LINE_COUNT - shown;
    if (ui.scroll > ui.scroll_max)
        ui.scroll = ui.scroll_max;
    if (ui.scroll < 0)
        ui.scroll = 0;

    int bar = ui.scroll_max ? 10 : 0;
    int x = r.x + 8, inner = r.w - 16 - bar;
    int slot_w = (inner - NAME_W - (slots - 1) * 6) / slots;
    int head_mid = r.y + HEAD_H / 2;
    text_clip(x + 10, head_mid, "PS1 BUTTON", NAME_W - 14, faint);
    text_clip(x + NAME_W + 7, head_mid, ui.tab ? "BINDING" : "KEYBOARD KEY", slot_w - 14, faint);
    if (slots > 1)
        text_clip(x + NAME_W + slot_w + 13, head_mid, "ALTERNATE", slot_w - 14, faint);

    for (int i = 0; i < shown; i++) {
        int line = ui.scroll + i, group = 0;
        int action = line_action(line, &group);
        int y = top + i * ROW_H, mid = y + (ROW_H - 2) / 2;
        if (action < 0) {
            int tw = text_w(groups[group].title);
            text_at(x + 6, mid, groups[group].title, faint);
            fill(x + 12 + tw, mid, inner - tw - 18, 1, edge);
            continue;
        }
        int down = (bits & Controls_Actions[action].bit) != 0;
        int chosen = action == ui.row;
        int over = ui.hover == ROW + action * 2 || ui.hover == ROW + action * 2 + 1;
        uint32_t row_face = down ? held : chosen ? hot : over ? raised : panel;
        fill(x, y, inner, ROW_H - 2, row_face);
        if (chosen)
            fill(x, y, 3, ROW_H - 2, accent);
        text_clip(x + 10, mid, Controls_Actions[action].name, NAME_W - 16, down ? on_held : text);
        hit(ROW + action * 2, rect(x, y, NAME_W, ROW_H - 2));
        for (int slot = 0; slot < slots; slot++) {
            Rect cell = rect(x + NAME_W + slot * (slot_w + 6), y, slot_w, ROW_H - 2);
            const ControlSource *src = &p->src[action][slot];
            int bound = src->kind != CTRL_SRC_UNBOUND;
            int target = chosen && slot == ui.slot;
            frame(cell, down ? held_cell : bg,
                  target ? accent : ui.hover == ROW + action * 2 + slot ? dim : edge);
            if (target && ui.capture.state)
                box(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, accent);
            text_clip(cell.x + 7, mid, bound ? Controls_SourceName(src) : "Unbound", cell.w - 14,
                      !bound ? faint : down ? on_held : text);
            hit(ROW + action * 2 + slot, cell);
        }
    }
    if (ui.scroll_max) {
        int track = shown * ROW_H - 2, tx = r.x + r.w - 12;
        int knob = track * shown / LINE_COUNT;
        if (knob < 20)
            knob = 20;
        fill(tx, top, 5, track, bg);
        fill(tx, top + (track - knob) * ui.scroll / ui.scroll_max, 5, knob, edge);
    }
}

/* The device list, drawn over everything else and owning every hit. */
static int device_entries(int *out)
{
    int count = 0;
    out[count++] = 0; /* Automatic */
    out[count++] = 1; /* None */
    if (ui.draft.port[ui.player].mode == 2 && current_device() < 0)
        out[count++] = CONTROLS_DEVICES + 2; /* the saved, absent device */
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (ControlsRuntime_Device(i)->connected)
            out[count++] = i + 2;
    return count;
}
static int entry_chosen(int entry)
{
    const ControlsPort *p = &ui.draft.port[ui.player];
    if (entry == 0)
        return p->mode == 1;
    if (entry == 1)
        return p->mode == 0;
    if (entry == CONTROLS_DEVICES + 2)
        return p->mode == 2 && current_device() < 0;
    ControllerDevice *d = ControlsRuntime_Device(entry - 2);
    return p->mode == 2 && d && !strcmp(p->identity, d->identity);
}
static void draw_popup(Rect anchor)
{
    int entries[CONTROLS_DEVICES + 3], count = device_entries(entries);
    char line[256];
    ui.count = 0;
    ui.popup_max = count > POPUP_ROWS ? count - POPUP_ROWS : 0;
    if (ui.popup_scroll > ui.popup_max)
        ui.popup_scroll = ui.popup_max;
    if (ui.popup_scroll < 0)
        ui.popup_scroll = 0;
    int shown = count - ui.popup_scroll;
    if (shown > POPUP_ROWS)
        shown = POPUP_ROWS;
    Rect list = rect(anchor.x, anchor.y + anchor.h + 2, anchor.w, shown * POPUP_ROW + 8);
    if (list.y + list.h > ui.height - 8)
        list.y = anchor.y - list.h - 2;
    shade(list.x + 3, list.y + 3, list.w, list.h);
    frame(list, raised, accent);
    for (int i = 0; i < shown; i++) {
        int entry = entries[ui.popup_scroll + i];
        Rect row = rect(list.x + 4, list.y + 4 + i * POPUP_ROW, list.w - 8, POPUP_ROW);
        const char *name;
        if (entry < 2)
            name = entry ? "None - no controller for this player" : "Automatic - first free controller";
        else if (entry == CONTROLS_DEVICES + 2)
            name = "Saved controller (not connected)";
        else {
            ControllerDevice *d = ControlsRuntime_Device(entry - 2);
            snprintf(line, sizeof(line), "%s #%d%s", d->name, entry - 1,
                     ControlsRuntime_Assigned(&ui.draft, 1 - ui.player) == entry - 2 ? "  (other player)"
                                                                                     : "");
            name = line;
        }
        int over = ui.hover == CHOICE + entry, focused = ui.focus == CHOICE + entry;
        if (over || focused)
            fill(row.x, row.y, row.w, row.h, focused ? accent : hot);
        if (entry_chosen(entry))
            ellipse(row.x + 12, row.y + row.h / 2, 3, 3, focused ? on_accent : accent);
        text_clip(row.x + 24, row.y + row.h / 2, name, row.w - 34, focused ? on_accent : text);
        hit(CHOICE + entry, row);
    }
    if (ui.popup_max)
        text_right(list.x + list.w - 8, list.y + list.h - 4, "scroll for more", faint);
}

static void draw_modal(void)
{
    char body[256];
    const char *title, *message;
    ui.count = 0;
    shade(0, 0, ui.width, ui.height);
    if (ui.modal == 1) {
        title = "Unsaved changes";
        message = "Save your new bindings before closing the window?";
    } else if (ui.modal == 2) {
        title = "Restore defaults";
        message = ui.tab ? "Restore the default bindings for this controller? Other players and the "
                           "keyboard are not touched."
                         : "Restore the default keyboard bindings? Controllers are not touched.";
    } else {
        int other = ui.capture.conflict_dest;
        snprintf(body, sizeof(body), "%s is already bound to %s. Move it to %s instead?",
                 Controls_SourceName(&ui.capture.pending),
                 other >= 0 && other < CTRL_DEST_COUNT ? Controls_Actions[other].name : "another button",
                 Controls_Actions[ui.row].name);
        title = "Input already in use";
        message = body;
    }
    int w = ui.width - 60 < 460 ? ui.width - 60 : 460;
    int lines = text_wrap(0, 0, message, w - 32, dim, 3, 0);
    int h = 30 + 26 + lines * 18 + 16 + BTN_H + 16;
    Rect m = rect((ui.width - w) / 2, (ui.height - h) / 2, w, h);
    frame(m, panel, accent);
    fill(m.x + 1, m.y + 1, m.w - 2, 30, raised);
    text_at(m.x + 16, m.y + 16, title, text);
    text_wrap(m.x + 16, m.y + 52, message, m.w - 32, dim, 3, 1);
    int y = m.y + m.h - 16 - BTN_H, right = m.x + m.w - 16;
    const char *accept = ui.modal == 1 ? "Apply" : ui.modal == 2 ? "Restore" : "Move binding";
    const char *reject = ui.modal == 1 ? "Discard" : "Cancel";
    int aw = button_w(accept), rw = button_w(reject);
    button(YES, rect(right - aw, y, aw, BTN_H), accept, BTN_PRIMARY);
    button(NO, rect(right - aw - 8 - rw, y, rw, BTN_H), reject, BTN_NORMAL);
    if (ui.modal == 1) {
        int kw = button_w("Keep editing");
        button(KEEP, rect(m.x + 16, y, kw, BTN_H), "Keep editing", BTN_NORMAL);
    }
}

/* ------------------------------------------------------------------ */
/*  Header, footer and the main pass                                   */
/* ------------------------------------------------------------------ */

static void segment(int id, Rect r, const char *label, int on)
{
    int over = ui.hover == id;
    frame(r, on ? accent : over ? hot : raised, on ? accent_hot : edge);
    text_clip(r.x + (r.w - text_w(label)) / 2, r.y + r.h / 2, label, r.w - 8, on ? on_accent : text);
    if (ui.focus == id)
        ring(r);
    hit(id, r);
}
static int tab_item(int id, int x, const char *label, int on, int disabled)
{
    Rect r = rect(x, HEADER_H - 30, text_w(label) + 28, 29);
    int over = ui.hover == id;
    if (on)
        fill(r.x, r.y, r.w, r.h, bg);
    else if (over && !disabled)
        fill(r.x, r.y, r.w, r.h, raised);
    text_at(r.x + 14, r.y + r.h / 2 - 1, label, disabled ? faint : on ? text : dim);
    fill(r.x, r.y + r.h - 2, r.w, 2, on ? accent : edge);
    if (ui.focus == id)
        box(r.x + 2, r.y + 2, r.w - 4, r.h - 6, accent);
    hit(id, r);
    return x + r.w + 4;
}
static void draw_header(void)
{
    int w = ui.width;
    fill(0, 0, w, HEADER_H, panel);
    fill(0, HEADER_H - 1, w, 1, edge);
    text_at(PAD, 22, "Controls", text);

    int seg = 92, seg_x = w - PAD - 2 * seg;
    text_right(seg_x - 10, 22, "Editing", faint);
    segment(PLAYER, rect(seg_x, 10, seg, 25), "Player 1", ui.player == 0);
    segment(PLAYER_2, rect(seg_x + seg, 10, seg, 25), "Player 2", ui.player == 1);

    int x = PAD;
    x = tab_item(KEYBOARD, x, "Keyboard", !ui.tab, ui.player == 1);
    tab_item(CONTROLLER, x, "Controller", ui.tab, 0);
    if (dirty())
        text_right(w - PAD, HEADER_H - 16, "Unsaved changes", warn);
}

static void draw_device_row(Rect r, ControllerDevice *d)
{
    char label[256];
    const ControlsPort *p = &ui.draft.port[ui.player];
    if (!ui.tab) {
        text_clip(r.x + 2, r.y + r.h / 2,
                  "Keyboard bindings always play as Player 1. One key per PlayStation button.", r.w - 4, dim);
        return;
    }
    const char *state;
    uint32_t state_ink;
    if (p->mode == 0) {
        state = "Switched off";
        state_ink = faint;
    } else if (d && d->connected) {
        /* Two identical pads cannot be told apart again after a restart. */
        state = d->ambiguous ? "Choose again after a restart" : "Connected";
        state_ink = d->ambiguous ? warn : good;
    } else if (p->mode == 2) {
        state = "Not connected";
        state_ink = warn;
    } else {
        state = getenv("MEMORIES_NO_GAMEPAD") ? "Disabled this session" : "No controller found";
        state_ink = warn;
    }
    if (p->mode == 0)
        snprintf(label, sizeof(label), "None - no controller for this player");
    else if (p->mode == 1)
        snprintf(label, sizeof(label), "Automatic%s%s", d ? " - " : "", d ? d->name : "");
    else
        snprintf(label, sizeof(label), "%s", d ? d->name : "Saved controller (not connected)");

    text_at(r.x + 2, r.y + r.h / 2, "Controller", dim);
    int left = r.x + 2 + text_w("Controller") + 12;
    int pill_w = text_w(state) + 24, pill_x = r.x + r.w - pill_w;
    Rect drop = rect(left, r.y, pill_x - 10 - left, r.h);
    device_drop = drop;
    int over = ui.hover == DEVICE;
    frame(drop, over || ui.popup == DEVICE ? hot : raised, ui.popup == DEVICE ? accent : edge);
    text_clip(drop.x + 10, drop.y + drop.h / 2, label, drop.w - 34, text);
    caret(drop.x + drop.w - 14, drop.y + drop.h / 2, dim);
    if (ui.focus == DEVICE)
        ring(drop);
    hit(DEVICE, drop);
    frame(rect(pill_x, r.y, pill_w, r.h), panel, edge);
    text_at(pill_x + 12, r.y + r.h / 2, state, state_ink);
}

void ControlsWindow_Draw(MenuCanvas *c)
{
    ControlSource sources[CTRL_KEY_COUNT];
    char line[256];
    canvas = c;
    scale = Menu_Scale();
    while (scale > 1 && (c->width / scale < MIN_W || c->height / scale < MIN_H))
        scale--;
    ui.draw_scale = scale;
    int w = ui.width = c->width / scale, h = ui.height = c->height / scale;
    ui.count = 0;
    memset(pad_rect, 0, sizeof(pad_rect)); /* refilled only if the picture fits */
    fill(0, 0, w, h, bg);

    int device = current_device();
    ControllerDevice *d = ControlsRuntime_Device(device);
    uint16_t bits = 0;
    if (ui.tab) {
        if (d) {
            ui.preview.activation = d->threshold;
            bits = Controls_EvalController(profile(0), &d->snapshot, &ui.preview);
        }
    } else {
        int n = ControlsRuntime_Keys(sources);
        bits = Controls_EvalKeyboard(profile(0), sources, n);
    }

    draw_header();
    Rect device_row = rect(PAD, HEADER_H + 12, w - 2 * PAD, BTN_H);
    draw_device_row(device_row, d);

    int foot_top = h - FOOTER_H;
    int status_mid = foot_top - 17;
    int squat = h < 560; /* too short for both message lines */
    int hint_mid = status_mid - 19;
    int action_y = (squat ? status_mid : hint_mid) - 12 - BTN_H;
    Rect content = rect(PAD, device_row.y + device_row.h + 14, w - 2 * PAD,
                        action_y - 12 - (device_row.y + device_row.h + 14));
    if (content.h < 120)
        content.h = 120;
    content.h = table_height(content.h);

    int slots = ui.tab ? 2 : 1;
    int table_w = NAME_W + slots * (ui.tab ? SLOT_W : KEY_W) + (slots - 1) * 6 + 16;
    if (table_w > content.w)
        table_w = content.w;
    int diagram_w = content.w - GAP - table_w;
    if (diagram_w > DIAGRAM_MAX) {
        table_w += diagram_w - DIAGRAM_MAX;
        diagram_w = DIAGRAM_MAX;
    }
    int show_diagram = diagram_w >= DIAGRAM_MIN && content.h >= 210;
    Rect table;
    if (show_diagram) {
        draw_diagram(rect(content.x, content.y, diagram_w, content.h), bits);
        table = rect(content.x + diagram_w + GAP, content.y, table_w, content.h);
    } else {
        int width = table_w + 160 < content.w ? table_w + 160 : content.w;
        table = rect(content.x + (content.w - width) / 2, content.y, width, content.h);
    }
    draw_table(table, bits);

    /* Selection and the two buttons that act on it. */
    const ControlSource *src = &profile(0)->src[ui.row][ui.slot];
    snprintf(line, sizeof(line), "%s / %s%s: %s", action_group(ui.row), Controls_Actions[ui.row].name,
             ui.tab ? (ui.slot ? " (alternate)" : " (main)") : "",
             src->kind != CTRL_SRC_UNBOUND ? Controls_SourceName(src) : "Unbound");
    int rebind_w = button_w("Rebind"), clear_w = button_w("Clear");
    text_clip(PAD, action_y + BTN_H / 2, line, w - 2 * PAD - rebind_w - clear_w - 24, text);
    button(CLEAR, rect(w - PAD - rebind_w - 8 - clear_w, action_y, clear_w, BTN_H), "Clear",
           src->kind != CTRL_SRC_UNBOUND ? BTN_NORMAL : BTN_QUIET);
    button(REBIND, rect(w - PAD - rebind_w, action_y, rebind_w, BTN_H),
           ui.capture.state ? "Stop" : "Rebind", BTN_NORMAL);

    /* Hint line: what to do here, or what the window is waiting for. */
    const char *hint;
    if (ui.capture.state) {
        uint64_t now = ControlsRuntime_Now();
        int left = ui.capture.deadline_us > now ? (int)((ui.capture.deadline_us - now + 999999) / 1000000) : 0;
        snprintf(line, sizeof(line), "Listening for %s%s - %d second%s left. Escape cancels.",
                 Controls_Actions[ui.row].name, ui.tab && ui.slot ? " (alternate)" : "", left,
                 left == 1 ? "" : "s");
        hint = line;
    } else if (ui.hover >= DIAGRAM && ui.hover < CHOICE) {
        snprintf(line, sizeof(line), "%s - click to select it, double-click to rebind it.",
                 Controls_Actions[ui.hover - DIAGRAM].name);
        hint = line;
    } else
        hint = "Double-click a binding, or select one and press Enter, to change it. Delete clears it.";
    if (!squat)
        text_clip(PAD, hint_mid, hint, w - 2 * PAD, ui.capture.state ? accent : faint);
    if (squat && ui.capture.state)
        text_clip(PAD, status_mid, hint, w - 2 * PAD, accent);
    else
        text_clip(PAD, status_mid, ui.status, w - 2 * PAD,
                  ui.say == SAY_WARN ? warn : ui.say == SAY_DONE ? good : ui.say == SAY_BUSY ? accent : dim);

    /* Footer. */
    fill(0, foot_top, w, FOOTER_H, panel);
    fill(0, foot_top, w, 1, edge);
    int by = foot_top + (FOOTER_H - BTN_H) / 2;
    const char *restore = ui.tab ? "Restore controller defaults" : "Restore keyboard defaults";
    button(DEFAULTS, rect(PAD, by, button_w(restore), BTN_H), restore, BTN_NORMAL);
    int ok_w = button_w("OK") + 18, apply_w = button_w("Apply"), cancel_w = button_w("Cancel");
    button(OK, rect(w - PAD - ok_w, by, ok_w, BTN_H), "OK", BTN_PRIMARY);
    button(APPLY, rect(w - PAD - ok_w - 8 - apply_w, by, apply_w, BTN_H), "Apply",
           dirty() ? BTN_READY : BTN_QUIET);
    button(CANCEL, rect(w - PAD - ok_w - 8 - apply_w - 8 - cancel_w, by, cancel_w, BTN_H), "Cancel",
           BTN_NORMAL);

    if (ui.popup == DEVICE)
        draw_popup(device_drop);
    if (ui.modal)
        draw_modal();
}
