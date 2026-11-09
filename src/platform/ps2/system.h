#pragma once 

#include <stddef.h>
#include <stdio.h>
#include <memory.h>

#ifndef LOG
#define LOG printf
#endif

#define I(cat, f, ...) LOG("[I] %s @ %s: " f "\n", cat, __func__ __VA_OPT__(,) __VA_ARGS__)

#define osBreak() __asm__ volatile ("break")

bool osLoadModule(const char *name, void *img = NULL, size_t size = 0);
void osInit();
