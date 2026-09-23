#define _POSIX_C_SOURCE 200809L
#include "controls_runtime.h"
#include "controls_config.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static ControlsConfig active;
static ControllerDevice devices[CONTROLS_DEVICES];
static int initialized, assigned[2] = {-1, -1};
static volatile sig_atomic_t blocked, gate = 1;
static unsigned char keys[CTRL_KEY_COUNT];
static ControlsEvaluator evaluators[2];
static volatile uint16_t keyboard_bits, pad_bits[2];
static volatile int connected[2];
static char load_error[256];
uint64_t ControlsRuntime_Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (unsigned)ts.tv_nsec / 1000;
}
void ControlsRuntime_Init(void)
{
    if (initialized)
        return;
    initialized = 1;
    if (ControlsConfig_Load(&active, load_error, sizeof(load_error)) < 0)
        fprintf(stderr, "memories-pc: %s\n", load_error);
}
const ControlsConfig *ControlsRuntime_Config(void)
{
    ControlsRuntime_Init();
    return &active;
}
const char *ControlsRuntime_Error(void) { return load_error; }
ControllerDevice *ControlsRuntime_Device(int i)
{
    return i >= 0 && i < CONTROLS_DEVICES ? &devices[i] : NULL;
}
static int explicit_device(const ControlsConfig *cfg, int p)
{
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (devices[i].connected && !strcmp(devices[i].identity, cfg->port[p].identity))
            return i;
    return -1;
}
static void resolve(const ControlsConfig *cfg, int result[2])
{
    for (int p = 0; p < 2; p++)
        result[p] = cfg->port[p].mode == 2 ? explicit_device(cfg, p) : -1;
    for (int p = 0; p < 2; p++)
        if (cfg->port[p].mode == 1) {
            int old = assigned[p];
            if (old >= 0 && devices[old].connected && result[1 - p] != old)
                result[p] = old;
            else
                for (int i = 0; i < CONTROLS_DEVICES; i++)
                    if (devices[i].connected && i != result[1 - p]) {
                        result[p] = i;
                        break;
                    }
        }
}
int ControlsRuntime_Assigned(const ControlsConfig *cfg, int p)
{
    int out[2];
    resolve(cfg, out);
    return p >= 0 && p < 2 ? out[p] : -1;
}
void ControlsRuntime_Reconcile(void)
{
    int out[2];
    ControlsRuntime_Init();
    resolve(&active, out);
    for (int p = 0; p < 2; p++)
        if (out[p] != assigned[p]) {
            assigned[p] = out[p];
            memset(&evaluators[p], 0, sizeof(evaluators[p]));
            gate = 1;
        }
}
static ControlsDeviceProfile *profile(ControlsConfig *cfg, int p, int create)
{
    int device;
    if (p < 0 || p >= 2)
        return NULL; /* cfg->port has two entries */
    device = ControlsRuntime_Assigned(cfg, p);
    const char *id = cfg->port[p].mode == 2 ? cfg->port[p].identity
                     : device >= 0          ? devices[device].identity
                                            : "";
    if (!*id)
        return NULL;
    for (int i = 0; i < cfg->profile_count; i++)
        if (!strcmp(id, cfg->profiles[i].identity))
            return &cfg->profiles[i];
    if (!create || cfg->profile_count == CTRL_PROFILE_MAX)
        return NULL;
    ControlsDeviceProfile *pr = &cfg->profiles[cfg->profile_count++];
    memset(pr, 0, sizeof(*pr));
    snprintf(pr->identity, sizeof(pr->identity), "%s", id);
    pr->bindings = cfg->ctrl[p];
    pr->icon = cfg->port[p].icon;
    return pr;
}
ControlsProfile *ControlsRuntime_Profile(ControlsConfig *cfg, int p, int create)
{
    ControlsDeviceProfile *pr = profile(cfg, p, create);
    return pr ? &pr->bindings : &cfg->ctrl[p];
}
CtrlIconStyle *ControlsRuntime_Style(ControlsConfig *cfg, int p, int create)
{
    ControlsDeviceProfile *pr = profile(cfg, p, create);
    return pr ? &pr->icon : &cfg->port[p].icon;
}
int ControlsRuntime_Apply(const ControlsConfig *cfg, char *error, unsigned size)
{
    if (!ControlsConfig_Save(cfg, error, size))
        return 0;
    active = *cfg;
    load_error[0] = 0;
    ControlsRuntime_Gate();
    ControlsRuntime_Reconcile();
    return 1;
}
void ControlsRuntime_Key(int key, int down)
{
    if (key > 0 && key < CTRL_KEY_COUNT)
        keys[key] = down != 0;
}
void ControlsRuntime_ResetKeys(void)
{
    memset(keys, 0, sizeof(keys));
    ControlsRuntime_Gate();
}
int ControlsRuntime_Keys(ControlSource *out)
{
    int n = 0;
    for (int k = 1; k < CTRL_KEY_COUNT; k++)
        if (keys[k])
            out[n++] = (ControlSource){CTRL_SRC_KEY, (uint16_t)k, 0};
    return n;
}
void ControlsRuntime_Gate(void)
{
    sigset_t all, previous;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &previous);
    gate = 1;
    keyboard_bits = pad_bits[0] = pad_bits[1] = 0;
    memset(evaluators, 0, sizeof(evaluators));
    sigprocmask(SIG_SETMASK, &previous, NULL);
}
void ControlsRuntime_Block(int b)
{
    blocked = b;
    ControlsRuntime_Gate();
}
int ControlsRuntime_Blocked(void) { return blocked || gate; }
uint16_t ControlsRuntime_Keyboard(void) { return keyboard_bits; }
uint16_t ControlsRuntime_Pad(int p) { return p >= 0 && p < 2 ? pad_bits[p] : 0; }
int ControlsRuntime_Connected(int p) { return p >= 0 && p < 2 ? connected[p] : 0; }
int ControlsRuntime_Sources(const ControllerDevice *d, ControlSource *out, int capacity, int neutral)
{
    int n = 0;
    float a = d->threshold > 0 ? d->threshold : 1.0f / 3;
    if (neutral)
        a *= 0.8f;
#define ADD(kind_, code_, sign_)                                                                             \
    do {                                                                                                     \
        if (n < capacity)                                                                                    \
            out[n++] = (ControlSource){kind_, (uint16_t)(code_), sign_};                                     \
    } while (0)
    if (!d->connected)
        return 0;
    for (int b = 1; b < CTRL_BTN_COUNT; b++)
        if (d->snapshot.buttons_down & (1u << (b - 1)))
            ADD(CTRL_SRC_BUTTON, b, 0);
    for (int h = 1; h <= 8; h *= 2)
        if (d->snapshot.hat_down & h)
            ADD(CTRL_SRC_HAT, h, 0);
    for (int i = 1; i < CTRL_AXIS_COUNT; i++) {
        if (d->snapshot.axis[i] > a)
            ADD(CTRL_SRC_AXIS, i, 1);
        if (d->snapshot.axis[i] < -a)
            ADD(CTRL_SRC_AXIS, i, -1);
    }
    for (int i = 1; i < CTRL_TRIGGER_COUNT; i++)
        if (d->snapshot.trigger[i] > (neutral ? 0.8f / 3 : 1.0f / 3))
            ADD(CTRL_SRC_TRIGGER, i, 0);
#undef ADD
    return n;
}
void ControlsRuntime_Update(void)
{
    ControlSource held[CTRL_KEY_COUNT], sources[64];
    uint16_t kb, pads[2] = {0};
    int conn[2], neutral;
    sigset_t all, previous;
    ControlsRuntime_Reconcile();
    int n = ControlsRuntime_Keys(held);
    neutral = n == 0;
    kb = Controls_EvalKeyboard(&active.kb, held, n);
    for (int p = 0; p < 2; p++) {
        int i = assigned[p];
        conn[p] = i >= 0;
        if (i < 0)
            continue;
        if (ControlsRuntime_Sources(&devices[i], sources, 64, 1))
            neutral = 0;
        evaluators[p].activation = devices[i].threshold;
        pads[p] = Controls_EvalController(ControlsRuntime_Profile(&active, p, 0), &devices[i].snapshot,
                                          &evaluators[p]);
    }
    if (gate && neutral && !blocked)
        gate = 0;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &previous);
    keyboard_bits = blocked || gate ? 0 : kb;
    for (int p = 0; p < 2; p++) {
        pad_bits[p] = blocked || gate ? 0 : pads[p];
        connected[p] = conn[p];
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}
