#include "mf1_emulator_crypto.h"
#include "parity.h"

/* Crypto1's two four-input Boolean functions: bit n is the output for input n.
 * The five nibbles of odd use A, B, A, A, B, producing bits 4 through 0 of
 * the final five-input combiner's index (see filter() in mf1_crapto1.h). */
#define CRYPTO1_FILTER_A_TRUTH_TABLE 0xf22cu
#define CRYPTO1_FILTER_B_TRUTH_TABLE 0xd938u
#define CRYPTO1_FILTER_COMBINER_TRUTH_TABLE 0xec57e80au

/* Use spare retained RAM without moving the existing authentication logs. */
#define MF1_TABLE_STORAGE __attribute__((section(".mf1_crypto_tables")))
#define MF1_TABLES_PREPARED 0x4d463143u
static uint32_t table_marker MF1_TABLE_STORAGE;

enum {
    CRYPTO1_FILTER_NIBBLE_BITS = 4,
    CRYPTO1_FILTER_NIBBLE_MASK = (1u << CRYPTO1_FILTER_NIBBLE_BITS) - 1,
    CRYPTO1_FILTER_LOW_OUTCOMES = 4,
    CRYPTO1_FILTER_LOW_OUTCOME_MASK = (1u << CRYPTO1_FILTER_LOW_OUTCOMES) - 1,
    CRYPTO1_LFSR_HALF_BYTES = 3,
    CRYPTO1_LFSR_BYTE_LANES = 2 * CRYPTO1_LFSR_HALF_BYTES,
    CRYPTO1_BYTE_VALUES = 1u << 8,
    CRYPTO1_FEEDBACK_CLOCKS = 32,
};

uint8_t mf1_filter_low[MF1_FILTER_LOW_SIZE] MF1_TABLE_STORAGE;
uint8_t mf1_filter_high[MF1_FILTER_HIGH_SIZE] MF1_TABLE_STORAGE;
static bool prepared;
/* Linear zero-input feedback over 32 clocks.
 * Six byte lanes cover the effective 48-bit LFSR. */
static uint32_t feedback32[CRYPTO1_LFSR_BYTE_LANES][CRYPTO1_BYTE_VALUES] MF1_TABLE_STORAGE;
static uint32_t input_response;
static inline uint32_t feedback(uint32_t odd, uint32_t even);

void mf1_emulator_crypto_prepare(void) {
    // Cold startup clears retained RAM after the first slot load.
    if (table_marker != MF1_TABLES_PREPARED) {
        prepared = false;
    }
    if (prepared) {
        return;
    }
    /* Shift each truth table to its output bit before selecting the input.
     * High inputs select one of four outcomes stored for each low input. */
    for (unsigned i = 0; i < MF1_FILTER_HIGH_SIZE; ++i) {
        mf1_filter_high[i] = (((CRYPTO1_FILTER_A_TRUTH_TABLE << 1) >>
                              (i & CRYPTO1_FILTER_NIBBLE_MASK)) & 2) |
                             ((CRYPTO1_FILTER_B_TRUTH_TABLE >>
                               (i >> CRYPTO1_FILTER_NIBBLE_BITS)) & 1);
    }
    for (unsigned i = 0; i < MF1_FILTER_LOW_SIZE; ++i) {
        unsigned low = (((CRYPTO1_FILTER_A_TRUTH_TABLE << 4) >>
                         (i & CRYPTO1_FILTER_NIBBLE_MASK)) & 16) |
                       (((CRYPTO1_FILTER_B_TRUTH_TABLE << 3) >>
                         ((i >> CRYPTO1_FILTER_NIBBLE_BITS) & CRYPTO1_FILTER_NIBBLE_MASK)) & 8) |
                       (((CRYPTO1_FILTER_A_TRUTH_TABLE << 2) >>
                         (i >> (2 * CRYPTO1_FILTER_NIBBLE_BITS))) & 4);
        mf1_filter_low[i] = (CRYPTO1_FILTER_COMBINER_TRUTH_TABLE >> low) &
                            CRYPTO1_FILTER_LOW_OUTCOME_MASK;
    }
    for (unsigned lane = 0; lane < CRYPTO1_LFSR_BYTE_LANES; ++lane) {
        feedback32[lane][0] = 0;
        /* Zero-input LFSR feedback is linear. Clock only the eight basis
         * vectors, then compose every byte from their XORs. Each expansion
         * reads the already completed lower half of this lane. */
        for (unsigned value = 1; value < CRYPTO1_BYTE_VALUES; value <<= 1) {
            uint32_t odd = lane < CRYPTO1_LFSR_HALF_BYTES ? value << (8 * lane) : 0;
            uint32_t even = lane >= CRYPTO1_LFSR_HALF_BYTES ?
                            value << (8 * (lane - CRYPTO1_LFSR_HALF_BYTES)) : 0;
            uint32_t bits = 0;
            for (unsigned bit = 0; bit < CRYPTO1_FEEDBACK_CLOCKS; ++bit) {
                uint32_t f = feedback(odd, even);
                bits |= f << bit;
                uint32_t next = (even << 1) | f;
                even = odd;
                odd = next;
            }
            for (unsigned lower = 0; lower < value; ++lower) {
                feedback32[lane][value + lower] = bits ^ feedback32[lane][lower];
            }
        }
    }
    /* Bit zero is the injected bit; subsequent clocks are its linear feedback. */
    input_response = 1u | (feedback32[0][1] << 1);
    table_marker = MF1_TABLES_PREPARED;
    prepared = true;
}

static inline uint32_t feedback(uint32_t odd, uint32_t even) {
    uint32_t v = (odd & LF_POLY_ODD) ^ (even & LF_POLY_EVEN);
    v ^= v >> 16;
    v ^= v >> 8;
    return evenparity8((uint8_t)v);
}

uint8_t mf1_emulator_crypto_bit(struct Crypto1State *s, uint8_t in, int encrypted) {
    uint8_t key = mf1_emulator_filter(s->odd);
    uint32_t next = (s->even << 1) |
        (feedback(s->odd, s->even) ^ !!in ^ (key & !!encrypted));
    s->even = s->odd;
    s->odd = next;
    return key;
}

static inline uint32_t predicted_feedback(uint32_t odd, uint32_t even) {
    return feedback32[0][odd & UINT8_MAX] ^ feedback32[1][(odd >> 8) & UINT8_MAX] ^
           feedback32[2][(odd >> 16) & UINT8_MAX] ^ feedback32[3][even & UINT8_MAX] ^
           feedback32[4][(even >> 8) & UINT8_MAX] ^ feedback32[5][(even >> 16) & UINT8_MAX];
}

/* Predict zero-input feedback once, then superpose each injected bit's
 * response. Two clocks restore the odd/even orientation without swapping.
 * Constant call arguments remove unused stream/parity work, notably for Nr. */
static inline __attribute__((always_inline)) uint32_t clock_bits(
        struct Crypto1State *state, uint32_t input, bool encrypted,
        bool emit_stream, uint8_t *parity, unsigned bits) {
    uint32_t odd = state->odd, even = state->even;
    uint32_t f = predicted_feedback(odd, even), stream = 0;
    for (unsigned bit = 0; bit < bits; bit += 2) {
        uint32_t key = mf1_emulator_filter(odd);
        uint32_t injection = (input & 1) ^ (encrypted ? key : 0);
        f ^= (0u - injection) & input_response;
        if (emit_stream) {
            stream |= key << bit;
        }
        even = (even << 1) | (f & 1);
        f >>= 1;
        input >>= 1;
        key = mf1_emulator_filter(even);
        injection = (input & 1) ^ (encrypted ? key : 0);
        f ^= (0u - injection) & input_response;
        if (emit_stream) {
            stream |= key << (bit + 1);
        }
        odd = (odd << 1) | (f & 1);
        f >>= 1;
        input >>= 1;
        if (parity != NULL && (bit & 7) == 6) {
            parity[bit >> 3] = mf1_emulator_filter(odd);
        }
    }
    state->odd = odd;
    state->even = even;
    return stream;
}

uint8_t mf1_emulator_crypto_byte(struct Crypto1State *s, uint8_t in, int encrypted) {
    if (in == 0 && !encrypted) {
        return clock_bits(s, 0, false, true, NULL, 8);
    }
    return clock_bits(s, in, encrypted, true, NULL, 8);
}

uint32_t mf1_emulator_crypto_word(struct Crypto1State *s, uint32_t in, int encrypted) {
    uint32_t result = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        result |= (uint32_t)mf1_emulator_crypto_byte(s, in >> shift, encrypted) << shift;
    }
    return result;
}

void mf1_emulator_prepare_read(const struct Crypto1State *state, mf1_read_stream_t *cache) {
    struct Crypto1State next = *state;
    for (unsigned i = 0; i < 4; ++i) {
        cache->command[i] = mf1_emulator_crypto_byte(&next, 0, 0);
    }
    cache->after_command = next;
    for (unsigned i = 0; i < 18; ++i) {
        cache->response[i] = mf1_emulator_crypto_byte(&next, 0, 0);
        cache->response_filter[i] = mf1_emulator_filter(next.odd);
    }
    cache->after_read = next;
    cache->valid = true;
}

static inline uint32_t load_wire_word(const uint8_t bytes[4]) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool mf1_emulator_authenticate(struct Crypto1State *state, const uint8_t reader[8],
                               const uint8_t expected[4], const uint8_t answer[4],
                               uint8_t encrypted_answer[4], uint8_t parity[4]) {
    (void)clock_bits(state, load_wire_word(reader), true, false, NULL, 32);
    uint32_t ar = clock_bits(state, 0, false, true, NULL, 32);
    if ((load_wire_word(reader + 4) ^ ar) != load_wire_word(expected)) {
        return false;
    }
    uint32_t at = clock_bits(state, 0, false, true, parity, 32);
    for (unsigned i = 0; i < 4; ++i) {
        encrypted_answer[i] = answer[i] ^ (at >> (8 * i));
        parity[i] ^= oddparity8(answer[i]);
    }
    return true;
}
