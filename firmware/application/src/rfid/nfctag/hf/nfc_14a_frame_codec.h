#ifndef NFC_14A_FRAME_CODEC_H
#define NFC_14A_FRAME_CODEC_H
#include <stdint.h>
#include <stddef.h>

/* Full bytes with explicit (possibly encrypted) parity, LSB first on air.
 * Partial anticollision frames keep using the existing bit-frame codec. */
static inline void nfc_14a_pack_bytes(const uint8_t *data, const uint8_t *parity,
                                    size_t bytes, uint8_t *frame) {
    uint32_t pending = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < bytes; ++i) {
        pending |= ((uint32_t)data[i] | ((uint32_t)(parity[i] & 1) << 8)) << bits;
        bits += 9;
        *frame++ = pending;
        pending >>= 8;
        bits -= 8;
        if (bits == 8) {
            *frame++ = pending;
            pending = 0;
            bits = 0;
        }
    }
    if (bits) *frame = pending;
}

/* Also supports in-place deparity: each output byte overwrites only input
 * already consumed, and both source bytes are fetched before that write. */
static inline void nfc_14a_unpack_bytes(const uint8_t *frame, size_t bytes,
                                      uint8_t *data, uint8_t *parity) {
    for (size_t i = 0; i < bytes; ++i) {
        size_t position = i + (i >> 3);
        unsigned shift = i & 7;
        uint8_t low = frame[position], high = frame[position + 1];
        data[i] = (low >> shift) | (high << (8 - shift));
        if (parity) parity[i] = (high >> shift) & 1;
    }
}
#endif
