#pragma once
#include <stdint.h>
typedef struct { uint32_t CYCCNT, CTRL; } mock_dwt_t;
extern mock_dwt_t mock_dwt;
#define DWT (&mock_dwt)
#define DWT_CTRL_CYCCNTENA_Msk 1u
typedef struct { uint32_t DEMCR; } mock_core_debug_t;
extern mock_core_debug_t mock_core_debug;
#define CoreDebug (&mock_core_debug)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
