#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "crc_utils.h"
#include "ulc_3des.h"

#define PCD_TRANSCEIVE 12
#define U8ARR_BIT_LEN(a) (sizeof(a) * 8)
#define MIFARE_ULC_AUTH_1 0x1a
#define MIFARE_ULC_AUTH_2 0xaf
static uint8_t key[16];
static uint8_t iv[8];
static uint8_t nonce[8] = {8, 7, 6, 5, 4, 3, 2, 1};
static unsigned exchanges;
static unsigned fault;
static unsigned rng_offset;

// MF0ICU2 rev. 3.5, section 7.5.6, Table 9 (NXP numerical example).
// https://www.nxp.com/docs/en/data-sheet/MF0ICU2.pdf
static const uint8_t nxp_key[16] = {
    0x49,0x45,0x4d,0x4b,0x41,0x45,0x52,0x42,
    0x21,0x4e,0x41,0x43,0x55,0x4f,0x59,0x46,
};
static const uint8_t nxp_rnda[8] = {0xa8,0xaf,0x3b,0x25,0x6c,0x75,0xed,0x40};
static const uint8_t nxp_challenge[8] = {0x57,0x72,0x93,0xfd,0x2f,0x34,0xca,0x51};
static const uint8_t nxp_token[16] = {
    0x0a,0x63,0x85,0x59,0xfc,0x77,0x37,0xf9,
    0xf1,0x5d,0x78,0x62,0xeb,0xbe,0x96,0x7a,
};
static const uint8_t nxp_response[8] = {0x3b,0x88,0x4f,0xa0,0x7c,0x13,0x7c,0xe1};

static void crc_14a_calculate(uint8_t *bytes, size_t size, uint8_t *crc) {
    calc_14a_crc_lut(bytes, (int)size, crc);
}

static void crc_14a_append(uint8_t *bytes, size_t size) {
    crc_14a_calculate(bytes, size, bytes + size);
}

#define NRF_SUCCESS 0

static void nrf_drv_rng_bytes_available(uint8_t *available) {
    *available = fault == 7 ? 0 : 4;
}

static uint8_t nrf_drv_rng_rand(uint8_t *bytes, uint8_t size) {
    assert(size <= 4);
    if (fault == 9) {
        return 1;
    }
    for (unsigned i = 0; i < size; i++) {
        bytes[i] = fault == 8 ? nxp_rnda[rng_offset++] : 0x10 + i;
    }
    return NRF_SUCCESS;
}

static uint8_t pcd_14a_reader_bytes_transfer(uint8_t operation, uint8_t *sent,
    uint16_t size, uint8_t *out, uint16_t *bits, uint16_t capacity) {
    assert(operation == PCD_TRANSCEIVE && capacity == 24 * 8);
    uint8_t crc[2];
    crc_14a_calculate(sent, size - 2, crc);
    assert(!memcmp(crc, sent + size - 2, 2));
    exchanges++;
    if (fault == 8) {
        if (exchanges == 1) {
            assert(size == 4 && sent[0] == 0x1a && sent[1] == 0);
            out[0] = 0xaf;
            memcpy(out + 1, nxp_challenge, 8);
        } else {
            assert(exchanges == 2 && size == 19 && sent[0] == 0xaf);
            assert(!memcmp(sent + 1, nxp_token, 16));
            out[0] = 0;
            memcpy(out + 1, nxp_response, 8);
        }
        crc_14a_append(out, 9);
        *bits = 88;
        return STATUS_HF_TAG_OK;
    }
    if (exchanges == 1) {
        assert(size == 4 && sent[0] == 0x1a && sent[1] == 0);
        memset(iv, 0, 8);
        out[0] = 0xaf;
        ulc_tdes_enc_cbc(out + 1, nonce, 8, key, iv);
    } else {
        assert(exchanges == 2 && size == 19 && sent[0] == 0xaf);
        if (fault == 6) {
            return STATUS_HF_TAG_NO;
        }
        uint8_t plain[16];
        ulc_tdes_dec_cbc(plain, sent + 1, 16, key, iv);
        uint8_t rotated[8];
        memcpy(rotated, nonce, 8);
        ulc_rol8(rotated);
        if (memcmp(plain + 8, rotated, 8)) {
            return STATUS_MF_ERR_AUTH;
        }
        ulc_rol8(plain);
        if (fault == 5) {
            plain[0] ^= 1;
        }
        out[0] = fault == 4 ? 0xaf : 0;
        ulc_tdes_enc_cbc(out + 1, plain, 8, key, iv);
    }
    crc_14a_append(out, 9);
    *bits = 88;
    if ((fault == 1 && exchanges == 1) || (fault == 2 && exchanges == 2)) {
        out[10] ^= 1;
    }
    if (fault == 3 && exchanges == 1) {
        *bits = 87;
    }
    return STATUS_HF_TAG_OK;
}

/* PRODUCTION AUTHENTICATION */

int main(void) {
    uint8_t read[] = {0x30, 4};
    _Alignas(uint16_t) uint8_t unaligned[3];
    crc_14a_calculate(read, sizeof(read), unaligned + 1);
    assert(unaligned[1] == 0x26 && unaligned[2] == 0xee);
    ulc_3des_init();
    for (unsigned i = 0; i < 16; i++) {
        key[i] = i;
    }
    for (fault = 0; fault <= 7; fault++) {
        exchanges = 0;
        uint8_t result = pcd_14a_reader_mf0_ulc_auth(key);
        assert((result == STATUS_HF_TAG_OK) == (fault == 0));
        if (fault == 1 || fault == 3 || fault == 7) {
            assert(exchanges == 1);
        }
    }
    fault = 9;
    exchanges = 0;
    assert(pcd_14a_reader_mf0_ulc_auth(key) != STATUS_HF_TAG_OK && exchanges == 1);
    fault = 8;
    exchanges = 0;
    rng_offset = 0;
    uint8_t example_key[16];
    memcpy(example_key, nxp_key, 16);
    assert(pcd_14a_reader_mf0_ulc_auth(example_key) == STATUS_HF_TAG_OK && exchanges == 2);
    assert(rng_offset == 8);
    fault = 0;
    exchanges = 0;
    uint8_t wrong_key[16] = {0};
    assert(pcd_14a_reader_mf0_ulc_auth(wrong_key) != STATUS_HF_TAG_OK);
}
