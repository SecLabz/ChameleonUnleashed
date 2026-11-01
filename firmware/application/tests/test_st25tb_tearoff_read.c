#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/rfid/reader/hf/st25tb_reader.c"

static unsigned exchanges;
static bool wrong_uid;
static uint8_t observed_uid[8] = {0xd0, 2, 0x33, 1, 2, 3, 4, 5};

uint16_t hf_radio_exchange(const uint8_t *frame, size_t length,
        size_t expected, uint8_t *reply, size_t capacity, size_t *received) {
    assert(capacity >= expected);
    memset(reply, 0, expected);
    if (frame[0] == 6) {
        assert(length == 2 && expected == 3);
        reply[0] = 0x22;
    } else if (frame[0] == 0x0e) {
        assert(length == 2 && frame[1] == 0x22 && expected == 3);
        reply[0] = 0x22;
    } else if (frame[0] == 0x0b) {
        assert(length == 1 && expected == 10);
        for (unsigned i = 0; i < 8; i++) reply[i] = observed_uid[7 - i];
        if (wrong_uid) reply[0] ^= 1;
    } else if (frame[0] == 8) {
        assert(length == 2 && frame[1] == 5 && expected == 6);
        reply[0] = 0xfe;
        reply[1] = reply[2] = reply[3] = 0xff;
    } else {
        assert(false);
    }
    exchanges++;
    *received = expected;
    return STATUS_HF_TAG_OK;
}

bool hf_radio_tearoff_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES], uint32_t delay_us,
        uint32_t *cycles) { (void)frame; (void)delay_us; (void)cycles; return false; }

bool hf_radio_send(const uint8_t *data, size_t length) {
    (void)data; (void)length; assert(false); return false;
}

bool hf_radio_st25tb_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES]) {
    (void)frame; assert(false); return false;
}

void hf_radio_depth(bool deeper) { (void)deeper; }

bool hf_radio_expired(void) { return false; }

void bsp_wdt_feed(void) {}

int main(void) {
    uint8_t uid[8] = {0xd0, 2, 0x33, 1, 2, 3, 4, 5};
    uint8_t value[4], stage;
    assert(st25tb_tearoff_read(uid, true, 5, value, &stage) == STATUS_HF_TAG_OK);
    assert(exchanges == 4 && stage == 4 && value[0] == 0xfe);
    exchanges = 0;
    assert(st25tb_tearoff_read(uid, false, 5, value, &stage) == STATUS_HF_TAG_OK);
    assert(exchanges == 3 && stage == 4);
    exchanges = 0;
    wrong_uid = true;
    assert(st25tb_tearoff_read(uid, true, 5, value, &stage) == STATUS_HF_TAG_NO);
    assert(exchanges == 3 && stage == 3);
    wrong_uid = false;
    uid[2] = observed_uid[2] = 0x1b;
    exchanges = 0;
    assert(st25tb_tearoff_read(uid, true, 5, value, &stage) == STATUS_HF_TAG_OK);
    assert(exchanges == 4 && stage == 4 && value[0] == 0xfe);
    uid[7] ^= 1;
    exchanges = 0;
    assert(st25tb_tearoff_read(uid, true, 5, value, &stage) == STATUS_HF_TAG_NO);
    assert(exchanges == 3 && stage == 3);
    puts("ST25TB tearoff read sequence passed");
}
