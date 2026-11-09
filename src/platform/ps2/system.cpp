#include <sifrpc.h>
#include <timer.h>
#include <kernel.h>
#include <loadfile.h>

#include "system.h"
#include "sprintf.h"

// timing
static int osStartTime = 0;
static const int osTimerFreq = kBUSCLK;

extern "C"
int __wrap_printf(const char *format, ...)
{
	int ret;
    va_list arglist;
    va_start(arglist, format);
	ret = stbsp_vprintf(format, arglist);
	va_end(arglist);
	return ret;
}

extern "C"
int __wrap_sprintf(char *str, const char *format, ...)
{
	int ret;
    va_list arglist;
    va_start(arglist, format);
	ret = stbsp_vsprintf(str, format, arglist);
	va_end(arglist);
	return ret;
}

bool osLoadModule(const char *name, void *img, size_t size)
{
    int ret;
	I("SYS", "Loading module '%s'", name);
    if (img && size) {
        ret = SifExecModuleBuffer(img, size, 0, NULL, NULL);
    } else {
        ret = SifLoadModule(name, 0, NULL);
    }

	if (ret < 0) {
		I("SYS", "Error: '%s' failed with code %d", name, ret);
		return false;
	}

	return true;
}

// multi-threading
#define OS_SEMA_MAX 256 // > any semaphore ID the kernel will hand out
static volatile int g_mtxOwner[OS_SEMA_MAX];
static int          g_mtxDepth[OS_SEMA_MAX];

void* osMutexInit()
{
    ee_sema_t sema = { .max_count = 1, .init_count = 1, .option = 0 };
    int id = CreateSema(&sema);
    if (id < 0 || id >= OS_SEMA_MAX) {
        I("SYS", "FATAL: CreateSema failed: %d", id);
        while (1) { osBreak(); }
    }
    g_mtxOwner[id] = -1;
    g_mtxDepth[id] = 0;
    return (void *)(id + 1);
}

void osMutexFree(void *obj)
{
    DeleteSema((int)obj - 1);
}

void osMutexLock(void *obj)
{
    int id  = (int)obj - 1;
    int tid = GetThreadId();
    if (g_mtxOwner[id] == tid) {   // re-entry by the owner
        g_mtxDepth[id]++;
        return;
    }
    WaitSema(id);
    g_mtxOwner[id] = tid;
    g_mtxDepth[id] = 1;
}

void osMutexUnlock(void *obj)
{
    int id = (int)obj - 1;
    if (--g_mtxDepth[id] == 0) {
        g_mtxOwner[id] = -1;
        SignalSema(id);
    }
}

int osGetTimeMS()
{
    u64 time = GetTimerSystemTime();
    return (int)(time * 1000 / osTimerFreq - osStartTime);
}

void osInit()
{
	SifInitRpc(0);
    StartTimerSystemTime();
    osStartTime = osGetTimeMS();
    ChangeThreadPriority(GetThreadId(), 0x70);
}
