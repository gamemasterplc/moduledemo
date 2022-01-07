#pragma once

#include <ultra64.h>

#define ROMREAD_BUF_SIZE 16384

void RomRead(void *dst, u32 src, u32 len);