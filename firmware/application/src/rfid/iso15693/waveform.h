/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "timing.h"

/* SOF and EOF each have four segments. Derive Manchester halves directly
 * from the frame so full-memory replies need no expanded waveform buffer. */
static inline __attribute__((always_inline)) uint8_t nfcv_waveform_segment(
        const uint8_t *frame, unsigned length, unsigned segment, unsigned rate) {
    if (segment < 4u) {
        unsigned cycles = segment < 2u ? NFCV_TX_LONG_SEGMENT_CYCLES : NFCV_TX_BIT_HALF_CYCLES;
        return cycles * rate | ((segment & 1u) ? NFCV_WAVEFORM_CARRIER_FLAG : 0u);
    }
    segment -= 4u;
    if (segment < length * NFCV_WAVEFORM_SEGMENTS_PER_BYTE) {
        bool one = (frame[segment >> 4u] >> ((segment >> 1u) & 7u)) & 1u;
        bool on = one == (bool)(segment & 1u);
        return NFCV_TX_BIT_HALF_CYCLES * rate | (on ? NFCV_WAVEFORM_CARRIER_FLAG : 0u);
    }
    segment -= length * NFCV_WAVEFORM_SEGMENTS_PER_BYTE;
    unsigned cycles = segment < 2u ? NFCV_TX_BIT_HALF_CYCLES : NFCV_TX_LONG_SEGMENT_CYCLES;
    return cycles * rate | ((segment & 1u) ? 0u : NFCV_WAVEFORM_CARRIER_FLAG);
}
