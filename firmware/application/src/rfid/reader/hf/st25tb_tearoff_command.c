#include "hf_reader.h"
#include "hf_radio.h"
#include "st25tb_tearoff.h"
#include "st25tb_reader.h"
#include "app_status.h"
#include "data_cmd.h"
#include "nfca_session.h"
#include "rfid_main.h"
#include "nrf.h"
#include "ble_main.h"
#include "usb_main.h"
#include "net/web_tearoff.h"
#include "utils/dataframe.h"
#include "bsp_wdt.h"

extern volatile bool m_is_field_on;

#define USB_DRAIN_TIMEOUT_CYCLES (100u * HF_READER_CYCLES_PER_MS)

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* One shared encoder avoids duplicating byte stores in the flash-limited web build. */
__attribute__((noinline))
static void write_u32_le(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < sizeof(value); i++) bytes[i] = value >> (i * 8);
}

/* Both progress and completion drain the same bounded USB queue. */
__attribute__((noinline))
static void drain_usb(void) {
    uint32_t started = DWT->CYCCNT;
    while (!usb_cdc_tx_ready() && DWT->CYCCNT - started < USB_DRAIN_TIMEOUT_CYCLES) {
        usb_poll();
        bsp_wdt_feed();
    }
}

static bool progress_timing;
static uint32_t usb_cycles;

static void tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    uint32_t measured = progress_timing ? DWT->CYCCNT : 0;
    if (!is_usb_working()) return;
    uint8_t payload[ST25TB_TEAROFF_PROGRESS_BYTES] = {kind, delay_us, delay_us >> 8,
                          value, value >> 8, value >> 16, value >> 24};
    if (!usb_cdc_tx_ready()) return;
    data_frame_tx_t *frame = data_frame_make(DATA_CMD_ST25TB_TEAROFF, ST25TB_TEAROFF_PROGRESS_STATUS,
                                              sizeof(payload), payload);
    usb_cdc_write(frame->buffer, frame->length);
    drain_usb();
    if (progress_timing) usb_cycles += DWT->CYCCNT - measured;
}

uint16_t st25tb_tearoff_command(const uint8_t *data, uint8_t *output, uint16_t *size) {
    bool serial_tearoff = false;
    st25tb_tearoff_request_t tearoff = {
        .uid = data + ST25TB_TEAROFF_REQUEST_UID,
        .block = data[ST25TB_TEAROFF_REQUEST_BLOCK],
        .target = read_u32_le(data + ST25TB_TEAROFF_REQUEST_TARGET),
        .reset_check = data[ST25TB_TEAROFF_REQUEST_MODE] == ST25TB_TEAROFF_MODE_RESET_CHECK,
        .duration_ms = read_u32_le(data + ST25TB_TEAROFF_REQUEST_DURATION),
        .adjustment_us = data[ST25TB_TEAROFF_REQUEST_ADJUSTMENT]
            ? data[ST25TB_TEAROFF_REQUEST_ADJUSTMENT] : ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US,
        .timing = data[ST25TB_TEAROFF_REQUEST_FORMAT] & ST25TB_TEAROFF_REQUEST_TIMING,
    };
    if ((data[ST25TB_TEAROFF_REQUEST_FORMAT] & ~ST25TB_TEAROFF_REQUEST_TIMING) != ST25TB_TEAROFF_FORMAT ||
            tearoff.uid[0] != ST25TB_UID_FAMILY || tearoff.uid[1] != ST25TB_UID_MANUFACTURER ||
            (tearoff.uid[ST25TB_UID_PRODUCT_OFFSET] != ST25TB_PRODUCT_512_AT &&
             (tearoff.uid[ST25TB_UID_PRODUCT_OFFSET] >> ST25TB_PRODUCT_SHIFT) != ST25TB_PRODUCT_SRI512) ||
            (tearoff.block != ST25TB_COUNTER_FIRST_BLOCK && tearoff.block != ST25TB_COUNTER_LAST_BLOCK) ||
            (data[ST25TB_TEAROFF_REQUEST_MODE] != ST25TB_TEAROFF_MODE_RUN &&
             data[ST25TB_TEAROFF_REQUEST_MODE] != ST25TB_TEAROFF_MODE_RESET_CHECK) ||
            tearoff.duration_ms < ST25TB_TEAROFF_MIN_DURATION_MS ||
            tearoff.duration_ms > ST25TB_TEAROFF_MAX_DURATION_MS ||
            tearoff.adjustment_us > ST25TB_TEAROFF_MAX_ADJUSTMENT_US || tearoff.target < ST25TB_TEAROFF_FLOOR ||
            (tearoff.reset_check && data[ST25TB_TEAROFF_REQUEST_ADJUSTMENT] != 0))
        return STATUS_PAR_ERR;
    if (web_tearoff_running()) {
        tearoff.progress = web_tearoff_progress;
        tearoff.cancelled = web_tearoff_cancelled;
        tearoff.service = web_tearoff_service;
    } else {
        if (data_frame_source() != DATA_SOURCE_USB) return STATUS_NOT_IMPLEMENTED;
        serial_tearoff = true;
        progress_timing = tearoff.timing;
        usb_cycles = 0;
        tearoff.progress = tearoff_progress;
        tearoff.cancelled = usb_cdc_tearoff_cancelled;
    }
    if (get_device_mode() != DEVICE_MODE_READER) return STATUS_DEVICE_MODE_ERROR;
    if (m_is_field_on || nfca_session_active()) return STATUS_BUSY;
    hf_radio_depth(false);
    uint32_t timing_started = 0;
    if (tearoff.timing) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        timing_started = DWT->CYCCNT;
    }
    hf_radio_tearoff_timing(tearoff.timing);
    bool resume_advertising = serial_tearoff && advertising_stop_checked();
    bool opened = hf_radio_open(false);
    st25tb_tearoff_result_t result = {.outcome = ST25TB_TEAROFF_TRANSPORT_ERROR};
    if (serial_tearoff) usb_cdc_tearoff_begin();
    if (opened) {
        hf_radio_tearoff_deadline(tearoff.duration_ms);
        result = st25tb_tearoff_run(&tearoff);
    }
    hf_tearoff_timing_t radio_timing = hf_radio_tearoff_timing_result();
    hf_radio_tearoff_timing(false);
    bool cleanup = hf_radio_close();
    if (serial_tearoff) {
        uint32_t measured = tearoff.timing ? DWT->CYCCNT : 0;
        drain_usb();
        usb_cdc_tearoff_end();
        if (tearoff.timing) usb_cycles += DWT->CYCCNT - measured;
    }
    if (resume_advertising) advertising_start(false);
    output[ST25TB_TEAROFF_RESULT_OUTCOME] = result.outcome;
    output[ST25TB_TEAROFF_RESULT_VALID] = result.valid;
    output[ST25TB_TEAROFF_RESULT_CLEANUP] = cleanup;
    output[ST25TB_TEAROFF_RESULT_FLAGS] = (result.initial_valid ? ST25TB_TEAROFF_RESULT_INITIAL_VALID : 0) |
        (tearoff.timing ? ST25TB_TEAROFF_RESULT_HAS_TIMING : 0);
    write_u32_le(output + ST25TB_TEAROFF_RESULT_VALUE, result.value);
    write_u32_le(output + ST25TB_TEAROFF_RESULT_INITIAL_VALUE, result.initial_value);
    write_u32_le(output + ST25TB_TEAROFF_RESULT_REQUEST_TO_OFF, result.request_to_off_cycles);
    write_u32_le(output + ST25TB_TEAROFF_RESULT_OFF_TO_ON, result.off_to_on_cycles);
    output[ST25TB_TEAROFF_RESULT_READ_STAGE] = result.read_stage;
    output[ST25TB_TEAROFF_RESULT_READ_ATTEMPTS] = result.read_attempts;
    output[ST25TB_TEAROFF_RESULT_READ_STATUS] = result.read_status;
    output[ST25TB_TEAROFF_RESULT_READ_STATUS + 1] = result.read_status >> 8;
    const uint32_t fields[ST25TB_TEAROFF_TIMING_FIELDS] = {
        tearoff.timing ? DWT->CYCCNT - timing_started : 0,
        radio_timing.timeslot_cycles, radio_timing.grant_wait_cycles,
        radio_timing.dma_cycles, radio_timing.decode_cycles,
        serial_tearoff ? usb_cycles : 0,
        result.pause_cycles, radio_timing.exchanges,
        radio_timing.writes, radio_timing.resets,
    };
    for (unsigned i = 0; i < ST25TB_TEAROFF_TIMING_FIELDS; i++)
        write_u32_le(output + ST25TB_TEAROFF_RESULT_TIMING + i * sizeof(fields[i]), fields[i]);
    *size = ST25TB_TEAROFF_RESULT_BYTES;
    return STATUS_HF_TAG_OK;
}
