#include <assert.h>
#include <string.h>

int main(void) {
    mf1_emulator_crypto_prepare();
    /* Independent bit-clock oracle for all 1536 feedback entries, including
     * every combination of basis bits. Do not reuse the table builder. */
    for (unsigned lane = 0; lane < 6; ++lane) {
        for (unsigned value = 0; value < 256; ++value) {
            uint32_t odd = lane < 3 ? value << (8 * lane) : 0;
            uint32_t even = lane >= 3 ? value << (8 * (lane - 3)) : 0;
            uint32_t expected = 0;
            for (unsigned bit = 0; bit < 32; ++bit) {
                uint32_t f = __builtin_parity((odd & LF_POLY_ODD) ^ (even & LF_POLY_EVEN));
                expected |= f << bit;
                uint32_t next = (even << 1) | f;
                even = odd;
                odd = next;
            }
            assert(feedback32[lane][value] == expected);
        }
    }
    uint8_t low[sizeof(mf1_filter_low)], high[sizeof(mf1_filter_high)];
    uint32_t expected_feedback[CRYPTO1_LFSR_BYTE_LANES][CRYPTO1_BYTE_VALUES];
    memcpy(low, mf1_filter_low, sizeof(low));
    memcpy(high, mf1_filter_high, sizeof(high));
    memcpy(expected_feedback, feedback32, sizeof(expected_feedback));
    for (unsigned reset = 0; reset < 2; reset++) {
        memset(mf1_filter_low, 0xff, sizeof(mf1_filter_low));
        memset(mf1_filter_high, 0xff, sizeof(mf1_filter_high));
        memset(feedback32, 0xff, sizeof(feedback32));
        if (reset == 0) {
            // Startup clears retained RAM after slot loading, before sensing.
            table_marker = UINT32_MAX;
        } else {
            // A warm restart clears BSS even if a retained marker looks valid.
            prepared = false;
        }
        mf1_emulator_crypto_prepare();
        assert(memcmp(low, mf1_filter_low, sizeof(low)) == 0);
        assert(memcmp(high, mf1_filter_high, sizeof(high)) == 0);
        assert(memcmp(expected_feedback, feedback32, sizeof(expected_feedback)) == 0);
    }
}
