#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "hf_frontend_stubs/rc522.h"

static void hf_sniff_stop(void) {
}

#define BUTTON_1 0
#define BUTTON_2 1
#define HF_ANT_SEL 2
#define HF_SPI_SCK 3
#define HF_SPI_MOSI 4
#define HF_SPI_MISO 5
#define ST25TB_REQUEST_MAX 9
#define ST25TB_RESPONSE_MAX 10
#define SPIM_SHORTS_END_START_Msk 1
#define SPIM_FREQUENCY_FREQUENCY_M8 8
#define SPIM_ENABLE_ENABLE_Enabled 7
#define SPI_ENABLE_ENABLE_Enabled 1
#define NRF_GPIO_PIN_DIR_OUTPUT 1
#define NRF_GPIO_PIN_INPUT_DISCONNECT 0
#define NRF_GPIO_PIN_NOPULL 0
#define NRF_GPIO_PIN_H0H1 0
#define NRF_GPIO_PIN_NOSENSE 0
#define NRF_SUCCESS 0
#define NRFX_SUCCESS 0
#define NRFX_NFCT_EVT_FIELD_DETECTED 1
#define NRFX_POWER_USB_STATE_DISCONNECTED 0
#define SettingsAnimationModeSymmetric 1
#define NRF_LOG_INFO(...) ((void)0)
#define APP_TIMER_TICKS(ms) (ms)
#define SLEEP_DELAY_MS_FIELD_NFC_LOST 1000
#define SLEEP_DELAY_MS_BUTTON_CLICK 4000
#define TAG_FIELD_LED_ON() ((void)0)
#define TAG_FIELD_LED_OFF() ((void)0)
#define __DSB() ((void)0)
#define SettingsButtonDisable 0
#define FULL_BOOT_GPREGRET2 2
#define GPREGRET_CLEAR_VALUE_DEFAULT 255
#define APP_ERROR_CHECK(x) assert((x) == 0)

typedef unsigned nrf_drv_gpiote_pin_t;
typedef unsigned nrf_gpiote_polarity_t;

typedef enum { DEVICE_MODE_TAG, DEVICE_MODE_READER } device_mode_t;

typedef struct {
    unsigned evt_id;
} nrfx_nfct_evt_t;

typedef struct {
    unsigned rxtx_int_mask;
    void (*cb)(nrfx_nfct_evt_t const *event);
} nrfx_nfct_config_t;

static uint32_t pins[6];
static uint32_t now;
static uint32_t timer_at;
static uint32_t radio_button_at;
static unsigned radio_button;
static unsigned radio_polls;
static unsigned stops;
static unsigned dispatches;
static unsigned dispatched;
static unsigned watches;
static unsigned uninitializations;
static bool rf_only;
static bool buttons_disabled;
static bool long_buttons_disabled;
static unsigned short_config[2] = {1, 2}, long_config[2] = {3, 4};
static bool disconnect_while_pressed;
static bool web_link;
static bool tag_emulation_web_link_active(void) { return web_link; }
static bool tag_emulation_web_link_stop(void) {
    bool active = web_link;
    web_link = false;
    return active;
}
static unsigned full_restarts;

static bool ble_slave_rf_only(void) {
    return rf_only;
}

static unsigned restart_step;

static void tag_emulation_sense_end(void) {
    assert(restart_step++ == 0);
}

static void tag_emulation_save(void) {
    assert(restart_step++ == 1);
}

static unsigned sd_power_gpregret_clr(unsigned n, unsigned bits) {
    assert(n == 1 && bits == 255 && restart_step++ == 2);
    return 0;
}

static unsigned sd_power_gpregret_set(unsigned n, unsigned bits) {
    assert(n == 1 && bits == FULL_BOOT_GPREGRET2 && restart_step++ == 3);
    return 0;
}

static unsigned sd_nvic_SystemReset(void) {
    assert(restart_step == 4);
    restart_step = 0;
    full_restarts++;
    return 0;
}

static bool radio_running;
static bool dma_stalled;
static bool m_is_field_on;
static bool g_is_tag_emulating;
static volatile bool g_usb_led_marquee_enable;
static bool g_usb_port_opened = true;
static bool polled_present = true;
static bool polled_active;
static bool usb_tx_ready = true;
static bool led_pwm_running;
static unsigned led_pwm_stops;
static bool sensing = true;
static bool field_active;
static bool probe_due;
static bool watching;
static uint32_t last_window;
static void *timer_arg;
static unsigned timer_result;
static const unsigned m_button_check_timer = 1;

/* RADIO_CONSTANTS */

/* BUTTON_STATE */

static device_mode_t get_device_mode(void) {
    return DEVICE_MODE_TAG;
}

static uint32_t app_timer_cnt_get(void) {
    return now;
}

static uint32_t app_timer_cnt_diff_compute(uint32_t a, uint32_t b) {
    return a - b;
}

static unsigned app_timer_start(unsigned timer, uint32_t ticks, void *arg) {
    if (timer_result) {
        return timer_result;
    }
    timer_at = now + ticks;
    timer_arg = arg;
    return NRF_SUCCESS;
}

static uint32_t nrf_gpio_pin_read(unsigned pin) {
    if (radio_running && pin == radio_button && now >= radio_button_at) {
        return 1;
    }
    return pins[pin];
}

static void nrf_gpio_pin_set(unsigned pin) {
    pins[pin] = 1;
}

static void nrf_gpio_pin_clear(unsigned pin) {
    pins[pin] = 0;
}

static void nrf_gpio_cfg_output(unsigned pin) {
}

static void nrf_gpio_cfg(unsigned pin, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e) {
}

static unsigned settings_get_button_press_config(char key) {
    if (buttons_disabled) {
        return SettingsButtonDisable;
    }
    return short_config[key == 'b'];
}

static unsigned settings_get_long_button_press_config(char key) {
    if (long_buttons_disabled) return SettingsButtonDisable;
    return long_config[key == 'b'];
}

static uint32_t settings_get_long_press_threshold(void) {
    return 500;
}

static void run_button_function_by_settings(unsigned function) {
    dispatches++;
    dispatched = function;
}

static void sleep_timer_start(uint32_t delay) {
}

static void sleep_timer_stop(void) {
}

static unsigned nrfx_nfct_init(nrfx_nfct_config_t const *config) {
    assert(!config->rxtx_int_mask && config->cb);
    watches++;
    return NRFX_SUCCESS;
}

static void nrfx_nfct_autocolres_disable(void) {
}

static void nrfx_nfct_enable(void) {
}

static void nrfx_nfct_uninit(void) {
    uninitializations++;
}

static bool usb_cdc_tx_ready(void) {
    return usb_tx_ready;
}

static unsigned tag_emulation_get_slot(void) {
    return 0;
}

static unsigned get_color_by_slot(unsigned slot) {
    assert(slot == 0);
    return 1;
}

static unsigned nrfx_power_usbstatus_get(void) {
    return 1;
}

static unsigned settings_get_animation_config(void) {
    return 0;
}

static bool rgb_marquee_is_enabled(void) {
    return g_usb_led_marquee_enable;
}

static void rgb_marquee_stop(void) {
    led_pwm_running = false;
    led_pwm_stops++;
}

static void rgb_marquee_usb_open_sweep(unsigned color, unsigned dir) {
    led_pwm_running = true;
}

static void rgb_marquee_usb_open_symmetric(unsigned color) {
    led_pwm_running = true;
}

static void rgb_marquee_usb_idle(void) {
    led_pwm_running = true;
}

static void set_slot_light_color(unsigned color) {
}

static void light_up_by_slot(void) {
}

static void nfc_st25tb_radio_stop(void) {
    stops++;
}

static bool nfc_st25tb_radio_poll(bool *present, bool active) {
    radio_polls++;
    polled_active = active;
    *present = polled_present;
    return true;
}

static void st25tb_field_lost(void) {
}

/* BUTTON_CALLBACKS */
/* FIELD_EVENT */
/* WATCH_FIELD */
/* SERVICE */
/* USB_LED_STATUS */

typedef struct {
    uint32_t CYCCNT;
} typeof_dwt;

static typeof_dwt dwt;

static void *tick_dwt(void) {
    dwt.CYCCNT = ++now * 64u;
    return &dwt;
}

#define DWT ((typeof_dwt *)tick_dwt())

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

typedef struct {
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

    uint32_t CONFIG;
    uint32_t ORC;
    uint32_t FREQUENCY;
    uint32_t ENABLE;
    uint32_t EVENTS_STARTED;
    uint32_t EVENTS_END;
    uint32_t SHORTS;
} test_spim_t;

static test_spim_t spim;
static test_spim_t spim0;

static struct {
    uint32_t ENABLE;
} spi;

static struct {
    uint32_t TASKS_CAPTURE[2];
    uint32_t CC[2];
} timer4;

#define NRF_NFCT (&nfct)
#define NRF_SPIM0 (&spim0)
#define NRF_SPIM3 (&spim)
#define NRF_SPI0 (&spi)
#define NRF_TIMER4 (&timer4)
static uint8_t dma_storage[STB_DMA_BANK_COUNT * STB_DMA_BANK_BYTES + 2 * STB_DMA_GUARD_BYTES];
static uint8_t *dma_rx;
static uint32_t stb_bank_start[STB_DMA_BANK_COUNT];
static bool radio_ok;
static bool stb_streaming;
static bool stb_field_known;
static bool stb_absence_pending;
static const uint32_t stb_t0_ticks = STB_RESPONSE_WAIT_US * STB_TIMER_TICKS_PER_US;
static unsigned spi_stops;

static void *rf_hf_sniff_workspace(void) {
    return dma_storage;
}

static void stb_spi_disable(void) {
    spim.ENABLE = 0;
}

static uint32_t rf_timeslot_deadline(void) {
    return 2000000u * 64u;
}

static void delay_us(uint32_t delay) {
    now += delay;
}

static void write_register(unsigned reg, unsigned value) {
}

static bool stb_probe_field(void) {
    return false;
}

static void st25tb_receiver_reset(void) {
}

static void st25tb_receiver_settle(void) {
}

static bool st25tb_receiver_idle(void) {
    return true;
}

static void st25tb_receiver_time(uint32_t tick) {
}

static bool st25tb_receiver_pair(uint8_t a, uint8_t b, uint8_t *request, unsigned *length) {
    *length = 0;
    return false;
}

static unsigned st25tb_receiver_batch(uint8_t *data, unsigned bytes, uint8_t *request, unsigned *length) {
    *length = 0;
    return bytes & ~1u;
}

static unsigned st25tb_reply(uint8_t *request, unsigned length, uint8_t *reply) {
    assert(false);
    return 0;
}

static void stb_spi_stop(void) {
    spi_stops++;
    stb_streaming = false;
}

static void stb_stream_start(void) {
    stb_streaming = true;
    spim.EVENTS_STARTED = 1;
    spim.RXD.AMOUNT = STB_DMA_BANK_BYTES;
}

/* This fixture exercises window/button ownership with busy or waiting input.
 * The complete peripheral and decoder behavior belongs to st25tb_radio.c. */
static unsigned stb_available(unsigned handled) {
    return dma_stalled ? 0 : STB_DMA_BANK_BYTES;
}

static uint32_t __get_PRIMASK(void) {
    return 0;
}

static void __disable_irq(void) {
    assert(false);
}

static void __set_PRIMASK(uint32_t irq) {
    assert(false);
}

static void stb_transmit(uint8_t *reply, unsigned bytes, uint32_t at) {
    assert(false);
}

/* RADIO_WINDOW */

static void test_radio_yield(unsigned pin, bool stalled) {
    spi_stops = 0;
    now = 0;
    radio_ok = true;
    radio_running = true;
    dma_stalled = stalled;
    radio_button = pin;
    /* Also catch presses while waiting for the first completed DMA bank. */
    radio_button_at = stalled ? 500 : 75000;
    nfct.INTENSET = 0x1234;
    stb_window();
    radio_running = false;
    assert(now >= radio_button_at && now < radio_button_at + 100);
    assert(spi_stops && !stb_streaming && radio_ok);
    assert(nfct.TASKS_DISABLE && nfct.INTENSET == 0x1234);
    assert(!spim.ENABLE && !spim.SHORTS && spi.ENABLE == SPI_ENABLE_ENABLE_Enabled);
    assert(pins[HF_ANT_SEL]);
}

static void service_must_wait(void) {
    for (unsigned active = 0; active < 2; active++) {
        field_active = active;
        watching = true;
        probe_due = true;
        unsigned polls_before = radio_polls;
        unsigned watches_before = watches;
        unsigned uninit_before = uninitializations;
        assert(app_buttons_pending());
        assert(nfc_st25tb_service() == (bool)active);
        assert(radio_polls == polls_before && watches == watches_before);
        assert(uninitializations == uninit_before && watching && probe_due);
    }
}

static void test_click(unsigned pin, uint32_t hold_ms) {
    now += 100;
    unsigned before = dispatches;
    bool shortcut = web_link;
    pins[pin] = 1;
    service_must_wait(); /* GPIO asserted before its interrupt can run. */
    button_pin_handler(pin, 0);
    now = timer_at;
    timer_button_event_handle(timer_arg);
    service_must_wait(); /* Press debounced and still held. */
    if (shortcut && disconnect_while_pressed) web_link = false;
    now += hold_ms;
    pins[pin] = 0;
    service_must_wait(); /* Release edge before the GPIOTE interrupt. */
    button_pin_handler(pin, 0);
    now += 49;
    service_must_wait(); /* Do not block the release debounce for two seconds. */
    now = timer_at;
    timer_button_event_handle(timer_arg);
    service_must_wait(); /* Release must reach main-loop dispatch first. */
    button_press_process();
    assert(!app_buttons_pending() && dispatches == before + !shortcut);
    if (shortcut) {
        assert(!web_link && !m_is_btn_web_link_exit);
    } else {
        char button = pin == BUTTON_2 ? 'a' : 'b';
        unsigned expected = hold_ms > 500 ? settings_get_long_button_press_config(button)
                                         : settings_get_button_press_config(button);
        assert(dispatched == expected);
    }
    unsigned polls_before = radio_polls;
    assert(nfc_st25tb_service());
    assert(radio_polls == polls_before + 1 && !probe_due);
}

static void test_field_leds(void) {
    web_link = true;
    blink_usb_led_status();
    assert(!led_pwm_running && !led_pwm_stops);
    web_link = false;
    g_usb_led_marquee_enable = true;
    field_active = false;
    g_is_tag_emulating = false;
    blink_usb_led_status();
    assert(led_pwm_running && !led_pwm_stops);

    nrfx_nfct_evt_t event = {.evt_id = 0};
    field_event(&event);
    assert(g_usb_led_marquee_enable && !field_active && !g_is_tag_emulating);
    event.evt_id = NRFX_NFCT_EVT_FIELD_DETECTED;
    field_event(&event);
    assert(!g_usb_led_marquee_enable && field_active && g_is_tag_emulating);
    /* Field callbacks change ownership; the next main-loop LED pass stops PWM. */
    assert(led_pwm_running);
    blink_usb_led_status();
    assert(!led_pwm_running && led_pwm_stops == 1);

    /* USB resets can re-enable animation between main-loop LED passes. RF
     * ownership must be restored even when service yields without polling. */
    unsigned polls_before = radio_polls;
    for (unsigned button_pending = 0; button_pending < 2; button_pending++) {
        g_usb_led_marquee_enable = true;
        blink_usb_led_status();
        assert(led_pwm_running);
        last_window = now;
        pins[BUTTON_1] = button_pending;
        assert(nfc_st25tb_service());
        assert(!g_usb_led_marquee_enable && radio_polls == polls_before);
        assert(led_pwm_running);
        blink_usb_led_status();
        assert(!led_pwm_running && led_pwm_stops == 2 + button_pending);
        pins[BUTTON_1] = 0;
    }

    g_usb_led_marquee_enable = true;
    blink_usb_led_status();
    assert(led_pwm_running);
    field_active = false;
    probe_due = true;
    now += 10000;
    assert(nfc_st25tb_service());
    assert(!g_usb_led_marquee_enable && field_active && g_is_tag_emulating);
    /* A field found inside a polling window reaches LED cleanup after that window. */
    assert(led_pwm_running);
    blink_usb_led_status();
    assert(!led_pwm_running && led_pwm_stops == 4);

    polled_present = false;
    now += 11;
    assert(!nfc_st25tb_service());
    assert(!g_usb_led_marquee_enable && !field_active && !g_is_tag_emulating);
    polled_present = true;
    last_window = 0;
}

static void test_usb_backpressure(void) {
    usb_tx_ready = false;
    assert(!usb_cdc_tx_ready());
    field_active = true;
    probe_due = true;
    watching = true;
    now = 10000;
    last_window = now - 9;
    unsigned polls_before = radio_polls;
    unsigned uninit_before = uninitializations;
    assert(nfc_st25tb_service());
    assert(radio_polls == polls_before && watching && probe_due);

    /* A queued reply preserves the 10 ms inter-window gap, then RF resumes. */
    now++;
    assert(nfc_st25tb_service());
    assert(radio_polls == polls_before + 1 && polled_active);
    assert(uninitializations == uninit_before + 1 && !watching && !probe_due);
    for (unsigned window = 0; window < 4; window++) {
        now += 10;
        assert(nfc_st25tb_service());
        assert(radio_polls == polls_before + window + 2);
    }

    /* A busy TX queue must also permit the first periodic field probe. */
    field_active = false;
    probe_due = true;
    watching = true;
    now += 10;
    polls_before = radio_polls;
    assert(nfc_st25tb_service());
    assert(radio_polls == polls_before + 1 && !polled_active);
    assert(field_active && !probe_due && !watching);

    field_active = false;
    probe_due = true;
    watching = true;
    polled_present = false;
    now += 10;
    polls_before = radio_polls;
    unsigned stops_before = stops;
    unsigned watches_before = watches;
    assert(!nfc_st25tb_service());
    assert(radio_polls == polls_before + 1 && !polled_active);
    assert(stops == stops_before + 1 && watches == watches_before + 1);
    assert(watching);

    /* Periodic probes can become due inside a 500 ms window. Even without a
     * field, preserve time for multiple USB polls before starting another. */
    for (unsigned window = 0; window < 4; window++) {
        probe_due = true;
        polls_before = radio_polls;
        uninit_before = uninitializations;
        watches_before = watches;
        for (unsigned elapsed_ms = 0; elapsed_ms < 10; elapsed_ms++) {
            now = last_window + elapsed_ms;
            assert(!nfc_st25tb_service());
            assert(radio_polls == polls_before && watching && probe_due);
            assert(uninitializations == uninit_before && watches == watches_before);
        }
        now = last_window + 10;
        assert(!nfc_st25tb_service());
        assert(radio_polls == polls_before + 1 && !polled_active && watching && !probe_due);
        assert(uninitializations == uninit_before + 1 && watches == watches_before + 1);
    }

    /* Physical input still wins over both queued USB work and RF polling. */
    pins[BUTTON_1] = 1;
    service_must_wait();
    pins[BUTTON_1] = 0;
    assert(!app_buttons_pending());
    usb_tx_ready = true;
    polled_present = true;
    last_window = 0;
}

int main(void) {
    test_field_leds();
    test_usb_backpressure();
    test_radio_yield(BUTTON_1, false);
    test_radio_yield(BUTTON_2, false);
    test_radio_yield(BUTTON_1, true);
    test_radio_yield(BUTTON_2, true);
    now = 0;
    dma_stalled = false;
    radio_running = true;
    radio_ok = true;
    radio_button_at = UINT32_MAX;
    stb_window();
    radio_running = false;
    assert(now > 1990000 && radio_ok && !stb_streaming);
    for (unsigned pin = BUTTON_1; pin <= BUTTON_2; pin++) {
        test_click(pin, 75);
        test_click(pin, 750);
    }
    /* The new action is routed on either duration and either physical button. */
    for (unsigned pin = BUTTON_1; pin <= BUTTON_2; pin++) {
        short_config[0] = short_config[1] = 6;
        long_config[0] = long_config[1] = 6;
        test_click(pin, 75);
        test_click(pin, 750);
    }
    short_config[0] = 1; short_config[1] = 2;
    long_config[0] = 3; long_config[1] = 4;
    buttons_disabled = true;
    for (unsigned pin = BUTTON_1; pin <= BUTTON_2; pin++) test_click(pin, 750);
    /* Exit works even when both actions are disabled, and is consumed if an
     * automatic exit happens between the debounced press and release. */
    long_buttons_disabled = true;
    for (unsigned raced = 0; raced < 2; raced++) {
        disconnect_while_pressed = raced;
        for (unsigned pin = BUTTON_1; pin <= BUTTON_2; pin++) {
            web_link = true; test_click(pin, 75);
            web_link = true; test_click(pin, 750);
        }
    }
    buttons_disabled = long_buttons_disabled = false;
    disconnect_while_pressed = false;
    /* A rejected debounce timer must not leave RF permanently suspended. */
    timer_result = 1;
    button_pin_handler(BUTTON_1, 0);
    assert(!app_buttons_pending());
    /* A bounce shorter than the debounce period does not change slot. */
    timer_result = 0;
    pins[BUTTON_1] = 1;
    button_pin_handler(BUTTON_1, 0);
    pins[BUTTON_1] = 0;
    /* Queue exhaustion must not erase the already-pending debounce. */
    timer_result = 1;
    button_pin_handler(BUTTON_1, 0);
    service_must_wait();
    now = timer_at;
    timer_button_event_handle(timer_arg);
    unsigned before = dispatches;
    button_press_process();
    assert(!app_buttons_pending() && dispatches == before);
    /* Disabled actions still allow either debounced release to recover a
     * restricted boot; full startup continues to honor disabled actions. */
    buttons_disabled = true;
    long_buttons_disabled = true;
    for (unsigned restricted = 0; restricted < 2; restricted++) {
        rf_only = restricted;
        for (unsigned pin = BUTTON_1; pin <= BUTTON_2; pin++) {
            m_is_b_btn_press = false;
            m_is_a_btn_press = false;
            m_is_b_btn_release = false;
            m_is_a_btn_release = false;
            unsigned restarts_before = full_restarts;
            pins[pin] = 1;
            button_pin_handler(pin, 0);
            now = timer_at;
            timer_button_event_handle(timer_arg);
            button_press_process();
            assert(full_restarts == restarts_before && dispatches == before);
            pins[pin] = 0;
            button_pin_handler(pin, 0);
            now = timer_at;
            timer_button_event_handle(timer_arg);
            button_press_process();
            assert(full_restarts == restarts_before + restricted && dispatches == before);
        }
    }
    return 0;
}
