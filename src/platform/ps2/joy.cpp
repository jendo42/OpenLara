#include "joy.h"
#include "system.h"

#include <libpad.h>

static joy_callback_t g_callback = NULL;
static char g_padBuf[NUM_PADS][256] __attribute__((aligned(64)));
static int g_padState[NUM_PADS];
static float g_padVibration[NUM_PADS][2];
static bool g_padUpdateVibration[NUM_PADS];

bool osWaitPad(int port, int slot)
{
	while(1) {
		int state = padGetState(port, slot);
		switch(state) {
			case PAD_STATE_FINDPAD:
			case PAD_STATE_FINDCTP1:
			case PAD_STATE_EXECCMD:
				// wait for operation to be done
				break;
			case PAD_STATE_STABLE:
				// pad is ready
				return true;
			case PAD_STATE_DISCONN:
			case PAD_STATE_ERROR:
				// pad is not ready
				return false;
		}
	}
}

bool osReinitPad(int port, int slot)
{
    padSetMainMode(port, slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    osWaitPad(port, slot);
    padEnterPressMode(port, slot);
	osWaitPad(port, slot);
    I("PAD", "%u%u reinitialized!", port, slot);
}

// input
bool osJoyReady(int index)
{
    if (index >= NUM_PADS) {
        return false;
    }
	int state = padGetState(0, index);
	switch(state) {
		case PAD_STATE_FINDPAD:
		case PAD_STATE_FINDCTP1:
		case PAD_STATE_EXECCMD:
			// wait for operation to be done
            return false;
		case PAD_STATE_STABLE:
			// pad is ready
			return true;
		case PAD_STATE_DISCONN:
		case PAD_STATE_ERROR:
			// pad is not ready
			return false;
	}
}

void osJoyVibrate(int i, float L, float R)
{
    if (i >= NUM_PADS || i < 0) {
        return;
    }
    g_padVibration[i][0] = L;
    g_padVibration[i][1] = R;
    g_padUpdateVibration[i] = true;
}

void joyInit(joy_callback_t callback)
{
    g_callback = callback;
	osLoadModule("rom0:SIO2MAN");
	osLoadModule("rom0:PADMAN");
    padInit(0);
    for (int i = 0; i < NUM_PADS; i++) {
        g_padState[i] = -1;
        g_padUpdateVibration[i] = false;
	    padPortOpen(0, i, &g_padBuf[i][0]);
    }
}

void joyUpdate()
{
    for (int i = 0; i < NUM_PADS; i++) {
        int padState = padGetState(0, i);
        int lastPadState = g_padState[i];
        if (lastPadState != padState) {
            I("PAD", "#%d: State Changed: %d -> %d", i, lastPadState, padState);
            if (lastPadState == PAD_STATE_DISCONN && padState == PAD_STATE_EXECCMD && osWaitPad(0, i)) {
                // pad was connected, reinitialize
                osReinitPad(0, i);
            }
        }
        if (padState == PAD_STATE_STABLE) {
            struct padButtonStatus pad;
            int ret = padRead(0, i, &pad);
            if (ret && g_callback) {
                g_callback(i, pad);
            }
            if (g_padUpdateVibration[i]) {
                // TODO: send vibration to controller
                char act[6] = {
                    g_padVibration[i][0] > 0.0f,
                    g_padVibration[i][1] * 255,
                    0, 0, 0, 0
                };
                padSetActDirect(0, i, act);
                g_padUpdateVibration[i] = false;
            }
        }
        g_padState[i] = padState;
    }
}
