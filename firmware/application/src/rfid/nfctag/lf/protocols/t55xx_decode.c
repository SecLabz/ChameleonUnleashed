#include "t55xx_decode.h"
#include "utils/manchester.h"

#include <string.h>

static const uint16_t clocks[] = {8, 16, 32, 40, 50, 64, 100, 128};
/* PSK2/3 cannot be distinguished reliably from a single repeated block.
 * Only PSK1 is identified here; ambiguous captures remain inconclusive. */
static const uint8_t modulations[] = {8, 5, 7, 4, 6, 0, 16, 24, 1};
static const uint8_t rate_order[] = {5, 2, 4, 1, 0, 3, 6, 7};

bool t55xx_configuration_valid(uint32_t word, uint16_t clock, uint8_t modulation) {
    unsigned key = word >> 28;
    unsigned blocks = (word >> 5) & 7;
    unsigned carrier = (word >> 10) & 3;
    return (key == 0 || key == 6) && !(word & 0x0fe20000u) && blocks != 0 &&
        clocks[(word >> 18) & 7] == clock && ((word >> 12) & 31) == modulation &&
        carrier < 3 && (modulation == 1 || carrier == 0);
}

static unsigned mean(const uint16_t *samples, size_t start, size_t count) {
    unsigned sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += samples[start + i];
    }
    return sum / count;
}

static float power(const uint16_t *samples, size_t start, size_t count, float coefficient) {
    float previous = 0;
    float before_previous = 0;
    float center = (float)mean(samples, start, count);
    for (size_t i = 0; i < count; i++) {
        float next = samples[start + i] - center + coefficient * previous - before_previous;
        before_previous = previous;
        previous = next;
    }
    return previous * previous + before_previous * before_previous -
        coefficient * previous * before_previous;
}

typedef struct {
    size_t position;
    bool level;
    bool previous_second;
    bool has_previous;
} level_cursor_t;

static bool level_window(const uint16_t *samples, size_t start, size_t length,
    const t55xx_demod_t *demod, level_cursor_t *cursor) {
    unsigned high = 0;
    while (cursor->position < start + length) {
        uint16_t sample = samples[cursor->position];
        if (sample >= demod->level_high) {
            cursor->level = true;
        } else if (sample <= demod->level_low) {
            cursor->level = false;
        }
        if (cursor->position >= start && cursor->level) {
            high++;
        }
        cursor->position++;
    }
    return high > length / 2;
}

static bool bit_at(const uint16_t *samples, size_t count, const t55xx_demod_t *demod,
    size_t start, level_cursor_t *cursor, bool *bit) {
    unsigned clock = demod->clock_cycles;
    if (start + clock > count) {
        return false;
    }
    bool value;
    unsigned modulation = demod->modulation;
    if (modulation >= 4 && modulation <= 7) {
        bool fsk1 = modulation == 4 || modulation == 6;
        float first = power(samples, start, clock, fsk1 ? 0.61803399f : 1.41421356f);
        float second = power(samples, start, clock, fsk1 ? 1.41421356f : 1.61803399f);
        if (first < 1 || second < 1 || (first < second * 1.2f && second < first * 1.2f)) {
            return false;
        }
        value = second > first;
    } else if (modulation == 0 || modulation == 1) {
        /* The analogue front end filters out the PSK subcarrier. Opposite
         * phase transitions leave positive/negative envelope excursions;
         * retain their polarity through the flat parts between transitions. */
        value = level_window(samples, start + clock / 4, clock / 2, demod, cursor);
    } else {
        if (modulation == 8) {
            /* The analogue envelope drifts during long runs. Compare the two
             * halves locally instead of assuming a fixed DC midpoint. */
            if (!manchester_sample_bit(samples + start, clock, &value)) {
                return false;
            }
        } else {
            bool first = level_window(samples, start + clock / 8, clock / 4, demod, cursor);
            bool second = level_window(samples, start + clock * 5 / 8, clock / 4, demod, cursor);
            bool boundary = !cursor->has_previous || first != cursor->previous_second;
            cursor->has_previous = true;
            cursor->previous_second = second;
            if (!boundary) {
                return false;
            }
            value = first == second;
        }
    }
    *bit = value != demod->inverted;
    return true;
}

static bool signal_limits(const uint16_t *samples, size_t count, t55xx_demod_t *demod,
    bool phase_envelope) {
    unsigned minimum = UINT16_MAX;
    unsigned maximum = 0;
    /* Ignore the first 2 ms of the command's field transient for PSK limits.
     * Its peak is much larger than the actual phase-change excursions. */
    size_t start = phase_envelope ? 256 : 0;
    if (count <= start) {
        return false;
    }
    for (size_t i = start; i < count; i++) {
        if (samples[i] < minimum) {
            minimum = samples[i];
        }
        if (samples[i] > maximum) {
            maximum = samples[i];
        }
    }
    /* The LF front end rings above the actual NRZ high level. Use the same
     * clipping ceiling and Schmitt dead zone as the PAC receiver (14-bit ADC
     * 8000); a long run may settle inside that zone. */
    unsigned clipped_high = maximum;
    if (!phase_envelope && clipped_high > 8000 && minimum < 8000) {
        clipped_high = 8000;
    }
    unsigned range = clipped_high - minimum;
    demod->level_low = minimum + range / 4;
    demod->level_high = minimum + range * 3 / 4;
    if (phase_envelope) {
        /* A positive phase excursion also has a shallow negative undershoot.
         * Long one-runs make the genuine falling excursion weaker. Reference
         * the low threshold to the DC baseline and positive peak, so neither
         * that undershoot nor a word's weakest negative peak sets the level. */
        unsigned baseline = mean(samples, start, count - start);
        unsigned drop = (maximum - baseline) * 3 / 10;
        demod->level_low = baseline > minimum + drop ? baseline - drop : minimum;
    }
    return maximum - minimum >= 512;
}

bool t55xx_detect_samples(const uint16_t *samples, size_t count, t55xx_demod_t *demod,
    void (*keepalive)(void)) {
    t55xx_demod_t limits = {0};
    t55xx_demod_t phase_limits = {0};
    if (!samples || !demod || !signal_limits(samples, count, &limits, false)) {
        return false;
    }
    signal_limits(samples, count, &phase_limits, true);
    for (unsigned rate = 0; rate < sizeof(clocks) / sizeof(clocks[0]); rate++) {
        for (unsigned modulation = 0; modulation < sizeof(modulations); modulation++) {
            t55xx_demod_t best = {0};
            unsigned best_margin = 0;
            for (unsigned inverted = 0; inverted < 2; inverted++) {
                t55xx_demod_t candidate = {
                    .clock_cycles = clocks[rate_order[rate]],
                    .modulation = modulations[modulation],
                    .inverted = inverted != 0,
                    .level_low = limits.level_low,
                    .level_high = limits.level_high
                };
                if (candidate.modulation == 1) {
                    candidate.level_low = phase_limits.level_low;
                    candidate.level_high = phase_limits.level_high;
                }
                size_t capture_limit = 1024 + candidate.clock_cycles * 100u;
                if (capture_limit > count) {
                    capture_limit = count;
                }
                for (unsigned phase = 0; phase < candidate.clock_cycles;
                    phase += candidate.clock_cycles / 8) {
                    if (keepalive) {
                        keepalive();
                    }
                    uint32_t preceding[32] = {0};
                    uint32_t word = 0;
                    unsigned valid = 0;
                    unsigned index = 0;
                    level_cursor_t cursor = {0};
                    for (size_t start = phase; start + candidate.clock_cycles <= capture_limit;
                        start += candidate.clock_cycles, index++) {
                        bool bit;
                        if (!bit_at(samples, capture_limit, &candidate, start, &cursor, &bit)) {
                            valid = 0;
                            memset(preceding, 0, sizeof(preceding));
                            continue;
                        }
                        word = (word << 1) | bit;
                        valid++;
                        if (valid >= 64 && preceding[index % 32] == word &&
                            t55xx_configuration_valid(word, candidate.clock_cycles, candidate.modulation)) {
                            candidate.configuration = word;
                            candidate.sample_offset = (uint16_t)(start - 63 * candidate.clock_cycles);
                            if (candidate.modulation != 8) {
                                *demod = candidate;
                                return true;
                            }
                            /* Prefer the middle of the analogue eye. The first
                             * decodable phase may sit on a transition and fail
                             * when the next capture starts a little later. */
                            unsigned margin = UINT16_MAX;
                            for (unsigned i = 0; i < 64; i++) {
                                size_t bit_start = candidate.sample_offset + i * candidate.clock_cycles;
                                unsigned first = mean(samples, bit_start + candidate.clock_cycles / 8, candidate.clock_cycles / 4);
                                unsigned second = mean(samples, bit_start + candidate.clock_cycles * 5 / 8, candidate.clock_cycles / 4);
                                unsigned difference = first > second ? first - second : second - first;
                                if (difference < margin) {
                                    margin = difference;
                                }
                            }
                            if (margin > best_margin) {
                                best_margin = margin;
                                best = candidate;
                            }
                            break;
                        }
                        preceding[index % 32] = word;
                    }
                }
            }
            if (best_margin) {
                *demod = best;
                return true;
            }
        }
    }
    return false;
}

bool t55xx_read_samples(const uint16_t *samples, size_t count,
    const t55xx_demod_t *demod, uint32_t *word) {
    if (!samples || !demod || !word) {
        return false;
    }
    t55xx_demod_t capture_demod = *demod;
    /* PSK excursion amplitude changes with a word's run lengths. Recalculate
     * its levels while retaining the configuration's alignment and polarity. */
    if (demod->modulation == 1 && !signal_limits(samples, count, &capture_demod, true)) {
        return false;
    }
    uint32_t first = 0;
    uint32_t second = 0;
    size_t start = demod->sample_offset;
    level_cursor_t cursor = {0};
    for (unsigned i = 0; i < 64; i++, start += demod->clock_cycles) {
        bool bit;
        if (!bit_at(samples, count, &capture_demod, start, &cursor, &bit)) {
            return false;
        }
        if (i < 32) {
            first = (first << 1) | bit;
        } else {
            second = (second << 1) | bit;
        }
    }
    /* The envelope of a constant NRZ/PSK1 word cannot establish its polarity.
     * Keep it unknown instead of claiming an all-zero/all-one read. */
    if (first != second || (demod->modulation <= 1 && (first == 0 || first == UINT32_MAX))) {
        return false;
    }
    *word = first;
    return true;
}
