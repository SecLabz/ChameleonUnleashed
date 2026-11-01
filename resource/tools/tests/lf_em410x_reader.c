#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_time.h"
#include "circular_buffer.h"
#include "lf_reader_data.h"
#include "protocols/em410x.h"

static autotimer timer;
static uint16_t intervals[1024];
static size_t interval_count;
static size_t next_interval;
static unsigned elapsed_cycles;
static unsigned active_resources;

bool cb_init(circular_buffer *buffer, size_t capacity, size_t size) {
    active_resources++;
    return true;
}

void cb_free(circular_buffer *buffer) {
    active_resources--;
}

bool cb_push_back(circular_buffer *buffer, const void *value) {
    abort();
}

bool cb_pop_front(circular_buffer *buffer, void *value) {
    if (next_interval == interval_count) {
        timer.time++;
        return false;
    }
    uint16_t interval = intervals[next_interval++];
    *(uint16_t *)value = interval;
    elapsed_cycles += interval;
    timer.time = elapsed_cycles / 125;
    return true;
}

autotimer *bsp_obtain_timer(uint32_t initial) {
    timer.time = initial;
    return &timer;
}

void bsp_return_timer(autotimer *value) {
    assert(value == &timer);
}

void register_rio_callback(RIO_CALLBACK_S callback) {}

void unregister_rio_callback(void) {}

uint32_t get_lf_counter_value(void) {
    return 0;
}

void clear_lf_counter_value(void) {}

void lf_125khz_radio_gpiote_enable(void) {
    active_resources++;
}

void lf_125khz_radio_gpiote_disable(void) {
    active_resources--;
}

void start_lf_125khz_radio(void) {
    active_resources++;
}

void stop_lf_125khz_radio(void) {
    active_resources--;
}

static void waveform(uint32_t *blocks, unsigned count, unsigned clock) {
    interval_count = 0;
    next_interval = 0;
    elapsed_cycles = 0;
    unsigned since_rising = 0;
    bool previous = false;
    for (unsigned repeat = 0; repeat < 8; repeat++) {
        for (unsigned block = 1; block < count; block++) {
            for (unsigned bit = 32; bit > 0; bit--) {
                bool data = (blocks[block] >> (bit - 1)) & 1;
                for (unsigned half = 0; half < 2; half++) {
                    bool level = data != (half != 0);
                    if (level && !previous && since_rising != 0) {
                        assert(interval_count < sizeof(intervals) / sizeof(intervals[0]));
                        intervals[interval_count++] = since_rising;
                        since_rising = 0;
                    }
                    previous = level;
                    since_rising += clock / 2;
                }
            }
        }
    }
}

int main(void) {
    uint8_t identity[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    uint32_t blocks[5];
    uint8_t result[15];
    em410x_electra_t55xx_writer(identity, blocks);
    waveform(blocks, 5, 64);
    assert(em410x_read(result, 500));
    assert(result[0] == 0 && result[1] == 104);
    assert(memcmp(result + 2, identity, sizeof(identity)) == 0);
    assert(active_resources == 0);

    em410x_t55xx_writer(identity, blocks);
    uint8_t expected_type = 101;
    for (unsigned clock = 16; clock <= 64; clock *= 2) {
        waveform(blocks, 3, clock);
        assert(em410x_read(result, 500));
        assert(result[1] == expected_type);
        assert(memcmp(result + 2, identity, 5) == 0);
        assert(active_resources == 0);
        expected_type++;
    }
    interval_count = 0;
    next_interval = 0;
    elapsed_cycles = 0;
    assert(!em410x_read(result, 20));
    assert(active_resources == 0);
    return 0;
}
