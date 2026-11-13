#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "hex_utils.h"
#include "mf1_crapto1.h"
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

#define AUTH_FIRST 0
typedef struct { uint8_t key[6]; } mf1_key_t;
typedef struct {
    uint8_t block, key_type, keys_len;
    mf1_key_t *keys;
    const uint8_t *expected_uid;
    uint8_t uid_length;
} mf1_toolbox_check_keys_on_block_in_t;
typedef struct { uint8_t found; mf1_key_t key; } mf1_toolbox_check_keys_on_block_out_t;
typedef struct { uint8_t uid[10], uid_len; } picc_14a_tag_t;
static picc_14a_tag_t tag = {.uid = {1, 2, 3, 4, 5, 6, 7}, .uid_len = 7};
static picc_14a_tag_t *p_tag_info = &tag;
static unsigned calls, selections, lose_on;
static unsigned attempted[8];
static unsigned good;
static bool slow;
static uint16_t pcd_14a_reader_timeout_get(void) { return 25; }
static uint32_t get_u32_tag_uid(const picc_14a_tag_t *tag) { return 0x01020304; }
static void mf1_toolbox_report_healthy(void) {}
static uint8_t pcd_14a_reader_fast_select(picc_14a_tag_t *tag) {
    return ++selections == lose_on ? STATUS_HF_TAG_NO : STATUS_HF_TAG_OK;
}
static uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) { return pcd_14a_reader_fast_select(tag); }
static uint8_t authex(struct Crypto1State *cipher, uint32_t uid, uint8_t block,
    uint8_t type, uint64_t key, uint8_t mode, uint32_t *nonce, bool *retry_silence) {
    assert(uid == 0x01020304 && block == 4 && type == 0x60 && key <= 3 && calls < 8);
    attempted[calls++] = (unsigned)key + (retry_silence ? 10u : 0u);
    if (key == good && (!slow || !retry_silence)) return STATUS_HF_TAG_OK;
    // Key 2 receives a definitive negative reply, so it needs no slow retry.
    if (retry_silence) *retry_silence = key != 2;
    return STATUS_MF_ERR_AUTH;
}
/* DICTIONARY FUNCTION */

int main(void) {
    mf1_key_t keys[] = {{{0,0,0,0,0,1}}, {{0,0,0,0,0,2}}, {{0,0,0,0,0,3}}};
    mf1_toolbox_check_keys_on_block_in_t in = {.block = 4, .key_type = 0x60, .keys_len = 3, .keys = keys};
    mf1_toolbox_check_keys_on_block_out_t out;
    good = 3;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_OK);
    assert(out.found == 1 && out.key.key[5] == 3 && calls == 3);
    assert(!memcmp(attempted, (unsigned[]){11,12,13}, 3 * sizeof(unsigned)));

    slow = true; calls = selections = 0;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_OK);
    assert(out.found == 1 && out.key.key[5] == 3 && calls == 5);
    assert(!memcmp(attempted, (unsigned[]){11,12,13,1,3}, 5 * sizeof(unsigned)));

    calls = selections = 0; lose_on = 4;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_NO);
    assert(!out.found && calls == 3);

    calls = selections = lose_on = 0; good = 1; in.keys_len = 1;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_OK);
    assert(out.found == 1 && calls == 1 && attempted[0] == 1);

    uint8_t expected_uid[] = {1, 2, 3, 4, 5, 6, 7};
    in.expected_uid = expected_uid;
    in.uid_length = sizeof(expected_uid);
    calls = selections = 0;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_OK);
    assert(out.found && calls == 1 && selections == 1);

    calls = selections = 0;
    expected_uid[0] ^= 1;
    assert(mf1_toolbox_check_keys_on_block(&in, &out) == STATUS_HF_TAG_NO);
    assert(!out.found && calls == 0 && selections == 1);
}
