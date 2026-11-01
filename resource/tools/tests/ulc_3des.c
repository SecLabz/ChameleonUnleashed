#include "ulc_3des.h"
#include <assert.h>
#include <string.h>

static void check(const uint8_t key[16], const uint8_t initial_iv[8],
                  const uint8_t *plain, const uint8_t *cipher, size_t length) {
    uint8_t out[32], iv[8];
    memcpy(iv, initial_iv, 8);
    ulc_tdes_enc_cbc(out, plain, length, key, iv);
    assert(!memcmp(out, cipher, length));
    assert(!memcmp(iv, cipher + length - 8, 8));
    memcpy(iv, initial_iv, 8);
    ulc_tdes_dec_cbc(out, cipher, length, key, iv);
    assert(!memcmp(out, plain, length));
    assert(!memcmp(iv, cipher + length - 8, 8));

    memcpy(out, plain, length);
    memcpy(iv, initial_iv, 8);
    ulc_tdes_enc_cbc(out, out, length, key, iv);
    assert(!memcmp(out, cipher, length));
    memcpy(iv, initial_iv, 8);
    ulc_tdes_dec_cbc(out, out, length, key, iv);
    assert(!memcmp(out, plain, length));

    memcpy(iv, initial_iv, 8);
    ulc_tdes_enc_cbc(out, plain, 8, key, iv);
    ulc_tdes_enc_cbc(out + 8, plain + 8, length - 8, key, iv);
    assert(!memcmp(out, cipher, length));
    memcpy(iv, initial_iv, 8);
    ulc_tdes_dec_cbc(out, cipher, 8, key, iv);
    ulc_tdes_dec_cbc(out + 8, cipher + 8, length - 8, key, iv);
    assert(!memcmp(out, plain, length));
}

int main(void) {
    /* Independent OpenSSL DES-EDE/CBC outputs, without padding. */
    const uint8_t des_key[16] = {
        0x13,0x34,0x57,0x79,0x9b,0xbc,0xdf,0xf1,
        0x13,0x34,0x57,0x79,0x9b,0xbc,0xdf,0xf1,
    };
    const uint8_t zero_iv[8] = {0};
    const uint8_t des_plain[8] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
    const uint8_t des_cipher[8] = {0x85,0xe8,0x13,0x54,0x0f,0x0a,0xb4,0x05};
    check(des_key, zero_iv, des_plain, des_cipher, 8);
    ulc_3des_init();
    ulc_3des_init();
    check(des_key, zero_iv, des_plain, des_cipher, 8);

    const uint8_t key[16] = {
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
        0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
    };
    const uint8_t initial_iv[8] = {0x12,0x34,0x56,0x78,0x90,0xab,0xcd,0xef};
    uint8_t plain[32];
    for (unsigned i = 0; i < sizeof(plain); i++) plain[i] = i;
    const uint8_t cipher[32] = {
        0x18,0x85,0x08,0x51,0x4e,0xeb,0x60,0xd2,
        0x4f,0x8e,0x12,0x5f,0x9c,0x27,0x54,0xe9,
        0x77,0xbc,0xc7,0x9e,0x1c,0x71,0x91,0x1d,
        0xe0,0xf7,0xed,0x47,0xb0,0x84,0xf2,0x09,
    };
    check(key, initial_iv, plain, cipher, sizeof(plain));

    uint8_t out[32], iv[8];
    memset(out, 0xa5, sizeof(out));
    memcpy(iv, initial_iv, 8);
    ulc_tdes_enc_cbc(out, plain, 7, key, iv);
    ulc_tdes_dec_cbc(out, plain, 7, key, iv);
    ulc_tdes_enc_cbc(out, plain, 0, key, iv);
    ulc_tdes_dec_cbc(out, plain, 0, key, iv);
    for (unsigned i = 0; i < sizeof(out); i++) assert(out[i] == 0xa5);
    assert(!memcmp(iv, initial_iv, 8));
}
