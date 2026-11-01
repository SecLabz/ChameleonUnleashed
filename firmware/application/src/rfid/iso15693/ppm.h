/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "frame.h"
#include "timing.h"

/* These expressions deliberately expand in place: even forced-inline functions
 * changed the receiver's register allocation under the validated ARM compiler.
 * Pass side-effect-free elapsed/tick values, never volatile register reads.
 * Parenthesized arguments preserve the existing unsigned timing arithmetic. */
#define NFCV_PPM_TICK_FROM_CYCLES(elapsed)                                                         \
    (((elapsed) * NFCV_PPM_TICK_CYCLES_DENOMINATOR + NFCV_PPM_TICK_ROUNDING) /                     \
     NFCV_PPM_TICK_CYCLES_NUMERATOR)
#define NFCV_PPM_CYCLES_FROM_TICK(tick)                                                            \
    ((tick) * NFCV_PPM_TICK_CYCLES_NUMERATOR / NFCV_PPM_TICK_CYCLES_DENOMINATOR)
#define NFCV_PPM_SYMBOL_INDEX(tick, coding_1_of_256)                                               \
    ((coding_1_of_256) && (tick) >= NFCV_PPM_1_OF_256_SOF_TICKS                                    \
         ? ((tick) - NFCV_PPM_1_OF_256_SOF_TICKS) / NFCV_PPM_1_OF_256_SYMBOL_TICKS + 1u            \
         : (tick) / NFCV_PPM_1_OF_4_SYMBOL_TICKS)
#define NFCV_PPM_PULSE_POSITION(tick, coding_1_of_256)                                             \
    ((coding_1_of_256) && (tick) >= NFCV_PPM_1_OF_256_SOF_TICKS                                    \
         ? ((tick) - NFCV_PPM_1_OF_256_SOF_TICKS) % NFCV_PPM_1_OF_256_SYMBOL_TICKS                 \
         : (tick) % NFCV_PPM_1_OF_4_SYMBOL_TICKS)
#define NFCV_PPM_MAX_SYMBOLS(coding_1_of_256)                                                      \
    ((coding_1_of_256) ? ISO15693_FRAME_MAX_BYTES                                                  \
                       : ISO15693_FRAME_MAX_BYTES * NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE)
#define NFCV_PPM_MIN_FRAME_SYMBOLS(coding_1_of_256)                                                \
    ((coding_1_of_256) ? NFCV_RX_MIN_FRAME_BYTES                                                   \
                       : NFCV_RX_MIN_FRAME_BYTES * NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE)
#define NFCV_PPM_FRAME_BYTES(symbols, coding_1_of_256)                                             \
    ((coding_1_of_256) ? (symbols) : (symbols) / NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE)
