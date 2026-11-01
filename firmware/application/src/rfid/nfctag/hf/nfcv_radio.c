/* SPDX-License-Identifier: GPL-3.0-only */
#include "iso15693/commands.h"
#include "ble_main.h"
#include "bsp_wdt.h"
#include "hw_connect.h"
#include "nfcv_radio.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrfx_nfct.h"
#include "rf_timeslot.h"
#include "iso15693/session.h"
#include "usb_main.h"
#include <string.h>
#include "iso15693/timing.h"
#include "iso15693/ppm.h"
#include "iso15693/waveform.h"
static bool enabled;
static uint32_t window_end;
extern volatile bool g_is_ble_connected;
static uint32_t last_field_cycles, sense_started_cycles;
static iso15693_frame_t frame;
static nfcv_reply_t response;
static uint32_t tx_deadline_cycles, tx_fraction;
static uint32_t gpio_mask;
static uint32_t observed_eof_cycles;
static uint8_t tx_frame[ISO15693_RESPONSE_MAX_BYTES];
static unsigned tx_length;

static void sense(void) {
    NRF_NFCT->EVENTS_FIELDDETECTED = 0;
    NRF_NFCT->EVENTS_FIELDLOST = 0;
    NRF_NFCT->TASKS_SENSE = 1;
    sense_started_cycles = DWT->CYCCNT;
}

static void activate(void) {
    NRF_NFCT->EVENTS_READY = 0;
    NRF_NFCT->TASKS_ACTIVATE = 1;
    uint32_t begin = DWT->CYCCNT;
    while (!NRF_NFCT->EVENTS_READY && DWT->CYCCNT - begin < NFCV_ACTIVATE_TIMEOUT_CYCLES) {
    }
    NRF_NFCT->EVENTS_FIELDLOST = 0;
}

/* Full subcarrier cycles; rational 75/76 CPU-cycle half periods. Each symbol
   uses an integer number of cycles, so no final half-cycle gets cut short. */
NFCV_RAM_FUNCTION static void transmit_waveform(unsigned rate) {
    uint32_t next = tx_deadline_cycles, phase = tx_fraction;
    unsigned segments = tx_length * NFCV_WAVEFORM_SEGMENTS_PER_BYTE + NFCV_WAVEFORM_FRAMING_SEGMENTS;
    for (unsigned s = 0; s < segments; s++) {
        uint8_t segment = nfcv_waveform_segment(tx_frame, tx_length, s, rate);
        unsigned count = (segment & NFCV_WAVEFORM_CYCLE_MASK) * 2u;
        bool on = (segment & NFCV_WAVEFORM_CARRIER_FLAG) != 0;
        /* Keep contiguous carrier segments in one timed edge loop. */
        while (on && s + 1u < segments) {
            uint8_t following = nfcv_waveform_segment(tx_frame, tx_length, s + 1u, rate);
            if (!(following & NFCV_WAVEFORM_CARRIER_FLAG))
                break;
            count += (following & NFCV_WAVEFORM_CYCLE_MASK) * 2u;
            s++;
        }
        if (!on) {
            /* Every carrier cycle ends high. Silence needs no repeated writes;
               advance the same rational schedule and prepare the next edge early. */
            uint32_t accumulated = phase + count * NFCV_TX_HALF_PERIOD_FRACTION;
            next += count * NFCV_TX_HALF_PERIOD_BASE_CYCLES + accumulated / NFCV_TX_HALF_PERIOD_DENOMINATOR;
            phase = accumulated % NFCV_TX_HALF_PERIOD_DENOMINATOR;
            continue;
        }
        for (unsigned h = 0; h < count; h++) {
            volatile uint32_t *out = !(h & 1u) ? &NRF_P1->OUTCLR : &NRF_P1->OUTSET;
            while ((int32_t)(DWT->CYCCNT - next) < 0) {
            }
            *out = gpio_mask;
            next += NFCV_TX_HALF_PERIOD_BASE_CYCLES;
            phase += NFCV_TX_HALF_PERIOD_FRACTION;
            if (phase >= NFCV_TX_HALF_PERIOD_DENOMINATOR) {
                phase -= NFCV_TX_HALF_PERIOD_DENOMINATOR;
                next++;
            }
            next += (h & 1u) ? -NFCV_TX_LOW_HALF_BIAS_CYCLES : NFCV_TX_LOW_HALF_BIAS_CYCLES;
        }
    }
    tx_deadline_cycles = next;
    tx_fraction = phase;
}

NFCV_RAM_FUNCTION static void send_response(void) {
    if (!response.len || response.len > sizeof(tx_frame) - ISO15693_CRC_SIZE)
        return;
    memcpy(tx_frame, response.bytes, response.len);
    iso15693_append_crc(tx_frame, response.len);
    tx_length = response.len + ISO15693_CRC_SIZE;
    unsigned rate = nfcv_session_high_rate() ? 1u : NFCV_TX_LOW_RATE_MULTIPLIER;
    unsigned tx_us = (tx_length * 8u * 32u + 256u) * rate * 10000u / 8475u;
    if ((int32_t)(window_end - DWT->CYCCNT) < (int32_t)((tx_us + NFCV_TX_WINDOW_GUARD_US) * NFCV_CPU_CYCLES_PER_US))
        return;
    /* Keep the detector active: ACTIVATE calibration would miss the next SOF. */
    uint32_t when = observed_eof_cycles + NFCV_TX_RESPONSE_WAIT_CYCLES;
    while ((int32_t)(DWT->CYCCNT - when) < 0) {
    }
    tx_deadline_cycles = DWT->CYCCNT;
    tx_fraction = 0;
    transmit_waveform(rate);
    while ((int32_t)(DWT->CYCCNT - tx_deadline_cycles) < 0) {
    }
    NRF_P1->OUTSET = gpio_mask;
    __DSB();
    last_field_cycles = DWT->CYCCNT;
}

/* Physical PPM reconstruction: no command templates or phase search.
 * Acquisition, symbol acceptance and EOF are kept in the timed polling loop:
 * moving work into helpers can change edge observation latency. The state
 * below advances only on observed LOCK transitions; CRC gates dispatch. */
NFCV_RAM_FUNCTION static bool receive_frame_or_slot(void) {
    uint32_t receive_started_cycles = DWT->CYCCNT, last_lock_fall_cycles = 0, sof_reference_cycles = 0,
             decoded_symbol_count = 0;
    bool previous_lock = (NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_LOCKDETECT_Msk) != 0, sof_acquired = false,
         coding_1_of_256 = false;
    memset(&frame, 0, sizeof(frame));
    /* Poll and timestamp field indicators before any decoding work. */
    while ((int32_t)(window_end - DWT->CYCCNT) > NFCV_WINDOW_WORK_GUARD_CYCLES &&
           DWT->CYCCNT - receive_started_cycles <
               (sof_acquired ? NFCV_RX_FRAME_TIMEOUT_CYCLES : NFCV_RX_SOF_TIMEOUT_CYCLES)) {
        uint32_t now_cycles = DWT->CYCCNT, field_indicators = NRF_NFCT->FIELDPRESENT;
        bool current_lock = (field_indicators & NFCT_FIELDPRESENT_LOCKDETECT_Msk) != 0;
        if (!(field_indicators & NFCT_FIELDPRESENT_FIELDPRESENT_Msk))
            return false;
        last_field_cycles = now_cycles;
        /* LOCK can merge adjacent PPM pulses separated by two ticks. The
           measured low interval is then four ticks instead of two. Only
           pos7 -> pos1 (or pos511 -> pos1) permits this within legal PPM;
           infer the second zero symbol from the measured interval, not CRC
           or a command template. CRC still validates the reconstructed frame. */
        if (!previous_lock && current_lock && sof_acquired && last_lock_fall_cycles) {
            uint32_t lock_low_width_cycles = now_cycles - last_lock_fall_cycles;
            uint32_t last_fall_tick = NFCV_PPM_TICK_FROM_CYCLES(last_lock_fall_cycles - sof_reference_cycles);
            unsigned last_fall_position = NFCV_PPM_PULSE_POSITION(last_fall_tick, coding_1_of_256);
            if (lock_low_width_cycles > NFCV_RX_MERGED_PULSE_MIN_CYCLES &&
                lock_low_width_cycles < NFCV_RX_MERGED_PULSE_MAX_CYCLES &&
                last_fall_position ==
                    (coding_1_of_256 ? NFCV_PPM_1_OF_256_LAST_POSITION : NFCV_PPM_1_OF_4_LAST_POSITION) &&
                decoded_symbol_count < NFCV_PPM_MAX_SYMBOLS(coding_1_of_256)) {
                decoded_symbol_count++;
            }
        }
        if (previous_lock && !current_lock) {
            uint32_t fall_gap_cycles = now_cycles - last_lock_fall_cycles;
            /* Acquire SOF, then decode one PPM symbol per falling transition. */
            if (!sof_acquired) {
                if (last_lock_fall_cycles && ((fall_gap_cycles > NFCV_RX_1_OF_4_SOF_MIN_CYCLES &&
                                               fall_gap_cycles < NFCV_RX_1_OF_4_SOF_MAX_CYCLES) ||
                                              (fall_gap_cycles > NFCV_RX_1_OF_256_SOF_MIN_CYCLES &&
                                               fall_gap_cycles < NFCV_RX_1_OF_256_SOF_MAX_CYCLES))) {
                    coding_1_of_256 = fall_gap_cycles > NFCV_RX_SOF_CODING_SPLIT_CYCLES;
                    sof_reference_cycles = last_lock_fall_cycles;
                    decoded_symbol_count = 0;
                    sof_acquired = true;
                    memset(frame.bytes, 0, sizeof(frame.bytes));
                }
            } else {
                uint32_t elapsed_cycles = now_cycles - sof_reference_cycles;
                uint32_t pulse_tick = NFCV_PPM_TICK_FROM_CYCLES(elapsed_cycles);
                uint32_t expected_pulse_cycles = NFCV_PPM_CYCLES_FROM_TICK(pulse_tick);
                uint32_t tick_error_cycles = elapsed_cycles > expected_pulse_cycles
                                                 ? elapsed_cycles - expected_pulse_cycles
                                                 : expected_pulse_cycles - elapsed_cycles;
                unsigned symbol_index = NFCV_PPM_SYMBOL_INDEX(pulse_tick, coding_1_of_256);
                unsigned pulse_position = NFCV_PPM_PULSE_POSITION(pulse_tick, coding_1_of_256);
                if (tick_error_cycles <= NFCV_RX_TICK_TOLERANCE_CYCLES && symbol_index == decoded_symbol_count + 1u &&
                    (pulse_position & 1u) && decoded_symbol_count < NFCV_PPM_MAX_SYMBOLS(coding_1_of_256)) {
                    if (coding_1_of_256)
                        frame.bytes[decoded_symbol_count] = (pulse_position - 1u) / 2u;
                    else
                        frame.bytes[decoded_symbol_count / NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE] |=
                            ((pulse_position - 1u) / 2u)
                            << ((decoded_symbol_count % NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE) * 2u);
                    decoded_symbol_count++;
                    /* A 1-of-256 byte spans 4.8 ms. Track the observed pulse
                     * phase per byte instead of accumulating reader/CPU clock
                     * error from SOF across a long addressed request. */
                    if (coding_1_of_256)
                        sof_reference_cycles = now_cycles - expected_pulse_cycles;
                } else if (tick_error_cycles <= NFCV_RX_TICK_TOLERANCE_CYCLES &&
                           symbol_index == decoded_symbol_count + 1u && pulse_position == NFCV_PPM_EOF_POSITION &&
                           decoded_symbol_count >= NFCV_PPM_MIN_FRAME_SYMBOLS(coding_1_of_256) &&
                           (coding_1_of_256 || !(decoded_symbol_count % NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE))) {
                    /* Complete bytes and CRC are required before command dispatch. */
                    frame.byte_count = NFCV_PPM_FRAME_BYTES(decoded_symbol_count, coding_1_of_256);
                    frame.crc_ok = iso15693_check_crc(frame.bytes, frame.byte_count);
                    frame.high_data_rate = (frame.bytes[0] & ISO15693_REQ_HIGH_RATE) != 0;
                    frame.single_subcarrier = (frame.bytes[0] & ISO15693_REQ_DUAL_SUBCARRIER) == 0;
                    observed_eof_cycles = now_cycles;
                    return frame.crc_ok;
                } else {
                    sof_acquired = false;
                    last_lock_fall_cycles = 0;
                }
            }
            last_lock_fall_cycles = now_cycles;
        }
        /* A single EOF-only pulse advances a pending 16-slot Inventory. Wait
           beyond both SOF gaps so a new command never masquerades as a slot. */
        if (!sof_acquired && last_lock_fall_cycles && nfcv_session_slot_pending() &&
            now_cycles - last_lock_fall_cycles > NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES) {
            observed_eof_cycles = last_lock_fall_cycles;
            frame.byte_count = 0;
            return true;
        }
        if (sof_acquired &&
            now_cycles - last_lock_fall_cycles >
                (coding_1_of_256 ? NFCV_RX_1_OF_256_GAP_TIMEOUT_CYCLES : NFCV_RX_1_OF_4_GAP_TIMEOUT_CYCLES)) {
            sof_acquired = false;
            last_lock_fall_cycles = 0;
        }
        previous_lock = current_lock;
    }
    return false;
}

bool nfcv_radio_start(void) {
    if (enabled)
        return true;
    nrfx_nfct_config_t config = {.rxtx_int_mask = 0, .cb = NULL};
    if (nrfx_nfct_init(&config) != NRFX_SUCCESS) {
        enabled = false;
        return false;
    }
    nrfx_nfct_uninit();
    nrf_gpio_cfg_output(READER_POWER);
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_cfg_output(LF_ANT_DRIVER);
    nrf_gpio_pin_clear(LF_ANT_DRIVER);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    nrf_gpio_cfg(HF_ANT_SEL, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
    nrf_gpio_pin_set(HF_ANT_SEL);
    gpio_mask = 1u << (HF_ANT_SEL & 31u);
    NRF_NFCT->INTENCLR = 0xffffffffu;
    NRF_NFCT->AUTOCOLRESCONFIG |= NFCT_AUTOCOLRESCONFIG_MODE_Disabled;
    NRF_NFCT->SHORTS = 0;
    last_field_cycles = DWT->CYCCNT;
    sense();
    enabled = true;
    return true;
}

static void poll_window(void) {
    window_end = rf_timeslot_deadline() - NFCV_WINDOW_CLEANUP_CYCLES;
    if (!enabled)
        return;
    uint32_t now = DWT->CYCCNT;
    if (NRF_NFCT->NFCTAGSTATE < NFCT_NFCTAGSTATE_NFCTAGSTATE_Idle) {
        if (now - sense_started_cycles >= NFCV_ACTIVATE_TIMEOUT_CYCLES)
            activate();
        else
            return;
    }
    if (!(NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_FIELDPRESENT_Msk)) {
        if (now - last_field_cycles > NFCV_FIELD_LOSS_GUARD_CYCLES) {
            nfcv_session_field_lost();
            sense();
            last_field_cycles = now;
        }
        return;
    }
    /* Confirm field loss in this grant so the next reader cannot inherit
     * SELECTED/QUIET state. Short detector dips retain the current session. */
    while ((int32_t)(window_end - DWT->CYCCNT) > NFCV_WINDOW_WORK_GUARD_CYCLES) {
        if (!(NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_FIELDPRESENT_Msk)) {
            if (DWT->CYCCNT - last_field_cycles > NFCV_FIELD_LOSS_GUARD_CYCLES) {
                nfcv_session_field_lost();
                sense();
                last_field_cycles = DWT->CYCCNT;
                return;
            }
            continue;
        }
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        bool complete = receive_frame_or_slot();
        if (complete) {
            bool reply = frame.byte_count ? nfcv_session_handle(&frame, &response)
                                          : nfcv_session_next_slot(&response);
            if (reply)
                send_response();
        }
        __set_PRIMASK(primask);
        /* Finish an acquired RF exchange before yielding. In particular,
         * restoring SLIX emulation after sniff STOP must not starve its USB
         * acknowledgement for the rest of the idle radio window. */
        if (usb_work_pending()) {
            break;
        }
    }
}

void nfcv_radio_poll(void) {
    if (!enabled)
        return;
    bool connected = g_is_ble_connected;
    bool resume = false;
    bool idle = advertising_pause(&resume);
    if (idle)
        bsp_wdt_feed();
    uint32_t window_us = idle ? NFCV_IDLE_WINDOW_US : connected ? NFCV_CONNECTED_WINDOW_US : NFCV_DISCOVERY_WINDOW_US;
    rf_timeslot_run(window_us, !connected, idle, poll_window);
    if (resume)
        advertising_start(false);
}

void nfcv_radio_stop(void) {
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    enabled = false;
    NRF_NFCT->TASKS_DISABLE = 1;
    nrf_gpio_pin_set(HF_ANT_SEL);
    __set_PRIMASK(irq);
}

bool nfcv_radio_field_present(void) {
    return enabled && (NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_FIELDPRESENT_Msk);
}
