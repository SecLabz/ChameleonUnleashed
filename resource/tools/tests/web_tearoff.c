#include "web_tearoff.h"
#include "hf_reader.h"
#include "hf_radio.h"
#include "app_status.h"
#include "data_cmd.h"
#include "app_timer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t ticks;
static unsigned commands, polls;
static unsigned ble_polls, lease_polls;
static bool cancel_from_ble;
static uint16_t command_status, result_size = 64;
static const uint8_t start[] = {0xd0, 2, 0x33, 1, 2, 3, 4, 5, 5, 0x78, 0x56, 0x34, 0x12};
static const uint8_t expected[] = {
    0xa1, 0xd0, 2, 0x33, 1, 2, 3, 4, 5, 5, 0x78, 0x56, 0x34, 0x12,
    0, 22, 0x40, 0x77, 0x1b, 0,
};

uint32_t app_timer_cnt_get(void) { return ticks; }
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) { return now - before; }
void usb_poll(void) { polls++; }
void data_frame_process_ble_control(void) {
    ble_polls++;
    if (cancel_from_ble) web_tearoff_cancel();
}
void web_session_poll(void) { lease_polls++; }
hf_tearoff_timing_t hf_radio_tearoff_timing_result(void) {
    return (hf_tearoff_timing_t){.writes = 9};
}

uint16_t hf_reader_command(uint16_t command, uint16_t length, const uint8_t *data,
        uint8_t *output, size_t capacity, uint16_t *size) {
    commands++;
    assert(command == DATA_CMD_ST25TB_TEAROFF && length == sizeof(expected));
    assert(memcmp(data, expected, sizeof(expected)) == 0);
    assert(capacity >= 64 && web_tearoff_active() && web_tearoff_running());
    uint8_t state[20];
    web_tearoff_progress(1, 0x12345678, 321);
    web_tearoff_progress(4, 0x12345678, 999); /* Initial reports retain the last delay. */
    ticks += APP_TIMER_TICKS(60);
    web_tearoff_service();
    assert(ble_polls == polls && lease_polls == polls);
    assert(web_tearoff_cancelled() == cancel_from_ble);
    assert(web_tearoff_state(state) == 20);
    assert(state[0] == 2 && state[3] == 1 && state[12] == 9);
    assert(state[16] == 0x41 && state[17] == 1);
    memset(output, 0, 64);
    output[1] = output[2] = 1;
    memcpy(output + 4, start + 9, 4);
    output[56] = 0x11; output[57] = 0x22; output[58] = 0x33; output[59] = 0x44;
    *size = result_size;
    return command_status;
}

int main(void) {
    uint8_t state[20], invalid[sizeof(start)];
    assert(!web_tearoff_start(NULL, sizeof(start)));
    assert(!web_tearoff_start(start, sizeof(start) - 1));
    for (unsigned offset = 0; offset < sizeof(start); offset++) {
        if (offset > 2 && offset != 8) continue;
        memcpy(invalid, start, sizeof(start));
        invalid[offset] ^= 1;
        if (offset == 8) invalid[offset] = 4;
        assert(!web_tearoff_start(invalid, sizeof(invalid)));
    }
    memcpy(invalid, start, sizeof(start));
    memset(invalid + 9, 0, 4);
    assert(!web_tearoff_start(invalid, sizeof(invalid)));
    memset(invalid + 9, 255, 4);
    assert(!web_tearoff_start(invalid, sizeof(invalid)));
    assert(commands == 0 && !web_tearoff_active());

    assert(web_tearoff_start(start, sizeof(start)));
    assert(web_tearoff_active() && !web_tearoff_running());
    assert(!web_tearoff_start(start, sizeof(start)));
    web_tearoff_poll();
    assert(commands == 1 && polls == 1 && !web_tearoff_active());
    assert(web_tearoff_state(state) == 20);
    const uint8_t completed[] = {
        3, 0, 5, 3, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12,
        0x11, 0x22, 0x33, 0x44, 0x41, 1, 0, 0,
    };
    assert(memcmp(state, completed, sizeof(completed)) == 0);

    assert(web_tearoff_start(start, sizeof(start)));
    web_tearoff_cancel();
    assert(web_tearoff_cancelled());
    web_tearoff_poll();
    assert(commands == 1 && !web_tearoff_active());
    web_tearoff_state(state);
    assert(state[0] == 3 && state[1] == 1 && state[3] == 0);

    assert(web_tearoff_start(start, sizeof(start)) && !web_tearoff_cancelled());
    command_status = STATUS_HF_ERR_STAT;
    web_tearoff_poll();
    web_tearoff_state(state);
    assert(commands == 2 && state[1] == 5);
    assert(web_tearoff_start(start, sizeof(start)));
    command_status = STATUS_HF_TAG_OK;
    result_size = 63;
    web_tearoff_poll();
    web_tearoff_state(state);
    assert(commands == 3 && state[1] == 5);
    assert(web_tearoff_start(start, sizeof(start)));
    cancel_from_ble = true;
    web_tearoff_poll();
    assert(ble_polls == 4 && web_tearoff_cancelled());
    puts("ST25TB HTTP request bytes, progress, result, cancellation and invalid input passed");
}
