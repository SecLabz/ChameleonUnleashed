#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "hex_utils.h"
#include "mf1_crapto1.h"
#include "parity.h"
#include "mf1_capture_stats.h"
#include "nrf.h"
test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

#define PICC_AUTHENT1A 0x60
#define PICC_AUTHENT1B 0x61
#define NRF_LOG_INFO(...) ((void)0)
/* CAPTURE DECLARATIONS */

typedef struct { uint8_t uid[10], uid_len; } picc_14a_tag_t;
static picc_14a_tag_t tag = {{0x12, 0x34, 0x56, 0x78}, 4};
static picc_14a_tag_t *p_tag_info = &tag;
static int auth_calls, send_calls, selects, nested_selects, scans, halts, cleanups, delays;
static int auth_failure, auth_failure_call, target_bits = 32, target_status, selection_status;
static void nfca_session_close(void) {}

static void reset(void) {
    auth_calls = send_calls = selects = nested_selects = scans = halts = cleanups = delays = 0;
    auth_failure = auth_failure_call = target_status = selection_status = 0;
    target_bits = 32;
    mf1_capture_stats_start(false);
}
static uint32_t get_u32_tag_uid(picc_14a_tag_t *info) { return bytes_to_num(info->uid, 4); }
static uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *info) {
    scans++;
    return selection_status;
}
static uint8_t pcd_14a_reader_fast_select(picc_14a_tag_t *info) {
    selects++;
    return selection_status;
}
static uint8_t pcd_14a_reader_reselect_after_nested(picc_14a_tag_t *info) {
    nested_selects++;
    return selection_status;
}
static void pcd_14a_reader_halt_tag(void) { halts++; }
static void mf1_toolbox_report_healthy(void) {}
static void bsp_delay_us(uint32_t delay) { assert(delay == 400); delays++; }
void crypto1_deinit(struct Crypto1State *cipher) { cleanups++; }
static uint8_t authex(struct Crypto1State *cipher, uint32_t uid, uint8_t block,
    uint8_t type, uint64_t key, uint8_t nested, uint32_t *nonce, bool *retry_silence) {
    assert(uid == 0x12345678 && block == 0 && type == 0x60 && key == 0xffffffffffff);
    auth_calls++;
    if (auth_failure && (!auth_failure_call || auth_calls == auth_failure_call)) return auth_failure;
    *nonce = nested ? 0xaabbccdd : 0x11223344;
    return STATUS_HF_TAG_OK;
}
static uint8_t send_cmd(struct Crypto1State *cipher, uint8_t encrypted,
    uint8_t type, uint8_t block, uint8_t *status, uint8_t *answer,
    uint8_t *parity, uint16_t capacity) {
    assert(encrypted == AUTH_NESTED && type == 0x61 && block == 4 && capacity == 32);
    send_calls++;
    memcpy(answer, (uint8_t[]){1, 2, 3, send_calls}, 4);
    memcpy(parity, (uint8_t[]){0, 1, 0, 1}, 4);
    *status = target_status;
    return target_bits;
}

typedef struct { uint16_t status, length; uint8_t bytes[232]; } data_frame_tx_t;
static data_frame_tx_t frame;
static data_frame_tx_t *data_frame_make(uint16_t command, uint16_t status,
    uint16_t length, uint8_t *bytes) {
    frame.status = status;
    frame.length = length;
    assert(length <= sizeof(frame.bytes));
    if (length) memcpy(frame.bytes, bytes, length);
    return &frame;
}
/* CAPTURE FUNCTIONS */

static uint8_t *request(uint8_t chained, uint8_t count) {
    static uint8_t bytes[23] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 4, 0, 0, 1};
    bytes[10] = chained;
    bytes[11] = count;
    return bytes;
}

static void checked_capture(void) {
    reset();
    uint8_t data[17];
    memcpy(data, request(0, 2), 12);
    data[12] = 4;
    memcpy(data + 13, tag.uid, 4);
    data_frame_tx_t *reply = cmd_processor_mf1_collect_observations(2023, 0, sizeof(data), data);
    assert(reply->status == STATUS_HF_TAG_OK && reply->length == 24 && scans == 1 && auth_calls == 2);
    assert(nested_selects == 1 && selects == 0);

    reset();
    data[13] ^= 1;
    reply = cmd_processor_mf1_collect_observations(2023, 0, sizeof(data), data);
    assert(reply->status == STATUS_HF_TAG_NO && reply->length == 0 && scans == 1 && auth_calls == 0);
}
static void normalized_capture(void) {
    reset();
    uint8_t *payload = request(0, 2);
    data_frame_tx_t *reply = cmd_processor_mf1_collect_observations(2023, 0, 12, payload);
    assert(reply->status == 0 && reply->length == 24);
    assert(memcmp(reply->bytes, (uint8_t[]){0x11,0x22,0x33,0x44,1,2,3,1,0,1,0,1,
        0x11,0x22,0x33,0x44,1,2,3,2,0,1,0,1}, 24) == 0);
    assert(scans == 1 && selects == 1 && cleanups == 2 && auth_calls == 2);

    reset();
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(1, 1));
    assert(reply->status == 0 && reply->length == 12);
    assert(memcmp(reply->bytes, (uint8_t[]){0xaa,0xbb,0xcc,0xdd}, 4) == 0);
    assert(auth_calls == 2 && cleanups == 1);

    reset();
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 16));
    assert(reply->status == 0 && reply->length == 192 && send_calls == 16);
}
static void profiled_capture(void) {
    reset();
    mf1_capture_stats_start(true);
    DWT->CYCCNT = 0xfffffff0;
    uint32_t started = mf1_capture_clock();
    DWT->CYCCNT = 0x10;
    assert(mf1_capture_elapsed(started) == 32);
    data_frame_tx_t *reply = cmd_processor_mf1_collect_observations(2023, 0, 13, request(0, 2));
    assert(reply->status == 0 && reply->length == 64);
    assert(bytes_to_num(reply->bytes, 4) == 1);
    assert(bytes_to_num(reply->bytes + 36, 4) == 2);
    assert(memcmp(reply->bytes + 40, (uint8_t[]){0x11,0x22,0x33,0x44,1,2,3,1,0,1,0,1}, 12) == 0);
    reset(); mf1_capture_stats_start(true);
    auth_failure = STATUS_MF_ERR_AUTH; auth_failure_call = 2;
    reply = cmd_processor_mf1_collect_observations(2023, 0, 13, request(0, 2));
    assert(reply->status == STATUS_MF_ERR_AUTH && reply->length == 40);
    assert(bytes_to_num(reply->bytes + 36, 4) == 1);

}
static void rejected_requests(void) {
    reset();
    for (uint16_t length = 0; length <= 20; length++) {
        if (length == 12 || length == 13) continue;
        assert(cmd_processor_mf1_collect_observations(2023, 0, length, request(0, 1))->status == STATUS_PAR_ERR);
    }
    assert(cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 0))->status == STATUS_PAR_ERR);
    assert(cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 17))->status == STATUS_PAR_ERR);
    assert(cmd_processor_mf1_collect_observations(2023, 0, 12, request(2, 1))->status == STATUS_PAR_ERR);
    uint8_t *payload = request(0, 1);
    payload[0] = 0;
    assert(cmd_processor_mf1_collect_observations(2023, 0, 12, payload)->status == STATUS_PAR_ERR);
    payload[0] = 0x60;
    assert(scans == 0 && auth_calls == 0);

    reset(); auth_failure = STATUS_MF_ERR_AUTH;
    data_frame_tx_t *reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 2));
    assert(reply->status == STATUS_MF_ERR_AUTH && reply->length == 0);
    assert(auth_calls == 1 && send_calls == 0 && cleanups == 1);
    reset(); auth_failure = STATUS_MF_ERR_AUTH; auth_failure_call = 2;
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 2));
    assert(reply->status == STATUS_MF_ERR_AUTH && reply->length == 0);
    assert(send_calls == 1 && cleanups == 2);
    reset(); auth_failure = STATUS_MF_ERR_AUTH; auth_failure_call = 2;
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(1, 1));
    assert(reply->status == STATUS_MF_ERR_AUTH && reply->length == 0);
    assert(auth_calls == 2 && send_calls == 0 && cleanups == 1);
    reset(); target_bits = 16;
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 2));
    assert(reply->status == STATUS_HF_ERR_STAT && reply->length == 0 && cleanups == 1);
    reset(); target_status = STATUS_HF_TAG_NO;
    reply = cmd_processor_mf1_collect_observations(2023, 0, 12, request(0, 2));
    assert(reply->status == STATUS_HF_TAG_NO && reply->length == 0);
}
static void legacy_formats(void) {
    reset();
    mf1_nested_core_t nested;
    assert(nested_recover_core(&nested, 0xffffffffffff, 0, 0x60, 4, 0x61) == 0);
    assert(memcmp(nested.nt1, (uint8_t[]){0x11,0x22,0x33,0x44}, 4) == 0);
    assert(memcmp(nested.nt2, (uint8_t[]){1,2,3,1}, 4) == 0);
    assert(nested.par == 6 && scans == 1 && halts == 1);

    reset();
    uint8_t nt1[4], nt2[4];
    assert(static_nested_recover_core(nt1, nt2, 0xffffffffffff, 0, 0x60, 4, 0x61, 1) == 0);
    assert(memcmp(nt1, (uint8_t[]){0x11,0x22,0x33,0x44}, 4) == 0);
    assert(auth_calls == 2 && selects == 1 && scans == 0 && halts == 1);

    reset();
    uint8_t nonces[9], count;
    assert(mf1_hardnested_nonces_acquire(true, 0, 0x60, 0xffffffffffff, 4, 0x61, nonces, 9, &count) == 0);
    assert(count == 2 && scans == 1 && selects == 2 && delays == 2);
    assert(memcmp(nonces, (uint8_t[]){1,2,3,1,1,2,3,2,0x55}, 9) == 0);

    reset(); auth_failure = STATUS_HF_ERR_STAT;
    assert(nested_recover_core(&nested, 0xffffffffffff, 0, 0x60, 4, 0x61) == STATUS_MF_ERR_AUTH);
    reset(); auth_failure = STATUS_MF_ERR_AUTH;
    assert(mf1_hardnested_nonces_acquire(false, 0, 0x60, 0xffffffffffff, 4, 0x61, nonces, 9, &count) == STATUS_MF_ERR_AUTH);
    assert(auth_calls == 15 && send_calls == 0 && count == 0 && cleanups == 15);
    reset(); target_bits = 16;
    assert(mf1_hardnested_nonces_acquire(false, 0, 0x60, 0xffffffffffff, 4, 0x61, nonces, 9, &count) == STATUS_HF_ERR_STAT);
    assert(send_calls == 15 && count == 0 && cleanups == 15);
    reset(); selection_status = STATUS_HF_TAG_NO;
    assert(mf1_hardnested_nonces_acquire(false, 0, 0x60, 0xffffffffffff, 4, 0x61, nonces, 9, &count) == STATUS_HF_TAG_NO);
    assert(auth_calls == 0);
}
int main(void) {
    normalized_capture();
    checked_capture();
    rejected_requests();
    profiled_capture();
    legacy_formats();
    return 0;
}
