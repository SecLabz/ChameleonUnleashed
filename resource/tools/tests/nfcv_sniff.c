#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "hf_sniff.h"
#include "nfcv_sniff.h"
#include "iso15693/sniff_decode.h"
#include "iso15693/timing.h"

#define RF_HF_SNIFF_BYTES 49152u
#define HF_ANT_SEL 1u
#define NRFX_SUCCESS 0u
#define NRF_TIMER_MODE_COUNTER 1u
#define NRF_TIMER_BIT_WIDTH_32 32u
#define NRFX_TIMER_INSTANCE(n) (n)
#define NRFX_TIMER_DEFAULT_CONFIG {0, 0}
#define NFCT_AUTOCOLRESCONFIG_MODE_Disabled 1u
#define NFCT_FIELDPRESENT_FIELDPRESENT_Msk 1u
#define NFCT_FIELDPRESENT_LOCKDETECT_Msk 2u

typedef unsigned nrfx_timer_t;
typedef unsigned nrf_timer_event_t;
typedef unsigned nrf_ppi_channel_t;
typedef struct {
    unsigned mode;
    unsigned bit_width;
} nrfx_timer_config_t;
typedef struct {
    unsigned rxtx_int_mask;
    void *cb;
} nrfx_nfct_config_t;

static union {
    uint64_t alignment;
    uint8_t bytes[RF_HF_SNIFF_BYTES];
} workspace;
static struct {
    uint32_t CYCCNT;
} clock_state;
static struct {
    uint32_t TASKS_CAPTURE[6];
    uint32_t TASKS_CLEAR;
    uint32_t TASKS_COUNT;
    uint32_t CC[6];
} timer3, timer4;
static struct {
    uint32_t EVENTS_END;
    uint32_t EVENTS_STARTED;
    struct {
        uint32_t PTR;
    } RXD;
} spim;
static struct {
    uint32_t INTENCLR;
    uint32_t AUTOCOLRESCONFIG;
    uint32_t SHORTS;
    uint32_t EVENTS_TXFRAMESTART;
    uint32_t TASKS_SENSE;
    uint32_t TASKS_ACTIVATE;
    uint32_t FIELDPRESENT;
    uint32_t TASKS_DISABLE;
} nfct;

static void *tick(void) {
    clock_state.CYCCNT += 64;
    return &clock_state;
}

#define DWT ((typeof(clock_state) *)tick())
#define NRF_TIMER3 (&timer3)
#define NRF_TIMER4 (&timer4)
#define NRF_SPIM0 (&spim)
#define NRF_NFCT (&nfct)
#define __DMB() ((void)0)

static unsigned failure;
static unsigned allocations;
static unsigned assignments;
static unsigned channels;
static unsigned enabled_channels;
static unsigned close_count;
static unsigned capture_stops;
static unsigned published;
static unsigned tx_observed;
static unsigned overruns;
static unsigned interrupt_mask;
static bool timer_owned;
static bool frontend_owned;
static bool advertising;
static bool hfclk_active;
static bool fail_dma;
static bool wrap_during_copy;
static uint8_t published_flags;
static uint8_t published_data[3];

enum {
    FAIL_NONE,
    FAIL_FRONTEND,
    FAIL_NFCT,
    FAIL_ADVERTISING,
    FAIL_TIMER,
    FAIL_ALLOCATE_FIRST,
    FAIL_ASSIGN_FIRST,
    FAIL_FORK,
    FAIL_ALLOCATE_SECOND,
    FAIL_ASSIGN_SECOND,
};

static void *rf_hf_sniff_workspace(void) {
    return workspace.bytes;
}

static bool usb_cdc_tx_ready(void) {
    return true;
}

static bool usb_hfclk_active(void) {
    return hfclk_active;
}

static bool rc522_raw_sniff_open(uint8_t gain) {
    assert(gain == 7 && !frontend_owned);
    frontend_owned = true;
    return failure != FAIL_FRONTEND;
}

static bool rc522_raw_close(void) {
    assert(frontend_owned);
    frontend_owned = false;
    close_count++;
    return true;
}

static unsigned nrfx_nfct_init(const nrfx_nfct_config_t *config) {
    (void)config;
    assert(!timer_owned);
    if (failure == FAIL_NFCT) {
        return 1;
    }
    timer_owned = true;
    return NRFX_SUCCESS;
}

static void nrfx_nfct_uninit(void) {
    assert(timer_owned);
    timer_owned = false;
}

static bool advertising_pause(bool *resume) {
    *resume = false;
    if (failure == FAIL_ADVERTISING) {
        return false;
    }
    *resume = advertising;
    advertising = false;
    return true;
}

static void advertising_start(bool erase_bonds) {
    assert(!erase_bonds && !advertising);
    advertising = true;
}

static unsigned nrfx_timer_init(const nrfx_timer_t *timer, const nrfx_timer_config_t *config,
        void (*handler)(nrf_timer_event_t, void *)) {
    (void)handler;
    assert(*timer == 4 && !timer_owned && config->mode == NRF_TIMER_MODE_COUNTER);
    if (failure == FAIL_TIMER) {
        return 1;
    }
    timer_owned = true;
    return NRFX_SUCCESS;
}

static void nrfx_timer_enable(const nrfx_timer_t *timer) {
    assert(*timer == 4 && timer_owned);
}

static void nrfx_timer_uninit(const nrfx_timer_t *timer) {
    assert(*timer == 4 && timer_owned);
    timer_owned = false;
}

static unsigned nrfx_ppi_channel_alloc(nrf_ppi_channel_t *channel) {
    allocations++;
    if ((allocations == 1 && failure == FAIL_ALLOCATE_FIRST) ||
            (allocations == 2 && failure == FAIL_ALLOCATE_SECOND)) {
        return 1;
    }
    *channel = allocations - 1;
    channels |= 1u << *channel;
    return NRFX_SUCCESS;
}

static unsigned nrfx_ppi_channel_assign(nrf_ppi_channel_t channel, uint32_t event, uint32_t task) {
    (void)event;
    (void)task;
    assert(channels & (1u << channel));
    assignments++;
    if ((assignments == 1 && failure == FAIL_ASSIGN_FIRST) ||
            (assignments == 2 && failure == FAIL_ASSIGN_SECOND)) {
        return 1;
    }
    return NRFX_SUCCESS;
}

static unsigned nrfx_ppi_channel_fork_assign(nrf_ppi_channel_t channel, uint32_t task) {
    assert(channels & (1u << channel));
    return task && failure == FAIL_FORK ? 1 : NRFX_SUCCESS;
}

static unsigned nrfx_ppi_channel_enable(nrf_ppi_channel_t channel) {
    assert(channels & (1u << channel));
    enabled_channels |= 1u << channel;
    return NRFX_SUCCESS;
}

static void nrfx_ppi_channel_disable(nrf_ppi_channel_t channel) {
    assert(channels & (1u << channel));
    enabled_channels &= ~(1u << channel);
}

static void nrfx_ppi_channel_free(nrf_ppi_channel_t channel) {
    assert(channels & (1u << channel));
    assert(!(enabled_channels & (1u << channel)));
    channels &= ~(1u << channel);
}

static void nrf_gpio_pin_set(unsigned pin) {
    assert(pin == HF_ANT_SEL);
}

static unsigned __get_PRIMASK(void) {
    return interrupt_mask;
}

static void __disable_irq(void) {
    interrupt_mask = 1;
}

static void __set_PRIMASK(unsigned value) {
    interrupt_mask = value;
}

static void rc522_raw_capture(uint8_t *samples, unsigned count) {
    (void)samples;
    assert((count == 44032 || count == 31744) && timer_owned && enabled_channels == channels);
    timer4.CC[0] = 0;
    timer3.CC[1] = 0;
    timer3.CC[2] = 0;
    timer3.CC[3] = 0;
    spim.EVENTS_STARTED = !fail_dma;
}

static void rc522_raw_capture_stop(void) {
    capture_stops++;
}

void hf_sniff_publish(uint8_t direction, const uint8_t *data, unsigned length, uint8_t flags) {
    assert(direction == 1 && length == 3);
    published++;
    published_flags = flags;
    memcpy(published_data, data, length);
}

void hf_sniff_transmit_observed(const uint8_t *data, uint16_t bits) {
    assert(!data && !bits);
    tx_observed++;
}

void hf_sniff_overrun(void) {
    overruns++;
}

static void *copy_samples(void *destination, const void *source, size_t length) {
    if (wrap_during_copy) {
        wrap_during_copy = false;
        timer4.CC[0] = 1;
        timer3.CC[3] = 1000;
        timer3.CC[2] = 2000;
        spim.EVENTS_STARTED = 1;
        memset((void *)source, 0xff, length);
    }
    return memcpy(destination, source, length);
}

#define memcpy copy_samples
#define __builtin_memcpy copy_samples
/* BACKEND */
#undef __builtin_memcpy
#undef memcpy

static void begin_session(bool high_rate) {
    allocations = 0;
    assignments = 0;
    fail_dma = false;
    assert(nfcv_sniff_start(7));
    capture()->reader.frame.single_subcarrier = true;
    capture()->reader.frame.high_data_rate = high_rate;
}

static void replacing_empty_receive_is_not_a_loss(void) {
    unsigned before_overruns = overruns;
    unsigned before_published = published;
    begin_session(true);
    start_response();
    assert(capture()->receiving && !capture()->tag.acquired && !capture()->tag.length);
    start_response();
    assert(capture()->receiving && overruns == before_overruns && published == before_published);
    capture()->tag.acquired = true;
    start_response();
    assert(capture()->receiving && overruns == before_overruns + 1u && published == before_published);
    nfcv_sniff_stop();
}

static void dma_loss_preserves_prefix(unsigned completed, bool during_copy) {
    unsigned before_overruns = overruns;
    unsigned before_published = published;
    const uint8_t prefix[] = {0x00, 0xaa, 0x55};
    begin_session(false);
    start_response();
    memcpy(capture()->tag.data, prefix, sizeof(prefix));
    capture()->tag.length = sizeof(prefix);
    capture()->tag.acquired = true;
    capture()->handled_bytes = 100;
    timer4.CC[0] = during_copy ? 0 : completed;
    timer3.CC[3] = 1000;
    timer3.CC[2] = 2000;
    spim.EVENTS_STARTED = 1;
    wrap_during_copy = during_copy;
    receive_response();
    assert(!capture()->receiving && !enabled_channels);
    assert(capture()->tag.samples == 0);
    assert(overruns == before_overruns + 1u && published == before_published + 1u);
    assert(published_flags == HF_SNIFF_TRUNCATED);
    assert(!memcmp(published_data, prefix, sizeof(prefix)));
    assert(!wrap_during_copy);
    nfcv_sniff_stop();
}

static void dma_restart_with_unread_tail_is_safe(void) {
    unsigned before_overruns = overruns;
    begin_session(false);
    start_response();
    capture()->handled_bytes = capture()->bank_bytes - 64u;
    memset((uint8_t *)capture()->workspace + capture()->handled_bytes, 0x22, 64);
    memset(capture()->workspace, 0x22, 32);
    timer4.CC[0] = 1;
    timer3.CC[1] = 999;
    timer3.CC[3] = 1000;
    timer3.CC[2] = 2000;
    spim.EVENTS_STARTED = 1;
    receive_response();
    assert(capture()->receiving && capture()->tag.samples == 32);
    receive_response();
    assert(capture()->handled_banks == 1 && capture()->handled_bytes == 0);
    assert(capture()->tag.samples == 65);
    receive_response();
    assert(capture()->receiving && capture()->tag.samples == 97);
    assert(overruns == before_overruns);
    nfcv_sniff_stop();
}

static void inventory_slot_rearms(bool replied, bool high_rate) {
    unsigned before_overruns = overruns;
    begin_session(high_rate);
    capture()->reader.inventory_slots = 15;
    start_response();
    if (replied) {
        capture()->tag.length = 3;
        capture()->tag.data[0] = 0;
        iso15693_append_crc(capture()->tag.data, 1);
        publish_response(false);
        assert(published_flags == (HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED | HF_SNIFF_CRC_VALID));
    } else {
        clock_state.CYCCNT += (SOF_TIMEOUT_US + 1u) * CPU_CYCLES_PER_US;
        receive_response();
    }
    assert(!capture()->receiving);
    unsigned before_slot = published;
    capture()->reader.previous_lock = true;
    capture()->reader.last_fall = clock_state.CYCCNT - NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES;
    nfct.FIELDPRESENT = NFCT_FIELDPRESENT_FIELDPRESENT_Msk | NFCT_FIELDPRESENT_LOCKDETECT_Msk;
    nfcv_sniff_poll();
    assert(capture()->receiving && capture()->reader.inventory_slots == 14);
    assert(capture()->bank_bytes == (high_rate ? HIGH_RATE_BANK_BYTES : LOW_RATE_BANK_BYTES));
    assert(published == before_slot && overruns == before_overruns);
    nfct.FIELDPRESENT = 0;
    nfcv_sniff_poll();
    assert(capture()->reader.inventory_slots == 0);
    nfcv_sniff_stop();
    assert(!timer_owned && !channels && !frontend_owned && advertising);
}

int main(void) {
    assert(!nfcv_sniff_start(7));
    assert(!frontend_owned && !timer_owned && !channels && !close_count);
    hfclk_active = true;
    for (failure = FAIL_FRONTEND; failure <= FAIL_ASSIGN_SECOND; failure++) {
        allocations = 0;
        assignments = 0;
        close_count = 0;
        advertising = true;
        assert(!nfcv_sniff_start(7));
        assert(!timer_owned && !channels && !frontend_owned && advertising);
        assert(close_count == 1);
    }
    failure = FAIL_NONE;
    allocations = 0;
    assignments = 0;
    close_count = 0;
    assert(nfcv_sniff_start(7));
    assert(timer_owned && channels == 3 && !advertising);
    nfct.EVENTS_TXFRAMESTART = 1;
    nfcv_sniff_poll();
    assert(tx_observed == 1 && !interrupt_mask);
    capture()->reader.frame.single_subcarrier = true;
    capture()->reader.frame.high_data_rate = true;
    start_response();
    capture()->tag.length = 3;
    start_response();
    assert(published == 1 && published_flags == HF_SNIFF_TRUNCATED && overruns == 1);
    assert(capture()->receiving);
    capture()->tag.length = 3;
    nfcv_sniff_stop();
    assert(published == 2 && published_flags == HF_SNIFF_TRUNCATED);
    assert(!timer_owned && !channels && !frontend_owned && advertising);
    assert(capture_stops == 2);

    allocations = 0;
    assignments = 0;
    assert(nfcv_sniff_start(7));
    capture()->reader.frame.single_subcarrier = true;
    fail_dma = true;
    start_response();
    assert(!capture()->receiving && !enabled_channels && overruns == 2);
    nfcv_sniff_stop();
    assert(!timer_owned && !channels && !frontend_owned && advertising);

    replacing_empty_receive_is_not_a_loss();
    dma_loss_preserves_prefix(1, false);
    dma_loss_preserves_prefix(2, false);
    dma_loss_preserves_prefix(1, true);
    dma_restart_with_unread_tail_is_safe();
    inventory_slot_rearms(false, false);
    inventory_slot_rearms(false, true);
    inventory_slot_rearms(true, false);
    inventory_slot_rearms(true, true);
}
