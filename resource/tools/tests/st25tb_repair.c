/* Exercise the real candidate repair and its shared-budget failure boundary. */
#include "st25tb/receiver.c"
#include <assert.h>
#include <stdio.h>

static unsigned cases;

static void expect(const receiver_t *input, const uint8_t *wanted, unsigned cost) {
    for (unsigned budget = 0; budget <= STB_RX_CRC_MAX_CHECKS; budget++) {
        receiver_t edge = *input;
        unsigned remaining = budget;
        bool accepted = check_candidate(&edge, &remaining);
        assert(accepted == (budget >= cost));
        if (accepted) {
            assert(memcmp(edge.data, wanted, edge.bytes) == 0);
            assert(remaining == budget - cost);
            memcpy(edge.data, input->data, sizeof(edge.data));
        } else {
            assert(remaining == 0);
        }
        assert(memcmp(&edge, input, sizeof(edge)) == 0);
        cases++;
    }
}

static void masks(void) {
    for (unsigned size = STB_RX_MIN_FRAME_BYTES; size <= ST25TB_REQUEST_MAX; size++) {
        for (unsigned seed = 0; seed < 3; seed++) {
            receiver_t original = {0};
            original.bytes = size;
            for (unsigned i = 0; i < size - ISO15693_CRC_SIZE; i++) {
                original.data[i] = seed == 0 ? 0 : 0xffu - (seed - 1) * (31 * i + size);
            }
            iso15693_append_crc(original.data, size - ISO15693_CRC_SIZE);
            expect(&original, original.data, 1);
            for (unsigned index = 0; index < size; index++) {
                for (unsigned bits = 1; bits <= STB_RX_RUN_MAX_BITS - 2; bits++) {
                    receiver_t single = original;
                    single.data[index] ^= (1u << bits) - 1;
                    single.start_gaps[index] = bits + 2;
                    expect(&single, original.data, 1 + bits);
                    for (unsigned earlier = 0; earlier < index; earlier++) {
                        for (unsigned other_bits = 1; other_bits <= STB_RX_RUN_MAX_BITS - 2; other_bits++) {
                            receiver_t pair = single;
                            pair.data[earlier] ^= (1u << other_bits) - 1;
                            pair.start_gaps[earlier] = other_bits + 2;
                            expect(&pair, original.data, 1 + bits + other_bits);
                        }
                    }
                }
            }
        }
    }
}

static void ambiguous_order(void) {
    receiver_t edge = {0};
    edge.bytes = ST25TB_REQUEST_MAX;
    iso15693_append_crc(edge.data, edge.bytes - ISO15693_CRC_SIZE);
    edge.data[6] ^= 0x14;
    edge.data[7] ^= 0x04;
    edge.start_gaps[0] = 3;
    edge.start_gaps[1] = 4;
    edge.start_gaps[4] = 3;
    edge.start_gaps[6] = 8;
    assert(crc_error(&edge) == 0x0414);

    /* Both repairs pass CRC. The earlier wire-byte hypothesis must win. */
    uint8_t first[ST25TB_REQUEST_MAX];
    memcpy(first, edge.data, sizeof(first));
    first[1] ^= 3;
    first[4] ^= 1;
    assert(iso15693_check_crc(first, sizeof(first)));
    expect(&edge, first, 5);

    /* An ineligible earlier choice still consumes its logical budget places. */
    edge.start_gaps[4] = 0;
    uint8_t second[ST25TB_REQUEST_MAX];
    memcpy(second, edge.data, sizeof(second));
    second[0] ^= 1;
    second[6] ^= 63;
    assert(iso15693_check_crc(second, sizeof(second)));
    expect(&edge, second, 10);
}

static void unrepaired(void) {
    receiver_t edge = {0};
    edge.bytes = ST25TB_REQUEST_MAX;
    iso15693_append_crc(edge.data, edge.bytes - ISO15693_CRC_SIZE);
    edge.data[7] ^= 0x80;
    for (unsigned gap = 0; gap <= STB_RX_RUN_MAX_BITS; gap++) {
        memset(edge.start_gaps, gap, sizeof(edge.start_gaps));
        for (unsigned budget = 0; budget <= STB_RX_CRC_MAX_CHECKS; budget++) {
            receiver_t actual = edge;
            unsigned remaining = budget;
            assert(!check_candidate(&actual, &remaining));
            unsigned cost = 1 + edge.bytes * (gap > 2 ? gap - 2 : 0);
            assert(remaining == (budget > cost ? budget - cost : 0));
            assert(memcmp(&actual, &edge, sizeof(edge)) == 0);
            cases++;
        }
    }
}

int main(void) {
    masks();
    ambiguous_order();
    unrepaired();
    printf("ST25TB repair: %u ordered repair/budget cases passed\n", cases);
}
