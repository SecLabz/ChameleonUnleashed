#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "nrf.h"
test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

typedef struct {
    uint8_t uid_len;
    uint8_t uid[10];
    uint8_t sak;
} picc_14a_tag_t;

static picc_14a_tag_t selected = {7, {4, 1, 2, 3, 4, 5, 6}, 0};
static uint8_t memory[44][4];
static unsigned scans;
static unsigned halts;
static unsigned auths;
static unsigned reads;
static unsigned writes;
static unsigned transfers;
static bool changed_uid;
static bool bad_prefix;
static bool bad_readback;
static uint16_t probe_status;
static uint16_t auth_status;
static uint16_t write_status;
static uint16_t probe_length = 9;

static void pcd_14a_reader_fast_halt_tag(void) {
    halts++;
}

static uint16_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    *tag = selected;
    if (changed_uid && scans % 2 == 1) {
        tag->uid[6] ^= 1;
    }
    scans++;
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_raw_cmd(bool open, bool wait, bool append,
    bool select, bool keep, bool crc, uint16_t timeout, uint16_t bits,
    uint8_t *sent, uint8_t *out, uint16_t *size, uint16_t capacity) {
    assert(!open && wait && append && !select && keep && crc && timeout == 25);
    transfers++;
    if (sent[0] == 0x1a) {
        assert(bits == 16 && sent[1] == 0 && capacity == 88);
        memset(out, 42, 11);
        out[0] = bad_prefix ? 0 : 0xaf;
        *size = probe_length;
        return probe_status;
    }
    if (sent[0] == 0x60) {
        const uint8_t version[] = {0, 4, 4, 2, 1, 0, 0x11, 3};
        memcpy(out, version, sizeof(version));
        *size = sizeof(version);
        return STATUS_HF_TAG_OK;
    }
    assert(sent[0] == 0x1b && bits == 40);
    *size = 2;
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_mf0_ulc_auth(uint8_t *key) {
    auths++;
    for (unsigned i = 0; i < 16; i++) {
        assert(key[i] == i);
    }
    return auth_status;
}

static uint16_t pcd_14a_reader_mf1_auth(picc_14a_tag_t *tag, uint8_t kind,
    uint8_t unit, uint8_t *key) {
    (void)tag;
    (void)kind;
    (void)unit;
    (void)key;
    assert(false);
    return STATUS_MF_ERR_AUTH;
}

static uint16_t pcd_14a_reader_mf1_read(uint8_t page, uint8_t *out) {
    reads++;
    assert(page < 44);
    memset(out, 0, 16);
    memcpy(out, memory[page], 4);
    if (bad_readback && writes) {
        out[0] ^= 1;
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_mf0_ult_write_page(uint8_t page, uint8_t *data) {
    writes++;
    assert(page >= 4 && page < 40);
    if (!write_status) {
        memcpy(memory[page], data, 4);
    }
    return write_status;
}

static uint16_t pcd_14a_reader_mf1_write(uint8_t unit, uint8_t *data) {
    (void)unit;
    (void)data;
    assert(false);
    return STATUS_CMD_ERR;
}

/* PRODUCTION FUNCTIONS */

static void reset(void) {
    scans = 0;
    halts = 0;
    auths = 0;
    reads = 0;
    writes = 0;
    transfers = 0;
    changed_uid = false;
    bad_prefix = false;
    bad_readback = false;
    probe_status = 0;
    auth_status = 0;
    write_status = 0;
    probe_length = 9;
    for (unsigned page = 0; page < 44; page++) {
        memset(memory[page], page, 4);
    }
}

int main(void) {
    uint8_t input[59] = {19, 7, 4, 1, 2, 3, 4, 5, 6, 39, 4};
    for (unsigned i = 0; i < 16; i++) {
        input[11 + i] = i;
    }
    uint8_t out[16];
    uint16_t length = 0;
    reset();
    assert(type2_auth_read(27, input, out, &length) == 0);
    assert(length == 16 && out[0] == 39 && out[4] == 0);
    assert(scans == 2 && halts == 1 && auths == 1 && reads == 1);
    input[9] = 43;
    assert(type2_auth_read(27, input, out, &length) == 0 && out[0] == 43);
    input[9] = 44;
    unsigned previous = scans;
    assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR && scans == previous);
    input[9] = 39;
    input[10] = 3;
    assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR && scans == previous);
    input[10] = 4;
    for (unsigned failure = 0; failure < 5; failure++) {
        reset();
        changed_uid = failure == 0;
        bad_prefix = failure == 1;
        probe_status = failure == 2 ? STATUS_HF_ERR_CRC : 0;
        probe_length = failure == 3 ? 8 : 9;
        auth_status = failure == 4 ? STATUS_MF_ERR_AUTH : 0;
        assert(type2_auth_read(27, input, out, &length) != 0);
        assert(reads == 0 && writes == 0);
    }
    reset();
    input[10] = 0;
    memset(input + 11, 0, 16);
    assert(type2_auth_read(27, input, out, &length) == 0 && auths == 0);
    input[11] = 1;
    previous = scans;
    assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR && scans == previous);
    input[10] = 4;
    for (unsigned i = 0; i < 16; i++) {
        input[11 + i] = i;
    }
    memset(input + 27, 39, 4);
    memset(input + 43, 99, 4);
    reset();
    assert(write_user_unit(59, input, out, &length) == 0);
    assert(writes == 1 && reads == 2 && length == 4 && out[0] == 99);
    reset();
    input[27] ^= 1;
    assert(write_user_unit(59, input, out, &length) == STATUS_CMD_ERR && writes == 0);
    input[27] ^= 1;
    reset();
    auth_status = STATUS_MF_ERR_AUTH;
    assert(write_user_unit(59, input, out, &length) == STATUS_MF_ERR_AUTH && writes == 0);
    reset();
    changed_uid = true;
    assert(write_user_unit(59, input, out, &length) == STATUS_HF_TAG_NO && writes == 0);
    reset();
    write_status = STATUS_HF_ERR_STAT;
    assert(write_user_unit(59, input, out, &length) == STATUS_HF_ERR_STAT && writes == 1 && reads == 1);
    reset();
    bad_readback = true;
    assert(write_user_unit(59, input, out, &length) == STATUS_CMD_ERR && writes == 1);
    for (unsigned page = 40; page < 48; page++) {
        reset();
        input[9] = page;
        assert(write_user_unit(59, input, out, &length) == STATUS_PAR_ERR && scans == 0);
    }
    input[9] = 39;
    input[0] = 2;
    reset();
    assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR && scans == 0);
    input[10] = 3;
    memset(input + 11, 0, 16);
    assert(type2_auth_read(27, input, out, &length) == 0 && auths == 0);
    // Malformed sizes and credentials must be rejected before any RF operation.
    input[0] = 19;
    input[9] = 39;
    input[10] = 4;
    for (unsigned i = 0; i < 16; i++) {
        input[11 + i] = i;
    }
    for (unsigned size = 0; size < 70; size++) {
        reset();
        if (size != 27) {
            assert(type2_auth_read(size, input, out, &length) == STATUS_PAR_ERR);
            assert(scans == 0);
        }
        if (size != 59) {
            assert(write_user_unit(size, input, out, &length) == STATUS_PAR_ERR);
            assert(scans == 0);
        }
    }
    for (unsigned kind = 1; kind < 256; kind++) {
        if (kind == 4) {
            continue;
        }
        reset();
        input[10] = kind;
        assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR);
        assert(write_user_unit(59, input, out, &length) == STATUS_PAR_ERR);
        assert(scans == 0);
    }
    input[10] = 4;
    for (unsigned page = 0; page < 256; page++) {
        reset();
        input[9] = page;
        if (page >= 44) {
            assert(type2_auth_read(27, input, out, &length) == STATUS_PAR_ERR);
        }
        if (page < 4 || page >= 40) {
            assert(write_user_unit(59, input, out, &length) == STATUS_PAR_ERR);
        }
        assert(scans == 0);
    }
    reset();
    input[9] = 39;
    input[8] ^= 1;
    assert(type2_auth_read(27, input, out, &length) == STATUS_HF_TAG_NO);
    assert(scans == 1 && transfers == 0 && auths == 0 && reads == 0);
    reset();
    assert(write_user_unit(59, input, out, &length) == STATUS_HF_TAG_NO);
    assert(scans == 1 && transfers == 0 && auths == 0 && writes == 0);

}
