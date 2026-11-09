#define STB_SPRINTF_IMPLEMENTATION 1
#include "sprintf.h"

#include <unistd.h>

static char *stbsp_printf_cb(const char *buf, void *user, int len)
{
	write(1, buf, len);
	return (char *)buf;
}

int stbsp_vprintf(const char *format, va_list arglist)
{
	char buffer[STB_SPRINTF_MIN];
	return stbsp_vsprintfcb(stbsp_printf_cb, NULL, buffer, format, arglist);
}