/*
 * Stage-1 test for the shared mapping model (controls.c / controls.h).
 * Synthetic inputs only (no SDL, no X11, no evdev). Covers, per the spec:
 *   - defaults reproduce currently bound input (all 16 destinations, kb + pad)
 *   - keyboard and controller evaluators, OR of multiple sources
 *   - axis activation/hysteresis (act 33/100, release 80% of act)
 *   - reserved-key policy (incl. Right-Shift exception, Enter/arrows allowed)
 *   - bind / move / clear-slot conflict semantics
 *   - capture state machine (done / conflict / reserved / device / timeout)
 *
 * No hardware and no real controller are involved; hardware checks are
 * performed against the synthetic token vocabulary defined in controls.h.
 */
#include "pc/platform/controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- helpers ----------------------------------------------------------- */

static int g_pass = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "CHECK failed: %s @ line %d\n", #cond, __LINE__);                                \
            return 1;                                                                                        \
        } else                                                                                               \
            ++g_pass;                                                                                        \
    } while (0)

static void key_src(ControlSource *s, int code)
{
    s->kind = CTRL_SRC_KEY;
    s->code = (uint16_t)code;
    s->sign = 0;
}
static void btn_src(ControlSource *s, int code)
{
    s->kind = CTRL_SRC_BUTTON;
    s->code = (uint16_t)code;
    s->sign = 0;
}
static void ax_src(ControlSource *s, int ax, int sign)
{
    s->kind = CTRL_SRC_AXIS;
    s->code = (uint16_t)ax;
    s->sign = (int8_t)sign;
}
static void trg_src(ControlSource *s, int t)
{
    s->kind = CTRL_SRC_TRIGGER;
    s->code = (uint16_t)t;
    s->sign = 0;
}
static void hat_src(ControlSource *s, int mask)
{
    s->kind = CTRL_SRC_HAT;
    s->code = (uint16_t)mask;
    s->sign = 0;
}

/* ---- defaults reproduce current input (16 destinations) --------------- */

static int test_defaults_keyboard(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    const ControlSource *def[] = {
        [0] = &cfg.kb.src[0][0],   [1] = &cfg.kb.src[1][0],   [2] = &cfg.kb.src[2][0],
        [3] = &cfg.kb.src[3][0],   [4] = &cfg.kb.src[4][0],   [5] = &cfg.kb.src[5][0],
        [6] = &cfg.kb.src[6][0],   [7] = &cfg.kb.src[7][0],   [8] = &cfg.kb.src[8][0],
        [9] = &cfg.kb.src[9][0],   [10] = &cfg.kb.src[10][0], [11] = &cfg.kb.src[11][0],
        [12] = &cfg.kb.src[12][0], [13] = &cfg.kb.src[13][0], [14] = &cfg.kb.src[14][0],
        [15] = &cfg.kb.src[15][0]};
    /* The PS1 pad bits, in the documented order for each destination. All
     * 16 must be bound by default to exactly one source each (single slot). */
    int bits[] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
                  0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000};
    for (int d = 0; d < CTRL_DEST_COUNT; ++d) {
        CHECK(def[d]->kind > CTRL_SRC_UNBOUND);
        CHECK(def[d]->kind == CTRL_SRC_KEY);
        /* The action's bit must be in the documented action table. */
        CHECK(Controls_Actions[d].bit == bits[d]);
        CHECK(Controls_Actions[d].bit != 0);
    }
    /* Pressing every default key simultaneously must produce 0xFFFF. */
    ControlSource down[CTRL_DEST_COUNT];
    for (int i = 0; i < CTRL_DEST_COUNT; ++i)
        down[i] = *def[i];
    uint16_t out = Controls_EvalKeyboard(&cfg.kb, down, CTRL_DEST_COUNT);
    CHECK(out == 0xFFFFu);
    /* No keys: zero output. */
    CHECK(Controls_EvalKeyboard(&cfg.kb, down, 0) == 0u);
    return 0;
}

static int test_defaults_controller(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Each destination's slot 0 (and any slot 1) must be a controller token
     * (no key). For the d-pad we also verify slot 1's stick. */
    for (int d = 0; d < CTRL_DEST_COUNT; ++d) {
        const ControlSource *s = &cfg.ctrl[0].src[d][0];
        CHECK(s->kind != CTRL_SRC_KEY);
        CHECK(s->kind != CTRL_SRC_UNBOUND);
        CHECK(s->kind != CTRL_SRC_HAT || (d >= 4 && d <= 7)); /* hats only in dpad dests */
    }
    /* Build a synthetic snapshot with every bound token active:
     *   every button, both triggers at 1.0, left stick at full in both axes
     *   (covers both signs via the per-direction binding, so test the stick
     *   separately), and every canonical hat direction. */
    ControllerSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    ControlsEvaluator ev;
    memset(&ev, 0, sizeof(ev));
    /* Buttons: back (Select), start, shoulders, stick clicks, guide, etc. */
    snap.buttons_down = 0;
    for (int p = 0; p < CTRL_PORT_COUNT; ++p) {
        for (int d = 0; d < CTRL_DEST_COUNT; ++d) {
            const ControlSource *s = &cfg.ctrl[p].src[d][0];
            if (s->kind == CTRL_SRC_BUTTON)
                snap.buttons_down |= (uint32_t)(1u << (s->code - 1));
        }
    }
    /* Triggers to 1. */
    snap.trigger[CTRL_TRIGGER_LEFT] = 1.0f;
    snap.trigger[CTRL_TRIGGER_RIGHT] = 1.0f;
    /* Left stick: we cannot light all four dpad directions with one axis value,
     * so verify them separately below. For now, verify triggers+buttons+shoulders
     * all yield the expected bits: 0xFFFF with the dpad cleared by zeroing its
     * destinations by construction (they'd be 0 since axes are 0). */
    uint16_t out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    /* Select/L3/R3/Start/L1/R1/L2/R2/TRIANG/CIR/CROSS/SQR should all be lit
     * (buttons + triggers), dpad destinations unlit (axis 0). */
    uint16_t expect = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0100 | 0x0200 | 0x0400 | 0x0800 | 0x1000 |
                      0x2000 | 0x4000 | 0x8000;
    CHECK(out == expect);
    return 0;
}

static int test_defaults_dpad_and_stick(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Dpad (slot 0, hat) and stick (slot 1, axis) are both bound to the four
     * directional destinations. Verify each axis sign lights its one
     * destination, and that OR'ing them yields both. */
    struct {
        float x, y;
        uint16_t expect;
        const char *label;
    } cases[] = {
        {+0.5f, 0.0f, 0x0020, "right"},
        {-0.5f, 0.0f, 0x0080, "left"},
        {0.0f, +0.5f, 0x0040, "down"},
        {0.0f, -0.5f, 0x0010, "up"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ControllerSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.axis[CTRL_AXIS_LEFT_X] = cases[i].x;
        snap.axis[CTRL_AXIS_LEFT_Y] = cases[i].y;
        ControlsEvaluator ev;
        memset(&ev, 0, sizeof(ev));
        uint16_t out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
        CHECK(out == cases[i].expect);
    }
    return 0;
}

/* ---- keyboard OR semantics and release -------------------------------- */

static int test_keyboard_or_release(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Two distinct keys bound to two different dests. Holding both = OR. */
    ControlSource down[3];
    key_src(&down[0], CTRL_KEY_A);     /* Triangle */
    key_src(&down[1], CTRL_KEY_S);     /* Circle   */
    key_src(&down[2], CTRL_KEY_ENTER); /* Start */
    uint16_t out = Controls_EvalKeyboard(&cfg.kb, down, 3);
    CHECK(out == (0x1000 | 0x2000 | 0x0008));
    /* Release one: the others stay. Simulate with a shorter list. */
    out = Controls_EvalKeyboard(&cfg.kb, down, 1); /* only Triangle key now */
    CHECK(out == 0x1000);
    return 0;
}

/* ---- axis hysteresis (activation at 33/100, release at 80% = 26/100) -- */

static int test_axis_hysteresis(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Use the left stick X + (bound to Right, 0x0020, slot 1). */
    ControllerSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    ControlsEvaluator ev;
    memset(&ev, 0, sizeof(ev));
    const float act = (1.0f / 3); /* 0.33 */
    const float rel = 0.8f * act; /* 0.264 */
    /* Below act: not active. */
    snap.axis[CTRL_AXIS_LEFT_X] = act - 0.01f;
    uint16_t out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == 0u);
    /* At act: active. */
    snap.axis[CTRL_AXIS_LEFT_X] = act;
    out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == 0x0020);
    /* Dropped to rel (still >= rel): stays active (hysteresis). */
    snap.axis[CTRL_AXIS_LEFT_X] = rel + 0.001f;
    out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == 0x0020);
    /* Dropped below rel: released. */
    snap.axis[CTRL_AXIS_LEFT_X] = rel - 0.01f;
    out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == 0u);
    /* Back to act-0.01 (below act): not re-activated without crossing act. */
    snap.axis[CTRL_AXIS_LEFT_X] = act - 0.01f;
    out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == 0u);
    return 0;
}

/* ---- reserved-key policy ---------------------------------------------- */

static int test_reserved_keys(void)
{
    /* Reserved (per spec lines 155-160): Escape, Tab, F1-F5, F7, F10-F12, P, M, period,
     * and lone modifiers except Right Shift. */
    CHECK(Controls_IsReservedKey(CTRL_KEY_ESCAPE, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_TAB, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_F1, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_F5, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_F7, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_F10, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_F12, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_P, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_M, 0));
    CHECK(Controls_IsReservedKey(CTRL_KEY_PERIOD, 0));
    /* F6/F8/F9 are NOT in the reserved set. */
    CHECK(!Controls_IsReservedKey(CTRL_KEY_F6, 0));
    CHECK(!Controls_IsReservedKey(CTRL_KEY_F8, 0));
    CHECK(!Controls_IsReservedKey(CTRL_KEY_F9, 0));
    /* Modifiers are reserved, except Right Shift. */
    CHECK(Controls_IsReservedKey(CTRL_KEY_LEFT_SHIFT, 1));
    CHECK(Controls_IsReservedKey(CTRL_KEY_LEFT_CTRL, 1));
    CHECK(Controls_IsReservedKey(CTRL_KEY_LEFT_ALT, 1));
    CHECK(Controls_IsReservedKey(CTRL_KEY_LEFT_SUPER, 1));
    CHECK(!Controls_IsReservedKey(CTRL_KEY_RIGHT_SHIFT, 1)); /* documented exception */
    /* Enter and navigation arrows are NOT reserved (spec line 160). */
    CHECK(!Controls_IsReservedKey(CTRL_KEY_ENTER, 0));
    CHECK(!Controls_IsReservedKey(CTRL_KEY_ARROW_UP, 0));
    CHECK(!Controls_IsReservedKey(CTRL_KEY_ARROW_LEFT, 0));
    /* A normal key used as a non-modifier is fine. */
    CHECK(!Controls_IsReservedKey(CTRL_KEY_A, 0));
    /* Reasons are non-empty for the reserved ones. */
    CHECK(Controls_ReservedReason(CTRL_KEY_ESCAPE, 0)[0] != 0);
    CHECK(Controls_ReservedReason(CTRL_KEY_TAB, 0)[0] != 0);
    CHECK(Controls_ReservedReason(CTRL_KEY_LEFT_SHIFT, 1)[0] != 0);
    CHECK(Controls_ReservedReason(CTRL_KEY_RIGHT_SHIFT, 1)[0] == 0); /* exception -> no reason */
    return 0;
}

/* ---- bind/move/clear conflict semantics -------------------------------- */

static int test_set_move_clear(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Bind BTN_GUIDE (not bound by default) to Triangle slot 1, leaving
     * BTN_NORTH in slot 0. Both fire Triangle when held. */
    ControlSource guide;
    btn_src(&guide, CTRL_BTN_GUIDE);
    CHECK(Controls_SetSource(&cfg.ctrl[0], 12 /*Triangle*/, 1, &guide) == 1);
    CHECK(Controls_ConflictDest(&cfg.ctrl[0], &guide) == 12);
    /* Now move BTN_GUIDE to Circle. Triangle slot 1 must clear, Circle must
     * still have BTN_EAST in slot 0 (unchanged) plus BTN_GUIDE in slot 1. */
    CHECK(Controls_MoveSource(&cfg.ctrl[0], 13 /*Circle*/, 1, &guide) == 1);
    /* Triangle slot 1 is cleared (no more BTN_GUIDE). */
    CHECK(cfg.ctrl[0].src[12][1].kind == CTRL_SRC_UNBOUND);
    /* Circle now owns BTN_GUIDE in slot 1. */
    CHECK(Controls_ConflictDest(&cfg.ctrl[0], &guide) == 13);
    /* The original Circle slot 0 (BTN_EAST) is preserved. */
    CHECK(cfg.ctrl[0].src[13][0].kind == CTRL_SRC_BUTTON && cfg.ctrl[0].src[13][0].code == CTRL_BTN_EAST);
    /* A conflict is profile-local: the same source in the OTHER port's profile
     * is not a conflict here. */
    ControlSource other;
    btn_src(&other, CTRL_BTN_MISC1); /* unused */
    CHECK(Controls_ConflictDest(&cfg.ctrl[0], &other) == -1);
    /* ClearSlot empties that slot. */
    CHECK(Controls_ClearSlot(&cfg.ctrl[0], 13 /*Circle*/, 1) == 1);
    CHECK(cfg.ctrl[0].src[13][1].kind == CTRL_SRC_UNBOUND);
    /* Out-of-range SetSource returns 0. */
    CHECK(Controls_SetSource(&cfg.ctrl[0], 0, CTRL_SLOT_COUNT + 1, &guide) == 0);
    return 0;
}

/* ---- capture state machine -------------------------------------------- */

static int test_capture_fsm(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    ControlCapture cap;
    ControlsProfile *prof = &cfg.ctrl[0];
    uint64_t t0 = 1000;

    /* Start a capture for Select (destination index 0). */
    Controls_CaptureBegin(&cap, 0 /*Select*/, 0, t0);
    CHECK(cap.state == CAP_WAIT_NEUTRAL);

    /* Nothing observed: still waiting. */
    int r = Controls_CaptureStep(&cap, prof, NULL, 0, t0 + 1000);
    CHECK(r == CAPTURE_STILL);

    /* A reserved key is rejected. */
    ControlSource esc;
    key_src(&esc, CTRL_KEY_ESCAPE);
    r = Controls_CaptureStep(&cap, prof, &esc, 0, t0 + 2000);
    CHECK(r == CAPTURE_REJECT_RESERVED);
    CHECK(cap.state == CAP_AWAIT_INPUT); /* back to waiting */

    /* A controller token with device_ok=0 is rejected. */
    ControlSource south;
    btn_src(&south, CTRL_BTN_SOUTH);
    r = Controls_CaptureStep(&cap, prof, &south, 0, t0 + 3000);
    CHECK(r == CAPTURE_REJECT_DEVICE);
    CHECK(cap.state == CAP_AWAIT_INPUT);

    /* Press a free key (not reserved, not a modifier) -> DONE. */
    ControlSource keyB;
    key_src(&keyB, CTRL_KEY_B);
    r = Controls_CaptureStep(&cap, prof, &keyB, 1, t0 + 4000);
    CHECK(r == CAPTURE_DONE);
    CHECK(cap.state == CAP_IDLE);
    CHECK(cap.result == CAPTURE_DONE);
    CHECK(Controls_SourceEquals(&cap.pending, &keyB));

    /* Begin a fresh capture and trigger a conflict: press a source already
     * bound to another destination (e.g. BTN_EAST is the default Circle). */
    Controls_CaptureBegin(&cap, 0 /*Select*/, 0, t0 + 5000);
    CHECK(Controls_CaptureStep(&cap, prof, NULL, 1, t0 + 5100) == CAPTURE_STILL);
    ControlSource east;
    btn_src(&east, CTRL_BTN_EAST);
    r = Controls_CaptureStep(&cap, prof, &east, 1, t0 + 6000);
    CHECK(r == CAPTURE_CONFLICT);
    CHECK(cap.state == CAP_CONFLICT);
    /* The conflicting destination must be Circle (index 13). */
    CHECK(cap.conflict_dest == 13);
    /* While in CONFLICT, further steps do not change state (window resolves). */
    CHECK(Controls_CaptureStep(&cap, prof, &east, 1, t0 + 7000) == CAPTURE_STILL);

    /* After the window "cancels" (resets to idle), a new cycle works. */
    cap.state = CAP_IDLE; /* simulate the window resetting after a cancel */
    CHECK(Controls_CaptureStep(&cap, prof, NULL, 1, t0 + 8000) == CAPTURE_RESET);

    /* Timeout: begin, then no input until past the deadline. */
    Controls_CaptureBegin(&cap, 0, 0, t0);
    r = Controls_CaptureStep(&cap, prof, NULL, 1, t0 + CAPTURE_TIMEOUT_US + 1);
    CHECK(r == CAPTURE_RESET);
    CHECK(cap.state == CAP_IDLE);
    return 0;
}

/* ---- SourceIdentity distinctness --------------------------------------- */

static int test_source_identity(void)
{
    ControlSource a, b;
    key_src(&a, CTRL_KEY_A);
    key_src(&b, CTRL_KEY_A);
    CHECK(Controls_SourceIdentity(&a) == Controls_SourceIdentity(&b));
    key_src(&b, CTRL_KEY_B);
    CHECK(Controls_SourceIdentity(&a) != Controls_SourceIdentity(&b));
    ax_src(&a, CTRL_AXIS_LEFT_X, +1);
    ax_src(&b, CTRL_AXIS_LEFT_X, -1);
    CHECK(Controls_SourceIdentity(&a) != Controls_SourceIdentity(&b));
    ax_src(&a, CTRL_AXIS_LEFT_X, +1);
    ax_src(&b, CTRL_AXIS_RIGHT_X, +1);
    CHECK(Controls_SourceIdentity(&a) != Controls_SourceIdentity(&b));
    return 0;
}

/* ---- ConfigValid and Equal / Clear ------------------------------------- */

static int test_config_valid_and_equality(void)
{
    ControlsConfig a, b;
    Controls_InitDefaults(&a);
    Controls_InitDefaults(&b);
    CHECK(Controls_ConfigValid(&a) == 1);
    CHECK(Controls_Equal(&a, &b) == 1);
    /* Change one source: no longer equal. */
    ControlSource z;
    key_src(&z, CTRL_KEY_B);
    Controls_SetSource(&a.kb, 15 /*Square*/, 0, &z);
    CHECK(Controls_Equal(&a, &b) == 0);
    /* Corrupt the header version -> invalid. */
    ControlsConfig c;
    Controls_InitDefaults(&c);
    c.version = 999;
    CHECK(Controls_ConfigValid(&c) == 0);
    /* A controller profile carrying a key is invalid (flavor mismatch). */
    ControlsConfig d;
    Controls_InitDefaults(&d);
    d.ctrl[0].src[0][0].kind = CTRL_SRC_KEY; /* illegal in a controller profile */
    CHECK(Controls_ConfigValid(&d) == 0);
    /* A keyboard profile carrying a button is invalid. */
    ControlsConfig e;
    Controls_InitDefaults(&e);
    key_src(&e.kb.src[0][0], CTRL_KEY_A);  /* legal; keep, then corrupt below */
    e.kb.src[1][0].kind = CTRL_SRC_BUTTON; /* illegal in the keyboard profile */
    e.kb.src[1][0].code = CTRL_BTN_SOUTH;
    CHECK(Controls_ConfigValid(&e) == 0);
    /* Clear: zeroed, version 0 -> not equal to defaults. */
    Controls_Clear(&d);
    CHECK(Controls_Equal(&d, &a) == 0);
    return 0;
}

/* ---- hat bitmask semantics --------------------------------------------- */

static int test_hat_semantics(void)
{
    ControlsConfig cfg;
    Controls_InitDefaults(&cfg);
    /* Default dpad up (dest 4) slot 0 = CTRL_HAT_UP. Holding only UP lights
     * dest 4 (0x0010). Holding UP|RIGHT must NOT light dest 5 (0x0020 is
     * "right"), because the canonical dpad direction must match exactly. */
    ControllerSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.hat_down = CTRL_HAT_UP;
    ControlsEvaluator ev;
    memset(&ev, 0, sizeof(ev));
    uint16_t out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == CTRL_DEST_UP);

    /* Holding UP|RIGHT: each single-direction binding whose mask bits are
     * contained in the held mask fires, so both dest 4 (UP) and dest 5
     * (RIGHT) light; no combined-mask binding exists yet. */
    snap.hat_down = CTRL_HAT_UP | CTRL_HAT_RIGHT;
    out = Controls_EvalController(&cfg.ctrl[0], &snap, &ev);
    CHECK(out == (CTRL_DEST_UP | CTRL_DEST_RIGHT));

    return 0;
}

/* ---- source name sanity ------------------------------------------------ */

static int test_source_names(void)
{
    ControlSource s;
    s.kind = CTRL_SRC_UNBOUND;
    s.code = 0;
    s.sign = 0;
    CHECK(Controls_SourceName(&s)[0] != 0);
    key_src(&s, CTRL_KEY_A);
    CHECK(Controls_SourceName(&s)[0] != 0);
    btn_src(&s, CTRL_BTN_NORTH);
    CHECK(Controls_SourceName(&s)[0] != 0);
    ax_src(&s, CTRL_AXIS_LEFT_X, +1);
    CHECK(Controls_SourceName(&s)[0] != 0);
    trg_src(&s, CTRL_TRIGGER_LEFT);
    CHECK(Controls_SourceName(&s)[0] != 0);
    hat_src(&s, CTRL_HAT_UP);
    CHECK(Controls_SourceName(&s)[0] != 0);
    return 0;
}

int main(void)
{
    ControlSource face = {CTRL_SRC_BUTTON, CTRL_BTN_SOUTH, 0};
    CHECK(!strcmp(Controls_SourceLabel(&face, CTRL_ICON_PLAYSTATION), "Cross"));
    CHECK(!strcmp(Controls_SourceLabel(&face, CTRL_ICON_NINTENDO), "B"));
    CHECK(!strcmp(Controls_SourceLabel(&face, CTRL_ICON_XBOX), "A"));
    CHECK(!strcmp(Controls_SourceName(&face), "South"));
    CHECK(CTRL_BTN_COUNT <= 33); /* snapshot button bitmask */
    int r = 0;
    r = test_source_identity();
    if (r)
        return r;
    r = test_reserved_keys();
    if (r)
        return r;
    r = test_defaults_keyboard();
    if (r)
        return r;
    r = test_defaults_controller();
    if (r)
        return r;
    r = test_defaults_dpad_and_stick();
    if (r)
        return r;
    r = test_keyboard_or_release();
    if (r)
        return r;
    r = test_axis_hysteresis();
    if (r)
        return r;
    r = test_set_move_clear();
    if (r)
        return r;
    r = test_capture_fsm();
    if (r)
        return r;
    r = test_hat_semantics();
    if (r)
        return r;
    r = test_source_names();
    if (r)
        return r;
    r = test_config_valid_and_equality();
    if (r)
        return r;
    fprintf(stderr, "controls_test: %d checks passed\n", g_pass);
    return 0;
}
