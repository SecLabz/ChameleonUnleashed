/* Reconstruct NRZ runs from two 1 us ADC samples per call. */
#include "receiver.h"
#include "iso15693/frame.h"
#include <string.h>

/* Receiver times are ADC-pair ticks (2 us), not CPU cycles. Amplitudes
 * are signed I/Q nibble sums scaled before the integer baseline filter.
 * Keep the qualified thresholds and inclusive/strict comparisons unchanged. */
enum {
    STB_RX_SETTLE_TICKS = 10,
    STB_RX_IDLE_TICKS = 500,
    STB_RX_AXIS_SKEW_TICKS = 1,
    STB_RX_REPAIR_MAX_BYTES = 3,
    STB_RX_CRC_MAX_CHECKS = 16,
    STB_RX_GLITCH_MIN_TICKS = 3,
    STB_RX_SOF_LOW_MIN_TICKS = 43,
    STB_RX_SOF_LOW_MAX_TICKS = 52,
    STB_RX_SOF_HIGH_MIN_TICKS = 8,
    STB_RX_SOF_HIGH_MAX_TICKS = 16,
    STB_RX_FRAME_GAP_MAX_TICKS = 60,
    STB_RX_BITS_PER_CHARACTER = 10,
    STB_RX_CHARACTER_FRAME_MASK = 0x201,
    STB_RX_CHARACTER_STOP_BIT = 0x200,
    STB_RX_AFTER_START_MASK = (1u << STB_RX_BITS_PER_CHARACTER) - 2u,
    STB_RX_MIN_FRAME_BYTES = 3,
    STB_RX_RUN_MAX_BITS = 9,
    STB_RX_TICK_ETU_NUMERATOR = 339,
    STB_RX_TICK_ETU_DENOMINATOR = 1600,
    STB_RX_ETU_ROUNDING = 800,
    STB_RX_RUN_TOLERANCE = 678,
    STB_RX_EOF_MIN_TICKS = (STB_RX_BITS_PER_CHARACTER * STB_RX_TICK_ETU_DENOMINATOR - STB_RX_RUN_TOLERANCE +
                            STB_RX_TICK_ETU_NUMERATOR - 1) /
                           STB_RX_TICK_ETU_NUMERATOR,
    STB_RX_EOF_MAX_TICKS =
        (STB_RX_BITS_PER_CHARACTER * STB_RX_TICK_ETU_DENOMINATOR + STB_RX_RUN_TOLERANCE) / STB_RX_TICK_ETU_NUMERATOR,
    STB_RX_SAMPLE_SCALE = 128,
    STB_RX_BASELINE_DIVISOR = 64,
    STB_RX_EDGE_THRESHOLD = 768,
    STB_RX_RELEASE_THRESHOLD = 256,
};

typedef enum { SEARCH, FRAME } receive_state_t;

typedef struct {
    unsigned transition;
    int8_t previous;
    bool pending;
} sof_candidate_t;

typedef struct {
    unsigned transition;
    unsigned sof;
    sof_candidate_t candidate;
    int32_t bias;
    int pulse;
    int previous;
    int polarity;
    bool initialized;
    bool peer_seen;
    uint8_t start_gap;
    receive_state_t state;
    uint16_t character;
    uint8_t bits;
    uint8_t bytes;
    uint8_t data[ST25TB_REQUEST_MAX];
    uint8_t start_gaps[ST25TB_REQUEST_MAX];
} receiver_t;

static receiver_t receivers[2];
static unsigned sample;
static unsigned settling;

static void expire(receiver_t *edge, unsigned now) {
    unsigned gap = now - edge->transition;
    if (edge->candidate.pending && now - edge->candidate.transition > STB_RX_SOF_HIGH_MAX_TICKS) {
        edge->candidate.pending = false;
    }
    if (edge->state == FRAME && gap > STB_RX_FRAME_GAP_MAX_TICKS) {
        edge->state = SEARCH;
    }
}

void st25tb_receiver_reset(void) {
    memset(receivers, 0, sizeof(receivers));
    sample = 0;
    settling = 0;
}

void st25tb_receiver_settle(void) {
    /* 20 us, well inside the 14 ETU a Proxmark3 waits after our EOF. */
    settling = STB_RX_SETTLE_TICKS;
}

void st25tb_receiver_time(unsigned now) {
    sample = now;
}

bool st25tb_receiver_idle(void) {
    for (unsigned axis = 0; axis < 2; axis++) {
        receiver_t *edge = &receivers[axis];
        expire(edge, sample);
        if (edge->state != SEARCH || edge->candidate.pending ||
            (edge->initialized && sample - edge->transition <= STB_RX_IDLE_TICKS)) {
            return false;
        }
    }
    return true;
}

static unsigned distance(unsigned a, unsigned b) {
    return a > b ? a - b : b - a;
}

static void sync_to(receiver_t *src, receiver_t *dst) {
    if (dst->state != SEARCH || !dst->previous || distance(src->transition, dst->transition) > STB_RX_AXIS_SKEW_TICKS) {
        return;
    }
    if (src->candidate.pending && !dst->candidate.pending) {
        dst->candidate.transition = dst->transition;
        dst->candidate.previous = dst->previous;
        dst->candidate.pending = true;
    }
    if (src->state == FRAME) {
        /* Preserve this axis's completed bytes when rejoining the same SOF;
         * they may repair bytes the other axis missed. */
        unsigned kept = 0;
        if (distance(src->sof, dst->sof) <= STB_RX_AXIS_SKEW_TICKS) {
            kept = dst->bytes < src->bytes ? dst->bytes : src->bytes;
        }
        dst->state = FRAME;
        dst->polarity = (src->previous == src->polarity) ? dst->previous : -dst->previous;
        dst->start_gap = src->start_gap;
        dst->character = src->character;
        dst->bits = src->bits;
        dst->bytes = src->bytes;
        dst->sof = src->sof;
        memcpy(dst->data + kept, src->data + kept, src->bytes - kept);
        memcpy(dst->start_gaps + kept, src->start_gaps + kept, src->bytes - kept);
        src->peer_seen = true;
        dst->peer_seen = true;
    }
}

static uint16_t crc_error(const receiver_t *edge) {
    unsigned payload = edge->bytes - ISO15693_CRC_SIZE;
    return iso15693_crc16(edge->data, payload) ^ edge->data[payload] ^ ((uint16_t)edge->data[payload + 1] << 8);
}

/* A CRC error identifies at most two one/two-byte repairs. The generated
 * class skips impossible CRC work while retaining the original candidate order
 * and shared 16-hypothesis budget across all I/Q choices. */
typedef struct {
    uint16_t error;
    uint16_t choices[2];
} repair_entry_t;

#include "repair_data.h"

static const repair_entry_t *find_repairs(uint16_t error) {
    unsigned bucket = error >> 8;
    unsigned low = repair_limits[bucket];
    unsigned limit = repair_limits[bucket + 1];
    unsigned high = limit;
    while (low < high) {
        unsigned middle = (low + high) / 2;
        if (repair_entries[middle].error < error) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == limit || repair_entries[low].error != error) {
        return NULL;
    }
    return &repair_entries[low];
}

static unsigned repair_byte(unsigned symbol, unsigned payload) {
    unsigned row = (symbol - 1) / (STB_RX_RUN_MAX_BITS - 2);
    if (row < ST25TB_REQUEST_MAX - ISO15693_CRC_SIZE) {
        if (row >= payload) {
            return ST25TB_REQUEST_MAX;
        }
        return payload - row - 1;
    }
    return payload + row - (ST25TB_REQUEST_MAX - ISO15693_CRC_SIZE);
}

static bool verify_repair(receiver_t *edge, unsigned index, uint8_t mask,
                          unsigned other, uint8_t other_mask) {
    edge->data[index] ^= mask;
    if (other_mask) {
        edge->data[other] ^= other_mask;
    }
    if (!crc_error(edge)) {
        return true;
    }
    edge->data[index] ^= mask;
    if (other_mask) {
        edge->data[other] ^= other_mask;
    }
    return false;
}

static bool check_candidate(receiver_t *edge, unsigned *checks) {
    if (!*checks) {
        return false;
    }
    --*checks;
    uint16_t error = crc_error(edge);
    if (!error) {
        return true;
    }
    const repair_entry_t *entry = find_repairs(error);
    if (!entry) {
        /* No one/two-byte choice can pass. Preserve the consumed hypothesis
         * count and stop counting as soon as the shared budget is exhausted. */
        for (unsigned i = 0; i < edge->bytes && *checks; i++) {
            if (edge->start_gaps[i] > 2) {
                unsigned count = edge->start_gaps[i] - 2;
                if (count > *checks) {
                    count = *checks;
                }
                *checks -= count;
            }
        }
        return false;
    }
    uint8_t before[ST25TB_REQUEST_MAX];
    unsigned variants = 0;
    for (unsigned i = 0; i < edge->bytes; i++) {
        before[i] = variants;
        if (edge->start_gaps[i] > 2) {
            variants += edge->start_gaps[i] - 2;
        }
    }
    unsigned payload = edge->bytes - ISO15693_CRC_SIZE;
    for (unsigned choice = 0; choice < 2 && entry->choices[choice]; choice++) {
        unsigned code = entry->choices[choice];
        unsigned current = code & 63u;
        unsigned index = repair_byte(current, payload);
        unsigned bits = (current - 1) % (STB_RX_RUN_MAX_BITS - 2) + 1;
        if (index >= edge->bytes || bits + 1 >= edge->start_gaps[index]) {
            continue;
        }
        unsigned rank = before[index] + bits;
        if (rank > *checks) {
            continue;
        }
        unsigned other = 0;
        unsigned other_mask = 0;
        unsigned previous = code >> 6;
        if (previous) {
            other = repair_byte(previous, payload);
            unsigned other_bits = (previous - 1) % (STB_RX_RUN_MAX_BITS - 2) + 1;
            if (other >= index || other_bits + 1 >= edge->start_gaps[other]) {
                continue;
            }
            other_mask = (1u << other_bits) - 1u;
        }
        /* Impossible earlier choices still count toward the original budget. */
        if (verify_repair(edge, index, (1u << bits) - 1u, other, other_mask)) {
            *checks -= rank;
            return true;
        }
    }
    if (variants > *checks) {
        variants = *checks;
    }
    *checks -= variants;
    return false;
}

/* Each axis can miss a weak edge pair. Retry differing bytes of the same frame. */
static bool check_or_repair(receiver_t *edge, const receiver_t *other) {
    unsigned checks = STB_RX_CRC_MAX_CHECKS;
    if (check_candidate(edge, &checks)) {
        return true;
    }
    if (!checks || distance(edge->sof, other->sof) > STB_RX_AXIS_SKEW_TICKS) {
        return false;
    }
    unsigned count = other->bytes < edge->bytes ? other->bytes : edge->bytes;
    unsigned n = 0;
    uint8_t index[STB_RX_REPAIR_MAX_BYTES];
    uint8_t original[STB_RX_REPAIR_MAX_BYTES];
    uint8_t original_gaps[STB_RX_REPAIR_MAX_BYTES];
    for (unsigned i = 0; i < count; i++) {
        if (edge->data[i] == other->data[i] && edge->start_gaps[i] == other->start_gaps[i]) {
            continue;
        }
        if (n == STB_RX_REPAIR_MAX_BYTES) {
            return false;
        }
        index[n] = i;
        original[n] = edge->data[i];
        original_gaps[n++] = edge->start_gaps[i];
    }
    for (unsigned mask = 1; mask < (1u << n) && checks; mask++) {
        for (unsigned j = 0; j < n; j++) {
            bool take_other = ((mask >> j) & 1u) != 0;
            edge->data[index[j]] = take_other ? other->data[index[j]] : original[j];
            edge->start_gaps[index[j]] = take_other ? other->start_gaps[index[j]] : original_gaps[j];
        }
        if (check_candidate(edge, &checks)) {
            return true;
        }
    }
    /* Keep this candidate intact if the other axis reaches EOF next. */
    for (unsigned j = 0; j < n; j++) {
        edge->data[index[j]] = original[j];
        edge->start_gaps[index[j]] = original_gaps[j];
    }
    return false;
}

static inline __attribute__((always_inline)) bool consume_run(receiver_t *edge, bool level, unsigned bits) {
    while (bits) {
        unsigned room = STB_RX_BITS_PER_CHARACTER - edge->bits;
        unsigned take = bits < room ? bits : room;
        if (level) {
            edge->character |= ((1u << take) - 1) << edge->bits;
        }
        edge->bits += take;
        bits -= take;
        if (edge->bits != STB_RX_BITS_PER_CHARACTER) {
            continue;
        }
        if (edge->bytes == ST25TB_REQUEST_MAX) {
            return false;
        }
        uint8_t start_gap = 0;
        if ((edge->character & STB_RX_CHARACTER_FRAME_MASK) != STB_RX_CHARACTER_STOP_BIT) {
            if (!edge->start_gap || (edge->character & STB_RX_CHARACTER_FRAME_MASK) != 0) {
                return false;
            }
            /* A missed rise after the one-bit start inverts the remaining bits.
             * The complete request must still pass the existing EOF and CRC gates. */
            edge->character ^= STB_RX_AFTER_START_MASK;
            edge->previous = -edge->previous;
            level = !level;
            start_gap = edge->start_gap;
        }
        edge->start_gap = 0;
        edge->start_gaps[edge->bytes] = start_gap;
        edge->data[edge->bytes++] = edge->character >> 1;
        edge->character = 0;
        edge->bits = 0;
    }
    return true;
}

static bool closes_frame(const receiver_t *edge, unsigned now) {
    if (edge->state != FRAME || !edge->peer_seen || edge->bits || edge->bytes < STB_RX_MIN_FRAME_BYTES ||
        edge->previous == edge->polarity) {
        return false;
    }
    /* Exactly the existing 10 ETU run tolerance, without an extra division on every edge. */
    unsigned gap = now - edge->transition;
    return gap >= STB_RX_EOF_MIN_TICKS && gap <= STB_RX_EOF_MAX_TICKS;
}

static bool finish(receiver_t *edge, const receiver_t *other, uint8_t frame[ST25TB_REQUEST_MAX], unsigned *length) {
    edge->state = SEARCH;
    if (!check_or_repair(edge, other)) {
        return false;
    }
    memcpy(frame, edge->data, edge->bytes);
    *length = edge->bytes;
    return true;
}

/* A pending SOF and the current frame are separate hypotheses. The last
 * rejected rise remains a search anchor until another observed edge replaces it. */
static bool receive_sof(receiver_t *edge, receiver_t *other, unsigned now) {
    sof_candidate_t *candidate = &edge->candidate;
    if (!candidate->previous) {
        return false;
    }
    unsigned gap = now - candidate->transition;
    int sign = edge->pulse;
    if (sign == candidate->previous) {
        if (candidate->pending && gap >= STB_RX_GLITCH_MIN_TICKS && gap < STB_RX_SOF_HIGH_MIN_TICKS) {
            candidate->pending = false;
        } else if (!candidate->pending) {
            candidate->transition = now;
        }
        return false;
    }
    if (candidate->pending && gap < STB_RX_GLITCH_MIN_TICKS) {
        return false;
    }
    bool high = candidate->pending && gap >= STB_RX_SOF_HIGH_MIN_TICKS && gap <= STB_RX_SOF_HIGH_MAX_TICKS;
    bool low = edge->state == SEARCH || edge->previous != edge->polarity;
    candidate->pending = !high && low && gap >= STB_RX_SOF_LOW_MIN_TICKS && gap <= STB_RX_SOF_LOW_MAX_TICKS;
    candidate->transition = now;
    candidate->previous = sign;
    if (!high) {
        return false;
    }
    candidate->previous = 0;
    edge->state = FRAME;
    edge->polarity = -sign;
    edge->previous = sign;
    edge->transition = now;
    edge->sof = now - gap;
    edge->peer_seen = other->state == FRAME && distance(edge->sof, other->sof) <= STB_RX_AXIS_SKEW_TICKS;
    if (edge->peer_seen) {
        other->peer_seen = true;
    }
    edge->start_gap = 0;
    edge->character = 0;
    edge->bits = 0;
    edge->bytes = 0;
    sync_to(edge, other);
    return true;
}

/* Recover a partial character only when its measured peer stop edge and
 * this measured EOF edge satisfy the unchanged character/EOF timing. */
__attribute__((noinline)) static void complete_peer_character(receiver_t *edge, unsigned peer_transition, unsigned now) {
    if (edge->state != FRAME || !edge->peer_seen || edge->bytes < STB_RX_MIN_FRAME_BYTES ||
        edge->bytes == ST25TB_REQUEST_MAX || !edge->bits || edge->previous != edge->polarity ||
        (edge->character & 1u)) {
        return;
    }
    unsigned eof_gap = now - peer_transition;
    unsigned gap = peer_transition - edge->transition;
    if (eof_gap < STB_RX_EOF_MIN_TICKS || eof_gap > STB_RX_EOF_MAX_TICKS ||
        !gap || gap > STB_RX_FRAME_GAP_MAX_TICKS) {
        return;
    }
    unsigned scaled = gap * STB_RX_TICK_ETU_NUMERATOR;
    unsigned bits = STB_RX_BITS_PER_CHARACTER - edge->bits;
    unsigned target = bits * STB_RX_TICK_ETU_DENOMINATOR;
    unsigned error = scaled > target ? scaled - target : target - scaled;
    if (error > STB_RX_RUN_TOLERANCE) {
        return;
    }
    if (consume_run(edge, true, bits)) {
        edge->previous = -edge->previous;
        edge->transition = peer_transition;
    }
}

__attribute__((noinline)) static void complete_peer_characters(receiver_t *edge, unsigned now) {
    receiver_t *other = edge == &receivers[0] ? &receivers[1] : &receivers[0];
    complete_peer_character(edge, other->transition, now);
    complete_peer_character(other, edge->transition, now);
}

__attribute__((noinline)) static bool receive_edge(receiver_t *edge, unsigned now, uint8_t frame[ST25TB_REQUEST_MAX],
                                                   unsigned *length) {
    receiver_t *other = (edge == &receivers[0]) ? &receivers[1] : &receivers[0];
    /* A fading axis may miss EOF's rise while the other axis still sees it. */
    if (closes_frame(other, now) && finish(other, edge, frame, length)) {
        return true;
    }
    if (receive_sof(edge, other, now)) {
        return false;
    }
    unsigned gap = now - edge->transition;
    /* ADC ringing can reverse pulse polarity; qualified NRZ edges still toggle. */
    int sign = edge->pulse;
    if (edge->state == FRAME) {
        sign = -edge->previous;
    }
    if (sign == edge->previous) {
        if (edge->state == SEARCH) {
            edge->transition = now;
            sync_to(other, edge);
        }
        return false;
    }
    /* Genuine NRZ edges alternate. Ignore sub-bit comparator rebound. */
    if (edge->state != SEARCH && gap < STB_RX_GLITCH_MIN_TICKS) {
        return false;
    }
    int previous = edge->previous;
    edge->previous = sign;
    edge->transition = now;
    if (previous && gap >= STB_RX_SOF_LOW_MIN_TICKS && gap <= STB_RX_SOF_LOW_MAX_TICKS &&
        (edge->state == SEARCH || previous != edge->polarity)) {
        edge->candidate.transition = now;
        edge->candidate.previous = sign;
        edge->candidate.pending = true;
    }
    if (edge->state == FRAME) {
        /* One 2 us sample = 339/1600 ETU; round each edge interval. */
        unsigned scaled = gap * STB_RX_TICK_ETU_NUMERATOR;
        unsigned bits = (scaled + STB_RX_ETU_ROUNDING) / STB_RX_TICK_ETU_DENOMINATOR;
        unsigned target = bits * STB_RX_TICK_ETU_DENOMINATOR;
        unsigned error = scaled > target ? scaled - target : target - scaled;
        bool level = previous == edge->polarity;
        if (!level && !edge->bits && edge->bytes >= STB_RX_MIN_FRAME_BYTES && bits == STB_RX_BITS_PER_CHARACTER &&
            error <= STB_RX_RUN_TOLERANCE) {
            if (finish(edge, other, frame, length)) {
                return true;
            }
            /* A rejected EOF may be SOF's rising edge after a stale frame. */
        }
        if (bits && bits <= STB_RX_RUN_MAX_BITS && error <= STB_RX_RUN_TOLERANCE) {
            /* A repeated falling pulse can hide the start bit's rising edge.
             * Keep normal polarity unless character framing later proves it wrong. */
            if (!edge->bits && !level && edge->pulse == previous && bits >= 2) {
                edge->start_gap = bits;
            }
            if (consume_run(edge, level, bits)) {
                sync_to(edge, other);
                return false;
            }
        }
        edge->state = SEARCH;
    }
    if (previous && gap >= STB_RX_SOF_LOW_MIN_TICKS && gap <= STB_RX_SOF_LOW_MAX_TICKS) {
        edge->candidate.transition = now;
        edge->candidate.previous = sign;
        edge->candidate.pending = true;
    }
    sync_to(other, edge);
    return false;
}

static inline bool receive_value(receiver_t *edge, int value, unsigned now, uint8_t frame[ST25TB_REQUEST_MAX],
                                 unsigned *length) {
    if (!edge->initialized) {
        edge->bias = value;
        edge->initialized = true;
    }
    int32_t difference = value - edge->bias;
    edge->bias += difference / STB_RX_BASELINE_DIVISOR;
    int sign = 0;
    if (difference > STB_RX_EDGE_THRESHOLD) {
        sign = 1;
    } else if (difference < -STB_RX_EDGE_THRESHOLD) {
        sign = -1;
    }
    bool event = sign && sign != edge->pulse;
    if (event) {
        edge->pulse = sign;
    } else if ((difference < STB_RX_RELEASE_THRESHOLD && difference > -STB_RX_RELEASE_THRESHOLD) ||
               (!sign && now - edge->transition >= STB_RX_SOF_HIGH_MIN_TICKS)) {
        edge->pulse = 0;
    }
    if (!event) {
        return false;
    }
    /* Either recovery needs this axis's preceding edge at least one EOF ago:
     * as the peer stop edge, or before its own positive character tail + EOF. */
    if (now - edge->transition >= STB_RX_EOF_MIN_TICKS) {
        complete_peer_characters(edge, now);
    }
    expire(edge, now);
    return receive_edge(edge, now, frame, length);
}

static inline bool receive_pair(uint8_t first, uint8_t second, unsigned now,
                                 uint8_t frame[ST25TB_REQUEST_MAX], unsigned *length) {
    int i = (((int8_t)first >> 4) + ((int8_t)second >> 4)) * STB_RX_SAMPLE_SCALE;
    int q = (((int8_t)(first << 4) >> 4) + ((int8_t)(second << 4) >> 4)) * STB_RX_SAMPLE_SCALE;
    if (!receive_value(&receivers[0], i, now, frame, length) && !receive_value(&receivers[1], q, now, frame, length)) {
        return false;
    }
    /* One validated EOF ends both candidates, including a lagging axis. */
    receivers[0].state = SEARCH;
    receivers[0].candidate.previous = 0;
    receivers[0].candidate.pending = false;
    receivers[1].state = SEARCH;
    receivers[1].candidate.previous = 0;
    receivers[1].candidate.pending = false;
    return true;
}

bool st25tb_receiver_pair(uint8_t first, uint8_t second, uint8_t frame[ST25TB_REQUEST_MAX], unsigned *length) {
    unsigned now = sample++;
    if (settling) {
        settling--;
        return false;
    }
    return receive_pair(first, second, now, frame, length);
}

unsigned st25tb_receiver_batch(const uint8_t *samples, unsigned size, uint8_t frame[ST25TB_REQUEST_MAX],
                               unsigned *length) {
    *length = 0;
    unsigned pairs = size / 2;
    unsigned skipped = settling < pairs ? settling : pairs;
    settling -= skipped;
    unsigned now = sample + skipped;
    /* This receiver has one owner. Publish the consumed-pair clock before
     * returning, including recovery and an early return at EOF. */
    for (unsigned n = skipped * 2; n + 1 < size; n += 2) {
        if (receive_pair(samples[n], samples[n + 1], now++, frame, length)) {
            sample = now;
            return n + 2;
        }
    }
    sample = now;
    return size & ~1u;
}
