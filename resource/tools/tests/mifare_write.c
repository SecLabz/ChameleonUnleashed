#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "mf1_magic.h"

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
} picc_14a_tag_t;

static uint8_t memory[256][16];
static uint16_t signatures;
static unsigned writes;
static unsigned scans;
static unsigned replace_scan;
static bool corrupt_readback;
static bool incompatible_activation;
static bool lost_activation;
static bool corrupt_written_key_a;
static bool corrupt_written_key_b;
static unsigned new_key_a_authentications;
static unsigned new_key_b_authentications;
static bool unauthenticated;
static uint8_t selected_sak = 8;
static bool selected_sak_from_block0;
static uint16_t capacity_status = STATUS_HF_TAG_NO;
static unsigned capacity_checks;

uint16_t mf1_classic_2k_sl1_check(uint8_t uid_length, const uint8_t *uid,
                                  const uint8_t *atqa, uint8_t sak) {
    assert(uid_length == 4 && !memcmp(uid, memory[0], 4));
    assert(sak == 8 && atqa[0] == 4 && atqa[1] == 0);
    capacity_checks++;
    return capacity_status;
}

static uint16_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    if (lost_activation && writes) {
        return STATUS_HF_TAG_NO;
    }
    memset(tag, 0, sizeof(*tag));
    memcpy(tag->uid, memory[0], 4);
    tag->uid_len = 4;
    tag->sak = incompatible_activation && writes ? 9 :
        selected_sak_from_block0 ? memory[0][5] : selected_sak;
    tag->atqa[0] = 4;
    if (++scans == replace_scan) {
        tag->uid[0] ^= 1;
    }
    unauthenticated = true;
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_mf1_auth(picc_14a_tag_t *tag, uint8_t kind,
                                       uint8_t block, uint8_t *key) {
    assert(tag->uid_len == 4);
    unsigned trailer = block < 128 ? block / 4 * 4 + 3 : (block - 128) / 16 * 16 + 143;
    if (writes) {
        if (kind == 0x60) {
            new_key_a_authentications++;
        } else {
            new_key_b_authentications++;
        }
    }
    if (memcmp(memory[trailer] + (kind == 0x60 ? 0 : 10), key, 6)) {
        return STATUS_MF_ERR_AUTH;
    }
    unauthenticated = false;
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_gen1a_unlock(void) {
    unauthenticated = false;
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_mf1_read(uint8_t block, uint8_t *output) {
    assert(!unauthenticated);
    memcpy(output, memory[block], 16);
    if (corrupt_readback && writes) {
        output[9] ^= 1;
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t pcd_14a_reader_mf1_write(uint8_t block, uint8_t *data) {
    assert(!unauthenticated);
    memcpy(memory[block], data, 16);
    if (corrupt_written_key_a) {
        memory[block][0] ^= 1;
    }
    if (corrupt_written_key_b) {
        memory[block][10] ^= 1;
    }
    writes++;
    return STATUS_HF_TAG_OK;
}

static void pcd_14a_reader_reset(void) {
}

static void pcd_14a_reader_antenna_on(void) {
}

static void bsp_delay_ms(uint16_t milliseconds) {
    assert(milliseconds == 8);
}

uint16_t mf1_magic_probe(uint16_t length, const uint8_t *data,
                          uint8_t *output, uint16_t *output_length) {
    assert(length == 15 || length == 21);
    assert(data[0] == 4);
    assert(!memcmp(data + 1, memory[0], 4));
    output[0] = signatures >> 8;
    output[1] = signatures;
    output[2] = 0;
    *output_length = 3;
    return STATUS_HF_TAG_OK;
}

/* WRITE FUNCTIONS */

static void reset(void) {
    selected_sak = 8;
    selected_sak_from_block0 = false;
    capacity_status = STATUS_HF_TAG_NO;
    capacity_checks = 0;
    writes = 0;
    scans = 0;
    replace_scan = 0;
    corrupt_readback = false;
    incompatible_activation = false;
    lost_activation = false;
    corrupt_written_key_a = false;
    corrupt_written_key_b = false;
    new_key_a_authentications = 0;
    new_key_b_authentications = 0;
    signatures = MF1_MAGIC_GEN1A;
    memset(memory, 0, sizeof(memory));
    uint8_t identity[] = {1, 2, 3, 4, 4, 8, 4, 0};
    memcpy(memory[0], identity, sizeof(identity));
    for (unsigned i = 0; i < 256; i++) {
        if ((i < 128 && i % 4 == 3) || (i >= 128 && i % 16 == 15)) {
            memset(memory[i], 255, 16);
            memory[i][7] = 7;
            memory[i][8] = 128;
            memory[i][9] = 105;
        }
    }
}

static void test_block0(void) {
    uint8_t request[60] = {1, 4, 1, 2, 3, 4};
    memcpy(request + 27, memory[0], 16);
    memcpy(request + 43, memory[0], 16);
    uint8_t desired[] = {5, 6, 7, 8, 12, 9, 5, 1, 9, 10, 11, 12, 13, 14, 15, 16};
    memcpy(request + 43, desired, 16);
    request[59] = 1;
    uint8_t output[16];
    uint16_t length = 0;
    assert(block0_set(60, request, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 16 && writes == 0 && !memcmp(output, request + 27, 16));
    assert(block0_set(59, request, output, &length) == STATUS_HF_TAG_OK);
    assert(writes == 1 && length == 16 && !memcmp(output, desired, 16));

    reset();
    request[47] ^= 1;
    assert(block0_set(59, request, output, &length) == STATUS_PAR_ERR && writes == 0);
    request[47] ^= 1;
    request[42] ^= 1;
    assert(block0_set(59, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    request[42] ^= 1;
    signatures = MF1_MAGIC_GEN1A | MF1_MAGIC_UNFUSED;
    assert(block0_set(59, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    signatures = MF1_MAGIC_GTU;
    assert(block0_set(59, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    signatures = MF1_MAGIC_GEN2;
    request[10] = 1;
    memset(request + 11, 255, 6);
    assert(block0_set(60, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    assert(block0_set(59, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);

    reset();
    request[10] = 0;
    memset(request + 11, 0, 6);
    replace_scan = 2;
    assert(block0_set(59, request, output, &length) == STATUS_HF_TAG_NO && writes == 0);
    reset();
    corrupt_readback = true;
    assert(block0_set(59, request, output, &length) == STATUS_CMD_ERR && writes == 1);
    reset();
    incompatible_activation = true;
    assert(block0_set(59, request, output, &length) == STATUS_CMD_ERR && writes == 1);
    reset();
    lost_activation = true;
    assert(block0_set(59, request, output, &length) == STATUS_HF_TAG_NO && writes == 1);
}

static void test_trailers(void) {
    reset();
    uint8_t request[38] = {4, 1, 2, 3, 4, 0, 0, 0, 1, 0, 1};
    memset(request + 11, 255, 6);
    memcpy(request + 17, memory[3] + 6, 4);
    memset(request + 21, 42, 12);
    memcpy(request + 33, memory[3] + 6, 4);
    request[37] = 1;
    uint8_t output[16];
    uint16_t length = 0;
    assert(trailer_set(38, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    assert(trailer_set(37, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(!memcmp(memory[3], request + 21, 6));
    assert(!memcmp(memory[3] + 10, request + 27, 6));

    reset();
    request[33] ^= 1;
    assert(trailer_set(38, request, output, &length) == STATUS_PAR_ERR && writes == 0);
    request[33] ^= 1;
    request[20] ^= 1;
    assert(trailer_set(38, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    request[20] ^= 1;
    memory[3][6] = 0x7f;
    memory[3][8] = 0x88;
    memcpy(request + 17, memory[3] + 6, 4);
    assert(trailer_set(38, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    request[10] = 2;
    assert(trailer_set(38, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    assert(trailer_set(37, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);

    reset();
    request[8] = 7;
    request[9] = 39;
    request[10] = 1;
    memcpy(request + 17, memory[255] + 6, 4);
    assert(trailer_set(38, request, output, &length) == STATUS_HF_TAG_NO && writes == 0);
    request[8] = 1;
    request[9] = 0;
    corrupt_readback = true;
    assert(trailer_set(37, request, output, &length) == STATUS_CMD_ERR && writes == 1);

    reset();
    // Condition 3 hides Key B and requires authenticating it after the write.
    request[33] = 0x7f;
    request[34] = 0x07;
    request[35] = 0x88;
    memset(request + 21, 42, 6);
    memset(request + 27, 43, 6);
    assert(trailer_set(38, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    assert(new_key_a_authentications == 0 && new_key_b_authentications == 0);
    assert(trailer_set(37, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(new_key_a_authentications == 1 && new_key_b_authentications == 1);

    reset();
    corrupt_written_key_a = true;
    assert(trailer_set(37, request, output, &length) == STATUS_MF_ERR_AUTH && writes == 1);
    assert(new_key_a_authentications == 1 && new_key_b_authentications == 0);

    reset();
    corrupt_written_key_b = true;
    assert(trailer_set(37, request, output, &length) == STATUS_MF_ERR_AUTH && writes == 1);
    assert(new_key_a_authentications == 1 && new_key_b_authentications == 1);
}

int main(void) {
    reset();
    test_block0();
    test_trailers();
    return 0;
}
