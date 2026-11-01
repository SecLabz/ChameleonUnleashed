#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "bsp_time.h"
#include "lf_125khz_radio.h"
#include "lf_reader_data.h"
#include "protocols/viking.h"
#include "rf_workspace.h"

static uint16_t capture[RF_LF_CAPTURE_SAMPLES];
static int16_t incoming[RF_LF_CAPTURE_SAMPLES];
static size_t input_count;
static size_t input_offset;
static autotimer timer;
static lf_adc_callback_t callback;
static unsigned active_resources;

uint16_t *rf_lf_workspace(void) {
    return capture;
}

autotimer *bsp_obtain_timer(uint32_t initial) {
    timer.time = initial;
    active_resources++;
    return &timer;
}

void bsp_return_timer(autotimer *value) {
    assert(value == &timer);
    active_resources--;
}

void bsp_delay_ms(uint32_t milliseconds) {
    timer.time += milliseconds;
}

void bsp_wdt_feed(void) {
    size_t count = input_count - input_offset;
    if (count > 2048) {
        count = 2048;
    }
    if (count) {
        callback(incoming + input_offset, count);
        input_offset += count;
        timer.time += 17;
    } else {
        timer.time++;
    }
}

void start_lf_125khz_radio(void) {
    active_resources++;
}

void stop_lf_125khz_radio(void) {
    active_resources--;
}

void lf_125khz_radio_saadc_enable(lf_adc_callback_t handler) {
    callback = handler;
    active_resources++;
}

void lf_125khz_radio_saadc_disable(void) {
    active_resources--;
}

static void waveform(uint64_t first, uint64_t second, unsigned phase, bool inverted) {
    input_count = RF_LF_CAPTURE_SAMPLES;
    input_offset = 0;
    for (size_t i = 0; i < input_count; i++) {
        size_t position = i + phase;
        uint64_t frame = (position / 2048) % 2 ? second : first;
        unsigned bit = 63 - (position / 32) % 64;
        bool level = ((frame >> bit) & 1u) ^ (position % 32 >= 16) ^ inverted;
        int baseline = 6000 + i * 2000 / input_count;
        incoming[i] = baseline + (level ? 2500 : -2500) + (i * 17) % 41 - 20;
        capture[i] = incoming[i];
    }
}

int main(void) {
    const uint64_t frame = UINT64_C(0xf2000009876543f2);
    const uint8_t expected[] = {0x09, 0x87, 0x65, 0x43};
    uint8_t result[] = {0xaa, 0xaa, 0xaa, 0xaa};
    for (unsigned inverted = 0; inverted < 2; inverted++) {
        for (unsigned phase = 0; phase < 32; phase++) {
            waveform(frame, frame, phase, inverted);
            assert(viking_decode_samples(capture, input_count, result));
            assert(memcmp(result, expected, sizeof(result)) == 0);
        }
    }

    waveform(frame ^ 1u, frame ^ 1u, 0, false);
    assert(!viking_decode_samples(capture, input_count, result));
    assert(memcmp(result, expected, sizeof(result)) == 0);
    waveform(frame, UINT64_C(0xf200000300ff4fe9), 0, false);
    assert(!viking_decode_samples(capture, input_count, result));
    assert(memcmp(result, expected, sizeof(result)) == 0);
    assert(!viking_decode_samples(capture, 2048, result));
    memset(capture, 0, sizeof(capture));
    assert(!viking_decode_samples(capture, input_count, result));
    assert(memcmp(result, expected, sizeof(result)) == 0);

    uint32_t blocks[3];
    uint8_t identity[] = {0x09, 0x87, 0x65, 0x43};
    assert(viking_t55xx_writer(identity, blocks) == 3);
    assert(blocks[0] == 0x00088050 && blocks[1] == 0xf2000009 && blocks[2] == 0x876543f2);
    const nrf_pwm_sequence_t *sequence = viking.modulator(NULL, identity);
    assert(sequence->length == 256);
    for (unsigned bit = 0; bit < 64; bit++) {
        assert(sequence->values.p_wave_form[bit].counter_top == 32);
        assert(sequence->values.p_wave_form[bit].channel_0 == (16 | (((frame >> (63 - bit)) & 1u) << 15)));
    }

    waveform(frame, frame, 13, true);
    incoming[0] = -100;
    incoming[1] = 20000;
    assert(viking_read(result, 500));
    assert(memcmp(result, expected, sizeof(result)) == 0);
    assert(capture[0] == 0 && capture[1] == 16383);
    assert(active_resources == 0);

    input_count = 0;
    input_offset = 0;
    assert(!viking_read(result, 20));
    assert(memcmp(result, expected, sizeof(result)) == 0);
    assert(active_resources == 0 && timer.time <= 22);
    memset(capture, 0x22, sizeof(capture));
    callback(incoming, 2048);
    for (unsigned i = 0; i < RF_LF_CAPTURE_SAMPLES; i++) {
        assert(capture[i] == 0x2222);
    }
}
