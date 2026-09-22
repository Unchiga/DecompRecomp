#ifndef MEMORIES_CONTROLS_CONFIG_H
#define MEMORIES_CONTROLS_CONFIG_H
#include "controls.h"
/* 1 loaded, 0 missing, -1 malformed/I/O, -2 unsupported version. */
int ControlsConfig_Load(ControlsConfig *config, char *error, unsigned capacity);
int ControlsConfig_Save(const ControlsConfig *config, char *error, unsigned capacity);
void ControlsConfig_Token(ControlSource source, char *out, unsigned capacity);
#endif
