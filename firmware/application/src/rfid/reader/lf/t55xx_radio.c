#include <stdbool.h>
#include "bsp_delay.h"
#include "bsp_time.h"
#include "bsp_wdt.h"
#include "hw_connect.h"
#include "lf_125khz_radio.h"
#include "nrf_gpio.h"
#include "protocols/t55xx.h"
#include "t55xx_radio.h"
#include "timeslot.h"
#include "rf_workspace.h"

#define CAPTURE_SAMPLES RF_LF_CAPTURE_SAMPLES
#define CAPTURE_TIMEOUT_MS 130
#define FIELD_SETTLE_MS 75
#define PROGRAM_SETTLE_MS 12
#define START_GAP_US 240
#define WRITE_GAP_US 72
#define ZERO_CYCLES_US 192
#define ONE_CYCLES_US 432

static uint16_t *samples;
static volatile size_t sample_count;
static volatile bool capture_active;

static struct {
    uint8_t block;
    bool write;
    uint32_t word;
} transfer;

static void capture(nrf_saadc_value_t *values, size_t count) {
    for (size_t i = 0; capture_active && i < count && sample_count < CAPTURE_SAMPLES; i++) {
        int value = values[i];
        if (value < 0) {
            value = 0;
        }
        if (value > 16383) {
            value = 16383;
        }
        samples[sample_count++] = (uint16_t)value;
    }
}

static void gap(unsigned microseconds) {
    nrfx_pwm_stop(&m_pwm, true);
    nrf_gpio_cfg_output(LF_ANT_DRIVER);
    nrf_gpio_pin_clear(LF_ANT_DRIVER);
    bsp_delay_us(microseconds);
    start_lf_125khz_radio();
}

static void send_bit(bool bit) {
    bsp_delay_us(bit ? ONE_CYCLES_US : ZERO_CYCLES_US);
    gap(WRITE_GAP_US);
}

static void send_bits(uint32_t value, unsigned bits) {
    for (unsigned i = bits; i > 0; i--) {
        send_bit((value >> (i - 1)) & 1);
    }
}

static void exchange_timeslot(void) {
    gap(START_GAP_US);
    send_bits(T5577_OPCODE_PAGE0, 2);
    send_bit(false);
    if (transfer.write) {
        send_bits(transfer.word, 32);
    }
    send_bits(transfer.block, 3);
    if (!transfer.write) {
        /* Start DMA at the same point after every command. Repeated 32-bit
         * block replies alone cannot identify a word's rotational alignment. */
        sample_count = 0;
        capture_active = true;
        lf_125khz_radio_saadc_enable(capture);
    }
}

static bool read_capture(uint8_t block) {
    transfer.block = block;
    transfer.write = false;
    capture_active = false;
    sample_count = 0;
    start_lf_125khz_radio();
    bsp_delay_ms(FIELD_SETTLE_MS);
    request_timeslot(40000, exchange_timeslot);
    autotimer *timer = bsp_obtain_timer(0);
    while (sample_count < CAPTURE_SAMPLES && NO_TIMEOUT_1MS(timer, CAPTURE_TIMEOUT_MS)) {
        bsp_wdt_feed();
    }
    bsp_return_timer(timer);
    capture_active = false;
    lf_125khz_radio_saadc_disable();
    stop_lf_125khz_radio();
    return sample_count == CAPTURE_SAMPLES;
}

bool t55xx_radio_read(uint8_t block,
    const t55xx_demod_t *demod, uint32_t *word) {
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (read_capture(block) &&
            t55xx_read_samples(samples, sample_count, demod, word)) {
            return true;
        }
    }
    return false;
}

void t55xx_radio_write(uint8_t block, uint32_t word) {
    transfer.block = block;
    transfer.word = word;
    transfer.write = true;
    start_lf_125khz_radio();
    bsp_delay_ms(FIELD_SETTLE_MS);
    request_timeslot(40000, exchange_timeslot);
    bsp_delay_ms(PROGRAM_SETTLE_MS);
    stop_lf_125khz_radio();
}

bool t55xx_radio_detect(t55xx_demod_t *demod) {
    samples = rf_lf_workspace();
    return read_capture(0) &&
        t55xx_detect_samples(samples, sample_count, demod, bsp_wdt_feed);
}
