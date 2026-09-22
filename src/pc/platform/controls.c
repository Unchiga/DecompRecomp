/* Shared mapping model and held-state evaluator for the PC port's Controls
 * window and runtime evaluation (notes/controls-menu-workflow.md).
 * See controls.h for the token vocabulary. The evaluators here are the single
 * source of truth for "which PlayStation pad bits are held"; backends only
 * normalize raw input into these tokens. No allocation, no I/O, no globals
 * beyond the constant action/names tables. */
#include "controls.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Action table (UI order == destination order; index is the bit slot) */
/* ------------------------------------------------------------------ */

const ControlsAction Controls_Actions[CTRL_DEST_COUNT] = {{CTRL_DEST_SELECT, "Select"},
                                                          {CTRL_DEST_L3, "L3"},
                                                          {CTRL_DEST_R3, "R3"},
                                                          {CTRL_DEST_START, "Start"},
                                                          {CTRL_DEST_UP, "Up"},
                                                          {CTRL_DEST_RIGHT, "Right"},
                                                          {CTRL_DEST_DOWN, "Down"},
                                                          {CTRL_DEST_LEFT, "Left"},
                                                          {CTRL_DEST_L2, "L2"},
                                                          {CTRL_DEST_R2, "R2"},
                                                          {CTRL_DEST_L1, "L1"},
                                                          {CTRL_DEST_R1, "R1"},
                                                          {CTRL_DEST_TRIANGLE, "Triangle"},
                                                          {CTRL_DEST_CIRCLE, "Circle"},
                                                          {CTRL_DEST_CROSS, "Cross"},
                                                          {CTRL_DEST_SQUARE, "Square"}};
/* CTRL_DEST_COUNT is 16 because the PS1 digital pad has 16 active bits
 * (0x0001 .. 0x8000, 0x0000 carries no action). The list above must always
 * line up with that; the test suite asserts the match. */

/* ------------------------------------------------------------------ */
/*  Source name tables                                                */
/* ------------------------------------------------------------------ */

static const char *KEY_NAMES[CTRL_SRC_KEY_COUNT + 1] = {
    [0] = "", "a",      "b",        "c",         "d",           "e",          "f",    "g",        "h",
    "i",      "j",      "k",        "l",         "m",           "n",          "o",    "p",        "q",
    "r",      "s",      "t",        "u",         "v",           "w",          "x",    "y",        "z",
    "Num1",   "Num2",   "Num3",     "Num4",      "Num5",        "Num6",       "Num7", "Num8",     "Num9",
    "Num0",   "Num,",   "Num.",     "Num/",      "Num*",        "Num-",       "Num+", "NumEnter", "Num=",
    "Space",  "Esc",    "Tab",      "Backspace", "Enter",       "-",          "=",    "[",        "]",
    "\\",     ";",      "'",        "`",         ",",           ".",          "/",    "F1",       "F2",
    "F3",     "F4",     "F5",       "F6",        "F7",          "F8",         "F9",   "F10",      "F11",
    "F12",    "Up",     "Down",     "Left",      "Right",       "Insert",     "Home", "End",      "PgUp",
    "PgDn",   "LShift", "RShift",   "LCtrl",     "RCtrl",       "LAlt",       "RAlt", "LSuper",   "RSuper",
    "1",      "2",      "3",        "4",         "5",           "6",          "7",    "8",        "9",
    "0",      "Delete", "CapsLock", "NumLock",   "PrintScreen", "ScrollLock", "Pause"};

/* Indexes are CtrlButtonCode values in enum order. */
static const char *BTN_NAMES[CTRL_SRC_BUTTON_COUNT + 1] = {
    [0] = "",  "South",     "East",       "West",       "North",      "Back",
    "Guide",   "Start",     "L-Shoulder", "R-Shoulder", "Left Stick", "Right Stick",
    "Dpad Up", "Dpad Down", "Dpad Left",  "Dpad Right", "Misc 1",     "Misc 2"};

static const char *AXIS_NAMES[CTRL_SRC_AXIS_COUNT + 1] = {
    [0] = "", "L-Stick X", "L-Stick Y", "R-Stick X", "R-Stick Y"};

static const char *TRIGGER_NAMES[CTRL_SRC_TRIGGER_COUNT + 1] = {[0] = "", "L-Trigger", "R-Trigger"};

/* Hat directions are canonical masks (CTRL_HAT_*); name lookup is by mask
 * value in a 16-entry table (only bits 0..3 are meaningful; non-canonical
 * masks such as 0x07 simply have no name). */
static const char *HAT_NAMES[1 << 4] = {
    [CTRL_HAT_UP] = "Dpad Up",
    [CTRL_HAT_RIGHT] = "Dpad Right",
    [CTRL_HAT_DOWN] = "Dpad Down",
    [CTRL_HAT_LEFT] = "Dpad Left",
    [CTRL_HAT_UP | CTRL_HAT_RIGHT] = "Dpad Up-Right",
    [CTRL_HAT_UP | CTRL_HAT_LEFT] = "Dpad Up-Left",
    [CTRL_HAT_DOWN | CTRL_HAT_RIGHT] = "Dpad Down-Right",
    [CTRL_HAT_DOWN | CTRL_HAT_LEFT] = "Dpad Down-Left",
};

const char *Controls_KeyName(CtrlKeyCode code)
{
    if (code <= CTRL_KEY_NONE || code >= CTRL_KEY_COUNT)
        return "";
    return KEY_NAMES[code];
}

static char key_labels[CTRL_KEY_COUNT][32];
void Controls_SetKeyLabel(int code, const char *label)
{
    if (code > 0 && code < CTRL_KEY_COUNT && label)
        snprintf(key_labels[code], sizeof(key_labels[code]), "%s", label);
}
const char *Controls_KeyLabel(CtrlKeyCode code)
{
    return code > 0 && code < CTRL_KEY_COUNT && key_labels[code][0] ? key_labels[code]
                                                                    : Controls_KeyName(code);
}

const char *Controls_SourceName(const ControlSource *src)
{
    switch (src->kind) {
    case CTRL_SRC_UNBOUND:
        return "Unbound";
    case CTRL_SRC_KEY:
        return src->code <= CTRL_SRC_KEY_COUNT ? Controls_KeyLabel((CtrlKeyCode)src->code) : "";
    case CTRL_SRC_BUTTON:
        return src->code <= CTRL_SRC_BUTTON_COUNT ? BTN_NAMES[src->code] : "";
    case CTRL_SRC_AXIS: {
        /* Render an axis as "L-Stick X +" or "L-Stick Y -". The window
         * appends/omits the suffix itself when it needs the bare name; keep
         * this value complete and stable. */
        static char buf[32];
        if (src->code == 0 || src->code > CTRL_SRC_AXIS_COUNT)
            return "";
        snprintf(buf, sizeof(buf), "%s %s", AXIS_NAMES[src->code], src->sign > 0 ? "+" : "-");
        return buf;
    }
    case CTRL_SRC_TRIGGER:
        return src->code <= CTRL_SRC_TRIGGER_COUNT ? TRIGGER_NAMES[src->code] : "";
    case CTRL_SRC_HAT: {
        unsigned m = src->code & 0x0f;
        return m != 0 && HAT_NAMES[m] ? HAT_NAMES[m] : "";
    }
    }
    return "";
}

int Controls_SourceEquals(const ControlSource *a, const ControlSource *b)
{
    return a->kind == b->kind && a->code == b->code && a->sign == b->sign;
}

/* A 32-bit stable identity for (kind, code, sign); cheap to compare and to
 * hash. Kind uses 3 bits, code 16, sign 2. */
uint32_t Controls_SourceIdentity(const ControlSource *src)
{
    return (uint32_t)src->kind << 18 | (uint32_t)src->code << 2 | (uint32_t)(src->sign & 0x03);
}

/* ------------------------------------------------------------------ */
/*  Reserved-key and modifier policy (centralized per the spec)      */
/* ------------------------------------------------------------------ */

static int host_shortcut(int key)
{
    switch (key) {
    case CTRL_KEY_ESCAPE:
    case CTRL_KEY_TAB:
    case CTRL_KEY_F1:
    case CTRL_KEY_F2:
    case CTRL_KEY_F3:
    case CTRL_KEY_F4:
    case CTRL_KEY_F5:
    case CTRL_KEY_F7:
    case CTRL_KEY_F10:
    case CTRL_KEY_F11:
    case CTRL_KEY_F12:
    case CTRL_KEY_P:
    case CTRL_KEY_M:
    case CTRL_KEY_PERIOD: /* pause/mute/reset */
        return 1;
    default:
        return 0;
    }
}

int Controls_IsModifier(int key)
{
    switch (key) {
    case CTRL_KEY_LEFT_SHIFT:
    case CTRL_KEY_RIGHT_SHIFT:
    case CTRL_KEY_LEFT_CTRL:
    case CTRL_KEY_RIGHT_CTRL:
    case CTRL_KEY_LEFT_ALT:
    case CTRL_KEY_RIGHT_ALT:
    case CTRL_KEY_LEFT_SUPER:
    case CTRL_KEY_RIGHT_SUPER:
        return 1;
    default:
        return 0;
    }
}

int Controls_IsReservedKey(int key, int is_modifier)
{
    if (key <= CTRL_KEY_NONE || key >= CTRL_KEY_COUNT)
        return 0;
    if (host_shortcut(key))
        return 1;
    /* Modifiers are ambiguous (menu/system chord components), but Right
     * Shift's existing Select binding is the exception. */
    if (is_modifier && key != CTRL_KEY_RIGHT_SHIFT)
        return 1;
    return 0;
}

const char *Controls_ReservedReason(int key, int is_modifier)
{
    static const char *reasons[CTRL_KEY_COUNT] = {[CTRL_KEY_ESCAPE] = "Reserved: close window",
                                                  [CTRL_KEY_TAB] = "Reserved: menu focus",
                                                  [CTRL_KEY_F1] = "Reserved: state slot 1 / shortcut",
                                                  [CTRL_KEY_F2] = "Reserved: state slot 2 / shortcut",
                                                  [CTRL_KEY_F3] = "Reserved: debug HUD",
                                                  [CTRL_KEY_F4] = "Reserved: state slot 4 / shortcut",
                                                  [CTRL_KEY_F5] = "Reserved: save state",
                                                  [CTRL_KEY_F7] = "Reserved: shortcut",
                                                  [CTRL_KEY_F10] = "Reserved: shortcut",
                                                  [CTRL_KEY_F11] = "Reserved: shortcut",
                                                  [CTRL_KEY_F12] = "Reserved: shortcut",
                                                  [CTRL_KEY_P] = "Reserved: pause",
                                                  [CTRL_KEY_M] = "Reserved: mute",
                                                  [CTRL_KEY_PERIOD] = "Reserved: frame step"};
    (void)is_modifier;
    if (key <= CTRL_KEY_NONE || key >= CTRL_KEY_COUNT)
        return "";
    if (reasons[key])
        return reasons[key];
    if (Controls_IsModifier(key))
        /* Right Shift (the existing Select binding) is an exception and is
         * NOT rejected, so it has no rejection reason. */
        return key == CTRL_KEY_RIGHT_SHIFT ? "" : "Modifier keys cannot be bound alone";
    return "";
}

/* ------------------------------------------------------------------ */
/*  Defaults                                                          */
/* ------------------------------------------------------------------ */

static void ksrc(ControlSource *s, int key)
{
    s->kind = CTRL_SRC_KEY;
    s->code = (uint16_t)key;
    s->sign = 0;
}
static void bsrc(ControlSource *s, int btn)
{
    s->kind = CTRL_SRC_BUTTON;
    s->code = (uint16_t)btn;
    s->sign = 0;
}
static void asrc(ControlSource *s, int axis, int sign)
{
    s->kind = CTRL_SRC_AXIS;
    s->code = (uint16_t)axis;
    s->sign = (int8_t)sign;
}
static void tsrc(ControlSource *s, int trig)
{
    s->kind = CTRL_SRC_TRIGGER;
    s->code = (uint16_t)trig;
    s->sign = 0;
}
static void hsrc(ControlSource *s, int mask)
{
    s->kind = CTRL_SRC_HAT;
    s->code = (uint16_t)mask;
    s->sign = 0;
}

void Controls_Clear(ControlsConfig *cfg) { memset(cfg, 0, sizeof(*cfg)); }

/* Defaults reproduce the current, pre-window backend mappings:
 *  Keyboard (SDL/X11 keymap): RShift Select, T/Y L3/R3, Enter Start,
 *    arrows d-pad, E/R L2/R2, Q/W L1/R1, A Triangle, S Circle, X Cross, Z Square.
 *  Controller (SDL gamepad + evdev Xbox mapping table): Back Select, stick
 *    clicks L3/R3, Start, d-pad up/down/left/right (slot 0) plus the left
 *    stick (slot 1) for the four directions, triggers L2/R2, shoulders
 *    L1/R1, North Triangle, East Circle, South Cross, West Square. */
void Controls_InitDefaults(ControlsConfig *cfg)
{
    Controls_Clear(cfg);
    cfg->version = CTRL_CONFIG_VERSION;
    ControlSource s;

    /* Keyboard, Player 1 (single slot per destination). */
    ksrc(&s, CTRL_KEY_RIGHT_SHIFT);
    cfg->kb.src[0][0] = s; /* Select */
    ksrc(&s, CTRL_KEY_T);
    cfg->kb.src[1][0] = s; /* L3  */
    ksrc(&s, CTRL_KEY_Y);
    cfg->kb.src[2][0] = s; /* R3  */
    ksrc(&s, CTRL_KEY_ENTER);
    cfg->kb.src[3][0] = s; /* Start */
    ksrc(&s, CTRL_KEY_ARROW_UP);
    cfg->kb.src[4][0] = s; /* Up    */
    ksrc(&s, CTRL_KEY_ARROW_RIGHT);
    cfg->kb.src[5][0] = s; /* Right */
    ksrc(&s, CTRL_KEY_ARROW_DOWN);
    cfg->kb.src[6][0] = s; /* Down  */
    ksrc(&s, CTRL_KEY_ARROW_LEFT);
    cfg->kb.src[7][0] = s; /* Left  */
    ksrc(&s, CTRL_KEY_E);
    cfg->kb.src[8][0] = s; /* L2  */
    ksrc(&s, CTRL_KEY_R);
    cfg->kb.src[9][0] = s; /* R2  */
    ksrc(&s, CTRL_KEY_Q);
    cfg->kb.src[10][0] = s; /* L1  */
    ksrc(&s, CTRL_KEY_W);
    cfg->kb.src[11][0] = s; /* R1  */
    ksrc(&s, CTRL_KEY_A);
    cfg->kb.src[12][0] = s; /* Triangle */
    ksrc(&s, CTRL_KEY_S);
    cfg->kb.src[13][0] = s; /* Circle      */
    ksrc(&s, CTRL_KEY_X);
    cfg->kb.src[14][0] = s; /* Cross          */
    ksrc(&s, CTRL_KEY_Z);
    cfg->kb.src[15][0] = s; /* Square          */

    /* Controllers, both ports share the layout; each gets an independent
     * copy so a rebinding in one port never leaks into the other. */
    for (int p = 0; p < CTRL_PORT_COUNT; p++) {
        ControlsProfile *prof = &cfg->ctrl[p];
        bsrc(&s, CTRL_BTN_BACK);
        prof->src[0][0] = s; /* Select (View) */
        bsrc(&s, CTRL_BTN_LEFT_STICK);
        prof->src[1][0] = s; /* L3 */
        bsrc(&s, CTRL_BTN_RIGHT_STICK);
        prof->src[2][0] = s; /* R3 */
        bsrc(&s, CTRL_BTN_START);
        prof->src[3][0] = s; /* Start */
        /* D-pad directions come from the canonical hat token (what both
         * backends report); the left stick (slot 1) mirrors them. */
        hsrc(&s, CTRL_HAT_UP);
        prof->src[4][0] = s; /* Up, slot 0 */
        asrc(&s, CTRL_AXIS_LEFT_Y, -1);
        prof->src[4][1] = s; /* Up, slot 1 */
        hsrc(&s, CTRL_HAT_RIGHT);
        prof->src[5][0] = s;
        asrc(&s, CTRL_AXIS_LEFT_X, +1);
        prof->src[5][1] = s;
        hsrc(&s, CTRL_HAT_DOWN);
        prof->src[6][0] = s;
        asrc(&s, CTRL_AXIS_LEFT_Y, +1);
        prof->src[6][1] = s;
        hsrc(&s, CTRL_HAT_LEFT);
        prof->src[7][0] = s;
        asrc(&s, CTRL_AXIS_LEFT_X, -1);
        prof->src[7][1] = s;
        tsrc(&s, CTRL_TRIGGER_LEFT);
        prof->src[8][0] = s; /* L2 */
        tsrc(&s, CTRL_TRIGGER_RIGHT);
        prof->src[9][0] = s; /* R2 */
        bsrc(&s, CTRL_BTN_LEFT_SHOULDER);
        prof->src[10][0] = s; /* L1 */
        bsrc(&s, CTRL_BTN_RIGHT_SHOULDER);
        prof->src[11][0] = s; /* R1 */
        bsrc(&s, CTRL_BTN_NORTH);
        prof->src[12][0] = s; /* Triangle */
        bsrc(&s, CTRL_BTN_EAST);
        prof->src[13][0] = s; /* Circle   */
        bsrc(&s, CTRL_BTN_SOUTH);
        prof->src[14][0] = s; /* Cross    */
        bsrc(&s, CTRL_BTN_WEST);
        prof->src[15][0] = s; /* Square   */
    }

    /* Port assignments: automatic by default (fill in discovery order). */
    cfg->port[0].port = 0;
    cfg->port[0].mode = 1;
    cfg->port[0].icon = CTRL_ICON_AUTOMATIC;
    cfg->port[1].port = 1;
    cfg->port[1].mode = 1;
    cfg->port[1].icon = CTRL_ICON_AUTOMATIC;
    cfg->port[0].identity[0] = '\0';
    cfg->port[1].identity[0] = '\0';
}

static int profile_equal(const ControlsProfile *a, const ControlsProfile *b)
{
    for (int d = 0; d < CTRL_DEST_COUNT; d++)
        for (int k = 0; k < CTRL_SLOT_COUNT; k++)
            if (!Controls_SourceEquals(&a->src[d][k], &b->src[d][k]))
                return 0;
    return 1;
}
int Controls_Equal(const ControlsConfig *a, const ControlsConfig *b)
{
    if (a->version != b->version || a->profile_count != b->profile_count || !profile_equal(&a->kb, &b->kb))
        return 0;
    for (int p = 0; p < 2; p++) {
        if (!profile_equal(&a->ctrl[p], &b->ctrl[p]) || a->port[p].mode != b->port[p].mode ||
            a->port[p].icon != b->port[p].icon || strcmp(a->port[p].identity, b->port[p].identity))
            return 0;
    }
    for (int p = 0; p < a->profile_count; p++)
        if (strcmp(a->profiles[p].identity, b->profiles[p].identity) ||
            a->profiles[p].icon != b->profiles[p].icon ||
            !profile_equal(&a->profiles[p].bindings, &b->profiles[p].bindings))
            return 0;
    return 1;
}
int Controls_SourceValid(int controller, const ControlSource *s)
{
    if (s->kind == CTRL_SRC_UNBOUND)
        return s->code == 0 && s->sign == 0;
    if (!controller)
        return s->kind == CTRL_SRC_KEY && s->sign == 0 && s->code > 0 && s->code < CTRL_KEY_COUNT &&
               !Controls_IsReservedKey(s->code, Controls_IsModifier(s->code));
    if (s->kind == CTRL_SRC_AXIS)
        return s->code > 0 && s->code < CTRL_AXIS_COUNT && (s->sign == 1 || s->sign == -1);
    if (s->sign)
        return 0;
    if (s->kind == CTRL_SRC_BUTTON)
        return s->code > 0 && s->code < CTRL_BTN_COUNT;
    if (s->kind == CTRL_SRC_TRIGGER)
        return s->code > 0 && s->code < CTRL_TRIGGER_COUNT;
    if (s->kind == CTRL_SRC_HAT)
        return s->code == 1 || s->code == 2 || s->code == 4 || s->code == 8;
    return 0;
}
int Controls_ProfileValid(const ControlsProfile *profile, int controller)
{
    for (int d = 0; d < CTRL_DEST_COUNT; d++)
        for (int k = 0; k < CTRL_SLOT_COUNT; k++) {
            const ControlSource *s = &profile->src[d][k];
            if (!Controls_SourceValid(controller, s) || (!controller && k && s->kind))
                return 0;
            if (!s->kind)
                continue;
            for (int i = 0; i < d * 2 + k; i++)
                if (Controls_SourceEquals(s, &profile->src[i / 2][i % 2]))
                    return 0;
        }
    return 1;
}
int Controls_ConfigValid(const ControlsConfig *cfg)
{
    if (cfg->version != CTRL_CONFIG_VERSION || cfg->profile_count < 0 ||
        cfg->profile_count > CTRL_PROFILE_MAX || !Controls_ProfileValid(&cfg->kb, 0))
        return 0;
    for (int p = 0; p < 2; p++) {
        const ControlsPort *port = &cfg->port[p];
        if (!Controls_ProfileValid(&cfg->ctrl[p], 1) || port->mode < 0 || port->mode > 2 || port->icon < 0 ||
            port->icon >= CTRL_ICON_COUNT || !memchr(port->identity, 0, sizeof(port->identity)) ||
            (port->mode == 2 && !port->identity[0]))
            return 0;
    }
    if (cfg->port[0].mode == 2 && cfg->port[1].mode == 2 &&
        !strcmp(cfg->port[0].identity, cfg->port[1].identity))
        return 0;
    for (int p = 0; p < cfg->profile_count; p++) {
        const ControlsDeviceProfile *pr = &cfg->profiles[p];
        if (!memchr(pr->identity, 0, sizeof(pr->identity)) || !pr->identity[0] || pr->icon < 0 ||
            pr->icon >= CTRL_ICON_COUNT || !Controls_ProfileValid(&pr->bindings, 1))
            return 0;
        for (int i = 0; i < p; i++)
            if (!strcmp(pr->identity, cfg->profiles[i].identity))
                return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Held-state evaluators                                              */
/* ------------------------------------------------------------------ */

/* mag is already signed in the bound direction (axis value * sign; positive
 * when pushed the bound way). act = activation, rel = 80% of activation. */
static int hysteresis(int *active, float mag, float act)
{
    const float rel = 0.8f * act;
    int next = *active ? (mag >= rel) : (mag >= act);
    *active = next;
    return next;
}

uint16_t Controls_EvalKeyboard(const ControlsProfile *kb, const ControlSource *keys_down, int n)
{
    if (!kb)
        return 0;
    uint16_t out = 0;
    for (int d = 0; d < CTRL_DEST_COUNT; d++) {
        for (int k = 0; k < CTRL_SLOT_COUNT; k++) {
            const ControlSource *s = &kb->src[d][k];
            if (s->kind != CTRL_SRC_KEY)
                continue;
            for (int i = 0; i < n; i++)
                if (keys_down[i].kind == CTRL_SRC_KEY && keys_down[i].code == s->code) {
                    out |= Controls_Actions[d].bit;
                    break;
                }
        }
    }
    return out;
}

uint16_t Controls_EvalController(const ControlsProfile *ctrl, const ControllerSnapshot *snap,
                                 ControlsEvaluator *eval)
{
    if (!ctrl || !snap)
        return 0;
    uint16_t out = 0;
    for (int d = 0; d < CTRL_DEST_COUNT; d++) {
        uint16_t bit = Controls_Actions[d].bit;
        for (int k = 0; k < CTRL_SLOT_COUNT; k++) {
            const ControlSource *s = &ctrl->src[d][k];
            int held = 0;
            if (!Controls_SourceValid(1, s))
                continue;
            switch (s->kind) {
            case CTRL_SRC_UNBOUND:
            case CTRL_SRC_KEY:
                break;
            case CTRL_SRC_BUTTON:
                /* Project-owned button codes are one-based bit positions. */
                held = (snap->buttons_down >> (s->code - 1)) & 1;
                break;
            case CTRL_SRC_AXIS: {
                float mag = snap->axis[s->code] * (float)s->sign;
                if (eval) {
                    int which = s->sign > 0;
                    held = hysteresis(&eval->axis_active[s->code][which], mag,
                                      eval->activation > 0 ? eval->activation : 1.0f / 3);
                } else
                    held = mag >= (1.0f / 3);
                break;
            }
            case CTRL_SRC_TRIGGER: {
                float mag = snap->trigger[s->code]; /* 0..1, always positive */
                if (eval)
                    held = hysteresis(&eval->trig_active[s->code], mag, 1.0f / 3);
                else
                    held = mag >= (1.0f / 3);
                break;
            }
            case CTRL_SRC_HAT:
                /* A hat token is a direction mask; it is held when all of its
                 * bits are currently down. */
                held = (s->code != 0) && ((snap->hat_down & (s->code & 0x0f)) == (s->code & 0x0f));
                break;
            }
            if (held)
                out |= bit;
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Bind / move / conflict                                           */
/* ------------------------------------------------------------------ */

int Controls_ConflictDest(const ControlsProfile *profile, const ControlSource *src)
{
    if (!src || !src->kind)
        return -1;
    for (int d = 0; d < CTRL_DEST_COUNT; d++) {
        int hit = 0;
        for (int k = 0; k < CTRL_SLOT_COUNT; k++) {
            if (Controls_SourceEquals(&profile->src[d][k], src)) {
                hit = 1;
                break;
            }
        }
        if (hit)
            return d;
    }
    return -1;
}

int Controls_SetSource(ControlsProfile *profile, int dest, int slot, const ControlSource *src)
{
    if (!profile || dest < 0 || dest >= CTRL_DEST_COUNT || slot < 0 || slot >= CTRL_SLOT_COUNT || !src)
        return 0;
    if (src->kind == CTRL_SRC_UNBOUND)
        return Controls_ClearSlot(profile, dest, slot);
    /* A source has one slot per profile. Clear its previous slot before moving it. */
    for (int d = 0; d < CTRL_DEST_COUNT; d++)
        for (int k = 0; k < CTRL_SLOT_COUNT; k++)
            if ((d != dest || k != slot) && Controls_SourceEquals(&profile->src[d][k], src))
                memset(&profile->src[d][k], 0, sizeof(*src));
    profile->src[dest][slot] = *src;
    return 1;
}

int Controls_MoveSource(ControlsProfile *profile, int dest, int slot, const ControlSource *src)
{
    /* Same move semantics as SetSource; kept as a distinct name so the UI
     * (and future "move to another slot") can be disambiguated from a plain
     * rebind. */
    return Controls_SetSource(profile, dest, slot, src);
}

int Controls_ClearSlot(ControlsProfile *profile, int dest, int slot)
{
    if (!profile || dest < 0 || dest >= CTRL_DEST_COUNT || slot < 0 || slot >= CTRL_SLOT_COUNT)
        return 0;
    memset(&profile->src[dest][slot], 0, sizeof(profile->src[dest][slot]));
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Capture state machine                                            */
/* ------------------------------------------------------------------ */

void Controls_CaptureBegin(ControlCapture *c, int target, int slot, uint64_t now_us)
{
    memset(c, 0, sizeof(*c));
    c->state = CAP_WAIT_NEUTRAL;
    c->target = target;
    c->slot = slot;
    c->result = CAPTURE_STILL;
    c->deadline_us = now_us + CAPTURE_TIMEOUT_US;
}

int Controls_CaptureConflict(ControlsProfile *profile, int slot, const ControlSource *candidate,
                             int target_dest, int *dest_out)
{
    int d = Controls_ConflictDest(profile, candidate);
    (void)slot;
    if (dest_out)
        *dest_out = d;
    if (d < 0)
        return 0;
    if (d == target_dest)
        return 0; /* already bound here; a rebind is not a conflict */
    return 1;
}

int Controls_CaptureStep(ControlCapture *c, ControlsProfile *profile, const ControlSource *src, int device_ok,
                         uint64_t now_us)
{
    (void)profile;
    if (c->state == CAP_IDLE)
        return CAPTURE_RESET;
    if (now_us >= c->deadline_us) {
        c->state = CAP_IDLE;
        return CAPTURE_RESET;
    }
    if (c->state == CAP_WAIT_NEUTRAL) {
        if (!src || !src->kind)
            c->state = CAP_AWAIT_INPUT;
        return CAPTURE_STILL;
    }
    if (c->state == CAP_CONFLICT) {
        /* The conflict is resolved by the window (it moves or cancels and
         * then calls Reset/Begin). The machine itself stays put. */
        return CAPTURE_STILL;
    }
    if (c->state != CAP_AWAIT_INPUT)
        return CAPTURE_RESET;
    if (src == NULL || src->kind == CTRL_SRC_UNBOUND) {
        /* Timeout is the only way out of a wait. */
        if (now_us >= c->deadline_us) {
            c->state = CAP_IDLE;
            return CAPTURE_RESET;
        }
        return CAPTURE_STILL;
    }
    /* Keyboard keys only carry meaning for keyboard captures; controller
     * tokens require the selected pad still to be connected. */
    if (src->kind != CTRL_SRC_KEY && !device_ok)
        return CAPTURE_REJECT_DEVICE;
    if (src->kind == CTRL_SRC_KEY && Controls_IsReservedKey(src->code, Controls_IsModifier(src->code)))
        return CAPTURE_REJECT_RESERVED;
    int conflict = Controls_ConflictDest(profile, src);
    if (conflict >= 0 && conflict != c->target) {
        c->state = CAP_CONFLICT;
        c->pending = *src;
        c->conflict_dest = conflict;
        return CAPTURE_CONFLICT;
    }
    c->pending = *src;
    c->state = CAP_IDLE;
    c->result = CAPTURE_DONE;
    return CAPTURE_DONE;
}
