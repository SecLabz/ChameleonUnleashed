#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "crc_utils.h"
#include "mf1_magic.h"

#define MF1_TOOLBOX
typedef enum { PRNG_STATIC, PRNG_WEAK, PRNG_HARD } mf1_prng_type_t;
static uint8_t check_prng_type(mf1_prng_type_t *type);
typedef struct { uint8_t key[6]; } mf1_key_t;
typedef struct {
    uint8_t block;
    uint8_t key_type;
    uint8_t keys_len;
    mf1_key_t *keys;
} mf1_toolbox_check_keys_on_block_in_t;
typedef struct { uint8_t found; mf1_key_t key; } mf1_toolbox_check_keys_on_block_out_t;
static uint16_t mf1_toolbox_check_keys_on_block(mf1_toolbox_check_keys_on_block_in_t *in,
    mf1_toolbox_check_keys_on_block_out_t *out);


#define RC522_H
#define PCD_TRANSCEIVE 12
#define PICC_READ 0x30
#define PICC_WRITE 0xa0
#define DEF_FIFO_LENGTH 64

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
    uint8_t cascade;
} picc_14a_tag_t;
typedef struct {
    int8_t forcebcc;
    int8_t forcecl2;
    int8_t forcecl3;
    int8_t forcerats;
} hf14a_config_t;

static picc_14a_tag_t selected = {{1, 2, 3, 4}, 4, 8, {4, 0}, 1};
static hf14a_config_t config = {0, 0, 0, 1};
static uint16_t enabled;
static uint16_t timeout_ms = 25;
static unsigned scans;
static unsigned replace_at;
static unsigned fault;
static unsigned writes;
static bool field;
static bool awaiting_data;
static bool authenticated;
static uint8_t expected_key_type = 0x61;
static uint8_t write_reply = 0x0a;
static uint8_t prng_status;
static unsigned signature_rounds;
static unsigned authentications;
static uint8_t accepted_key = 0xff;
static uint8_t authenticated_type;
static bool key_b_write_only;
static bool selected_state;
static bool halted_state;
static bool cold_only;
static bool halt_only;

static void pcd_14a_reader_antenna_off(void) {
    field = false;
    awaiting_data = false;
    selected_state = false;
    halted_state = false;
}

static void pcd_14a_reader_mf1_unauth(void) {
    authenticated = false;
}

static void bsp_delay_ms(uint16_t delay_ms) {
    assert(delay_ms == 5 || delay_ms == 8);
}

static void pcd_14a_reader_reset(void) {
    assert(!field && !awaiting_data);
    timeout_ms = 25;
}

static void pcd_14a_reader_antenna_on(void) {
    field = true;
}

static uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    assert(field && !awaiting_data && config.forcerats == 2);
    *tag = selected;
    selected_state = true;
    if (++scans == replace_at) {
        tag->atqa[0] ^= 1;
    }
    return STATUS_HF_TAG_OK;
}

static void pcd_14a_reader_timeout_set(uint16_t value) {
    timeout_ms = value;
}

static uint16_t pcd_14a_reader_timeout_get(void) {
    return timeout_ms;
}

static hf14a_config_t *get_hf14a_config(void) {
    return &config;
}

static void set_hf14a_config(const hf14a_config_t *value) {
    config = *value;
}

static uint8_t pcd_14a_reader_halt_tag(void) {
    assert(field && !awaiting_data);
    assert(selected_state);
    halted_state = true;
    return STATUS_HF_TAG_OK;
}

static void crc_14a_calculate(uint8_t *data, size_t size, uint8_t *crc) {
    calc_14a_crc_lut(data, size, crc);
}

static void crc_14a_append(uint8_t *data, size_t size) {
    crc_14a_calculate(data, size, data + size);
}

static uint8_t ack(bool matches, uint8_t *out, uint16_t *bits) {
    *out = 0x0a;
    *bits = fault == 2 ? 8 : 4;
    return matches && fault != 3 ? STATUS_HF_TAG_OK : STATUS_HF_TAG_NO;
}

static uint8_t pcd_14a_reader_bits_transfer(uint8_t *tx, uint16_t tx_bits,
    uint8_t *tx_parity, uint8_t *rx, uint8_t *rx_parity, uint16_t *rx_bits, uint16_t capacity) {
    assert(field && !awaiting_data && tx_bits == 7 && capacity == 8);
    assert(!tx_parity && !rx_parity);
    if (*tx == 0x40) {
        if ((cold_only && selected_state) || (halt_only && !halted_state)) {
            return ack(false, rx, rx_bits);
        }
        return ack(enabled & (MF1_MAGIC_GEN1A | MF1_MAGIC_GEN1B | MF1_MAGIC_GDM_40 | MF1_MAGIC_GDM_ZUID), rx, rx_bits);
    }
    assert(*tx == 0x20);
    return ack(enabled & MF1_MAGIC_GDM_20, rx, rx_bits);
}

static uint8_t crc_reply(uint8_t *rx, uint16_t *bits, unsigned size) {
    memset(rx, 0x42, size);
    crc_14a_append(rx, size - 2);
    if (fault == 1) {
        rx[size - 1] ^= 1;
    }
    *bits = fault == 2 ? size * 8 - 1 : size * 8;
    return fault == 3 ? STATUS_HF_ERR_PARITY : STATUS_HF_TAG_OK;
}

static uint8_t pcd_14a_reader_bytes_transfer(uint8_t command, uint8_t *tx,
    uint8_t size, uint8_t *rx, uint16_t *bits, uint16_t capacity) {
    assert(field && !awaiting_data && command == PCD_TRANSCEIVE);
    if (size == 1) {
        assert(capacity == 8);
        if (*tx == 0x43) {
            return ack(enabled & (MF1_MAGIC_GEN1A | MF1_MAGIC_GDM_40 | MF1_MAGIC_GDM_ZUID), rx, bits);
        }
        assert(*tx == 0x23);
        return ack(enabled & MF1_MAGIC_GDM_20, rx, bits);
    }
    assert(size >= 4 && size <= 9 && capacity == 512);
    uint8_t crc[2];
    crc_14a_calculate(tx, size - 2, crc);
    assert(!memcmp(crc, tx + size - 2, 2));
    if (*tx == PICC_WRITE) {
        assert(authenticated && size == 4 && tx[1] == 0);
        writes++;
        awaiting_data = true;
        *rx = key_b_write_only && authenticated_type == 0x60 ? 4 : write_reply;
        *bits = fault == 2 ? 8 : 4;
        return fault == 3 ? STATUS_HF_TAG_NO : STATUS_HF_TAG_OK;
    }
    if (*tx == 0xe0 && tx[1] == 0x20 && (enabled & MF1_MAGIC_GEN2)) {
        static const uint8_t ats[] = {9, 0x78, 0, 0x91, 2, 0xda, 0xbc, 0x19, 0x10};
        crc_reply(rx, bits, sizeof(ats) + 2);
        memcpy(rx, ats, sizeof(ats));
        crc_14a_append(rx, sizeof(ats));
        if (fault == 1) {
            rx[sizeof(ats)] ^= 1;
        }
        return fault == 3 ? STATUS_HF_ERR_PARITY : STATUS_HF_TAG_OK;
    }
    if (*tx == 0xcf) {
        signature_rounds++;
    }
    unsigned response_size = 0;
    if (*tx == 0xe0 && tx[1] == 0 && (enabled & (MF1_MAGIC_GDM_20 | MF1_MAGIC_GDM_40 | MF1_MAGIC_GDM_ZUID))) {
        response_size = 18;
    } else if (*tx == 0x38 && (enabled & MF1_MAGIC_GDM_40)) {
        response_size = 18;
    } else if (*tx == 0xcf && (enabled & MF1_MAGIC_GTU)) {
        response_size = 34;
    } else if (*tx == PICC_READ && (enabled & MF1_MAGIC_GEN3)) {
        response_size = 18;
    } else if (*tx == 0x80 && (enabled & MF1_MAGIC_GDM_AUTH)) {
        response_size = 4;
    } else if (*tx == 0x0a && (enabled & (MF1_MAGIC_SUPER_GEN1 | MF1_MAGIC_SUPER_GEN2))) {
        response_size = 22;
    }
    return response_size ? crc_reply(rx, bits, response_size) : STATUS_HF_TAG_NO;
}

/* SELECTED UID FUNCTION */

static uint16_t pcd_14a_reader_mf1_auth(picc_14a_tag_t *tag, uint8_t type,
    uint8_t block, uint8_t *key) {
    assert(field && !awaiting_data && !memcmp(tag->uid, selected.uid, selected.uid_len));
    uint8_t authentication_uid[4];
    get_4byte_tag_uid(tag, authentication_uid);
    assert(!memcmp(authentication_uid, selected.uid + selected.uid_len - 4, 4));
    if (block == 68) {
        static const uint8_t expected[] = {0x70, 0x7b, 0x11, 0xfc, 0x14, 0x81};
        assert(type == 0x61 && !memcmp(key, expected, 6));
        return enabled & MF1_MAGIC_QL88 ? STATUS_HF_TAG_OK : STATUS_MF_ERR_AUTH;
    }
    assert(block == 0);
    assert(type == 0x60 || type == 0x61);
    authentications++;
    authenticated_type = type;
    authenticated = (!expected_key_type || type == expected_key_type) && key[0] == accepted_key;
    return authenticated ? STATUS_HF_TAG_OK : STATUS_MF_ERR_AUTH;
}

static uint8_t check_prng_type(mf1_prng_type_t *type) {
    assert(field && !awaiting_data && timeout_ms == 5);
    *type = PRNG_WEAK;
    return prng_status;
}

static uint16_t mf1_toolbox_check_keys_on_block(mf1_toolbox_check_keys_on_block_in_t *in,
    mf1_toolbox_check_keys_on_block_out_t *out) {
    assert(field && !awaiting_data && in->block == 0);
    for (unsigned index = 0; index < in->keys_len; index++) {
        if ((!expected_key_type || in->key_type == expected_key_type) && in->keys[index].key[0] == accepted_key) {
            out->found = 1;
            out->key = in->keys[index];
            return STATUS_HF_TAG_OK;
        }
    }
    return STATUS_MF_ERR_AUTH;
}

#include "mf1_magic.c"

static void inspect(void) {
    uint8_t request[27] = {4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 4, 0, 8, 2};
    memset(request + 15, 0xff, 6);
    memset(request + 21, 0xa0, 6);
    uint8_t out[11];
    uint16_t count;
    selected = (picc_14a_tag_t){{1, 2, 3, 4}, 4, 8, {4, 0}, 1};
    enabled = 0;
    expected_key_type = 0x60;
    writes = 0;
    scans = 0;
    signature_rounds = 0;
    authentications = 0;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    const uint8_t expected[] = {0, 4, MF1_MAGIC_GEN2_WRITE_ACK, PRNG_WEAK, 1};
    assert(count == sizeof(out) && !memcmp(out, expected, sizeof(expected)));
    assert(!memcmp(out + 5, request + 15, 6));
    assert(writes == 1 && authentications == 1 && signature_rounds == 1);

    accepted_key = 0xa0;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    assert(out[4] == 1 && !memcmp(out + 5, request + 21, 6));
    expected_key_type = 0x61;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    assert(out[4] == 2 && !memcmp(out + 5, request + 21, 6));
    accepted_key = 0xff;
    expected_key_type = 0;
    key_b_write_only = true;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    assert(out[2] == MF1_MAGIC_GEN2_WRITE_ACK && out[4] == 2);
    key_b_write_only = false;
    expected_key_type = 0x60;
    write_reply = 4;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    assert(out[2] == MF1_MAGIC_GEN2_WRITE_DENIED && out[4] == 1);
    write_reply = 0x0a;
    accepted_key = 0x42;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    const uint8_t empty[6] = {0};
    assert(out[2] == MF1_MAGIC_GEN2_AUTH_DENIED && out[4] == 0 && !memcmp(out + 5, empty, 6));

    prng_status = STATUS_HF_TAG_NO;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_NO);
    assert(count == 0);
    prng_status = 0;
    request[14] = 65;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_PAR_ERR);
    assert(count == 0);
    request[14] = 2;
    request[5] = 1;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_PAR_ERR);
    assert(count == 0);
    request[5] = 0;
    accepted_key = 0xff;
    expected_key_type = 0x60;
    // Every selection, including those around PRNG, rejects a changed target.
    scans = 0;
    assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_OK);
    unsigned selections = scans;
    for (replace_at = 1; replace_at <= selections; replace_at++) {
        scans = 0;
        assert(mf1_classic_inspect(sizeof(request), request, out, &count) == STATUS_HF_TAG_NO);
        assert(count == 0);
        assert(!field && !authenticated && !awaiting_data && timeout_ms == 25 && config.forcerats == 1);
    }
    replace_at = 0;
}


static void run(uint8_t *request, unsigned size, unsigned flags, unsigned evidence, unsigned status) {
    uint8_t out[3] = {0xaa, 0xaa, 0xaa};
    uint16_t count = 99;
    scans = 0;
    assert(mf1_magic_probe(size, request, out, &count) == status);
    if (status == STATUS_HF_TAG_OK) {
        assert(count == 3 && (((unsigned)out[0] << 8) | out[1]) == flags && out[2] == evidence);
    } else {
        assert(count == 0 && out[0] == 0xaa);
    }
    assert(!field && !authenticated && !awaiting_data && timeout_ms == 25 && config.forcerats == 1);
}

int main(void) {
    uint8_t request[21] = {4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 4, 0, 8, 0};
    const uint16_t cases[] = {0, MF1_MAGIC_GEN1A, MF1_MAGIC_GEN1B, MF1_MAGIC_GEN2,
        MF1_MAGIC_GEN3, MF1_MAGIC_GTU, MF1_MAGIC_GDM_AUTH, MF1_MAGIC_QL88,
        MF1_MAGIC_GDM_20, MF1_MAGIC_GDM_40 | MF1_MAGIC_GEN1A,
        MF1_MAGIC_GDM_ZUID | MF1_MAGIC_GEN1A, MF1_MAGIC_SUPER_GEN1,
        MF1_MAGIC_SUPER_GEN2 | MF1_MAGIC_GEN3};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        enabled = cases[i];
        run(request, 15, enabled, enabled & MF1_MAGIC_GEN2 ? MF1_MAGIC_GEN2_ATS : 0, 0);
    }
    // Gen1 peers can require either a fresh field or SELECT + HALT. Gen1B
    // must reopen the same path after the second wakeup is not acknowledged.
    for (unsigned mode = 0; mode < 2; mode++) {
        cold_only = mode == 0;
        halt_only = mode == 1;
        for (unsigned index = 1; index <= 2; index++) {
            enabled = cases[index];
            run(request, 15, enabled, 0, STATUS_HF_TAG_OK);
        }
    }
    cold_only = halt_only = false;
    // Neither malformed ACKs nor transport failures identify a Gen1 card.
    for (fault = 2; fault <= 3; fault++) {
        enabled = MF1_MAGIC_GEN1A;
        run(request, 15, 0, 0, STATUS_HF_TAG_OK);
    }
    for (fault = 1; fault <= 3; fault++) {
        enabled = MF1_MAGIC_GEN2 | MF1_MAGIC_GEN3 | MF1_MAGIC_GTU | MF1_MAGIC_SUPER_GEN1;
        run(request, 15, 0, 0, 0);
    }
    enabled = 0;
    request[14] = 2;
    memset(request + 15, 0xff, 6);
    fault = 0;
    run(request, 21, MF1_MAGIC_GEN2, MF1_MAGIC_GEN2_WRITE_ACK, 0);
    assert(writes == 1);
    request[14] = 1;
    expected_key_type = 0x60;
    run(request, 21, MF1_MAGIC_GEN2, MF1_MAGIC_GEN2_WRITE_ACK, 0);
    write_reply = 4;
    run(request, 21, 0, MF1_MAGIC_GEN2_WRITE_DENIED, 0);
    fault = 2;
    run(request, 21, 0, MF1_MAGIC_GEN2_INCONCLUSIVE, 0);
    fault = 3;
    run(request, 21, 0, MF1_MAGIC_GEN2_INCONCLUSIVE, 0);
    fault = 0;
    request[15] = 0;
    run(request, 21, 0, MF1_MAGIC_GEN2_AUTH_DENIED, 0);
    request[14] = 0;
    for (replace_at = 1; replace_at < 10; replace_at++) {
        run(request, 15, 0, 0, STATUS_HF_TAG_NO);
    }
    replace_at = 0;
    for (unsigned size = 0; size < 15; size++) {
        run(request, size, 0, 0, STATUS_PAR_ERR);
    }
    request[5] = 1;
    run(request, 15, 0, 0, STATUS_PAR_ERR);
    request[5] = 0;
    request[13] = 0;
    run(request, 15, 0, 0, STATUS_PAR_ERR);
    request[13] = selected.sak = 0x28;
    run(request, 15, 0, 0, 0);
    const uint8_t unfused[] = {0xaa, 0x55, 0xc3, 0x96};
    memcpy(request + 1, unfused, 4);
    memcpy(selected.uid, unfused, 4);
    run(request, 15, MF1_MAGIC_UNFUSED, 0, 0);
    const uint8_t identities[][3] = {{7, 2, 0x44}, {10, 3, 0x84}};
    request[14] = 1;
    memset(request + 15, 0xff, 6);
    write_reply = 0x0a;
    for (unsigned i = 0; i < sizeof(identities) / sizeof(*identities); i++) {
        selected.uid_len = identities[i][0];
        request[0] = selected.uid_len;
        selected.cascade = identities[i][1];
        selected.atqa[0] = identities[i][2];
        request[11] = selected.atqa[0];
        memset(request + 1, 0, 10);
        for (unsigned byte = 0; byte < selected.uid_len; byte++) {
            selected.uid[byte] = byte + 1;
            request[byte + 1] = selected.uid[byte];
        }
        run(request, 21, MF1_MAGIC_GEN2, MF1_MAGIC_GEN2_WRITE_ACK, 0);
    }
    inspect();
    return 0;
}
