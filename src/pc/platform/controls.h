#ifndef MEMORIES_PC_CONTROLS_H
#define MEMORIES_PC_CONTROLS_H
#include <stdint.h>

/* Shared, backend-independent mapping model for the PC port's Controls
 * window and runtime evaluation (notes/controls-menu-workflow.md).
 *
 * The PS1 destination bits mirror the platform pad getters: PS1 digital pad
 * bits, active high. The source model uses project-owned tokens; SDL
 * scancodes, X11/XKB keycodes and Linux input-event-codes never cross this
 * boundary. Both backends normalize raw input into these tokens first.
 *
 * Keyboard belongs to Player 1. Controllers have per-device profiles and
 * fallback profiles for each port. Distinct sources may hold one destination;
 * moving a source clears its old slot. Evaluation ORs all held sources.
 *
 * Every structure here is a plain, copyable POD value; no allocation, no
 * I/O. The device I/O and main-thread ownership live in the backends. */

/* PS1 destination buttons (active-high wire bits).
 *   Select 0x0001, L3 0x0002, R3 0x0004, Start 0x0008,
 *   Dpad up/right/down/left 0x0010/0x0020/0x0040/0x0080,
 *   L2 0x0100, R2 0x0200, L1 0x0400, R1 0x0800,
 *   Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000. */
#define CTRL_DEST_SELECT 0x0001u
#define CTRL_DEST_L3 0x0002u
#define CTRL_DEST_R3 0x0004u
#define CTRL_DEST_START 0x0008u
#define CTRL_DEST_UP 0x0010u
#define CTRL_DEST_RIGHT 0x0020u
#define CTRL_DEST_DOWN 0x0040u
#define CTRL_DEST_LEFT 0x0080u
#define CTRL_DEST_L2 0x0100u
#define CTRL_DEST_R2 0x0200u
#define CTRL_DEST_L1 0x0400u
#define CTRL_DEST_R1 0x0800u
#define CTRL_DEST_TRIANGLE 0x1000u
#define CTRL_DEST_CIRCLE 0x2000u
#define CTRL_DEST_CROSS 0x4000u
#define CTRL_DEST_SQUARE 0x8000u
#define CTRL_DEST_MASK 0xffffu

enum {
    CTRL_DEST_COUNT = 16, /* Select..Square == the 16 active PS1 pad bits */
    CTRL_PORT_COUNT = 2,  /* Player 1 and Player 2 */
    CTRL_SLOT_COUNT = 2,  /* two controller slots per destination; 1 for keyboard */
    CTRL_SRC_HAT_COUNT = 8,
    CTRL_SRC_AXIS_COUNT = 4,
    CTRL_SRC_TRIGGER_COUNT = 2,
    CTRL_IDENTITY_MAX = 256,
    /* Hysteresis: activation is a fraction of the normalized (|max| == 1)
     * range; release is 80% of that. The controller backend normalizes. */
    CTRL_AXIS_ACT_NUM = 1,
    CTRL_AXIS_ACT_DEN = 3,
    CTRL_AXIS_REL_NUM = 4,
    CTRL_AXIS_REL_DEN = 15
};

/* Icon style: changes only the controller face/shoulder visual labels. */
typedef enum {
    CTRL_ICON_AUTOMATIC = 0,
    CTRL_ICON_XBOX = 1,
    CTRL_ICON_PLAYSTATION = 2,
    CTRL_ICON_NINTENDO = 3,
    CTRL_ICON_GENERIC = 4,
    CTRL_ICON_COUNT = 5
} CtrlIconStyle;

/* Normalized sources. The evaluator never inspects backend enums. */
typedef enum {
    CTRL_SRC_UNBOUND = 0,
    CTRL_SRC_KEY,
    CTRL_SRC_BUTTON,
    CTRL_SRC_AXIS,
    CTRL_SRC_TRIGGER,
    CTRL_SRC_HAT
} CtrlSourceKind;

/* Keyboard tokens (physical keys, so a binding survives layout changes). */
typedef enum {
    CTRL_KEY_NONE = 0,
    CTRL_KEY_A,
    CTRL_KEY_B,
    CTRL_KEY_C,
    CTRL_KEY_D,
    CTRL_KEY_E,
    CTRL_KEY_F,
    CTRL_KEY_G,
    CTRL_KEY_H,
    CTRL_KEY_I,
    CTRL_KEY_J,
    CTRL_KEY_K,
    CTRL_KEY_L,
    CTRL_KEY_M,
    CTRL_KEY_N,
    CTRL_KEY_O,
    CTRL_KEY_P,
    CTRL_KEY_Q,
    CTRL_KEY_R,
    CTRL_KEY_S,
    CTRL_KEY_T,
    CTRL_KEY_U,
    CTRL_KEY_V,
    CTRL_KEY_W,
    CTRL_KEY_X,
    CTRL_KEY_Y,
    CTRL_KEY_Z,
    CTRL_KEY_KP_1,
    CTRL_KEY_KP_2,
    CTRL_KEY_KP_3,
    CTRL_KEY_KP_4,
    CTRL_KEY_KP_5,
    CTRL_KEY_KP_6,
    CTRL_KEY_KP_7,
    CTRL_KEY_KP_8,
    CTRL_KEY_KP_9,
    CTRL_KEY_KP_0,
    CTRL_KEY_KP_COMMA,
    CTRL_KEY_KP_DOT,
    CTRL_KEY_KP_SLASH,
    CTRL_KEY_KP_ASTERISK,
    CTRL_KEY_KP_MINUS,
    CTRL_KEY_KP_PLUS,
    CTRL_KEY_KP_ENTER,
    CTRL_KEY_KP_EQUAL,
    CTRL_KEY_SPACE,
    CTRL_KEY_ESCAPE,
    CTRL_KEY_TAB,
    CTRL_KEY_BACKSPACE,
    CTRL_KEY_ENTER,
    CTRL_KEY_MINUS,
    CTRL_KEY_EQUAL,
    CTRL_KEY_LBRACKET,
    CTRL_KEY_RBRACKET,
    CTRL_KEY_BACKSLASH,
    CTRL_KEY_SEMICOLON,
    CTRL_KEY_APOSTROPHE,
    CTRL_KEY_GRAVE,
    CTRL_KEY_COMMA,
    CTRL_KEY_PERIOD,
    CTRL_KEY_SLASH,
    CTRL_KEY_F1,
    CTRL_KEY_F2,
    CTRL_KEY_F3,
    CTRL_KEY_F4,
    CTRL_KEY_F5,
    CTRL_KEY_F6,
    CTRL_KEY_F7,
    CTRL_KEY_F8,
    CTRL_KEY_F9,
    CTRL_KEY_F10,
    CTRL_KEY_F11,
    CTRL_KEY_F12,
    CTRL_KEY_ARROW_UP,
    CTRL_KEY_ARROW_DOWN,
    CTRL_KEY_ARROW_LEFT,
    CTRL_KEY_ARROW_RIGHT,
    CTRL_KEY_INSERT,
    CTRL_KEY_HOME,
    CTRL_KEY_END,
    CTRL_KEY_PAGE_UP,
    CTRL_KEY_PAGE_DOWN,
    CTRL_KEY_LEFT_SHIFT,
    CTRL_KEY_RIGHT_SHIFT,
    CTRL_KEY_LEFT_CTRL,
    CTRL_KEY_RIGHT_CTRL,
    CTRL_KEY_LEFT_ALT,
    CTRL_KEY_RIGHT_ALT,
    CTRL_KEY_LEFT_SUPER,
    CTRL_KEY_RIGHT_SUPER,
    CTRL_KEY_1,
    CTRL_KEY_2,
    CTRL_KEY_3,
    CTRL_KEY_4,
    CTRL_KEY_5,
    CTRL_KEY_6,
    CTRL_KEY_7,
    CTRL_KEY_8,
    CTRL_KEY_9,
    CTRL_KEY_0,
    CTRL_KEY_DELETE,
    CTRL_KEY_CAPS_LOCK,
    CTRL_KEY_NUM_LOCK,
    CTRL_KEY_PRINT_SCREEN,
    CTRL_KEY_SCROLL_LOCK,
    CTRL_KEY_PAUSE,
    CTRL_KEY_COUNT
} CtrlKeyCode;
#define CTRL_SRC_KEY_COUNT (CTRL_KEY_COUNT - 1)

/* Controller button tokens (stable across backends: SDL3 GameButton vocab). */
typedef enum {
    CTRL_BTN_NONE = 0,
    CTRL_BTN_SOUTH,
    CTRL_BTN_EAST,
    CTRL_BTN_WEST,
    CTRL_BTN_NORTH,
    CTRL_BTN_BACK,
    CTRL_BTN_GUIDE,
    CTRL_BTN_START,
    CTRL_BTN_LEFT_SHOULDER,
    CTRL_BTN_RIGHT_SHOULDER,
    CTRL_BTN_LEFT_STICK,
    CTRL_BTN_RIGHT_STICK,
    CTRL_BTN_DPAD_UP,
    CTRL_BTN_DPAD_DOWN,
    CTRL_BTN_DPAD_LEFT,
    CTRL_BTN_DPAD_RIGHT,
    CTRL_BTN_MISC1,
    CTRL_BTN_MISC2,
    CTRL_BTN_COUNT
} CtrlButtonCode;
#define CTRL_SRC_BUTTON_COUNT (CTRL_BTN_COUNT - 1)

/* Analog axis / trigger tokens. `sign` is -1 or +1 for axes, 0 for triggers. */
typedef enum {
    CTRL_AXIS_NONE = 0,
    CTRL_AXIS_LEFT_X,
    CTRL_AXIS_LEFT_Y,
    CTRL_AXIS_RIGHT_X,
    CTRL_AXIS_RIGHT_Y,
    CTRL_AXIS_COUNT
} CtrlAxisCode;

typedef enum {
    CTRL_TRIGGER_NONE = 0,
    CTRL_TRIGGER_LEFT,
    CTRL_TRIGGER_RIGHT,
    CTRL_TRIGGER_COUNT
} CtrlTriggerCode;

/* Hat directions: the standard 8-way bitmask (0 = centred / no hat). */
#define CTRL_HAT_UP 0x01
#define CTRL_HAT_RIGHT 0x02
#define CTRL_HAT_DOWN 0x04
#define CTRL_HAT_LEFT 0x08
#define CTRL_HAT_CENTERED 0x00

typedef struct {
    CtrlSourceKind kind;
    uint16_t code;
    int8_t sign; /* -1, +1, or 0 (not applicable) */
} ControlSource;

/* Backend-neutral snapshot of one connected controller. Axis values are
 * already normalized to [-1, +1] by the backend (0 = centred; digital axes /
 * triggers are -1/0/+1). `buttons_down` is a bitmask of CtrlButton codes.
 * `hat_down` is a bitmask of CTRL_HAT_* directions. */
typedef struct {
    float axis[CTRL_AXIS_COUNT];
    float trigger[CTRL_TRIGGER_COUNT]; /* 0..1 */
    uint32_t buttons_down;
    uint8_t hat_down;
} ControllerSnapshot;

/* A named destination in UI order (top of the mapping table first). */
#define CTRL_NAME_MAX 12
typedef struct {
    uint16_t bit;
    char name[CTRL_NAME_MAX];
} ControlsAction;
extern const ControlsAction Controls_Actions[CTRL_DEST_COUNT];

/* One device+port profile: destination -> up to two sources. */
typedef struct {
    ControlSource src[CTRL_DEST_COUNT][CTRL_SLOT_COUNT];
} ControlsProfile;

typedef struct {
    int port;                         /* 0 = Player 1, 1 = Player 2 */
    int mode;                         /* 0: None, 1: Automatic, 2: explicit */
    char identity[CTRL_IDENTITY_MAX]; /* chosen device; empty for automatic */
    CtrlIconStyle icon;
} ControlsPort;

#define CTRL_PROFILE_MAX 16
typedef struct {
    char identity[CTRL_IDENTITY_MAX];
    ControlsProfile bindings;
    CtrlIconStyle icon;
} ControlsDeviceProfile;

typedef struct {
    int version;                           /* CTRL_CONFIG_VERSION */
    ControlsProfile kb;                    /* Player 1's keyboard (single) */
    ControlsProfile ctrl[CTRL_PORT_COUNT]; /* P1 and P2 controller profiles */
    ControlsPort port[CTRL_PORT_COUNT];
    int profile_count;
    ControlsDeviceProfile profiles[CTRL_PROFILE_MAX];
} ControlsConfig;
#define CTRL_CONFIG_VERSION 1

/* Capture UI state machine (the window drives this frame by frame). */
typedef enum { CAP_IDLE = 0, CAP_WAIT_NEUTRAL, CAP_AWAIT_INPUT, CAP_CONFLICT } CaptureState;

/* A pending capture. `target` is the destination being rebind; `slot` the
 * slot within it. `device_ok` is supplied by the window each step: 1 only
 * when the selected controller is still connected. */
typedef struct {
    CaptureState state;
    int target; /* destination index */
    int slot;
    ControlSource pending; /* the source being considered (valid at DONE/CONFLICT) */
    int conflict_dest;     /* destination already owning `pending` (CONFLICT only) */
    int result;            /* result of the last step (CAPTURE_* codes) */
    uint64_t deadline_us;  /* 0 = no timeout active */
} ControlCapture;

/* Held-state evaluator scratch (axis/release hysteresis). The window keeps
 * one per (profile, port) and passes the same structure to every step. */
typedef struct {
    int axis_active[CTRL_AXIS_COUNT][2]; /* [axis][0]=neg dir, [1]=pos dir */
    int trig_active[CTRL_TRIGGER_COUNT];
    float activation; /* zero uses 1/3; evdev supplies 1/2 for sticks */
} ControlsEvaluator;

/* Capture step results. */
enum {
    CAPTURE_STILL = 0,    /* keep waiting, same state */
    CAPTURE_DONE = 1,     /* `pending` holds the source; state = CAP_IDLE */
    CAPTURE_CONFLICT = 2, /* state = CAP_CONFLICT; window offers Move/Cancel */
    CAPTURE_REJECT_RESERVED = 3,
    CAPTURE_REJECT_DEVICE = 4,
    CAPTURE_REJECT_DISABLED = 5,
    CAPTURE_RESET = 6 /* cancelled/aborted; state = CAP_IDLE */
};
#define CAPTURE_TIMEOUT_US 10000000ULL /* 10 s */

/* ---------------------------------------------------------------- *
 *  API
 * ---------------------------------------------------------------- */

void Controls_InitDefaults(ControlsConfig *cfg);
void Controls_Clear(ControlsConfig *cfg);
int Controls_Equal(const ControlsConfig *a, const ControlsConfig *b);
int Controls_ProfileValid(const ControlsProfile *profile, int controller);
int Controls_SourceValid(int controller, const ControlSource *src);
int Controls_ConfigValid(const ControlsConfig *cfg);

const char *Controls_KeyName(CtrlKeyCode code);  /* canonical storage name */
const char *Controls_KeyLabel(CtrlKeyCode code); /* current keyboard layout */
void Controls_SetKeyLabel(int code, const char *label);
const char *Controls_SourceName(const ControlSource *src);
int Controls_SourceEquals(const ControlSource *a, const ControlSource *b);
/* Profile-local identity used for conflict detection across the two slots. */
uint32_t Controls_SourceIdentity(const ControlSource *src);

/* Reserved-key / modifier policy for keyboard capture. */
int Controls_IsReservedKey(int key, int is_modifier);
int Controls_IsModifier(int key);
const char *Controls_ReservedReason(int key, int is_modifier);

/* Held-state evaluators. `eval` carries hysteresis state for axes/triggers;
 * pass the same struct across frames. Keyboard evaluation is stateless and
 * ignores `eval`. The `profile` argument is the single keyboard/controller
 * profile for the device being evaluated. */
uint16_t Controls_EvalKeyboard(const ControlsProfile *kb, const ControlSource *keys_down, int n_keys_down);
uint16_t Controls_EvalController(const ControlsProfile *ctrl, const ControllerSnapshot *snap,
                                 ControlsEvaluator *eval);

/* Bind / move. Returns 1 on success, 0 on out-of-range arguments.
 * Set overwrites `dest`'s `slot`; if `src` already owned another destination
 * in the same profile (any slot) it is moved there (the old slot clears).
 * Move removes `src` from any destination in the profile and binds it to
 * `new_dest`'s `slot`. */
int Controls_SetSource(ControlsProfile *profile, int dest, int slot, const ControlSource *src);
int Controls_MoveSource(ControlsProfile *profile, int dest, int slot, const ControlSource *src);
int Controls_ClearSlot(ControlsProfile *profile, int dest, int slot);
/* Returns the destination index (0..CTRL_DEST_COUNT-1) that currently owns
 * `src` in this profile, or -1 if none. Checks all slots. */
int Controls_ConflictDest(const ControlsProfile *profile, const ControlSource *src);

/* Capture step machine. `profile` is the profile being edited; `slot` in the
 * structure names the slot. The window passes the just-observed source
 * (`src`; NULL when neutral) and whether a controller is the allowed and
 * connected device (`device_ok`; 0 for keyboard capture, or a dropped pad).
 * `now_us` enforces the deadline in both neutral and listening states.
 * Returns the step result for the UI to act on. */
void Controls_CaptureBegin(ControlCapture *c, int target, int slot, uint64_t now_us);
int Controls_CaptureStep(ControlCapture *c, ControlsProfile *profile, const ControlSource *src, int device_ok,
                         uint64_t now_us);

/* Conflict-detection helper for the capture UI. */
int Controls_CaptureConflict(ControlsProfile *profile, int slot, const ControlSource *candidate,
                             int target_dest, int *dest_out);

#endif
