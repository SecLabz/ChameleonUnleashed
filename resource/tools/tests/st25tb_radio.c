#include "st25tb/model.h"
#include "st25tb/receiver.h"
#include "iso15693/frame.h"
#include "hf_frontend_stubs/rc522.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Register values are deliberately independent of the production constants. */
#define BUTTON_1 0u
#define BUTTON_2 1u
#define HF_SPI_SELECT 2u
#define HF_SPI_SCK 3u
#define HF_SPI_MOSI 4u
#define HF_SPI_MISO 5u
#define READER_POWER 6u
#define HF_ANT_SEL 36u
#define NRF_GPIO_PIN_NOPULL 0u
#define NRF_GPIO_PIN_DIR_OUTPUT 1u
#define NRF_GPIO_PIN_INPUT_DISCONNECT 0u
#define NRF_GPIO_PIN_H0H1 3u
#define NRF_GPIO_PIN_NOSENSE 0u
#define SPI_FREQUENCY_FREQUENCY_M8 0x80000000u
#define SPIM_FREQUENCY_FREQUENCY_M8 0x80000000u
#define SPI_ENABLE_ENABLE_Enabled 1u
#define SPIM_ENABLE_ENABLE_Enabled 7u
#define SPIM_SHORTS_END_START_Msk 1u
#define RF_HF_SNIFF_BYTES 49152u
#include <stdlib.h>
static uint32_t rf_storage[49152 / 4];
static void *rf_hf_sniff_workspace(void) { return rf_storage; }
static bool start_ppi_owned;
static bool start_ppi_enabled;
static bool delayed_reply;
static unsigned transaction_start;
static unsigned written_bytes;
static unsigned transactions;
static unsigned produced_valid;
static unsigned request_offset;
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define NRFX_SUCCESS 0u
#define NRF_TIMER_MODE_COUNTER 1u
#define NRF_TIMER_MODE_TIMER 0u
#define NRF_TIMER_BIT_WIDTH_32 3u
#define NRF_TIMER_FREQ_16MHz 0u
#define NRFX_TIMER_INSTANCE(n) {n}
#define NRFX_TIMER_DEFAULT_CONFIG {0}
#define __DSB() hardware_sync()

typedef struct {
    unsigned instance_id;
} nrfx_timer_t;

typedef struct {
    unsigned mode;
    unsigned bit_width;
    unsigned frequency;
} nrfx_timer_config_t;

typedef unsigned nrf_timer_event_t;
typedef unsigned nrf_ppi_channel_t;

typedef struct {
    uint32_t ENABLE;
    uint32_t EVENTS_READY;
    uint32_t TXD;
    uint32_t RXD;
    uint32_t PSELSCK;
    uint32_t PSELMOSI;
    uint32_t PSELMISO;
    uint32_t FREQUENCY;
    uint32_t CONFIG;
} test_spi_t;

typedef struct {
    uint32_t ENABLE;
    uint32_t CONFIG;
    uint32_t ORC;
    uint32_t STALLSTAT;
    uint32_t FREQUENCY;
    uint32_t EVENTS_END;
    uint32_t EVENTS_STARTED;
    uint32_t EVENTS_STOPPED;
    uint32_t TASKS_START;
    uint32_t TASKS_STOP;
    uint32_t SHORTS;

    struct {
        uint32_t SCK;
        uint32_t MOSI;
        uint32_t MISO;
    } PSEL;

    struct {
        uint32_t PTR;
        uint32_t MAXCNT;
        uint32_t LIST;
        uint32_t AMOUNT;
    } TXD, RXD;
} test_spim_t;

typedef struct {
    uint32_t TASKS_CAPTURE[3];
    uint32_t TASKS_CLEAR;
    uint32_t TASKS_COUNT;
    uint32_t CC[3];
} test_timer_t;

typedef struct {
    uint32_t CYCCNT;
    uint32_t CTRL;
} test_dwt_t;

static test_spi_t spi;
static test_spim_t spim;
static test_spim_t spim0;
/* Native alias of the SPIM3 erratum195 store, reset on each new stream. */
static uint32_t spim3_erratum195;
static test_timer_t counter, clock_timer;
static test_dwt_t cycles;

static struct {
    uint32_t DEMCR;
} debug;

static struct {
    uint32_t INTENSET;
    uint32_t INTENCLR;
    uint32_t EVENTS_READY;
    uint32_t EVENTS_FIELDDETECTED;
    uint32_t EVENTS_FIELDLOST;
    uint32_t TASKS_SENSE;
    uint32_t TASKS_ACTIVATE;
    uint32_t TASKS_DISABLE;
    uint32_t FIELDPRESENT;
} nfct;

static struct {
    uint32_t OUTSET;
    uint32_t OUTCLR;
} gpio;

static const char *scenario;
static uint32_t pins[64];
static uint32_t slot_end;
static uint32_t timer_origin;
static uint32_t bank_due;
static uint32_t bank_pointer;
static uint32_t irq_mask;
static uint32_t initial_irq_mask;
static uint32_t gpio_first_cycles;
static uint32_t first_stream_cycles;
static uint8_t registers[64];
static uint8_t spi_address;
static uint8_t samples[50000];
static uint8_t consumed[50000];
static uint8_t gpio_levels[2500];
static uint32_t gpio_times[2500];
static unsigned pair_times[32768];
static unsigned pair_time_count;
static unsigned sample_count;
static unsigned sample_position;
static unsigned consumed_count;
static unsigned bank_count;
static unsigned stream_starts;
static unsigned stream_stops;
static unsigned resets;
static unsigned settles;
static unsigned gpio_edges;
static unsigned slot_calls;
static uint32_t stream_started[256];
static uint32_t field_stop_cycles;
static uint32_t field_loss_cycles;
static uint32_t nfct_ready_at;
static unsigned field_losses;
static unsigned decoded_sample_count;
static unsigned initialized;
static unsigned enabled;
static unsigned ppi_owned;
static unsigned ppi_enabled;
static unsigned advertising_pauses;
static unsigned advertising_resumes;
static bool selected;
static bool address_sent;
static bool streaming;
static bool external_field = true;
static bool injected;
static bool tracing = true;
static bool nfct_activating;
static bool fail_spi;
static bool fail_start;
static bool fail_stop;
static bool fail_dma;
bool g_is_reader_antenna_on;

static bool is_scenario(const char *name) {
    return strcmp(scenario, name) == 0;
}

static bool continuous_capture(void) {
    return is_scenario("field_dips") || is_scenario("field_drop_busy") || is_scenario("field_return") ||
           is_scenario("field_return_busy") || is_scenario("field_unreadable") || is_scenario("active_deadline") ||
           is_scenario("idle_deadline") || is_scenario("field_off_deadline");
}

static bool field_at_antenna(void) {
    if (!stream_starts) {
        return external_field;
    }
    unsigned elapsed_us = (cycles.CYCCNT - first_stream_cycles) / 64u;
    bool tag_path = pins[HF_ANT_SEL] != 0;
    if (is_scenario("field_off_deadline") && elapsed_us >= 1000) {
        return false;
    }
    if (is_scenario("field_return") || is_scenario("field_return_busy")) {
        return elapsed_us < 1000 || elapsed_us >= 21000;
    }
    if (is_scenario("field_drop_busy") && elapsed_us >= 1000) {
        return false;
    }
    if (!tag_path && is_scenario("field_unreadable")) {
        return false;
    }
    if (!tag_path && is_scenario("field_dips") && elapsed_us % 1750 < 1500) {
        return false;
    }
    if (!tag_path && is_scenario("field_dip_reply") && elapsed_us >= 150 && elapsed_us < 900) {
        return false;
    }
    return external_field;
}

static uint8_t next_sample(void) {
    if (continuous_capture()) {
        unsigned position = sample_position++;
        bool busy = is_scenario("field_drop_busy") || is_scenario("field_return_busy") ||
                    (is_scenario("field_unreadable") && position < 10000);
        if (busy) {
            /* Frequent sign changes keep the production decoder out of idle. */
            return (position / 10u) & 1u ? 0x77 : 0x88;
        }
        return 0;
    }
    return sample_position < sample_count ? samples[sample_position++] : 0;
}

static void hardware_sync(void);

static uint8_t *dma_pointer(uint32_t address);

static void latch_bank(unsigned started) {
    bank_pointer = spim.RXD.PTR;
    transaction_start = started;
    bank_due = started + 8192u * 64u + 32u;
    written_bytes = 0;
    transactions++;
    spim.EVENTS_STARTED = !(is_scenario("next_start_timeout") && transactions == 2);
    if (start_ppi_enabled && spim.EVENTS_STARTED) {
        clock_timer.CC[0] = (started - timer_origin) / 4u;
    }
}

static void hardware_sync(void) {
    if (gpio.OUTSET || gpio.OUTCLR) {
        assert((gpio.OUTSET | gpio.OUTCLR) == (1u << 4));
        if (!gpio_edges) {
            gpio_first_cycles = cycles.CYCCNT;
        }
        assert(gpio_edges < sizeof(gpio_levels));
        gpio_levels[gpio_edges] = gpio.OUTSET != 0;
        gpio_times[gpio_edges] = cycles.CYCCNT;
        gpio_edges++;
        gpio.OUTCLR = 0;
        gpio.OUTSET = 0;
    }
    if (counter.TASKS_CLEAR) {
        counter.TASKS_CLEAR = 0;
        bank_count = 0;
    }
    if (counter.TASKS_CAPTURE[0]) {
        counter.TASKS_CAPTURE[0] = 0;
        counter.CC[0] = bank_count;
    }
    if (clock_timer.TASKS_CAPTURE[1]) {
        clock_timer.TASKS_CAPTURE[1] = 0;
        if (!is_scenario("reply_clock_timeout")) {
            clock_timer.CC[1] = (cycles.CYCCNT - timer_origin) / 4u;
        }
    }
    if (nfct.TASKS_ACTIVATE) {
        nfct.TASKS_ACTIVATE = 0;
        nfct.TASKS_DISABLE = 0;
        nfct_activating = true;
        nfct_ready_at = cycles.CYCCNT + (is_scenario("delayed_ready") ? 500u * 64u : 0);
    }
    if (nfct.TASKS_DISABLE) {
        nfct_activating = false;
        nfct.EVENTS_READY = 0;
    }
    if (nfct_activating && !is_scenario("unready_frame") && (int32_t)(cycles.CYCCNT - nfct_ready_at) >= 0) {
        nfct.EVENTS_READY = 1;
    }
    nfct.FIELDPRESENT = nfct.EVENTS_READY && field_at_antenna() ? 1 : 0;
    if (spim.TASKS_STOP) {
        spim.TASKS_STOP = 0;
        stream_stops++;
        if (!fail_stop) {
            streaming = false;
            spim.EVENTS_STOPPED = 1;
        }
    }
    if (spim.TASKS_START) {
        spim.TASKS_START = 0;
        assert(!spi.ENABLE && spim.ENABLE == 7 && selected && ppi_enabled && start_ppi_enabled);
        assert(spim.RXD.MAXCNT == 8192 && !spim.TXD.MAXCNT && !spim.TXD.PTR && spim.ORC == 0xf6);
        if (!stream_starts) {
            first_stream_cycles = cycles.CYCCNT;
        }
        assert(stream_starts < sizeof(stream_started) / sizeof(stream_started[0]));
        stream_started[stream_starts++] = cycles.CYCCNT;
        if (!fail_start) {
            streaming = true;
            transactions = 0;
            spim3_erratum195 = 0;
            latch_bank(cycles.CYCCNT);
        }
    }
    if (!spim.ENABLE) {
        streaming = false;
    }
    while (streaming && !fail_dma) {
        unsigned ready = cycles.CYCCNT > transaction_start + 32u ?
                         (cycles.CYCCNT - transaction_start - 32u) / 64u : 0;
        if (ready > 8192) {
            ready = 8192;
        }
        unsigned ram_lag = is_scenario("slow_ram") ? 24 : 4;
        unsigned committed = ready == 8192 ? 8192 : ready > ram_lag ? ready - ram_lag : 0;
        uint8_t *bank = dma_pointer(bank_pointer);
        while (written_bytes < committed) {
            if (transactions == 1 && written_bytes == 0) {
                bank[written_bytes] = 0xff;
            } else {
                produced_valid++;
                if (!tracing && !continuous_capture()) {
                    unsigned physical_us = (transaction_start - first_stream_cycles +
                                            32u + (written_bytes + 1u) * 64u) / 64u;
                    unsigned position = physical_us >= 2 ? physical_us - 2 : 0;
                    bank[written_bytes] = position < sample_count ? samples[position] : 0;
                    sample_position = position + 1u;
                } else {
                    bank[written_bytes] = next_sample();
                }
            }
            written_bytes++;
        }
        if (is_scenario("rx_stall") && ready > 100) {
            spim.STALLSTAT = 2;
        }
        if (ready < 8192) {
            break;
        }
        spim.RXD.AMOUNT = is_scenario("partial_bank") ? 8191 : 8192;
        spim.EVENTS_END = 1;
        if (ppi_enabled) {
            bank_count++;
            clock_timer.CC[2] = (bank_due - timer_origin) / 4u;
            if (is_scenario("phase_fault")) {
                clock_timer.CC[2] += 40;
            }
        }
        if (!(spim.SHORTS & 1)) {
            streaming = false;
            break;
        }
        unsigned started = bank_due + (is_scenario("bank_gaps") ? 3u * 64u : 32u);
        latch_bank(started);
    }
}

static test_dwt_t *test_dwt(void) {
    /* Resolve TX at one CPU cycle; lifecycle scenarios advance one microsecond. */
    cycles.CYCCNT += is_scenario("tx_waveform") ? 1u : 64u;
    assert(cycles.CYCCNT < 4000000u * 64u);
    hardware_sync();
    /* Clock the byte after the caller has written TXD, before it polls READY. */
    if (selected && spi.ENABLE && !spi.EVENTS_READY && !fail_spi) {
        if (!address_sent) {
            spi_address = spi.TXD;
            address_sent = true;
        } else if (spi_address & 0x80) {
            spi.RXD = registers[(spi_address & 0x7e) >> 1];
        } else {
            registers[(spi_address & 0x7e) >> 1] = spi.TXD;
        }
        spi.EVENTS_READY = 1;
    }
    return &cycles;
}

static test_spi_t *test_spi(void) {
    hardware_sync();
    return &spi;
}

static test_spim_t *test_spim(void) {
    hardware_sync();
    return &spim;
}

static test_timer_t *test_counter(void) {
    hardware_sync();
    return &counter;
}

static test_timer_t *test_clock(void) {
    if (streaming && bank_count == 1 && !injected && is_scenario("descriptor_race")) {
        injected = true;
        cycles.CYCCNT += 30000u * 64u;
    }
    hardware_sync();
    return &clock_timer;
}

#define DWT test_dwt()
#define NRF_SPI0 test_spi()
#define NRF_SPIM0 (&spim0)
#define NRF_SPIM3 test_spim()
#define NRF_TIMER3 test_counter()
#define NRF_TIMER4 test_clock()
#define NRF_NFCT (&nfct)
#define NRF_P1 (&gpio)
#define CoreDebug (&debug)

static void nrf_gpio_pin_set(uint32_t pin) {
    pins[pin] = 1;
    if (pin == HF_SPI_SELECT) {
        selected = false;
    }
}

static void nrf_gpio_pin_clear(uint32_t pin) {
    pins[pin] = 0;
    if (pin == HF_SPI_SELECT) {
        selected = true;
        address_sent = false;
    }
}

static uint32_t nrf_gpio_pin_read(uint32_t pin) {
    if (is_scenario("button") && stream_starts && consumed_count >= 128) {
        return pin == BUTTON_2;
    }
    if (continuous_capture()) {
        if (!is_scenario("active_deadline") && !is_scenario("idle_deadline") && !is_scenario("field_off_deadline") &&
            cycles.CYCCNT >= field_stop_cycles) {
            return pin == BUTTON_2;
        }
    } else if (!is_scenario("window_deadline") && !is_scenario("dma_timeout") &&
               stream_starts && consumed_count >= sample_count) {
        /* Press after the final bank is acknowledged, before the next bank.
         * Finite captures use the same physical-button cleanup as real input. */
        return pin == BUTTON_2;
    }
    return pins[pin];
}

static void nrf_gpio_cfg_output(uint32_t pin) {
    (void)pin;
}

static void nrf_gpio_cfg_input(uint32_t pin, unsigned pull) {
    assert(pin == HF_SPI_MISO && pull == 0);
}

static void nrf_gpio_cfg(uint32_t pin, unsigned dir, unsigned input, unsigned pull, unsigned drive, unsigned sense) {
    assert(pin == HF_ANT_SEL && dir == 1 && input == 0 && pull == 0 && drive == 3 && sense == 0);
}

static void bsp_delay_ms(uint32_t ms) {
    cycles.CYCCNT += ms * 64000u;
}

static void bsp_wdt_feed(void) {
}

typedef enum {
    NRFX_POWER_USB_STATE_DISCONNECTED,
    NRFX_POWER_USB_STATE_CONNECTED,
    NRFX_POWER_USB_STATE_READY,
} nrfx_power_usb_state_t;

static nrfx_power_usb_state_t nrfx_power_usbstatus_get(void) {
    if (is_scenario("usb_enumerating_window")) {
        return NRFX_POWER_USB_STATE_CONNECTED;
    }
    if (is_scenario("usb_idle_window") || is_scenario("usb_active_window") ||
        is_scenario("usb_ble_window") || (is_scenario("usb_disconnect_window") && !slot_calls)) {
        return NRFX_POWER_USB_STATE_READY;
    }
    return NRFX_POWER_USB_STATE_DISCONNECTED;
}

static bool advertising_pause(bool *resume) {
    *resume = false;
    if (is_scenario("rf_only_window")) {
        return true;
    }
    if (is_scenario("ble_window") || is_scenario("usb_ble_window")) {
        return false;
    }
    *resume = true;
    advertising_pauses++;
    return true;
}

static void advertising_start(bool erase) {
    assert(!erase);
    advertising_resumes++;
}

static uint32_t __get_PRIMASK(void) {
    return irq_mask;
}

static void __disable_irq(void) {
    irq_mask = 1;
}

static void __set_PRIMASK(uint32_t mask) {
    irq_mask = mask;
}

static unsigned nrfx_timer_init(const nrfx_timer_t *timer, const nrfx_timer_config_t *config,
                                void (*handler)(nrf_timer_event_t, void *)) {
    assert(handler && (timer->instance_id == 3 || timer->instance_id == 4));
    assert(config->mode == (timer->instance_id == 3) && config->bit_width == 3);
    if ((timer->instance_id == 3 && is_scenario("counter_init")) ||
        (timer->instance_id == 4 && is_scenario("clock_init"))) {
        return 1;
    }
    assert(!(initialized & (1u << timer->instance_id)));
    initialized |= 1u << timer->instance_id;
    if (timer->instance_id == 4) {
        timer_origin = cycles.CYCCNT;
    }
    return 0;
}

static void nrfx_timer_uninit(const nrfx_timer_t *timer) {
    assert(initialized & (1u << timer->instance_id));
    initialized &= ~(1u << timer->instance_id);
    enabled &= ~(1u << timer->instance_id);
}

static void nrfx_timer_enable(const nrfx_timer_t *timer) {
    assert(initialized & (1u << timer->instance_id));
    enabled |= 1u << timer->instance_id;
}

static unsigned nrfx_ppi_channel_alloc(nrf_ppi_channel_t *channel) {
    if (is_scenario("ppi_alloc")) {
        return 1;
    }
    if (ppi_owned) {
        if (is_scenario("start_ppi_alloc")) {
            return 1;
        }
        assert(!start_ppi_owned);
        start_ppi_owned = true;
        *channel = 8;
        return 0;
    }
    ppi_owned = 1;
    *channel = 7;
    return 0;
}

static unsigned nrfx_ppi_channel_free(nrf_ppi_channel_t channel) {
    if (channel == 8) {
        assert(start_ppi_owned && !start_ppi_enabled);
        start_ppi_owned = false;
        return 0;
    }
    assert(channel == 7 && ppi_owned);
    ppi_enabled = 0;
    ppi_owned = 0;
    return 0;
}

static unsigned nrfx_ppi_channel_assign(nrf_ppi_channel_t channel, uint32_t event, uint32_t task) {
    if (channel == 8) {
        assert(start_ppi_owned && event == (uint32_t)&spim.EVENTS_STARTED);
        assert(task == (uint32_t)&clock_timer.TASKS_CAPTURE[0]);
        return is_scenario("start_ppi_assign");
    }
    assert(channel == 7 && ppi_owned && event == (uint32_t)&spim.EVENTS_END);
    assert(task == (uint32_t)&counter.TASKS_COUNT);
    return is_scenario("ppi_assign");
}

static unsigned nrfx_ppi_channel_fork_assign(nrf_ppi_channel_t channel, uint32_t task) {
    if (channel == 8) {
        assert(start_ppi_owned && !task);
        return is_scenario("start_ppi_fork");
    }
    assert(channel == 7 && ppi_owned);
    assert(!task || task == (uint32_t)&clock_timer.TASKS_CAPTURE[2]);
    return task && is_scenario("ppi_fork");
}

static unsigned nrfx_ppi_channel_enable(nrf_ppi_channel_t channel) {
    assert(enabled == ((1u << 3) | (1u << 4)));
    if (channel == 8) {
        assert(start_ppi_owned);
        if (is_scenario("start_ppi_enable")) {
            return 1;
        }
        start_ppi_enabled = true;
        return 0;
    }
    assert(channel == 7 && ppi_owned);
    if (is_scenario("ppi_enable")) {
        return 1;
    }
    ppi_enabled = 1;
    return 0;
}

static unsigned nrfx_ppi_channel_disable(nrf_ppi_channel_t channel) {
    hardware_sync();
    if (channel == 8) {
        assert(start_ppi_owned);
        start_ppi_enabled = false;
        return 0;
    }
    assert(channel == 7 && ppi_owned);
    ppi_enabled = 0;
    return 0;
}

static uint32_t rf_timeslot_deadline(void) {
    return slot_end;
}

static bool rf_timeslot_run(uint32_t us, bool high, bool extend, void (*work)(void)) {
    unsigned expected_us = 500000;
    bool ble_connected = is_scenario("ble_window") || is_scenario("usb_ble_window");
    if (ble_connected || is_scenario("usb_idle_window") || is_scenario("usb_active_window") ||
        is_scenario("usb_enumerating_window") || (is_scenario("usb_disconnect_window") && !slot_calls)) {
        expected_us = 25000;
    } else if (is_scenario("active_window") || is_scenario("rf_only_window") || is_scenario("active_deadline") ||
               is_scenario("field_off_deadline")) {
        expected_us = 2000000;
    }
    assert(us == expected_us && high && extend == !ble_connected);
    slot_calls++;
    if (is_scenario("timeslot_denied")) {
        return false;
    }
    unsigned duration_us =
        is_scenario("banks") || is_scenario("bank_gaps") || is_scenario("descriptor_race") ||
        is_scenario("partial_bank") || is_scenario("next_start_timeout") || is_scenario("phase_fault") ?
        60000 : 25000;
    if (is_scenario("window_deadline")) {
        duration_us = 4200;
    } else if (is_scenario("field_unreadable")) {
        duration_us = 240000;
    } else if (is_scenario("field_return") || is_scenario("field_return_busy")) {
        duration_us = 50000;
    } else if (is_scenario("active_deadline") || is_scenario("idle_deadline") || is_scenario("field_off_deadline")) {
        duration_us = us;
    }
    slot_end = cycles.CYCCNT + duration_us * 64u;
    work();
    return true;
}

/* Spies retain the real decoder, while allowing exact byte-order assertions. */
static void observed_receiver_reset(void) {
    resets++;
    st25tb_receiver_reset();
}

static void observed_receiver_settle(void) {
    settles++;
    st25tb_receiver_settle();
}

static void observed_field_lost(void) {
    field_losses++;
    field_loss_cycles = cycles.CYCCNT;
    st25tb_field_lost();
}

static void observed_receiver_time(unsigned now) {
    if (continuous_capture()) {
        st25tb_receiver_time(now);
        return;
    }
    assert(pair_time_count < sizeof(pair_times) / sizeof(pair_times[0]));
    pair_times[pair_time_count++] = now;
    st25tb_receiver_time(now);
}

static void observe_samples(const uint8_t *data, unsigned size) {
    /* An END arriving while the descriptor is published invalidates the bank
     * before any decoder call, rather than after its samples were consumed. */

    if (continuous_capture()) {
        consumed_count += size;
        return;
    }
    assert(consumed_count + size <= sizeof(consumed));
    memcpy(consumed + consumed_count, data, size);
    consumed_count += size;
}

static void after_decode(unsigned length) {
    if (length) {
        decoded_sample_count = consumed_count;
    }
    if (delayed_reply && !injected && consumed_count >= 128) {
        cycles.CYCCNT += 500u * 64u;
        injected = true;
    }
    if (!length || injected) {
        return;
    }
    decoded_sample_count = consumed_count;
    if (is_scenario("decode_overrun")) {
        cycles.CYCCNT += 195u * 64u;
        injected = true;
    } else if (is_scenario("reply_deadline")) {
        cycles.CYCCNT = slot_end - 5000u * 64u;
        injected = true;
    }
}

static bool observed_receiver_pair(uint8_t first, uint8_t second, uint8_t *frame, unsigned *length) {
    uint8_t pair[] = {first, second};
    observe_samples(pair, 2);
    bool found = !tracing && st25tb_receiver_pair(first, second, frame, length);
    after_decode(*length);
    return found;
}

static unsigned observed_receiver_batch(const uint8_t *data, unsigned size, uint8_t *frame, unsigned *length) {
    unsigned used = tracing ? size : st25tb_receiver_batch(data, size, frame, length);
    observe_samples(data, used);
    after_decode(*length);
    return used;
}

void nfc_st25tb_radio_stop(void);

/* RADIO_IMPLEMENTATION */

static uint8_t *dma_pointer(uint32_t address) {
    for (unsigned bank = 0; bank < STB_DMA_BANK_COUNT; bank++) {
        uint8_t *pointer = stb_dma_at(bank);
        if ((uint32_t)(uintptr_t)pointer == address) {
            return pointer;
        }
    }
    assert(!"DMA outside RF banks");
    return NULL;
}

static void assert_released(void) {
    assert(!spim.ENABLE);
    if (stream_starts) {
        assert(spim3_erratum195 == 1);
    }
    hardware_sync();
    assert(!initialized && !enabled && !ppi_owned && !ppi_enabled && !start_ppi_owned && !start_ppi_enabled);
    assert(!streaming && !spim.ENABLE && !spim.SHORTS && !selected);
    assert(irq_mask == initial_irq_mask);
    assert(advertising_pauses == advertising_resumes);
    if (stream_starts) {
        assert(nfct.TASKS_DISABLE && nfct.INTENSET == 0x1234);
        assert(pins[HF_ANT_SEL]);
    }
}

static void assert_stopped(void) {
    nfc_st25tb_radio_stop();
    assert_released();
    assert(!pins[READER_POWER] && !spi.ENABLE && !g_is_reader_antenna_on);
    if (radio_ok) {
        assert(registers[0x14] == 0x80 && registers[0x2a] == 0);
        assert(registers[0x15] == 0x40 && registers[0x11] == 0x3d);
    }
    nfc_st25tb_radio_stop(); /* Idempotent, including after a partial acquisition. */
}

static void load_selected_tag(void) {
    uint8_t image[77] = {0};
    image[5] = 0x33;
    image[6] = 0x02;
    image[7] = 0xd0;
    image[76] = 0xa1;
    image[8] = 0x12;
    image[9] = 0x34;
    image[10] = 0x56;
    image[11] = 0x78;
    assert(st25tb_load(image));
    uint8_t inventory[] = {6, 0, 0, 0};
    uint8_t select[] = {14, 0xa1, 0, 0};
    uint8_t reply[10];
    iso15693_append_crc(inventory, 2);
    iso15693_append_crc(select, 2);
    assert(st25tb_reply(inventory, 4, reply) == 3);
    assert(st25tb_reply(select, 4, reply) == 3);
}

static void prepare_read(void) {
    /* A complete NRZ request, filtered like the receiver's synthetic captures. */
    uint8_t frame[] = {8, 0, 0, 0};
    uint8_t bits[80];
    bool eof_noise = is_scenario("eof_noise") || is_scenario("eof_noise_stale");
    iso15693_append_crc(frame, 2);
    unsigned count = 0;
    for (unsigned n = 0; n < 10; n++) {
        bits[count++] = 0;
    }
    for (unsigned n = 0; n < 2; n++) {
        bits[count++] = 1;
    }
    for (unsigned n = 0; n < 4; n++) {
        bits[count++] = 0;
        for (unsigned b = 0; b < 8; b++) {
            bits[count++] = (frame[n] >> b) & 1;
        }
        bits[count++] = 1;
    }
    unsigned eof_bits = eof_noise ? 11 : 10;
    for (unsigned n = 0; n < eof_bits; n++) {
        bits[count++] = 0;
    }
    for (unsigned n = 0; n < 10; n++) {
        bits[count++] = 1;
    }
    double baseline = 1;
    sample_count = 1500 + request_offset;
    for (unsigned t = 0; t < sample_count; t++) {
        unsigned bit = t < 100 + request_offset ? count : (t - 100 - request_offset) * 1356u / 12800u;
        double level = bit < count ? bits[bit] : 1;
        baseline += (level - baseline) / 5;
        int value = (int)round((level - baseline) * 10 - 0.5);
        if (value < -8) {
            value = -8;
        }
        if (value > 7) {
            value = 7;
        }
        samples[t] = ((value & 15) << 4) | (value & 15);
        if (eof_noise) {
            /* An isolated Q pulse cannot end I's invalid EOF. */
            samples[t] &= 0xf0;
            if (t >= 686 && t < 688) {
                samples[t] |= 7;
            }
            if (is_scenario("eof_noise_stale")) {
                /* An unrelated, expired Q SOF is insufficient evidence too. */
                if ((t >= 200 && t < 202) || (t >= 313 && t < 315)) {
                    samples[t] |= 9;
                }
                if (t >= 294 && t < 296) {
                    samples[t] |= 7;
                }
            }
        }
    }
    tracing = false;
}

static void assert_read_waveform(void) {
    uint8_t reply[] = {0x12, 0x34, 0x56, 0x78, 0, 0};
    iso15693_append_crc(reply, 4);
    assert(gpio_edges == 102u * 16u + 1u);
    for (unsigned bit = 0; bit < 102; bit++) {
        unsigned value;
        if (bit < 16) {
            value = 1;
        } else if (bit < 27) {
            value = 0;
        } else if (bit < 29) {
            value = 1;
        } else if (bit < 89) {
            unsigned position = (bit - 29) % 10;
            unsigned byte = (bit - 29) / 10;
            value = position == 9 || (position && ((reply[byte] >> (position - 1)) & 1));
        } else {
            value = bit >= 100;
        }
        for (unsigned half = 0; half < 16; half++) {
            assert(gpio_levels[bit * 16 + half] == ((half & 1u) != value));
        }
    }
    assert(gpio_levels[102 * 16] == 0);
}

static void check_tx_timing(void) {
    uint8_t reply[] = {0x12, 0x34, 0x56, 0x78, 0, 0};
    iso15693_append_crc(reply, 4);
    radio_ok = true;
    slot_end = 10000u * 64u;
    uint32_t eof_cycles = 1000u * 64u;
    cycles.CYCCNT = eof_cycles;
    stb_transmit(reply, sizeof(reply), eof_cycles / 4u + stb_t0_ticks);
    /* Observe the final idle write at the same clock boundary as every edge. */
    (void)DWT->CYCCNT;
    assert(radio_ok && stb_responded);
    assert_read_waveform();
    /* 64 MHz / (2 * 847.5 kHz) = 12800/339 CPU cycles per half-period. */
    for (unsigned edge = 1; edge < gpio_edges; edge++) {
        assert(gpio_times[edge] - gpio_times[0] == edge * 12800u / 339u);
    }
    uint32_t carrier_delay = gpio_times[0] - eof_cycles;
    assert(carrier_delay >= 158u * 64u + 128u);
    assert(carrier_delay < 158u * 64u + 140u);
    uint32_t training_cycles = gpio_times[16u * 16u] - gpio_times[0];
    assert(training_cycles >= 151u * 64u && training_cycles < 152u * 64u);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    delayed_reply = !strcmp(argv[1], "delayed_reply");
    scenario = delayed_reply ? "reply" : argv[1];
    if (!strncmp(argv[1], "shift_", 6)) {
        request_offset = (unsigned)strtoul(argv[1] + 6, NULL, 10);
        scenario = "reply";
    }
    if (is_scenario("tx_waveform")) {
        check_tx_timing();
        puts(argv[1]);
        return 0;
    }
    nfct.INTENSET = 0x1234;
    load_selected_tag();
    sample_count = 454;
    if (is_scenario("banks") || is_scenario("bank_gaps") || is_scenario("descriptor_race") ||
        is_scenario("partial_bank") || is_scenario("next_start_timeout") || is_scenario("phase_fault")) {
        sample_count = 40000;
    }
    for (unsigned n = 0; n < sample_count; n++) {
        samples[n] = n;
    }
    if (is_scenario("reply") || is_scenario("unready_frame") || is_scenario("delayed_ready") ||
        is_scenario("decode_overrun") || is_scenario("reply_deadline") || is_scenario("reply_clock_timeout") ||
        is_scenario("field_dip_reply") || is_scenario("eof_noise") || is_scenario("eof_noise_stale")) {
        prepare_read();
    }
    if (continuous_capture()) {
        tracing = false;
        unsigned capture_us = 6000;
        if (is_scenario("field_dips")) {
            capture_us = 8000;
        } else if (is_scenario("field_return") || is_scenario("field_return_busy")) {
            capture_us = 45000;
        } else if (is_scenario("field_unreadable")) {
            capture_us = 225000;
        }
        field_stop_cycles = capture_us * 64u;
    }
    if (is_scenario("reply")) {
        irq_mask = 1;
        initial_irq_mask = 1;
    }
    fail_spi = is_scenario("spi_timeout");
    fail_dma = is_scenario("dma_timeout");
    fail_start = is_scenario("start_timeout");
    fail_stop = is_scenario("stop_timeout");
    if (is_scenario("unloaded")) {
        st25tb_unload();
    }
    if (is_scenario("idle_deadline")) {
        external_field = false;
    }
    bool present;
    uint32_t poll_started = cycles.CYCCNT;
    bool sampled =
        nfc_st25tb_radio_poll(&present, is_scenario("active_window") || is_scenario("rf_only_window") ||
                                            is_scenario("active_deadline") || is_scenario("field_off_deadline") ||
                                            is_scenario("usb_active_window"));
    assert_released();
    if (is_scenario("banks") || is_scenario("bank_gaps") || is_scenario("button") || is_scenario("ble_window") ||
        is_scenario("active_window") || is_scenario("rf_only_window") ||
        is_scenario("usb_idle_window") || is_scenario("usb_active_window") ||
        is_scenario("usb_ble_window") || is_scenario("usb_enumerating_window") ||
        is_scenario("usb_disconnect_window")) {
        assert(sampled && present && !gpio_edges && !settles && stream_starts == 1);
        unsigned wanted = is_scenario("button") ? 128 : sample_count;
        assert(consumed_count >= wanted && consumed_count < wanted + STB_BATCH);
        assert(!memcmp(consumed, samples, wanted));
        for (unsigned n = 1; n < pair_time_count; n++) {
            assert(pair_times[n] >= pair_times[n - 1]);
        }
        assert(cycles.CYCCNT < slot_end - 10000u * 64u);
        if (is_scenario("usb_disconnect_window")) {
            sample_position = 0;
            assert(nfc_st25tb_radio_poll(&present, false) && present);
            assert(slot_calls == 2);
            assert_released();
        }
    } else if (is_scenario("descriptor_race")) {
        assert(!sampled && injected && consumed_count && !gpio_edges);
    } else if (is_scenario("reply") || is_scenario("delayed_ready") || is_scenario("unready_frame") ||
               is_scenario("field_dip_reply")) {
        assert(sampled && present && stb_responded && settles == 1 && stream_starts == 2);
        assert_read_waveform();
        /* One address byte precedes the decoded samples; allow the simulator's
         * coarse clock observations around the scheduled first GPIO write. */
        uint32_t expected_tx = first_stream_cycles +
            (decoded_sample_count + 1u + (decoded_sample_count + 1u) / 8192u + 158u) * 64u;
        assert(gpio_first_cycles >= expected_tx && gpio_first_cycles < expected_tx + 10u * 64u);
    } else if (is_scenario("idle_deadline")) {
        assert(sampled && !present && !gpio_edges && !pins[READER_POWER]);
        assert(consumed_count > 490000 && consumed_count <= produced_valid && stream_starts > 1 && !field_losses);
        assert(slot_end >= poll_started + 500000u * 64u);
        assert(cycles.CYCCNT >= slot_end - 4100u * 64u);
        assert(cycles.CYCCNT < poll_started + 515000u * 64u);
    } else if (is_scenario("field_off_deadline")) {
        assert(sampled && !present && !stb_absence_pending && !gpio_edges);
        assert(field_losses == 1 && !pins[READER_POWER]);
        unsigned loss_us = (field_loss_cycles - first_stream_cycles) / 64u;
        assert(loss_us >= 18000 && loss_us < 19000);
        assert(cycles.CYCCNT >= slot_end - 4100u * 64u);
        assert(cycles.CYCCNT < poll_started + 2010000u * 64u);
    } else if (is_scenario("active_deadline")) {
        assert(sampled && present && !stb_absence_pending);
        assert(cycles.CYCCNT >= slot_end - 4100u * 64u);
        assert(cycles.CYCCNT < poll_started + 2000000u * 64u);
    } else if (is_scenario("field_dips")) {
        assert(sampled && present && !gpio_edges && stream_starts == 1 && resets == 1);
        assert(consumed_count > 4000 && !stb_absence_pending);
    } else if (is_scenario("eof_noise") || is_scenario("eof_noise_stale")) {
        assert(sampled && present && !gpio_edges && !settles && stream_starts == 1);
        assert(!decoded_sample_count && resets == 1);
    } else if (is_scenario("field_drop_busy")) {
        assert(sampled && present && !gpio_edges && stream_starts == 2);
        assert(stb_absence_pending && !st25tb_receiver_idle());
        unsigned detection_us = (stream_started[1] - first_stream_cycles) / 64u;
        assert(detection_us >= 3000 && detection_us < 3300);
    } else if (is_scenario("field_return") || is_scenario("field_return_busy")) {
        assert(sampled && present && !gpio_edges && !stb_absence_pending);
        assert(field_losses == 1);
        unsigned loss_us = (field_loss_cycles - first_stream_cycles) / 64u;
        assert(loss_us >= 18000 && loss_us < 19000);
        if (is_scenario("field_return_busy")) {
            assert(!st25tb_receiver_idle());
        }
        unsigned elapsed_us = (cycles.CYCCNT - first_stream_cycles) / 64u;
        assert(elapsed_us >= 21000 && elapsed_us < 36000);
        assert(cycles.CYCCNT < slot_end - 10000u * 64u);
        unsigned starts = stream_starts;
        uint32_t next_window = cycles.CYCCNT;
        field_stop_cycles = next_window + 6000u * 64u;
        assert(nfc_st25tb_radio_poll(&present, false) && present);
        assert(slot_calls == 2 && stream_starts == starts + 1);
        assert(slot_end >= next_window + 50000u * 64u);
        assert_released();
    } else if (is_scenario("field_unreadable")) {
        assert(sampled && present && !gpio_edges && stream_starts == 3 && resets == 3);
        assert(!stb_absence_pending);
        unsigned first_probe_us = (stream_started[1] - first_stream_cycles) / 64u;
        unsigned periodic_probe_us = (stream_started[2] - stream_started[1]) / 64u;
        assert(first_probe_us >= 2000 && first_probe_us < 2300);
        assert(periodic_probe_us >= 200000 && periodic_probe_us < 200300);
        /* The fallback belongs to one window; the next window tries NFCT again. */
        unsigned starts = stream_starts;
        uint32_t next_window = cycles.CYCCNT;
        field_stop_cycles = next_window + 6000u * 64u;
        assert(nfc_st25tb_radio_poll(&present, false) && present);
        assert(slot_calls == 2 && stream_starts == starts + 2);
        unsigned next_probe_us = (stream_started[starts + 1] - stream_started[starts]) / 64u;
        assert(next_probe_us >= 2000 && next_probe_us < 2300);
        assert_released();
    } else if (is_scenario("decode_overrun")) {
        assert(sampled && injected && !gpio_edges && !settles && stream_starts == 1);
    } else if (is_scenario("reply_deadline")) {
        assert(!sampled && injected && !gpio_edges && !settles);
    } else if (is_scenario("window_deadline")) {
        assert(sampled && cycles.CYCCNT >= slot_end - 4000u * 64u && !gpio_edges);
    } else if (is_scenario("timeslot_denied")) {
        assert(!sampled && slot_calls == 1 && !stream_starts);
    } else if (is_scenario("unloaded")) {
        assert(!sampled && !present && !slot_calls && !stream_starts);
    } else if (is_scenario("field_loss")) {
        assert(sampled && present);
        external_field = false;
        sample_position = 0;
        /* Probe sees absence, then confirms it after the guard has elapsed. */
        assert(nfc_st25tb_radio_poll(&present, false) && present);
        cycles.CYCCNT += 4000u * 64u;
        sample_position = 0;
        assert(nfc_st25tb_radio_poll(&present, false) && !present);
        assert(!pins[READER_POWER]);
    } else {
        assert(!sampled && !gpio_edges);
        assert(!pins[READER_POWER] && !spi.ENABLE);
    }
    assert_stopped();
    /* Every injected failure must leave enough state to open a fresh window. */
    scenario = "banks";
    fail_stop = false;
    fail_start = false;
    fail_dma = false;
    fail_spi = false;
    external_field = true;
    load_selected_tag();
    tracing = true;
    sample_count = 454;
    pair_time_count = 0;
    consumed_count = 0;
    sample_position = 0;
    for (unsigned n = 0; n < sample_count; n++) {
        samples[n] = n;
    }
    assert(nfc_st25tb_radio_poll(&present, false) && present);
    assert(consumed_count >= sample_count && consumed_count < sample_count + STB_BATCH);
    assert(!memcmp(consumed, samples, sample_count));
    assert_stopped();
    puts(argv[1]);
    return 0;
}
