#pragma once
#include <stdint.h>
typedef struct { uint32_t CYCCNT, CTRL; } test_dwt_t;
typedef struct { uint32_t DEMCR; } test_debug_t;
extern test_dwt_t test_dwt;
extern test_debug_t test_debug;
#define DWT (&test_dwt)
#define CoreDebug (&test_debug)
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define CoreDebug_DEMCR_TRCENA_Msk 1u
