#pragma once
#define NUM_PADS 1

#include <stddef.h>
#include <stdbool.h>

typedef void (*joy_callback_t)(int i, const struct padButtonStatus &status);

bool osWaitPad(int port, int slot);
bool osReinitPad(int port, int slot);
bool osJoyReady(int index);
void osJoyVibrate(int i, float L, float R);

void joyInit(joy_callback_t callback);
void joyUpdate();
