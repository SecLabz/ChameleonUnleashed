#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "nrf.h"
test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;
static unsigned authentications, reads;
static int auth_status, failure = -1;
static uint16_t failure_status = STATUS_MF_ERR_AUTH;
static uint8_t expected_first, expected_type;
static uint16_t auth_key_use_522_hw(uint8_t block, uint8_t type, uint8_t *key) {
    authentications++;
    assert(block == expected_first && type == expected_type && key[0] == 0xff);
    return auth_status;
}
static uint16_t pcd_14a_reader_mf1_read(uint8_t block, uint8_t *out) {
    reads++;
    memset(out, block, 16);
    return block == failure ? failure_status : STATUS_HF_TAG_OK;
}
typedef struct { uint8_t uid_len, uid[10]; } picc_14a_tag_t;
static picc_14a_tag_t selected = {4, {1, 2, 3, 4}};
static unsigned closes, halts, unauths, scans, rejected_cleanups;
static uint16_t scan_status;
static void nfca_session_close(void) { closes++; }
static uint16_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    scans++;
    *tag = selected;
    return scan_status;
}
static uint16_t pcd_14a_reader_mf1_auth(picc_14a_tag_t *tag, uint8_t type,
                                      uint8_t block, uint8_t *key) {
    assert(tag->uid_len == selected.uid_len);
    return auth_key_use_522_hw(block, type, key);
}
static void pcd_14a_reader_fast_halt_tag(void) { halts++; }
static void pcd_14a_reader_mf1_unauth(void) {
    if (auth_status == STATUS_MF_ERR_AUTH) { rejected_cleanups++; return; }
    assert(halts == unauths + 1); unauths++;
}
/* SECTOR FUNCTION */
int main(void) {
    uint8_t request[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 4, 1, 2, 3, 4}, out[272];
    uint16_t size = 0;
    expected_type = 0x60;
    for (unsigned sector = 0; sector < 40; sector++) {
        request[1] = sector;
        expected_first = sector < 32 ? sector * 4 : 128 + (sector - 32) * 16;
        unsigned count = sector < 32 ? 4 : 16;
        authentications = reads = 0;
        for (unsigned length = 8; length <= sizeof(request); length += 5) {
            authentications = reads = 0;
            assert(read_sector(length, request, out, &size) == STATUS_HF_TAG_OK);
            assert(authentications == 1 && reads == count && size == 2 + count * 16);
            assert((((unsigned)out[0] << 8) | out[1]) == (1u << count) - 1);
            for (unsigned i = 0; i < count; i++)
                for (unsigned j = 0; j < 16; j++) assert(out[2 + i * 16 + j] == expected_first + i);
        }
    }
    request[0] = expected_type = 0x61;
    request[1] = 32; expected_first = 128; failure = 130;
    assert(read_sector(sizeof(request), request, out, &size) == STATUS_HF_TAG_OK);
    assert(out[0] == 0 && out[1] == 3 && size == 258);
    for (unsigned i = 2 + 2 * 16; i < size; i++) assert(out[i] == 0);
    failure = 128;
    assert(read_sector(sizeof(request), request, out, &size) == STATUS_HF_TAG_OK);
    assert(out[0] == 0 && out[1] == 0);
    auth_status = STATUS_MF_ERR_AUTH; reads = 0;
    assert(read_sector(sizeof(request), request, out, &size) == STATUS_MF_ERR_AUTH && reads == 0);
    authentications = 0;
    for (unsigned n = 0; n < 8; n++) assert(read_sector(n, request, out, &size) == STATUS_PAR_ERR);
    request[1] = 40;
    assert(read_sector(sizeof(request), request, out, &size) == STATUS_PAR_ERR);
    request[1] = 0; request[0] = 0x62;
    assert(read_sector(sizeof(request), request, out, &size) == STATUS_PAR_ERR && authentications == 0);
    request[0] = expected_type = 0x60;
    request[1] = expected_first = 0;
    auth_status = 0; failure = -1; closes = 0;
    uint8_t keep[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                      4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0};
    for (unsigned uid_length = 4; uid_length <= 10; uid_length += 3) {
        selected.uid_len = keep[8] = uid_length;
        assert(read_sector(9 + uid_length, keep, out, &size) == STATUS_HF_TAG_OK);
        assert(out[1] == 15 && halts == unauths && closes == 0);
    }
    unsigned before = authentications;
    selected.uid[0] ^= 1;
    assert(read_sector(19, keep, out, &size) == STATUS_HF_TAG_NO);
    assert(authentications == before && closes == 1);
    selected.uid[0] ^= 1;
    scan_status = STATUS_HF_TAG_NO;
    assert(read_sector(19, keep, out, &size) == STATUS_HF_TAG_NO);
    assert(authentications == before && closes == 2);
    scan_status = 0; auth_status = STATUS_MF_ERR_AUTH;
    unsigned previous_cleanups = rejected_cleanups;
    assert(read_sector(19, keep, out, &size) == STATUS_MF_ERR_AUTH);
    assert(closes == 2 && rejected_cleanups == previous_cleanups + 1);
    assert(size == 0);
    auth_status = 0; failure = -1;
    unsigned previous_scans_after_rejection = scans;
    unsigned previous_auths_after_rejection = authentications;
    assert(read_sector(19, keep, out, &size) == STATUS_HF_TAG_OK);
    assert(scans == previous_scans_after_rejection + 1);
    assert(authentications == previous_auths_after_rejection + 1 && out[1] == 15);
    auth_status = 0; failure = 2;
    unsigned previous_halts = halts;
    assert(read_sector(19, keep, out, &size) == STATUS_HF_TAG_OK);
    assert(out[1] == 3 && closes == 3 && halts == previous_halts);
    for (unsigned n = 0; n < 19; n++) {
        if (n == 8) continue;
        assert(read_sector(n, keep, out, &size) == STATUS_PAR_ERR);
    }
    keep[8] = 5;
    assert(read_sector(14, keep, out, &size) == STATUS_PAR_ERR);

    // Legacy callers send exactly eight bytes and receive the original response.
    uint8_t legacy[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    unsigned previous_scans = scans;
    scan_status = STATUS_HF_TAG_NO; failure = -1;
    assert(read_sector(sizeof(legacy), legacy, out, &size) == STATUS_HF_TAG_OK);
    assert(size == 66 && out[0] == 0 && out[1] == 15 && scans == previous_scans);
    assert(halts == previous_halts);
    failure = 2;
    assert(read_sector(sizeof(legacy), legacy, out, &size) == STATUS_HF_TAG_OK);
    assert(size == 66 && out[1] == 3);
    for (unsigned i = 34; i < size; i++) assert(out[i] == 0);
    auth_status = STATUS_MF_ERR_AUTH; reads = 0;
    assert(read_sector(sizeof(legacy), legacy, out, &size) == STATUS_MF_ERR_AUTH);
    assert(reads == 0);

    // Optional checked-read diagnostics preserve the legacy body and RF calls.
    uint8_t diagnostic[21] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                             4, 1, 2, 3, 4};
    uint8_t ordinary[272];
    auth_status = scan_status = 0; selected.uid_len = 4;
    for (unsigned sector = 0; sector < 40; sector++) {
        expected_first = sector < 32 ? sector * 4 : 128 + (sector - 32) * 16;
        unsigned count = sector < 32 ? 4 : 16;
        diagnostic[1] = sector;
        for (unsigned uid_length = 4; uid_length <= 10; uid_length += 3) {
            selected.uid_len = diagnostic[8] = uid_length;
            memcpy(diagnostic + 9, selected.uid, uid_length);
            unsigned base = 9 + uid_length;
            failure = -1;
            authentications = reads = 0;
            assert(read_sector(base, diagnostic, ordinary, &size) == STATUS_HF_TAG_OK);
            assert(size == 2 + count * 16 && authentications == 1 && reads == count);
            diagnostic[base] = 1;
            authentications = reads = 0;
            assert(read_sector(base + 1, diagnostic, out, &size) == STATUS_HF_TAG_OK);
            assert(size == 6 + count * 16 && authentications == 1 && reads == count);
            assert(!memcmp(out, ordinary, size - 4));
            assert(out[size - 4] == 1 && out[size - 3] == 0xff);
            assert(out[size - 2] == 0 && out[size - 1] == 0);
            for (unsigned offset = 0; offset < count; offset++) {
                for (unsigned error = 1; error <= 7; error++) {
                    failure = expected_first + offset; failure_status = error;
                    authentications = reads = 0;
                    assert(read_sector(base + 1, diagnostic, out, &size) == STATUS_HF_TAG_OK);
                    assert(size == 6 + count * 16 && authentications == 1 && reads == offset + 1);
                    assert((((unsigned)out[0] << 8) | out[1]) == (1u << offset) - 1);
                    for (unsigned i = 2 + offset * 16; i < size - 4; i++) assert(out[i] == 0);
                    assert(out[size - 4] == 1 && out[size - 3] == offset);
                    assert(out[size - 2] == 0 && out[size - 1] == error);
                }
            }
        }
    }
    selected.uid_len = diagnostic[8] = 4;
    diagnostic[1] = expected_first = 0;
    diagnostic[13] = 1; failure = 0; failure_status = 0x1234;
    assert(read_sector(14, diagnostic, out, &size) == STATUS_HF_TAG_OK);
    assert(size == 70 && out[67] == 0 && out[68] == 0x12 && out[69] == 0x34);
    authentications = reads = 0; failure = -1;
    for (unsigned flag = 0; flag < 256; flag++) {
        if (flag == 1) continue;
        diagnostic[13] = flag;
        assert(read_sector(14, diagnostic, out, &size) == STATUS_PAR_ERR && size == 0);
    }
    diagnostic[13] = 1;
    assert(read_sector(15, diagnostic, out, &size) == STATUS_PAR_ERR && size == 0);
    assert(authentications == 0 && reads == 0);
    selected.uid[0] ^= 1;
    assert(read_sector(14, diagnostic, out, &size) == STATUS_HF_TAG_NO && size == 0);
    assert(authentications == 0 && reads == 0);
    selected.uid[0] ^= 1;
    scan_status = STATUS_MF_ERR_AUTH;
    unsigned closes_before_scan_error = closes;
    unsigned cleanups_before_scan_error = rejected_cleanups;
    assert(read_sector(14, diagnostic, out, &size) == STATUS_MF_ERR_AUTH && size == 0);
    assert(authentications == 0 && reads == 0);
    assert(closes == closes_before_scan_error + 1 && rejected_cleanups == cleanups_before_scan_error);
    scan_status = 0; auth_status = STATUS_MF_ERR_AUTH;
    assert(read_sector(14, diagnostic, out, &size) == STATUS_MF_ERR_AUTH && size == 0);
    assert(authentications == 1 && reads == 0);
}
