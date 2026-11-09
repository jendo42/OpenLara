#pragma once

#define AUDIO_PERIOD (2048)
#define AUDIO_LATENCY (3 * AUDIO_PERIOD)

typedef void (*sndfill_t)(void *buffer, int count);

void sndInit(int frameSize, sndfill_t sndfill, int prio);
void sndDeinit();
