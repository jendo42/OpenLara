#include "audio.h"
#include "system.h"

#include <kernel.h>
#include <audsrv.h>

static sndfill_t g_sndfill = NULL;
static int g_snd_frame_size = 0;
static volatile bool g_snd = false;
static int exit_sema = -1;
static int audio_sema = -1;
static int audio_thread = -1;
static u8 audio_buffer[AUDIO_PERIOD] ALIGNED(64);
static u8 sndThreadStack[64 * 1024] ALIGNED(16);

static int sndFillBuff(void* arg)
{
    SignalSema((int)arg);
    return 0;
}

static void sndThread(void *arg)
{
    while (g_snd) {
        while (g_snd && audsrv_available() >= AUDIO_PERIOD) {
            g_sndfill(audio_buffer, AUDIO_PERIOD / g_snd_frame_size);
            audsrv_play_audio((const char *)audio_buffer, AUDIO_PERIOD);
        }
        WaitSema(audio_sema);
    }
    audsrv_stop_audio();
    SignalSema(exit_sema);
    ExitDeleteThread();
}

void sndInit(int frameSize, sndfill_t sndfill, int prio)
{
    g_sndfill = sndfill;
    g_snd_frame_size = frameSize;
    if (!osLoadModule("host:freesd.irx", NULL, 0)) {
        return;
    }
    if (!osLoadModule("host:audsrv.irx", NULL, 0)) {
        return;
    }

    ee_sema_t sema = {
        .max_count = 1,
        .init_count = 0,
        .option = 0
    };
    audio_sema = CreateSema(&sema);
    if (audio_sema < 0) {
        I("AUD", "audsrv_init: CreateSema failed: %d", audio_sema);
        return;
    }
    exit_sema = CreateSema(&sema);
    if (exit_sema < 0) {
        I("AUD", "audsrv_init: CreateSema failed: %d", exit_sema);
        return;
    }

    int r = audsrv_init();
    I("AUD", "audsrv_init: %d", r);

    struct audsrv_fmt_t fmt = {
        .freq = 44100,
        .bits = 16,
        .channels = 2,
    };
    r = audsrv_set_format(&fmt);
    I("AUD", "audsrv_set_format: %d", r);

    r = audsrv_set_volume(MAX_VOLUME);
    I("AUD", "audsrv_set_volume: %d", r);

    r = audsrv_on_fillbuf(AUDIO_PERIOD, sndFillBuff, (void *)audio_sema);
    I("AUD", "audsrv_on_fillbuf: %d", r);

    I("AUD", "audsrv_available: %d", audsrv_available());
    I("AUD", "audsrv_queued: %d", audsrv_queued());

    g_snd = true;

    ee_thread_t th = {
        .func=(void *)&sndThread,
		.stack=sndThreadStack,
		.stack_size=sizeof(sndThreadStack),
		.gp_reg=&_gp,
		.initial_priority=prio,
		.attr = 0,
        .option = 0
    };
    audio_thread = CreateThread(&th);
    StartThread(audio_thread, NULL);
}

void sndDeinit()
{
    if (audio_thread < 0) {
        return;
    }
    g_snd = false;
    SignalSema(audio_sema);
    WaitSema(exit_sema);
}
