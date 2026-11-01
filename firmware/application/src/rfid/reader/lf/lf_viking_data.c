#include "lf_reader_data.h"

#include "bsp_delay.h"
#include "bsp_time.h"
#include "bsp_wdt.h"
#include "lf_125khz_radio.h"
#include "nrfx_saadc.h"
#include "protocols/viking.h"
#include "rf_workspace.h"

#define VIKING_FIELD_SETTLE_MS 75

static uint16_t *samples;
static volatile size_t sample_count;
static volatile bool capturing;

static void viking_saadc_cb(nrf_saadc_value_t *values, size_t count) {
    for (size_t i = 0; capturing && i < count && sample_count < RF_LF_CAPTURE_SAMPLES; i++) {
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

bool viking_read(uint8_t *data, uint32_t timeout_ms) {
    // The comparator loses short transitions at RF/32. Capture the envelope
    // with DMA instead; USB interrupt latency cannot change its sample timing.
    samples = rf_lf_workspace();
    sample_count = 0;
    autotimer *timer = bsp_obtain_timer(0);
    start_lf_125khz_radio();
    uint32_t settle_ms = timeout_ms < VIKING_FIELD_SETTLE_MS ? timeout_ms : VIKING_FIELD_SETTLE_MS;
    bsp_delay_ms(settle_ms);
    capturing = true;
    lf_125khz_radio_saadc_enable(viking_saadc_cb);
    while (sample_count < RF_LF_CAPTURE_SAMPLES && NO_TIMEOUT_1MS(timer, timeout_ms)) {
        bsp_wdt_feed();
    }
    capturing = false;
    lf_125khz_radio_saadc_disable();
    stop_lf_125khz_radio();
    bsp_return_timer(timer);
    return viking_decode_samples(samples, sample_count, data);
}
