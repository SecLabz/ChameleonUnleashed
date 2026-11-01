#ifndef MF1_EMULATOR_CRYPTO_H
#define MF1_EMULATOR_CRYPTO_H

#include "mf1_crapto1.h"

enum {
    MF1_FILTER_LOW_BITS = 12,
    MF1_FILTER_HIGH_BITS = 8,
    MF1_FILTER_LOW_SIZE = 1u << MF1_FILTER_LOW_BITS,
    MF1_FILTER_HIGH_SIZE = 1u << MF1_FILTER_HIGH_BITS,
};

/* Compact Crypto1 filter decomposition: 4 KiB of low-bit outcomes and
 * 256 high-bit selectors. Prepare before enabling NFCT, never in its IRQ.
 * Separate symbols keep the reader/recovery cipher and its state untouched. */
extern uint8_t mf1_filter_low[MF1_FILTER_LOW_SIZE];
extern uint8_t mf1_filter_high[MF1_FILTER_HIGH_SIZE];
void mf1_emulator_crypto_prepare(void);
uint8_t mf1_emulator_crypto_bit(struct Crypto1State *s, uint8_t in, int encrypted);
uint8_t mf1_emulator_crypto_byte(struct Crypto1State *s, uint8_t in, int encrypted);
uint32_t mf1_emulator_crypto_word(struct Crypto1State *s, uint32_t in, int encrypted);

/* Speculative stream from a copy of the live cipher. Commit after_command for
 * every four-byte command; after_read only for an accepted READ response. */
typedef struct {
    bool valid;
    uint8_t command[4], response[18], response_filter[18];
    struct Crypto1State after_command, after_read;
} mf1_read_stream_t;
void mf1_emulator_prepare_read(const struct Crypto1State *state, mf1_read_stream_t *cache);
bool mf1_emulator_authenticate(struct Crypto1State *state, const uint8_t reader[8],
                              const uint8_t expected[4], const uint8_t answer[4],
                              uint8_t encrypted_answer[4], uint8_t parity[4]);

static inline int mf1_emulator_filter(uint32_t odd) {
    return (mf1_filter_low[odd & (MF1_FILTER_LOW_SIZE - 1)] >>
            mf1_filter_high[(odd >> MF1_FILTER_LOW_BITS) & (MF1_FILTER_HIGH_SIZE - 1)]) & 1;
}

#endif
