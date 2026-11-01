#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "hex_utils.h"
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

#define PACKED __attribute__((packed))
#define PCD_TRANSCEIVE 0
#define U8ARR_BIT_LEN(a) (sizeof(a) * 8)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_PROCESS() 0
#define APP_TIMER_TICKS(ms) (ms)
#define RGB_LIST_NUM 8
#define RGB_GREEN 1

typedef struct { uint8_t uid_len, uid[10], sak, atqa[2]; } picc_14a_tag_t;
typedef enum { DARKSIDE_OK, DARKSIDE_CANT_FIX_NT, DARKSIDE_LUCKY_AUTH_OK,
    DARKSIDE_NO_NAK_SENT, DARKSIDE_TAG_CHANGED, DARKSIDE_IN_PROGRESS, DARKSIDE_EXHAUSTED } mf1_darkside_status_t;
typedef struct { uint8_t uid[4], nt[4], par_list[8], ks_list[8], nr[4], ar[4]; } PACKED DarksideCore_t;

static picc_14a_tag_t selected = {.uid_len = 7, .uid = {9, 8, 7, 1, 2, 3, 4}, .sak = 8};
static picc_14a_tag_t tag, *p_tag_info = &tag;
static uint32_t ticks, led_pins[8];
static unsigned exchanges, led_runs;
static uint8_t current_delay, led_mask;
static bool fail_fast_select, wrong_nonce, no_nack;

uint32_t app_timer_cnt_get(void) { return ticks; }
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) { return now - before; }
void bsp_wdt_feed(void) {}
void bsp_delay_ms(uint32_t delay) { ticks += delay; current_delay = delay; }
void pcd_14a_reader_antenna_off(void) {}
void pcd_14a_reader_antenna_on(void) {}
void reset_radio_field_with_delay(void) { bsp_delay_ms(100); }
void crc_14a_append(uint8_t *bytes, uint8_t length) { (void)bytes; (void)length; }
void nfca_session_close(void) {}
void rgb_marquee_stop(void) { led_runs++; }
void set_slot_light_color(unsigned color) { assert(color == RGB_GREEN); }
uint32_t *hw_get_led_array(void) { return led_pins; }
void nrf_gpio_pin_clear(uint32_t pin) { assert(pin < 8); led_mask &= ~(1u << pin); }
void nrf_gpio_pin_set(uint32_t pin) { assert(pin < 8); led_mask |= 1u << pin; }
uint32_t get_u32_tag_uid(picc_14a_tag_t *card) { return bytes_to_num(card->uid + card->uid_len - 4, 4); }
void get_4byte_tag_uid(picc_14a_tag_t *card, uint8_t *uid) { memcpy(uid, card->uid + card->uid_len - 4, 4); }
uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *card) { *card = selected; return STATUS_HF_TAG_OK; }
uint8_t pcd_14a_reader_fast_select(picc_14a_tag_t *card) {
    (void)card;
    return fail_fast_select && current_delay < 100 ? STATUS_HF_TAG_NO : STATUS_HF_TAG_OK;
}
uint8_t pcd_14a_reader_bytes_transfer(int command, uint8_t *tx, unsigned size,
    uint8_t *reply, uint16_t *bits, unsigned capacity) {
    (void)command; (void)tx; (void)size; (void)capacity;
    num_to_bytes(wrong_nonce ? 0x99887766 : 0x7c39d2e6, 4, reply);
    *bits = 32;
    return STATUS_HF_TAG_OK;
}
void pcd_14a_reader_bits_transfer(uint8_t *tx, unsigned size, uint8_t *parity,
    uint8_t *reply, uint8_t *reply_parity, uint16_t *bits, unsigned capacity) {
    (void)size; (void)reply_parity; (void)capacity;
    exchanges++;
    uint8_t packed = 0;
    for (unsigned i = 0; i < 8; i++) packed = packed * 2 + parity[i];
    *bits = !no_nack && packed == 0x17 ? 4 : 0;
    reply[0] = (tx[3] >> 5) ^ 5;
}

/* DARKSIDE FUNCTIONS */

static uint16_t step(uint8_t action, const uint8_t *uid, uint8_t *output, uint16_t *length) {
    uint8_t request[] = {0x60, 0, action, 7, 0, 0, 0, 0, 0, 0, 0};
    memcpy(request + 4, uid, 7);
    unsigned before = exchanges;
    uint16_t status = mf1_darkside_step(sizeof(request), request, output, length);
    assert(exchanges - before <= 8);
    return status;
}

int main(void) {
    const uint8_t uid[] = {9, 8, 7, 1, 2, 3, 4};
    uint8_t output[33];
    uint16_t length;
    assert(step(1, uid, output, &length) == STATUS_PAR_ERR);
    uint8_t other[] = {8, 8, 7, 1, 2, 3, 4};
    assert(step(0, other, output, &length) == STATUS_HF_TAG_OK && output[0] == DARKSIDE_TAG_CHANGED);

    fail_fast_select = true;
    assert(step(0, uid, output, &length) == STATUS_HF_TAG_OK && output[0] == DARKSIDE_IN_PROGRESS);
    for (unsigned i = 0; i < 100 && output[0] == DARKSIDE_IN_PROGRESS; i++)
        assert(step(1, uid, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 33 && output[0] == DARKSIDE_OK && current_delay == 100);
    fail_fast_select = false;
    assert(step(2, uid, output, &length) == STATUS_HF_TAG_OK);
    for (unsigned i = 0; i < 100 && output[0] == DARKSIDE_IN_PROGRESS; i++)
        assert(step(1, uid, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 33 && output[0] == DARKSIDE_OK && bytes_to_num(output + 1, 4) == 0x01020304);

    assert(step(2, uid, output, &length) == STATUS_HF_TAG_OK);
    selected.uid[0] ^= 1;
    assert(step(1, uid, output, &length) == STATUS_HF_TAG_OK && output[0] == DARKSIDE_TAG_CHANGED);
    selected.uid[0] ^= 1;
    assert(step(1, uid, output, &length) == STATUS_PAR_ERR);
    assert(step(0, uid, output, &length) == STATUS_HF_TAG_OK);
    ticks += 10000;
    assert(step(1, uid, output, &length) == STATUS_PAR_ERR);
    assert(step(0, uid, output, &length) == STATUS_HF_TAG_OK);
    bounded_darkside.cycles = 1200;
    assert(step(1, uid, output, &length) == STATUS_HF_TAG_OK && output[0] == DARKSIDE_EXHAUSTED);

    DarksideCore_t capture;
    mf1_darkside_status_t result;
    assert(darkside_recover_key(0, 0x60, 1, 30, &capture, &result) == STATUS_HF_TAG_OK && result == DARKSIDE_OK);
    uint32_t nonce = legacy_darkside.nt;
    mf1_darkside_reset();
    assert(legacy_darkside.nt == nonce);
    assert(darkside_recover_key(0, 0x60, 0, 30, &capture, &result) == STATUS_HF_TAG_OK && result == DARKSIDE_OK);
    assert(led_runs == 2 && capture.nr[3] == 1);
    return 0;
}
