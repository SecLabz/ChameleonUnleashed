/* SPDX-License-Identifier: GPL-3.0-only */
#include "hf_radio.h"
#include "rc522_raw.h"
#include "hf_reader.h"
#include "st25tb_reader.h"
#include "hf_iq.h"
#include "nfcb_decode.h"
#include "nfcb_tx.h"
#include "nfcv_reader_codec.h"
#include "iso15693/frame.h"
#include "rf_timeslot.h"
#include "hfclk.h"
#include "usb_main.h"
#include "app_status.h"
#include "nrf.h"
#include <string.h>

#define NFCB_ATQB_IQ_SAMPLES 8192u
#define ST25TB_READ_SLOT_US 15000u
#define READER_EXCHANGE_SLOT_US 25000u
#define ST25TB_WRITE_SLOT_US 12000u
#define READER_RESET_SLOT_US 3000u
#define ST25TB_WRITE_COMMIT_US 6000u

static bool tearoff_session, timing_enabled, exchange_timeslot_failed;
static hf_tearoff_timing_t timing;
static uint32_t operation_last_check;
static uint64_t operation_elapsed, operation_limit = HF_READER_DEADLINE_CYCLES;
typedef enum { RADIO_NFCB, RADIO_NFCV } radio_profile_t;
typedef enum { TX_TIMESLOT, TX_USB_HARDWARE } radio_execution_t;
static radio_profile_t profile;
static radio_execution_t execution;
static bool crc_error;
static bool retained, vicinity_clock;
static uint8_t tx_frame[NFCV_READER_MAX_FRAME];
static rc522_raw_transfer_t transfer = {.data = tx_frame};
static uint32_t tearoff_delay_us;
extern volatile bool g_is_ble_connected;
volatile uint32_t hf_radio_tearoff_last_delta_cycles;

static bool usb_clock_ready(void) {
    return usb_hfclk_active() && rc522_raw_clock_ready();
}

static void record_slot(void) {
    if (!timing_enabled) return;
    timing.timeslot_cycles += rf_timeslot_last_run_cycles();
    timing.grant_wait_cycles += rf_timeslot_last_grant_wait_cycles();
}

static void vicinity_exchange_window(void) {
    uint8_t levels[NFCV_READER_MAX_TICKS];
    rc522_raw_transfer_t vicinity = transfer;
    vicinity.length = nfcv_reader_encode(tx_frame, transfer.length, levels, sizeof(levels));
    vicinity.data = levels;
    rc522_raw_exchange(&vicinity);
}

static void exchange_window(void) {
    if (profile == RADIO_NFCV) vicinity_exchange_window();
    else rc522_raw_exchange(&transfer);
}

static void tearoff_window(void) { rc522_raw_tearoff(&transfer, tearoff_delay_us); }

static void st25tb_write_window(void) { rc522_raw_write(&transfer); }

static void send_window(void) { rc522_raw_send(&transfer); }

static void prepare_frame(const uint8_t *frame, size_t length) {
    memcpy(tx_frame, frame, length);
    if (profile == RADIO_NFCV) iso15693_append_crc(tx_frame, length);
    else {
        uint16_t crc = nfcb_crc_b(tx_frame, length);
        tx_frame[length] = crc;
        tx_frame[length + 1] = crc >> 8;
    }
    transfer.data = tx_frame;
    transfer.length = length + HF_READER_CRC_BYTES;
}

static void select_execution(bool ready) {
    execution = ready && profile == RADIO_NFCB && usb_clock_ready() &&
        rc522_raw_acquire_tx() ? TX_USB_HARDWARE : TX_TIMESLOT;
}

typedef struct {
    unsigned samples;
    uint32_t slot_us;
} receive_window_t;

static receive_window_t receive_window(size_t expected, bool hardware_tx) {
    if (profile == RADIO_NFCV) {
        /* Reserve only ASK emission plus frontend/setup margin. The complete
         * ADC capture continues in DMA after the radio grant is returned. */
        uint32_t ticks = 12u + 32u * transfer.length;
        uint32_t tx_us = (ticks * 12800u + 1355u) / 1356u;
        uint32_t slot_us = ((tx_us + 2000u + 999u) / 1000u) * 1000u;
        return (receive_window_t){NFCV_READER_IQ_SAMPLES, slot_us};
    }
    if (expected == NFCB_ATQB_BYTES) {
        return (receive_window_t){NFCB_ATQB_IQ_SAMPLES, READER_EXCHANGE_SLOT_US};
    }
    return (receive_window_t){st25tb_capture_samples(expected, hardware_tx), ST25TB_READ_SLOT_US};
}

static size_t exchange_frame(const uint8_t *frame, size_t length, size_t expected, uint8_t *reply) {
    prepare_frame(frame, length);
    if (execution == TX_USB_HARDWARE && !usb_clock_ready()) execution = TX_TIMESLOT;
    bool direct = execution == TX_USB_HARDWARE;
    receive_window_t window = receive_window(expected, direct);
    transfer.sample_count = window.samples;
    transfer.hardware_tx = direct;
    /* NFC-B can stop at a valid live frame. NFC-V keeps the entire capture,
     * but must not hold a continuous radio grant throughout its reception. */
    transfer.live = direct || profile == RADIO_NFCV;
    transfer.dma_cycles = timing_enabled ? &timing.dma_cycles : NULL;
    unsigned offset;
    size_t received = 0;
    if (direct) {
        exchange_window();
        exchange_timeslot_failed = false;
        uint32_t measured = timing_enabled ? DWT->CYCCNT : 0;
        if (usb_clock_ready() && rc522_raw_ready() &&
                nfcb_decode_iq_live(transfer.samples + 1, transfer.sample_count, expected,
                                    reply, &offset, rc522_raw_available))
            received = expected;
        if (timing_enabled) timing.decode_cycles += DWT->CYCCNT - measured;
        rc522_raw_finish(received != 0);
        if (!usb_clock_ready()) rc522_raw_invalidate();
    } else {
        exchange_timeslot_failed = !rf_timeslot_run(window.slot_us, !g_is_ble_connected, false, exchange_window);
        if (profile == RADIO_NFCV && !exchange_timeslot_failed) {
            rc522_raw_finish(false);
            if (!rc522_raw_clock_ready()) rc522_raw_invalidate();
        }
    }
    if (timing_enabled) {
        timing.exchanges++;
        if (!direct) record_slot();
    }
    if (exchange_timeslot_failed || !rc522_raw_ready()) return 0;
    if (received) return received;
    uint32_t measured = timing_enabled ? DWT->CYCCNT : 0;
    received = profile == RADIO_NFCV ? nfcv_reader_decode_iq_checked(transfer.samples + 1,
            transfer.sample_count, reply, NFCV_READER_MAX_FRAME, &offset, &crc_error)
        : nfcb_decode_iq_checked(transfer.samples + 1, transfer.sample_count, expected,
                                  reply, &offset, &crc_error) ? expected : 0;
    if (timing_enabled) timing.decode_cycles += DWT->CYCCNT - measured;
    return received;
}

bool hf_radio_open(bool vicinity) {
    bool resume = retained && vicinity && profile == RADIO_NFCV &&
        vicinity_clock && rc522_raw_clock_ready() && rc522_raw_ready();
    if (retained && !resume && !hf_radio_close()) {
        return false;
    }
    retained = false;
    profile = vicinity ? RADIO_NFCV : RADIO_NFCB;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    operation_last_check = DWT->CYCCNT;
    operation_elapsed = 0;
    operation_limit = HF_READER_DEADLINE_CYCLES;
    tearoff_session = false;
    if (resume) {
        return true;
    }
    transfer.samples = hf_iq_prepare(vicinity);
    bool ready = rc522_raw_open(vicinity ? RC522_RAW_NFCV : RC522_RAW_NFCB);
    if (ready && vicinity) {
        /* A timeslot guarantees HFXO only until END. Hold the shared clock
         * across the deferred capture, including when USB is unplugged. */
        uint32_t result = app_hfclk_acquire(HFCLK_READER);
        vicinity_clock = (app_hfclk_owners() & HFCLK_READER) != 0;
        ready = !result && vicinity_clock && !app_hfclk_wait();
    }
    select_execution(ready);
    return ready;
}

bool hf_radio_close(void) {
    retained = false;
    execution = TX_TIMESLOT;
    bool closed = rc522_raw_close();
    if (vicinity_clock) {
        uint32_t result = app_hfclk_release(HFCLK_READER);
        vicinity_clock = (app_hfclk_owners() & HFCLK_READER) != 0;
        if (result || vicinity_clock) return false;
    }
    return closed;
}

void hf_radio_keep(void) {
    retained = true;
}

bool hf_radio_release(void) {
    return !retained || hf_radio_close();
}

void hf_radio_tearoff_deadline(uint32_t milliseconds) {
    operation_limit = (uint64_t)milliseconds * HF_READER_CYCLES_PER_MS;
    tearoff_session = true;
}

void hf_radio_tearoff_timing(bool enabled) {
    timing_enabled = enabled;
    timing = (hf_tearoff_timing_t){0};
}

hf_tearoff_timing_t hf_radio_tearoff_timing_result(void) { return timing; }

bool hf_radio_tearoff_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES], uint32_t delay_us,
        uint32_t *request_to_write_cycles) {
    if (profile == RADIO_NFCV || !rc522_raw_ready() || hf_radio_expired() ||
            delay_us > ST25TB_WRITE_COMMIT_US || !request_to_write_cycles) return false;
    prepare_frame(frame, ST25TB_WRITE_FRAME_BYTES);
    tearoff_delay_us = delay_us;
    hf_radio_tearoff_last_delta_cycles = 0;
    hf_radio_tearoff_last_off_cycles = 0;
    /* Keep the qualified write-to-cut waveform independent of read execution. */
    transfer.hardware_tx = false;
    bool ran = rf_timeslot_run(READER_EXCHANGE_SLOT_US, !g_is_ble_connected, false, tearoff_window);
    if (timing_enabled) {
        timing.writes++;
        record_slot();
    }
    *request_to_write_cycles = rc522_raw_take_write_cycles();
    hf_radio_tearoff_last_delta_cycles = *request_to_write_cycles;
    return ran && rc522_raw_ready() && *request_to_write_cycles;
}

bool hf_radio_st25tb_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES]) {
    if (profile == RADIO_NFCV || !rc522_raw_ready() || hf_radio_expired()) return false;
    prepare_frame(frame, ST25TB_WRITE_FRAME_BYTES);
    transfer.hardware_tx = false;
    return rf_timeslot_run(ST25TB_WRITE_SLOT_US, !g_is_ble_connected, false, st25tb_write_window) && rc522_raw_ready();
}

bool hf_radio_tearoff_reset(void) {
    hf_radio_tearoff_last_off_cycles = 0;
    if (!rc522_raw_ready()) return false;
    bool ran = rf_timeslot_run(READER_RESET_SLOT_US, !g_is_ble_connected, false, rc522_raw_reset);
    if (timing_enabled) {
        timing.resets++;
        record_slot();
    }
    return ran && rc522_raw_ready();
}

bool hf_radio_recover(void) {
    if (hf_radio_expired()) return false;
    bool ready = rc522_raw_recover();
    select_execution(ready);
    return ready;
}

uint16_t hf_radio_exchange(const uint8_t *data, size_t length,
        size_t expected, uint8_t *reply, size_t capacity, size_t *received) {
    *received = 0;
    size_t maximum_frame = profile == RADIO_NFCV ? NFCV_READER_MAX_FRAME : NFCB_TX_MAX_FRAME_BYTES;
    if (!data || !length || length > maximum_frame - HF_READER_CRC_BYTES ||
            !reply || capacity < NFCV_READER_MAX_FRAME ||
            (profile != RADIO_NFCV && expected != ST25TB_CHIP_ID_REPLY_BYTES &&
             expected != ST25TB_BLOCK_REPLY_BYTES && expected != ST25TB_UID_REPLY_BYTES &&
             expected != NFCB_ATQB_BYTES)) {
        return STATUS_PAR_ERR;
    }
    if (!rc522_raw_ready()) return STATUS_HF_ERR_STAT;
    if (hf_radio_expired()) return STATUS_BUSY;
    crc_error = false;
    *received = exchange_frame(data, length, expected, reply);
    if (!rc522_raw_ready()) return STATUS_HF_ERR_STAT;
    if (tearoff_session && exchange_timeslot_failed) return STATUS_HF_ERR_STAT;
    if (*received) return STATUS_HF_TAG_OK;
    return crc_error ? STATUS_HF_ERR_CRC : STATUS_HF_TAG_NO;
}

void hf_radio_depth(bool deeper) {
    rc522_raw_depth(deeper);
}

bool hf_radio_expired(void) {
    /* Reserve the existing timeslot acquisition/close bounds and RF cleanup. */
    uint32_t now = DWT->CYCCNT;
    operation_elapsed += (uint32_t)(now - operation_last_check);
    operation_last_check = now;
    return operation_elapsed >= operation_limit - HF_READER_CLEANUP_RESERVE_MS * HF_READER_CYCLES_PER_MS;
}

bool hf_radio_send(const uint8_t *data, size_t length) {
    if (profile == RADIO_NFCV || !data || !length || length > NFCB_TX_MAX_FRAME_BYTES - HF_READER_CRC_BYTES ||
            !rc522_raw_ready() || hf_radio_expired()) {
        return false;
    }
    prepare_frame(data, length);
    transfer.hardware_tx = execution == TX_USB_HARDWARE;
    bool ran = rf_timeslot_run(READER_RESET_SLOT_US, !g_is_ble_connected, false, send_window);
    if (timing_enabled) { timing.exchanges++; record_slot(); }
    return ran && rc522_raw_ready();
}
