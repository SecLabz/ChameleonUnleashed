#include "viking.h"

#include <string.h>

#include "t55xx.h"
#include "utils/manchester.h"

#define VIKING_RAW_SIZE 64
#define VIKING_DATA_SIZE 4
#define VIKING_HEADER 0xf20000
#define VIKING_CLOCK_CYCLES 32
#define VIKING_FRAME_SAMPLES (VIKING_RAW_SIZE * VIKING_CLOCK_CYCLES)
#define VIKING_T55XX_BLOCK_COUNT 3

static nrf_pwm_values_wave_form_t m_viking_pwm_seq_vals[VIKING_RAW_SIZE];

static const nrf_pwm_sequence_t m_viking_pwm_seq = {
    .values.p_wave_form = m_viking_pwm_seq_vals,
    .length = NRF_PWM_VALUES_LENGTH(m_viking_pwm_seq_vals),
    .repeats = 0,
    .end_delay = 0,
};

static uint64_t viking_raw_data(uint8_t *uid) {
    uint64_t raw = VIKING_HEADER;
    uint8_t crc = 0x5A;
    for (unsigned i = 0; i < VIKING_DATA_SIZE; i++) {
        raw = (raw << 8) | uid[i];
        crc ^= uid[i];
    }
    return (raw << 8) | crc;
}

static bool viking_decode_frame(uint64_t frame, uint8_t *result) {
    if ((frame >> (VIKING_RAW_SIZE - 24)) != VIKING_HEADER) {
        frame = ~frame;
    }
    if ((frame >> (VIKING_RAW_SIZE - 24)) != VIKING_HEADER) {
        return false;
    }

    uint8_t crc = 0x5A;
    uint8_t data[VIKING_DATA_SIZE];
    for (unsigned i = 0; i < VIKING_DATA_SIZE; i++) {
        uint8_t byte = (frame >> ((i + 1) * 8)) & 0xff;
        crc ^= byte;
        data[VIKING_DATA_SIZE - i - 1] = byte;
    }
    if (crc != (frame & 0xff)) {
        return false;
    }
    memcpy(result, data, sizeof(data));
    return true;
}

bool viking_decode_samples(const uint16_t *samples, size_t count, uint8_t *data) {
    if (!samples || count < 2 * VIKING_FRAME_SAMPLES) {
        return false;
    }
    for (unsigned phase = 0; phase < VIKING_CLOCK_CYCLES; phase += VIKING_CLOCK_CYCLES / 8) {
        uint64_t frame = 0;
        unsigned valid_bits = 0;
        uint8_t previous[VIKING_DATA_SIZE];
        size_t previous_start = 0;
        bool have_previous = false;
        for (size_t start = phase; start + VIKING_CLOCK_CYCLES <= count; start += VIKING_CLOCK_CYCLES) {
            bool bit;
            if (!manchester_sample_bit(samples + start, VIKING_CLOCK_CYCLES, &bit)) {
                valid_bits = 0;
                have_previous = false;
                continue;
            }
            frame = (frame << 1) | bit;
            if (valid_bits < VIKING_RAW_SIZE) {
                valid_bits++;
            }
            uint8_t current[VIKING_DATA_SIZE];
            if (valid_bits < VIKING_RAW_SIZE || !viking_decode_frame(frame, current)) {
                continue;
            }
            // Confirm the broadcast in the next complete frame. A weak XOR
            // checksum alone must not turn one damaged capture into an ID.
            if (have_previous && start == previous_start + VIKING_FRAME_SAMPLES &&
                memcmp(previous, current, sizeof(current)) == 0) {
                memcpy(data, current, sizeof(current));
                return true;
            }
            memcpy(previous, current, sizeof(previous));
            previous_start = start;
            have_previous = true;
        }
    }
    return false;
}

static const nrf_pwm_sequence_t *viking_modulator(void *codec, uint8_t *buf) {
    uint64_t raw = viking_raw_data(buf);
    for (unsigned i = 0; i < VIKING_RAW_SIZE; i++) {
        uint16_t polarity = ((raw >> (VIKING_RAW_SIZE - i - 1)) & 1u) << 15;
        m_viking_pwm_seq_vals[i].channel_0 = polarity | 16;
        m_viking_pwm_seq_vals[i].counter_top = VIKING_CLOCK_CYCLES;
    }
    return &m_viking_pwm_seq;
}

const protocol viking = {
    .tag_type = TAG_TYPE_VIKING,
    .data_size = VIKING_DATA_SIZE,
    .modulator = (modulator)viking_modulator,
};

uint8_t viking_t55xx_writer(uint8_t *uid, uint32_t *blks) {
    uint64_t raw = viking_raw_data(uid);
    blks[0] = T5577_VIKING_CONFIG;
    blks[1] = raw >> 32;
    blks[2] = raw & 0xffffffff;
    return VIKING_T55XX_BLOCK_COUNT;
}
