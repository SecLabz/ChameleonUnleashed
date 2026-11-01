/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* 64 MHz CPU-cycle timings. Preserve integer arithmetic, strict acceptance
 * bounds and polling order. Detector tolerances do not replace nominal
 * ISO15693 timings. */
#define NFCV_RAM_FUNCTION __attribute__((section(".data_nfcv"), noinline, aligned(4)))
#define NFCV_CPU_CYCLES_PER_US 64u
#define NFCV_WINDOW_CLEANUP_CYCLES (2000u * NFCV_CPU_CYCLES_PER_US)
/* Signed constant: deadline comparisons intentionally use int32_t deltas. */
#define NFCV_WINDOW_WORK_GUARD_CYCLES 128000
#define NFCV_IDLE_WINDOW_US 1500000u
#define NFCV_CONNECTED_WINDOW_US 10000u
#define NFCV_DISCOVERY_WINDOW_US 25000u
#define NFCV_TX_WINDOW_GUARD_US 1500u
#define NFCV_ACTIVATE_TIMEOUT_CYCLES 64000u
#define NFCV_FIELD_LOSS_GUARD_CYCLES 128000u
#define NFCV_RX_SOF_TIMEOUT_CYCLES 640000u
#define NFCV_RX_FRAME_TIMEOUT_CYCLES 23040000u
#define NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES 6400u
#define NFCV_RX_1_OF_4_GAP_TIMEOUT_CYCLES 19200u
#define NFCV_RX_1_OF_256_GAP_TIMEOUT_CYCLES 704000u

/* A PPM tick is 604.13 CPU cycles. Half-denominator rounding is intentional. */
#define NFCV_PPM_TICK_CYCLES_NUMERATOR 60413u
#define NFCV_PPM_TICK_CYCLES_DENOMINATOR 100u
#define NFCV_PPM_TICK_ROUNDING 30206u
#define NFCV_RX_TICK_TOLERANCE_CYCLES 160u
#define NFCV_RX_1_OF_4_SOF_MIN_CYCLES 2830u
#define NFCV_RX_1_OF_4_SOF_MAX_CYCLES 3220u
#define NFCV_RX_1_OF_256_SOF_MIN_CYCLES 4037u
#define NFCV_RX_1_OF_256_SOF_MAX_CYCLES 4420u
#define NFCV_RX_SOF_CODING_SPLIT_CYCLES 3500u
#define NFCV_RX_MERGED_PULSE_MIN_CYCLES 1993u
#define NFCV_RX_MERGED_PULSE_MAX_CYCLES 2780u

#define NFCV_PPM_1_OF_4_SYMBOL_TICKS 8u
#define NFCV_PPM_1_OF_256_SOF_TICKS 8u
#define NFCV_PPM_1_OF_256_SYMBOL_TICKS 512u
#define NFCV_PPM_1_OF_4_LAST_POSITION 7u
#define NFCV_PPM_1_OF_256_LAST_POSITION 511u
#define NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE 4u
#define NFCV_PPM_EOF_POSITION 2u
#define NFCV_RX_MIN_FRAME_BYTES 4u

/* 64 MHz / (2 * 423.75 kHz): rational 75/76-cycle half periods. */
#define NFCV_TX_HALF_PERIOD_BASE_CYCLES 75u
#define NFCV_TX_HALF_PERIOD_FRACTION 437500u
#define NFCV_TX_HALF_PERIOD_DENOMINATOR 847500u
#define NFCV_TX_LONG_SEGMENT_CYCLES 24u
#define NFCV_TX_BIT_HALF_CYCLES 8u
#define NFCV_TX_LOW_RATE_MULTIPLIER 4u
#define NFCV_WAVEFORM_CYCLE_MASK 127u
#define NFCV_WAVEFORM_CARRIER_FLAG 128u
#define NFCV_WAVEFORM_SEGMENTS_PER_BYTE 16u
#define NFCV_WAVEFORM_FRAMING_SEGMENTS 8u

/* Detector timestamp to SOF start at 64 MHz. SOF begins with silence. */
#define NFCV_TX_RESPONSE_WAIT_CYCLES 20508u
/* Paired offsets preserve the carrier period while lengthening its low half. */
#define NFCV_TX_LOW_HALF_BIAS_CYCLES 8
