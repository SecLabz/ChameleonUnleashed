#include "nfcv_sniff.h"
#include "ble_main.h"
#include "hf_sniff.h"
#include "rc522_raw.h"
#include "rf_workspace.h"
#include "iso15693/sniff_decode.h"
#include "hw_connect.h"
#include "nrfx_nfct.h"
#include "nrfx_timer.h"
#include "nrfx_ppi.h"
#include "usb_main.h"
#include "nrf_gpio.h"
#include <string.h>

#define HIGH_RATE_BANK_BYTES 44032u
#define LOW_RATE_BANK_BYTES 31744u
#define POLL_IDLE_US 2000u
#define POLL_USB_US 100u
#define CPU_CYCLES_PER_US 64u
#define SOF_TIMEOUT_US 5000u
#define RESPONSE_TIMEOUT_US 200000u
#define DMA_TIMEOUT_CYCLES 64000u
#define DMA_LAG_BYTES 8u
#define DMA_CLOCK_MARGIN_DIVISOR 64u
#define DMA_RESTART_READY_US 8u
#define DECODE_CHUNK_BYTES 32u

static const nrfx_timer_t bank_counter = NRFX_TIMER_INSTANCE(4);

typedef struct {
    nfcv_sniff_reader_t reader;
    nfcv_sniff_tag_t tag;
    nrf_ppi_channel_t ppi;
    nrf_ppi_channel_t started_ppi;
    unsigned bank_bytes;
    unsigned handled_banks;
    unsigned handled_bytes;
    uint32_t started;
    bool receiving;
    bool counter_owned;
    bool ppi_owned;
    bool started_ppi_owned;
    bool resume_advertising;
    float workspace[];
} capture_t;

_Static_assert(sizeof(capture_t) + HIGH_RATE_BANK_BYTES +
    4u * NFCV_SNIFF_HIGH_INTEGRALS * sizeof(float) <= RF_HF_SNIFF_BYTES, "High-rate NFC-V capture exceeds RF workspace");
_Static_assert(sizeof(capture_t) + LOW_RATE_BANK_BYTES +
    4u * NFCV_SNIFF_LOW_INTEGRALS * sizeof(float) <= RF_HF_SNIFF_BYTES, "Low-rate NFC-V capture exceeds RF workspace");

static capture_t *capture(void) {
    return rf_hf_sniff_workspace();
}

static void timer_handler(nrf_timer_event_t event, void *context) {
    (void)event;
    (void)context;
}

static unsigned completed_banks(void) {
    NRF_TIMER4->TASKS_CAPTURE[0] = 1;
    __DMB();
    return NRF_TIMER4->CC[0];
}

static void stop_response(void) {
    capture_t *state = capture();
    if (!state->receiving) {
        return;
    }
    nrfx_ppi_channel_disable(state->ppi);
    nrfx_ppi_channel_disable(state->started_ppi);
    rc522_raw_capture_stop();
    state->receiving = false;
}

static void publish_response(bool truncated);

static void start_response(void) {
    capture_t *state = capture();
    if (state->receiving) {
        if (state->tag.acquired || state->tag.length) {
            hf_sniff_overrun();
        }
        publish_response(true);
    }
    if (!state->reader.frame.single_subcarrier) {
        return;
    }
    state->bank_bytes = state->reader.frame.high_data_rate ? HIGH_RATE_BANK_BYTES : LOW_RATE_BANK_BYTES;
    uint8_t *samples = (uint8_t *)state->workspace;
    nfcv_sniff_tag_reset(&state->tag, state->reader.frame.high_data_rate,
        (float *)(samples + state->bank_bytes));
    state->handled_banks = 0;
    state->handled_bytes = 1;
    NRF_TIMER4->TASKS_CLEAR = 1;
    NRF_SPIM0->EVENTS_STARTED = 0;
    if (nrfx_ppi_channel_enable(state->ppi) != NRFX_SUCCESS ||
            nrfx_ppi_channel_enable(state->started_ppi) != NRFX_SUCCESS) {
        nrfx_ppi_channel_disable(state->ppi);
        nrfx_ppi_channel_disable(state->started_ppi);
        hf_sniff_overrun();
        return;
    }
    rc522_raw_capture(samples, state->bank_bytes);
    state->started = DWT->CYCCNT;
    while (!NRF_SPIM0->EVENTS_STARTED) {
        if (DWT->CYCCNT - state->started > DMA_TIMEOUT_CYCLES) {
            rc522_raw_capture_stop();
            nrfx_ppi_channel_disable(state->ppi);
            nrfx_ppi_channel_disable(state->started_ppi);
            hf_sniff_overrun();
            return;
        }
    }
    NRF_SPIM0->EVENTS_STARTED = 0;
    state->receiving = true;
}

static void publish_response(bool truncated) {
    capture_t *state = capture();
    if (!state->receiving) {
        return;
    }
    stop_response();
    if (state->tag.length) {
        uint8_t flags = truncated ? HF_SNIFF_TRUNCATED : HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED;
        if (!truncated && iso15693_check_crc(state->tag.data, state->tag.length)) {
            flags |= HF_SNIFF_CRC_VALID;
        }
        hf_sniff_publish(1, state->tag.data, state->tag.length, flags);
    }
}

static bool dma_overwrote(const capture_t *state, unsigned completed) {
    if (completed == state->handled_banks) {
        return false;
    }
    if (completed != state->handled_banks + 1u) {
        return true;
    }
    if (!NRF_SPIM0->EVENTS_STARTED) {
        return false;
    }
    NRF_TIMER3->TASKS_CAPTURE[2] = 1;
    unsigned elapsed_us = NRF_TIMER3->CC[2] - NRF_TIMER3->CC[3];
    /* The restarted transfer rewrites this same bank. Use an upper bound
     * (1.5% faster plus bus margin), unlike the availability lower bound. */
    unsigned overwritten = elapsed_us + elapsed_us / DMA_CLOCK_MARGIN_DIVISOR + DMA_LAG_BYTES;
    return completed_banks() != completed || overwritten > state->handled_bytes;
}

static void receive_response(void) {
    capture_t *state = capture();
    if (!state->receiving) {
        return;
    }
    unsigned completed = completed_banks();
    if (dma_overwrote(state, completed)) {
        hf_sniff_overrun();
        publish_response(true);
        return;
    }
    unsigned available = state->bank_bytes;
    if (completed == state->handled_banks) {
        NRF_TIMER3->TASKS_CAPTURE[2] = 1;
        uint32_t origin = NRF_TIMER3->CC[3];
        unsigned elapsed_us = NRF_TIMER3->CC[2] - origin;
        unsigned elapsed = elapsed_us * DMA_CLOCK_MARGIN_DIVISOR / (DMA_CLOCK_MARGIN_DIVISOR + 1u);
        available = elapsed > DMA_LAG_BYTES ? elapsed - DMA_LAG_BYTES : 0;
        if (available > state->bank_bytes) {
            available = state->bank_bytes;
        }
    }
    /* Decode full chunks: per-sample copies/checks otherwise consume the
     * next reader SOF margin. The final bank tail may be shorter. */
    unsigned end = state->handled_bytes + DECODE_CHUNK_BYTES;
    if (end > state->bank_bytes) {
        end = state->bank_bytes;
    }
    uint8_t *bank = (uint8_t *)state->workspace;
    if (end <= available && end > state->handled_bytes) {
        unsigned count = end - state->handled_bytes;
        uint8_t samples[DECODE_CHUNK_BYTES];
        /* Firmware disables builtin memcpy globally. Keep the normal fixed
         * chunk inline; only a final short bank tail needs the library call. */
        if (count == DECODE_CHUNK_BYTES) {
            __builtin_memcpy(samples, bank + state->handled_bytes, DECODE_CHUNK_BYTES);
        } else {
            memcpy(samples, bank + state->handled_bytes, count);
        }
        /* Check again after copying: END may have occurred during the copy.
         * Never advance decoder state with a chunk that DMA could overwrite. */
        if (NRF_SPIM0->EVENTS_STARTED && dma_overwrote(state, completed_banks())) {
            hf_sniff_overrun();
            publish_response(true);
            return;
        }
        bool complete = nfcv_sniff_tag_feed(&state->tag, samples, count);
        state->handled_bytes = end;
        if (complete) {
            publish_response(state->tag.truncated);
            return;
        }
    }
    if (completed != state->handled_banks && state->handled_bytes == state->bank_bytes) {
        if (!NRF_SPIM0->EVENTS_STARTED || completed_banks() != completed) {
            hf_sniff_overrun();
            publish_response(true);
            return;
        }
        /* A DMA restart leaves a short unsampled interval. Advance time and
         * mixer phase without injecting a zero ADC code (which is not DC). */
        unsigned gap_us = NRF_TIMER3->CC[3] - NRF_TIMER3->CC[1];
        if (gap_us > 2u) {
            hf_sniff_overrun();
            publish_response(true);
            return;
        }
        NRF_TIMER3->TASKS_CAPTURE[2] = 1;
        if (NRF_TIMER3->CC[2] - NRF_TIMER3->CC[3] < DMA_RESTART_READY_US) {
            return;
        }
        NRF_SPIM0->EVENTS_STARTED = 0;
        if (gap_us && nfcv_sniff_tag_gap(&state->tag, gap_us, bank[state->bank_bytes - 1u], bank[0])) {
            publish_response(state->tag.truncated);
            return;
        }
        state->handled_banks++;
        state->handled_bytes = 0;
    }
    if ((!state->tag.acquired && DWT->CYCCNT - state->started > SOF_TIMEOUT_US * CPU_CYCLES_PER_US) ||
            DWT->CYCCNT - state->started > RESPONSE_TIMEOUT_US * CPU_CYCLES_PER_US) {
        publish_response(true);
    }
}

bool nfcv_sniff_start(uint8_t gain) {
    capture_t *state = capture();
    memset(state, 0, sizeof(*state));
    if (!usb_hfclk_active()) {
        return false;
    }
    if (!rc522_raw_sniff_open(gain)) {
        rc522_raw_close();
        return false;
    }
    /* NFCT initialization temporarily owns TIMER4 for its silicon workaround.
     * Release that driver before acquiring the DMA completion counter. */
    nrfx_nfct_config_t nfct_config = {.rxtx_int_mask = 0, .cb = NULL};
    if (nrfx_nfct_init(&nfct_config) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    nrfx_nfct_uninit();
    if (!advertising_pause(&state->resume_advertising)) {
        nfcv_sniff_stop();
        return false;
    }
    nrfx_timer_config_t config = NRFX_TIMER_DEFAULT_CONFIG;
    config.mode = NRF_TIMER_MODE_COUNTER;
    config.bit_width = NRF_TIMER_BIT_WIDTH_32;
    if (nrfx_timer_init(&bank_counter, &config, timer_handler) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    state->counter_owned = true;
    if (nrfx_ppi_channel_alloc(&state->ppi) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    state->ppi_owned = true;
    if (nrfx_ppi_channel_assign(state->ppi, (uint32_t)&NRF_SPIM0->EVENTS_END,
            (uint32_t)&NRF_TIMER4->TASKS_COUNT) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    if (nrfx_ppi_channel_fork_assign(state->ppi, (uint32_t)&NRF_TIMER3->TASKS_CAPTURE[1]) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    if (nrfx_ppi_channel_alloc(&state->started_ppi) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    state->started_ppi_owned = true;
    if (nrfx_ppi_channel_assign(state->started_ppi, (uint32_t)&NRF_SPIM0->EVENTS_STARTED,
            (uint32_t)&NRF_TIMER3->TASKS_CAPTURE[3]) != NRFX_SUCCESS) {
        nfcv_sniff_stop();
        return false;
    }
    nrfx_timer_enable(&bank_counter);
    NRF_NFCT->INTENCLR = 0xffffffffu;
    NRF_NFCT->AUTOCOLRESCONFIG |= NFCT_AUTOCOLRESCONFIG_MODE_Disabled;
    NRF_NFCT->SHORTS = 0;
    NRF_NFCT->EVENTS_TXFRAMESTART = 0;
    nrf_gpio_pin_set(HF_ANT_SEL);
    NRF_NFCT->TASKS_SENSE = 1;
    NRF_NFCT->TASKS_ACTIVATE = 1;
    return true;
}

static void publish_reader(void) {
    capture_t *state = capture();
    uint8_t flags = HF_SNIFF_TRUNCATED;
    if (!state->reader.truncated) {
        flags = HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED;
        if (state->reader.frame.crc_ok) {
            flags |= HF_SNIFF_CRC_VALID;
        }
    }
    hf_sniff_publish(0, state->reader.frame.bytes, state->reader.frame.byte_count, flags);
    if (!state->reader.truncated) {
        start_response();
    }
}

void nfcv_sniff_poll(void) {
    capture_t *state = capture();
    uint32_t begin = DWT->CYCCNT;
    uint32_t budget_us = usb_cdc_tx_ready() ? POLL_IDLE_US : POLL_USB_US;
    /* NFCT LOCK is polled: protect PPM pulse observation. Yield during tag
     * DMA so USB can drain the queue even under back-to-back exchanges. */
    uint32_t interrupts = __get_PRIMASK();
    __disable_irq();
    do {
        uint32_t now = DWT->CYCCNT;
        unsigned field = NRF_NFCT->FIELDPRESENT;
        if (NRF_NFCT->EVENTS_TXFRAMESTART) {
            NRF_NFCT->EVENTS_TXFRAMESTART = 0;
            hf_sniff_transmit_observed(NULL, 0);
        }
        nfcv_sniff_reader_event_t reader_event = NFCV_SNIFF_READER_NONE;
        if (field & NFCT_FIELDPRESENT_FIELDPRESENT_Msk) {
            bool lock = (field & NFCT_FIELDPRESENT_LOCKDETECT_Msk) != 0;
            if (!state->receiving || !state->tag.acquired) {
                reader_event = nfcv_sniff_reader_feed(&state->reader, lock, now);
            } else {
                state->reader.acquired = false;
                state->reader.last_fall = 0;
                state->reader.previous_lock = lock;
            }
        } else {
            if (nfcv_sniff_reader_stop(&state->reader)) {
                reader_event = NFCV_SNIFF_READER_FRAME;
            }
            state->reader.inventory_slots = 0;
            state->reader.previous_lock = false;
            state->reader.last_fall = 0;
        }
        if (reader_event == NFCV_SNIFF_READER_FRAME) {
            publish_reader();
        } else if (reader_event == NFCV_SNIFF_READER_SLOT) {
            start_response();
        }
        receive_response();
    } while (DWT->CYCCNT - begin < budget_us * CPU_CYCLES_PER_US || state->reader.acquired);
    __set_PRIMASK(interrupts);
}

void nfcv_sniff_stop(void) {
    capture_t *state = capture();
    if (nfcv_sniff_reader_stop(&state->reader)) {
        publish_reader();
    }
    publish_response(true);
    if (state->ppi_owned) {
        nrfx_ppi_channel_disable(state->ppi);
        nrfx_ppi_channel_fork_assign(state->ppi, 0);
        nrfx_ppi_channel_free(state->ppi);
        state->ppi_owned = false;
    }
    if (state->started_ppi_owned) {
        nrfx_ppi_channel_disable(state->started_ppi);
        nrfx_ppi_channel_free(state->started_ppi);
        state->started_ppi_owned = false;
    }
    if (state->counter_owned) {
        nrfx_timer_uninit(&bank_counter);
        state->counter_owned = false;
    }
    NRF_NFCT->TASKS_DISABLE = 1;
    rc522_raw_close();
    if (state->resume_advertising) {
        state->resume_advertising = false;
        advertising_start(false);
    }
}
