#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mf1_emulator_crypto.h"
#include "crypto1_helper.h"
#include "nfc_14a_frame_codec.h"

/* Independent oracle: the original bit-clock cipher and wire parity rules. */
static void reference_encrypt_ex(struct Crypto1State *state, uint8_t *plain,
                                 uint8_t *input, uint8_t *out, uint16_t len,
                                 uint8_t *parity) {
    for (unsigned i = 0; i < len; i++) {
        uint8_t byte = plain[i];
        out[i] = crypto1_byte(state, input ? input[i] : 0, 0) ^ byte;
        parity[i] = filter(state->odd) ^ oddparity8(byte);
    }
}

static void reference_encrypt(struct Crypto1State *state, uint8_t *data,
                              uint16_t len, uint8_t *parity) {
    reference_encrypt_ex(state, data, NULL, data, len, parity);
}

static uint8_t reference_nibble(struct Crypto1State *state, uint8_t in) {
    uint8_t out = 0;
    for (unsigned bit = 0; bit < 4; bit++) {
        out |= (crypto1_bit(state, 0, 0) ^ ((in >> bit) & 1)) << bit;
    }
    return out;
}

static void reference_decrypt(struct Crypto1State *state, uint8_t *data, int len) {
    if (len == 1) {
        data[0] = reference_nibble(state, data[0]);
    } else {
        for (int i = 0; i < len; i++) {
            data[i] ^= crypto1_byte(state, 0, 0);
        }
    }
}

static uint32_t random_state = 0x12345678;

static uint32_t random32(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void same(struct Crypto1State a, struct Crypto1State b) {
    assert(a.odd == b.odd && a.even == b.even);
}

static void test_frame_codec(void) {
    /* An independent, bit-by-bit wire representation covers all offsets,
     * trailing partial frame bytes and aliasing of the receive buffer. */
    for (unsigned length = 1; length <= 128; ++length) {
        uint8_t data[128], parity[128], frame[144], expected[144] = {0};
        uint8_t decoded[128], decoded_parity[128], aliased[144];
        for (unsigned i = 0; i < length; ++i) {
            data[i] = random32(); parity[i] = random32() & 1;
            for (unsigned bit = 0; bit < 9; ++bit) {
                unsigned position = 9 * i + bit;
                unsigned value = bit == 8 ? parity[i] : ((data[i] >> bit) & 1);
                expected[position / 8] |= value << (position % 8);
            }
        }
        memset(frame, 0xa5, sizeof(frame));
        nfc_14a_pack_bytes(data, parity, length, frame);
        unsigned packed_length = (9 * length + 7) / 8;
        assert(memcmp(frame, expected, packed_length) == 0);
        if (packed_length < sizeof(frame)) assert(frame[packed_length] == 0xa5);
        nfc_14a_unpack_bytes(frame, length, decoded, decoded_parity);
        assert(memcmp(data, decoded, length) == 0);
        assert(memcmp(parity, decoded_parity, length) == 0);
        memcpy(aliased, frame, sizeof(frame));
        nfc_14a_unpack_bytes(aliased, length, aliased, NULL);
        assert(memcmp(data, aliased, length) == 0);
    }
}

int main(void) {
    test_frame_codec();
    mf1_emulator_crypto_prepare();
    mf1_emulator_crypto_prepare(); /* idempotent */
    for (uint32_t i = 0; i < (1u << 20); ++i)
        assert(mf1_emulator_filter(i) == filter(i));
    for (unsigned i = 0; i < 20000; ++i) {
        uint64_t key = ((uint64_t)random32() << 32) | random32();
        struct Crypto1State a, b;
        crypto1_init(&a, key);
        b = a;
        for (unsigned mode = 0; mode < 3; ++mode) {
            uint32_t input = mode == 0 ? 0 : random32();
            int encrypted = mode == 2;
            assert(crypto1_bit(&a, input & 1, encrypted) ==
                   mf1_emulator_crypto_bit(&b, input & 1, encrypted));
            same(a, b);
            assert(crypto1_byte(&a, input, encrypted) ==
                   mf1_emulator_crypto_byte(&b, input, encrypted));
            same(a, b);
            assert(crypto1_word(&a, input, encrypted) ==
                   mf1_emulator_crypto_word(&b, input, encrypted));
            same(a, b);
        }
        uint8_t plain[18], input[18], x[18], y[18], px[18], py[18];
        for (unsigned j = 0; j < 18; ++j) {
            plain[j] = random32();
            input[j] = random32();
        }
        /* A speculative READ must not advance the live cipher. Its command
         * endpoint must also serve WRITE/unknown commands and CRC rejection. */
        mf1_read_stream_t cache = {0};
        struct Crypto1State live = b, reference = a;
        mf1_emulator_prepare_read(&live, &cache);
        assert(cache.valid); same(live, b);
        for (unsigned j = 0; j < 4; ++j)
            assert(cache.command[j] == crypto1_byte(&reference, 0, 0));
        same(cache.after_command, reference);
        struct Crypto1State non_read = cache.after_command;
        assert(reference_nibble(&reference, 5) == mf_crypto1_encrypt4bit(&non_read, 5));
        same(non_read, reference);
        reference = cache.after_command;
        for (unsigned j = 0; j < 18; ++j) {
            assert(cache.response[j] == crypto1_byte(&reference, 0, 0));
            assert(cache.response_filter[j] == filter(reference.odd));
        }
        same(cache.after_read, reference);
        /* Fused authentication: arbitrary encrypted Nr, valid/invalid Ar,
         * exact encrypted At/parity and cipher endpoint. */
        uint8_t reader[8], expected[4], answer[4], at[4], at_ref[4], at_parity[4], at_ref_parity[4];
        reference = a;
        for (unsigned j = 0; j < 4; ++j) {
            reader[j] = random32(); expected[j] = random32(); answer[j] = random32();
            (void)crypto1_byte(&reference, reader[j], 1);
        }
        for (unsigned j = 0; j < 4; ++j)
            reader[j + 4] = expected[j] ^ crypto1_byte(&reference, 0, 0);
        struct Crypto1State after_ar = reference, fused = a;
        memcpy(at_ref, answer, 4);
        reference_encrypt(&reference, at_ref, 4, at_ref_parity);
        assert(mf1_emulator_authenticate(&fused, reader, expected, answer, at, at_parity));
        same(fused, reference);
        assert(memcmp(at, at_ref, 4) == 0 && memcmp(at_parity, at_ref_parity, 4) == 0);
        /* Every Ar bit must be checked, including the first three bytes. */
        for (unsigned bit = 0; bit < 32; bit++) {
            fused = a;
            reader[4 + bit / 8] ^= 1u << (bit & 7);
            memset(at, 0x5a, sizeof(at));
            assert(!mf1_emulator_authenticate(&fused, reader, expected, answer, at, at_parity));
            same(fused, after_ar);
            for (unsigned j = 0; j < 4; j++) {
                assert(at[j] == 0x5a);
            }
            reader[4 + bit / 8] ^= 1u << (bit & 7);
        }
        /* Nested nonce: nonzero cipher input, encrypted parity, then READ. */
        reference_encrypt_ex(&a, plain, input, x, 4, px);
        mf_crypto1_encryptEx(&b, plain, input, y, 4, py);
        assert(memcmp(x, y, 4) == 0 && memcmp(px, py, 4) == 0);
        same(a, b);
        memcpy(x, plain, 18); memcpy(y, plain, 18);
        reference_encrypt(&a, x, 18, px);
        mf_crypto1_encrypt(&b, y, 18, py);
        assert(memcmp(x, y, 18) == 0 && memcmp(px, py, 18) == 0);
        same(a, b);
        reference_decrypt(&a, x, 18);
        mf_crypto1_decrypt(&b, y, 18);
        assert(memcmp(x, y, 18) == 0); same(a, b);
        assert(reference_nibble(&a, i & 15) == mf_crypto1_encrypt4bit(&b, i & 15));
        same(a, b);
        x[0] = y[0] = i & 15;
        reference_decrypt(&a, x, 1);
        mf_crypto1_decrypt(&b, y, 1);
        assert(x[0] == y[0]); same(a, b);
    }
    puts("PASS: exhaustive filter, 20,000 cipher sequences, speculative READ and fused authentication");
}
