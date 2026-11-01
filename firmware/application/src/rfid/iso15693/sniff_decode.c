#include "sniff_decode.h"
#include "commands.h"
#include "ppm.h"
#include <string.h>

static const int8_t phase_shifts[] = {0, -1, 1, -2, 2};

bool nfcv_sniff_reader_stop(nfcv_sniff_reader_t *reader) {
    if (!reader->acquired) {
        return false;
    }
    reader->frame.byte_count = NFCV_PPM_FRAME_BYTES(reader->symbols, reader->slow);
    reader->frame.crc_ok = false;
    reader->truncated = true;
    reader->acquired = false;
    reader->last_fall = 0;
    return reader->frame.byte_count != 0;
}

nfcv_sniff_reader_event_t nfcv_sniff_reader_feed(nfcv_sniff_reader_t *reader, bool lock, uint32_t cycles) {
    nfcv_sniff_reader_event_t event = NFCV_SNIFF_READER_NONE;
    if (!reader->previous_lock && lock && reader->acquired && reader->last_fall) {
        uint32_t width = cycles - reader->last_fall;
        unsigned tick = NFCV_PPM_TICK_FROM_CYCLES(reader->last_fall - reader->reference);
        unsigned position = NFCV_PPM_PULSE_POSITION(tick, reader->slow);
        if (width > NFCV_RX_MERGED_PULSE_MIN_CYCLES && width < NFCV_RX_MERGED_PULSE_MAX_CYCLES &&
                position == (reader->slow ? NFCV_PPM_1_OF_256_LAST_POSITION : NFCV_PPM_1_OF_4_LAST_POSITION) &&
                reader->symbols < NFCV_PPM_MAX_SYMBOLS(reader->slow)) {
            reader->symbols++;
        }
    }
    if (reader->previous_lock && !lock) {
        uint32_t gap = cycles - reader->last_fall;
        if (!reader->acquired) {
            if (reader->last_fall && ((gap > NFCV_RX_1_OF_4_SOF_MIN_CYCLES && gap < NFCV_RX_1_OF_4_SOF_MAX_CYCLES) ||
                    (gap > NFCV_RX_1_OF_256_SOF_MIN_CYCLES && gap < NFCV_RX_1_OF_256_SOF_MAX_CYCLES))) {
                reader->slow = gap > NFCV_RX_SOF_CODING_SPLIT_CYCLES;
                reader->reference = reader->last_fall;
                reader->symbols = 0;
                reader->inventory_slots = 0;
                reader->acquired = true;
                reader->truncated = false;
                memset(&reader->frame, 0, sizeof(reader->frame));
            }
        } else {
            uint32_t elapsed = cycles - reader->reference;
            unsigned tick = NFCV_PPM_TICK_FROM_CYCLES(elapsed);
            uint32_t expected = NFCV_PPM_CYCLES_FROM_TICK(tick);
            unsigned error = elapsed > expected ? elapsed - expected : expected - elapsed;
            unsigned symbol = NFCV_PPM_SYMBOL_INDEX(tick, reader->slow);
            unsigned position = NFCV_PPM_PULSE_POSITION(tick, reader->slow);
            if (error <= NFCV_RX_TICK_TOLERANCE_CYCLES && symbol == reader->symbols + 1u &&
                    (position & 1u) && reader->symbols < NFCV_PPM_MAX_SYMBOLS(reader->slow)) {
                if (reader->slow) {
                    reader->frame.bytes[reader->symbols] = (position - 1u) / 2u;
                    reader->reference = cycles - expected;
                } else {
                    reader->frame.bytes[reader->symbols / 4u] |= ((position - 1u) / 2u) << ((reader->symbols % 4u) * 2u);
                }
                reader->symbols++;
            } else if (error <= NFCV_RX_TICK_TOLERANCE_CYCLES && symbol == reader->symbols + 1u &&
                    position == NFCV_PPM_EOF_POSITION && reader->symbols >= NFCV_PPM_MIN_FRAME_SYMBOLS(reader->slow) &&
                    (reader->slow || !(reader->symbols % 4u))) {
                reader->frame.byte_count = NFCV_PPM_FRAME_BYTES(reader->symbols, reader->slow);
                reader->frame.crc_ok = iso15693_check_crc(reader->frame.bytes, reader->frame.byte_count);
                reader->frame.high_data_rate = (reader->frame.bytes[0] & ISO15693_REQ_HIGH_RATE) != 0;
                reader->frame.single_subcarrier = (reader->frame.bytes[0] & ISO15693_REQ_DUAL_SUBCARRIER) == 0;
                if (reader->frame.crc_ok && reader->frame.bytes[1] == ISO15693_CMD_INVENTORY &&
                        (reader->frame.bytes[0] & ISO15693_REQ_INVENTORY) &&
                        !(reader->frame.bytes[0] & ISO15693_REQ_SLOT1)) {
                    reader->inventory_slots = 15u;
                }
                reader->acquired = false;
                event = NFCV_SNIFF_READER_FRAME;
            } else if (nfcv_sniff_reader_stop(reader)) {
                event = NFCV_SNIFF_READER_FRAME;
            }
        }
        /* The command EOF must not become the first slot-advance EOF. */
        reader->last_fall = event == NFCV_SNIFF_READER_NONE ? cycles : 0;
    }
    reader->previous_lock = lock;
    if (reader->acquired && cycles - reader->last_fall >
            (reader->slow ? NFCV_RX_1_OF_256_GAP_TIMEOUT_CYCLES : NFCV_RX_1_OF_4_GAP_TIMEOUT_CYCLES)) {
        if (nfcv_sniff_reader_stop(reader)) {
            event = NFCV_SNIFF_READER_FRAME;
        }
    }
    if (!reader->acquired && reader->last_fall && reader->inventory_slots &&
            cycles - reader->last_fall > NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES) {
        reader->last_fall = 0;
        reader->inventory_slots--;
        event = NFCV_SNIFF_READER_SLOT;
    }
    return event;
}

static unsigned half_bin(const nfcv_sniff_tag_t *tag, unsigned half) {
    return (half * 12800u * tag->rate + 678u) / 1356u;
}

static inline __attribute__((always_inline)) float energy(const nfcv_sniff_tag_t *tag, unsigned begin, unsigned end) {
    float power = 0;
    for (unsigned axis = 0; axis < 4; axis++) {
        float value = tag->integral[axis][end & (tag->integral_count - 1u)] -
            tag->integral[axis][begin & (tag->integral_count - 1u)];
        power += value * value;
    }
    float length = end - begin;
    return power / (length * length);
}

static bool pattern(const nfcv_sniff_tag_t *tag, unsigned half, uint8_t bits, float *quality) {
    float values[8];
    float on = 0;
    float off = 0;
    unsigned on_count = 0;
    for (unsigned h = 0; h < 8; h++) {
        values[h] = energy(tag, tag->position + half_bin(tag, half + h),
            tag->position + half_bin(tag, half + h + 1u));
        if (bits & (0x80u >> h)) {
            on += values[h];
            on_count++;
        } else {
            off += values[h];
        }
    }
    on /= on_count;
    off /= 8u - on_count;
    if (on < 0.01f || on < off * 4.0f) {
        return false;
    }
    float threshold = (on + off) * 0.5f;
    for (unsigned h = 0; h < 8; h++) {
        if ((values[h] > threshold) != ((bits & (0x80u >> h)) != 0)) {
            return false;
        }
    }
    if (quality) {
        *quality = on - off;
    }
    return true;
}

void nfcv_sniff_tag_reset(nfcv_sniff_tag_t *tag, bool high_rate, float *workspace) {
    memset(tag, 0, sizeof(*tag));
    tag->integral_count = high_rate ? NFCV_SNIFF_HIGH_INTEGRALS : NFCV_SNIFF_LOW_INTEGRALS;
    for (unsigned axis = 0; axis < 4; axis++) {
        tag->integral[axis] = workspace + axis * tag->integral_count;
        tag->integral[axis][0] = 0;
    }
    /* Capture starts at reader EOF; the tag cannot start inside the first
     * 64 us. Ignore the receiver settling transient before SOF acquisition. */
    tag->position = 32u;
    tag->cosine = 1;
    tag->rate = high_rate ? 1u : 4u;
}

static bool decode_tag(nfcv_sniff_tag_t *tag) {
    while (!tag->acquired) {
        if (tag->bins < tag->position + half_bin(tag, 8)) {
            return false;
        }
        float on = energy(tag, tag->position + half_bin(tag, 3), tag->position + half_bin(tag, 6));
        float off = energy(tag, tag->position, tag->position + half_bin(tag, 3));
        if (on > 0.01f && on > off * 4.0f && pattern(tag, 0, 0x1d, NULL)) {
            if (tag->bins < tag->position + half_bin(tag, 9)) {
                return false;
            }
            unsigned first = tag->position;
            unsigned best = first;
            float best_quality = 0;
            for (unsigned shift = 0; shift <= half_bin(tag, 1); shift++) {
                tag->position = first + shift;
                float quality = 0;
                if (pattern(tag, 0, 0x1d, &quality) && quality > best_quality) {
                    best_quality = quality;
                    best = tag->position;
                }
            }
            tag->position = best;
            tag->acquired = true;
            tag->half = 8;
        } else {
            tag->position += 2u;
        }
    }
    if (tag->bins < tag->position + half_bin(tag, tag->half + 8u) + 2u) {
        return false;
    }
    if (!tag->eof_checked) {
        tag->eof_checked = true;
        float marker = energy(tag, tag->position + half_bin(tag, tag->half + 2u),
            tag->position + half_bin(tag, tag->half + 5u));
        float quiet = energy(tag, tag->position + half_bin(tag, tag->half + 5u),
            tag->position + half_bin(tag, tag->half + 8u));
        /* EOF has three carrier half-bits followed by three quiet half-bits.
         * Gate phase refinement on this non-Manchester marker so ordinary
         * data bytes do not spend the reader's next SOF margin searching EOF. */
        if (tag->length >= 3u && marker > 0.01f && marker > quiet * 6.0f) {
            unsigned reference = tag->position;
            for (unsigned candidate = 0; candidate < sizeof(phase_shifts) / sizeof(phase_shifts[0]); candidate++) {
                tag->position = reference + phase_shifts[candidate];
                if (pattern(tag, tag->half, 0xb8, NULL)) {
                    return true;
                }
            }
            tag->position = reference;
        }
    }
    if (tag->bins < tag->position + half_bin(tag, tag->half + 16u)) {
        return false;
    }
    if (tag->length == sizeof(tag->data)) {
        tag->truncated = true;
        return true;
    }
    /* Refine Manchester phase periodically and on weak contrast. A fixed
     * SOF phase accumulates reader/ADC clock error across a long SLIX reply. */
    if (tag->bins < tag->position + half_bin(tag, tag->half + 16u) + 2u) {
        return false;
    }
    unsigned reference = tag->position;
    unsigned best_position = reference;
    uint8_t best_value = 0;
    float best_quality = -1;
    for (unsigned candidate = 0; candidate < sizeof(phase_shifts) / sizeof(phase_shifts[0]); candidate++) {
        int shift = phase_shifts[candidate];
        unsigned position = reference + shift;
        uint8_t value = 0;
        float quality = 0;
        bool valid = true;
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned half = tag->half + bit * 2u;
            float first = energy(tag, position + half_bin(tag, half), position + half_bin(tag, half + 1u));
            float second = energy(tag, position + half_bin(tag, half + 1u), position + half_bin(tag, half + 2u));
            float high = first > second ? first : second;
            float low = first > second ? second : first;
            if (high < 0.01f || high < low * 1.5f) {
                valid = false;
                break;
            }
            quality += (high - low) / (high + low);
            if (second > first) {
                value |= 1u << bit;
            }
        }
        if (valid && quality > best_quality) {
            best_quality = quality;
            best_position = position;
            best_value = value;
            if (tag->length % 8u != 0) {
                break;
            }
        }
    }
    if (best_quality < 0) {
        tag->truncated = true;
        return true;
    }
    tag->position = best_position;
    tag->half += 16u;
    tag->data[tag->length++] = best_value;
    tag->eof_checked = false;
    return false;
}

static bool feed_samples(nfcv_sniff_tag_t *tag, const uint8_t *samples, unsigned count,
        float missing_i, float missing_q) {
    float cosine = tag->cosine;
    float sine = tag->sine;
    float sum_i_cos = tag->sum[0];
    float sum_i_sin = tag->sum[1];
    float sum_q_cos = tag->sum[2];
    float sum_q_sin = tag->sum[3];
    float dc_i = tag->dc_i;
    float dc_q = tag->dc_q;
    unsigned bins = tag->bins;
    unsigned total = tag->samples;
    for (unsigned n = 0; n < count; n++) {
        total++;
        if (total & 1u) {
            continue;
        }
        float signal_i = missing_i;
        float signal_q = missing_q;
        if (samples) {
            int i = samples[n] >> 4;
            int q = samples[n] & 15u;
            if (i >= 8) {
                i -= 16;
            }
            if (q >= 8) {
                q -= 16;
            }
            signal_i = i - dc_i;
            signal_q = q - dc_q;
            dc_i += signal_i * 0.03125f;
            dc_q += signal_q * 0.03125f;
        }
        sum_i_cos += signal_i * cosine;
        sum_i_sin += signal_i * sine;
        sum_q_cos += signal_q * cosine;
        sum_q_sin += signal_q * sine;
        /* At 500 ksample/s the 423.75 kHz subcarrier aliases to -76.25 kHz.
         * Keep the original 2 us time bins while halving mixing work. */
        float next = cosine * 0.5750052520f + sine * 0.8181497174f;
        sine = sine * 0.5750052520f - cosine * 0.8181497174f;
        cosine = next;
        bins++;
        unsigned index = bins & (tag->integral_count - 1u);
        tag->integral[0][index] = sum_i_cos;
        tag->integral[1][index] = sum_i_sin;
        tag->integral[2][index] = sum_q_cos;
        tag->integral[3][index] = sum_q_sin;
    }
    tag->dc_i = dc_i;
    tag->dc_q = dc_q;
    tag->sum[0] = sum_i_cos;
    tag->sum[1] = sum_i_sin;
    tag->sum[2] = sum_q_cos;
    tag->sum[3] = sum_q_sin;
    tag->cosine = cosine;
    tag->sine = sine;
    tag->samples = total;
    tag->bins = bins;
    return decode_tag(tag);
}

bool nfcv_sniff_tag_feed(nfcv_sniff_tag_t *tag, const uint8_t *samples, unsigned count) {
    return feed_samples(tag, samples, count, 0, 0);
}

static float adc_value(uint8_t nibble) {
    return nibble < 8u ? nibble : (int)nibble - 16;
}

bool nfcv_sniff_tag_gap(nfcv_sniff_tag_t *tag, unsigned count, uint8_t before, uint8_t after) {
    if (!count) {
        return false;
    }
    if (count > 2u) {
        tag->truncated = true;
        return true;
    }
    float before_i = adc_value(before >> 4) - tag->dc_i;
    float before_q = adc_value(before & 15u) - tag->dc_q;
    float after_i = adc_value(after >> 4) - tag->dc_i;
    float after_q = adc_value(after & 15u) - tag->dc_q;
    /* Band-limited interpolation at 423.75 kHz: the two real ADC neighbours
     * bound the one/two missing samples. DC and mixer phase stay continuous. */
    for (unsigned sample = 0; sample < count; sample++) {
        float left = -0.563435231f;
        float right = left;
        if (count == 2u) {
            left = sample ? 0.465114007f : -0.825496848f;
            right = sample ? -0.825496848f : 0.465114007f;
        }
        if (feed_samples(tag, NULL, 1, before_i * left + after_i * right,
                before_q * left + after_q * right)) {
            return true;
        }
    }
    return false;
}
