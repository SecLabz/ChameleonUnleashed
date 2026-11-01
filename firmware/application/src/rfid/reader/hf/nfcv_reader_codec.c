#include "hf_iq.h"
#include "nfcv_reader_codec.h"
#include "iso15693/frame.h"
#include <string.h>

size_t nfcv_reader_encode(const uint8_t *frame, size_t length, uint8_t *levels, size_t capacity) {
    if (!frame || !levels || !length || length > NFCV_READER_MAX_FRAME ||
            capacity < 12 + length * 32)
        return 0;
    size_t used = 8;
    memset(levels, 0, 12 + length * 32);
    levels[0] = levels[5] = 1; /* 1-of-4 SOF: 10000100. */
    for (size_t i = 0; i < length; i++) {
        for (unsigned pair = 0; pair < 4; pair++) {
            unsigned value = (frame[i] >> (pair * 2)) & 3;
            levels[used + value * 2 + 1] = 1;
            used += 8;
        }
    }
    levels[used + 2] = 1; /* EOF: 0010. */
    return used + 4;
}

/* Mix at 1 MHz, then integrate pairs to bound RAM use for longer captures. */



static unsigned half_sample(unsigned half) {
    return (half * 12800u + 678u) / 1356u;
}

static float power(unsigned pos, unsigned half) {
    unsigned begin = pos + half_sample(half), end = pos + half_sample(half + 1);
    float i = hf_iq_difference(hf_iq_integral_i, begin, end);
    float q = hf_iq_difference(hf_iq_integral_q, begin, end);
    float length = end - begin;
    return (i * i + q * q) / (length * length);
}

static bool pattern(unsigned pos, unsigned half, uint8_t bits) {
    float values[8], on = 0, off = 0;
    unsigned on_count = 0;
    for (unsigned h = 0; h < 8; h++) {
        values[h] = power(pos, half + h);
        if (bits & (0x80u >> h)) {
            on += values[h];
            on_count++;
        } else {
            off += values[h];
        }
    }
    on /= on_count;
    off /= 8 - on_count;
    if (on < 0.01f || on < off * 4)
        return false;
    float threshold = (on + off) * 0.5f;
    for (unsigned h = 0; h < 8; h++)
        if ((values[h] > threshold) != ((bits & (0x80u >> h)) != 0))
            return false;
    return true;
}

size_t nfcv_reader_decode_iq(const uint8_t *samples, size_t count, uint8_t *frame,
                             size_t capacity, unsigned *offset) {
    return nfcv_reader_decode_iq_checked(samples, count, frame, capacity, offset, NULL);
}

size_t nfcv_reader_decode_iq_checked(const uint8_t *samples, size_t count, uint8_t *frame,
        size_t capacity, unsigned *offset, bool *crc_error) {
    if (!samples || !frame || !offset || count > NFCV_READER_IQ_SAMPLES ||
            count / 2 < half_sample(64) ||
            capacity < 3 || capacity > NFCV_READER_MAX_FRAME)
        return 0;
    hf_iq_prepare(true);
    float mi = 0, mq = 0;
    for (size_t n = 0; n < count; n++) {
        int i = samples[n] >> 4, q = samples[n] & 15;
        mi += i < 8 ? i : i - 16;
        mq += q < 8 ? q : q - 16;
    }
    mi /= count;
    mq /= count;
    for (unsigned direction = 0; direction < 2; direction++) {
        float c = 1, s = 0;
        float pair_i = 0, pair_q = 0;
        const float dc = -0.8874134471f;
        float ds = direction ? -0.4609743745f : 0.4609743745f;
        hf_iq_integral_i[0] = hf_iq_integral_q[0] = 0;
        for (size_t n = 0; n < count; n++) {
            int ii = samples[n] >> 4, qq = samples[n] & 15;
            float i = (ii < 8 ? ii : ii - 16) - mi;
            float q = (qq < 8 ? qq : qq - 16) - mq;
            pair_i += i * c + q * s;
            pair_q += q * c - i * s;
            if (n & 1u) {
                unsigned k = (n + 1) / 2;
                hf_iq_integral_i[k] = hf_iq_accumulate(hf_iq_integral_i[k - 1], pair_i * 0.5f);
                hf_iq_integral_q[k] = hf_iq_accumulate(hf_iq_integral_q[k - 1], pair_q * 0.5f);
                pair_i = pair_q = 0;
            }
            float next = c * dc - s * ds;
            s = s * dc + c * ds;
            c = next;
        }
        for (unsigned pos = 0; pos + half_sample(64) <= count / 2; pos++) {
            if (!pattern(pos, 0, 0x1d))
                continue;
            size_t length = 0;
            unsigned half = 8;
            while (length < capacity && pos + half_sample(half + 16 + 8) <= count / 2) {
                uint8_t value = 0;
                bool valid = true;
                for (unsigned bit = 0; bit < 8; bit++, half += 2) {
                    float first = power(pos, half), second = power(pos, half + 1);
                    float high = first > second ? first : second;
                    float low = first > second ? second : first;
                    if (high < 0.01f || high < low * 1.5f) {
                        valid = false;
                        break;
                    }
                    if (second > first)
                        value |= 1u << bit;
                }
                if (!valid)
                    break;
                frame[length++] = value;
                if (length >= 3 && pattern(pos, half, 0xb8)) {
                    if (iso15693_check_crc(frame, length)) {
                        *offset = pos * 2;
                        return length;
                    }
                    if (crc_error) *crc_error = true;
                    break;
                }
            }
        }
    }
    return 0;
}
