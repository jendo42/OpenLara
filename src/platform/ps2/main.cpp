#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <new>

#include <libpad.h>

#include "game.h"

#include "system.h"
#include "joy.h"
#include "audio.h"

static vec2 padGetAnalogStick(unsigned char x, unsigned char y)
{
    vec2 stick = (vec2(float(x), float(y)) - 127.0f) / 127.0f;
    if (fabsf(stick.x) < 0.1f) {
        stick.x = 0.0f;
	}
    if (fabsf(stick.y) < 0.1f) {
        stick.y = 0.0f;
    }
    return stick;
}

static void joyCallback(int i, const struct padButtonStatus &pad)
{
    auto btns = (~pad.btns & 0xFFFF);
    auto left = padGetAnalogStick(pad.ljoy_h, pad.ljoy_v);
    auto right = padGetAnalogStick(pad.rjoy_h, pad.rjoy_v);

    //I("PAD", "btns: 0x%.4X", btns);
    //I("PAD", "left: (%u, %u);", (int)pad.ljoy_h, (int)pad.ljoy_v);
    //I("PAD", "buttons: %X; left: (%f, %f); right (%f, %f)", btns, left.x, left.y, right.x, right.y);
    //Input::setJoyPos(i, jkL,        left);
    //Input::setJoyPos(i, jkR,        right);
    Input::setJoyDown(i, jkUp,     (btns & PAD_UP));
    Input::setJoyDown(i, jkDown,   (btns & PAD_DOWN));
    Input::setJoyDown(i, jkLeft,   (btns & PAD_LEFT));
    Input::setJoyDown(i, jkRight,  (btns & PAD_RIGHT));
    Input::setJoyDown(i, jkA,      (btns & PAD_CROSS));
    Input::setJoyDown(i, jkB,      (btns & PAD_CIRCLE));
    Input::setJoyDown(i, jkX,      (btns & PAD_SQUARE));
    Input::setJoyDown(i, jkY,      (btns & PAD_TRIANGLE));
    Input::setJoyDown(i, jkLB,     (btns & PAD_L1));
    Input::setJoyDown(i, jkLT,     (btns & PAD_L2));
    Input::setJoyDown(i, jkRB,     (btns & PAD_R1));
    Input::setJoyDown(i, jkRT,     (btns & PAD_R2));
    Input::setJoyDown(i, jkStart,  (btns & PAD_START));
    Input::setJoyDown(i, jkSelect, (btns & PAD_SELECT));
}

static void sndFill(void *buff, int frames)
{
    Sound::fill((Sound::Frame*)buff, frames);
}

int main(int argc, char *argv[])
{
    // PCSX2 passes the full host path in argv[0]; use the host root instead
    if (argc > 0 && strncmp(argv[0], "host:", 5) == 0) {
        chdir("host:");
    }

    osInit();

    cacheDir[0] = 0;
    saveDir[0] = 0;
    contentDir[0] = 0;

    Sound::channelsCount = 0;

    sndInit(sizeof(Sound::Frame), sndFill, 0x30);
    joyInit(joyCallback);

    Game::init();

    while (!Core::isQuit) {
        joyUpdate();
        Game::update();
        Game::render();
    }

    sndDeinit();
    Game::deinit();

    // TODO: exit to PS2 OSD

    return 0;
}