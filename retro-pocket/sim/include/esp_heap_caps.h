#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, m, caps) calloc(n, m)
#define heap_caps_get_free_size(caps) (512 * 1024)
