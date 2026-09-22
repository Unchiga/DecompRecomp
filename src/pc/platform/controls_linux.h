#ifndef MEMORIES_CONTROLS_LINUX_H
#define MEMORIES_CONTROLS_LINUX_H
#include "controls_runtime.h"
/* Stable Linux identity shared by SDL and evdev when the device path is evdev. */
int ControlsLinux_Identity(const char *path, char *identity, unsigned capacity);
#endif
