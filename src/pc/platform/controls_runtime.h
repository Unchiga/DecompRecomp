#ifndef MEMORIES_CONTROLS_RUNTIME_H
#define MEMORIES_CONTROLS_RUNTIME_H
#include "controls.h"
#define CONTROLS_DEVICES 32
/* Registry slots are session handles, never serialized. */
typedef struct {
    int connected, ambiguous;
    char identity[CTRL_IDENTITY_MAX], name[160];
    CtrlIconStyle style;
    ControllerSnapshot snapshot;
    float threshold;
} ControllerDevice;
void ControlsRuntime_Init(void);
uint64_t ControlsRuntime_Now(void);
const ControlsConfig *ControlsRuntime_Config(void);
int ControlsRuntime_Apply(const ControlsConfig *cfg, char *error, unsigned size);
const char *ControlsRuntime_Error(void);
ControllerDevice *ControlsRuntime_Device(int index);
int ControlsRuntime_Assigned(const ControlsConfig *cfg, int port);
void ControlsRuntime_Reconcile(void);
ControlsProfile *ControlsRuntime_Profile(ControlsConfig *cfg, int port, int create);
CtrlIconStyle *ControlsRuntime_Style(ControlsConfig *cfg, int port, int create);
void ControlsRuntime_Key(int key, int down);
void ControlsRuntime_ResetKeys(void);
int ControlsRuntime_Keys(ControlSource *out);
void ControlsRuntime_Block(int block);
void ControlsRuntime_Gate(void);
int ControlsRuntime_Blocked(void);
uint16_t ControlsRuntime_Keyboard(void);
uint16_t ControlsRuntime_Pad(int port);
int ControlsRuntime_Connected(int port);
void ControlsRuntime_Update(void);
/* Snapshot source enumeration uses release threshold for neutral gating. */
int ControlsRuntime_Sources(const ControllerDevice *device, ControlSource *out, int capacity, int neutral);
#endif
