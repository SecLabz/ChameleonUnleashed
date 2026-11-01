#include "hf_iq.h"
/* BPSK recovery from the 522's signed four-bit I/Q test ADC at 1 MSample/s.
 * The 847.5 kHz subcarrier aliases to 152.5 kHz. No native Type-B decoder is used.
 */
#include "nfcb_decode.h"

uint16_t nfcb_crc_b(const uint8_t *bytes, size_t size) {
    uint16_t crc = 0xffff;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    }
    return ~crc;
}

static unsigned etu_sample(unsigned etu) {
    return (etu * 12800u + 678u) / 1356u;
}

/* The 1 MSample/s alias completes 61 subcarrier cycles every 400 samples. */
#define SUBCARRIER_PERIOD 400u
#define SUBCARRIER_STEP 61u
#define SUBCARRIER_QUARTER 100u
/* Fixed-point mixer: Q10 DC-removed ADC samples times Q14 cosine, emitted at
 * the 1/16 integral scale shared with the float candidate checks. */
#define MEAN_SHIFT 10
#define COSINE_SHIFT 14
#define MIX_SHIFT (MEAN_SHIFT + COSINE_SHIFT - 4)
/* Live DC estimate: about 20 subcarrier periods, before or within the reply. */
#define LIVE_MEAN_SAMPLES 131u

static int16_t cosine[SUBCARRIER_PERIOD];
/* Frontend ADC orientation is stable across exchanges; learn it from CRC-B. */
static bool conjugate_first;

static void build_cosine(void) {
    if (cosine[0]) return;
    /* cos/sin(2*pi/400); one recurrence pass avoids libm and a constant table. */
    const float dc = 0.9998766325f, ds = 0.0157073173f;
    float c = 1, s = 0;
    for (unsigned k = 0; k < SUBCARRIER_PERIOD; k++) {
        cosine[k] = (int16_t)(c * (1 << COSINE_SHIFT) + (c < 0 ? -0.5f : 0.5f));
        float next = c * dc - s * ds;
        s = s * dc + c * ds;
        c = next;
    }
}

static int32_t mean_q10(int32_t sum, size_t count) {
    int32_t half = (int32_t)count / 2;
    return (sum * (1 << MEAN_SHIFT) + (sum < 0 ? -half : half)) / (int32_t)count;
}

static float project(unsigned begin, unsigned end, float ri, float rq) {
    return hf_iq_difference(hf_iq_integral_i, begin, end) * ri +
           hf_iq_difference(hf_iq_integral_q, begin, end) * rq;
}

/* Align loops to limit layout-dependent decoding slowdowns. This changes code
 * placement, not decoding arithmetic or programmed RF delays.
 */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("align-loops=16")))
#endif
static bool decode_iq(const uint8_t *samples, size_t count, size_t expected, uint8_t *out,
        unsigned *offset, bool conjugate, int32_t mean_i, int32_t mean_q, bool *crc_error,
        unsigned (*available)(void)) {
    if (!samples || !out || !offset || count > 8192 ||
            (expected != 3 && expected != 6 && expected != 10 && expected != 14))
        return false;
    unsigned total = etu_sample(12 + expected * 10 + 10);
    if (count < total)
        return false;
    hf_iq_prepare(false);
    build_cosine();
    /* Reversing the oscillator conjugates the recovered Q axis: sin(x) is
     * cos(x - pi/2), and -sin(x) is cos(x + pi/2). */
    unsigned phase = 0, sine_offset = conjugate ? SUBCARRIER_QUARTER : 3 * SUBCARRIER_QUARTER;
    hf_iq_integral_i[0] = hf_iq_integral_q[0] = 0;
    unsigned low_end = etu_sample(10), high_end = etu_sample(12);
    float low_scale = 1.0f / low_end;
    float high_scale = 1.0f / (high_end - low_end);
    size_t ready = available ? 0 : count;
    for (size_t n = 0; n < count; n++) {
        while (n >= ready) ready = available();
        unsigned sample = samples[n];
        int32_t i = ((int8_t)sample >> 4) * (1 << MEAN_SHIFT) - mean_i;
        int32_t q = ((int)((sample & 15u) ^ 8u) - 8) * (1 << MEAN_SHIFT) - mean_q;
        unsigned sine = phase + sine_offset;
        if (sine >= SUBCARRIER_PERIOD) sine -= SUBCARRIER_PERIOD;
        int32_t c = cosine[phase], s = cosine[sine];
        const int32_t round = 1 << (MIX_SHIFT - 1);
        hf_iq_integral_i[n + 1] = (uint16_t)(hf_iq_integral_i[n] + ((i * c + q * s + round) >> MIX_SHIFT));
        hf_iq_integral_q[n + 1] = (uint16_t)(hf_iq_integral_q[n] + ((q * c - i * s + round) >> MIX_SHIFT));
        phase += SUBCARRIER_STEP;
        if (phase >= SUBCARRIER_PERIOD) phase -= SUBCARRIER_PERIOD;
        if (n + 1 < total) continue;
        unsigned pos = n + 1 - total;
        /* Reject matching SOF phases before float normalization. ADC/window
         * bounds keep the dot product in int32_t; leave rounding margin at zero. */
        int16_t low_i = (uint16_t)(hf_iq_integral_i[pos + low_end] - hf_iq_integral_i[pos]);
        int16_t low_q = (uint16_t)(hf_iq_integral_q[pos + low_end] - hf_iq_integral_q[pos]);
        int16_t high_i = (uint16_t)(hf_iq_integral_i[pos + high_end] - hf_iq_integral_i[pos + low_end]);
        int16_t high_q = (uint16_t)(hf_iq_integral_q[pos + high_end] - hf_iq_integral_q[pos + low_end]);
        if ((int32_t)low_i * high_i + (int32_t)low_q * high_q > 1024) continue;
        float li = hf_iq_difference(hf_iq_integral_i, pos, pos + low_end) * low_scale;
        float lq = hf_iq_difference(hf_iq_integral_q, pos, pos + low_end) * low_scale;
        float hi = hf_iq_difference(hf_iq_integral_i, pos + low_end, pos + high_end) *
                   high_scale;
        float hq = hf_iq_difference(hf_iq_integral_q, pos + low_end, pos + high_end) *
                   high_scale;
        float ll = li * li + lq * lq, hh = hi * hi + hq * hq;
        if (ll < 0.01f || hh < 0.01f || li * hi + lq * hq >= 0 ||
                hh < ll * 0.04f || ll < hh * 0.04f)
            continue;
        float ri = hi - li, rq = hq - lq;
        bool valid = true;
        for (unsigned b = 0; b < expected && valid; b++) {
            unsigned etu = 12 + b * 10;
            if (project(pos + etu_sample(etu), pos + etu_sample(etu + 1), ri, rq) > 0 ||
                    project(pos + etu_sample(etu + 9), pos + etu_sample(etu + 10),
                            ri, rq) <= 0) {
                valid = false;
                break;
            }
            uint8_t byte = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                unsigned begin = pos + etu_sample(etu + bit + 1);
                unsigned end = pos + etu_sample(etu + bit + 2);
                if (project(begin, end, ri, rq) > 0)
                    byte |= 1u << bit;
            }
            out[b] = byte;
        }
        if (!valid)
            continue;
        unsigned eof = 12 + expected * 10;
        for (unsigned bit = 0; bit < 10 && valid; bit++)
            valid = project(pos + etu_sample(eof + bit), pos + etu_sample(eof + bit + 1),
                            ri, rq) < 0;
        uint16_t crc = nfcb_crc_b(out, expected - 2);
        if (valid && crc_error && (out[expected - 2] != (uint8_t)crc ||
                out[expected - 1] != (uint8_t)(crc >> 8))) *crc_error = true;
        if (valid && out[expected - 2] == (uint8_t)crc &&
                out[expected - 1] == (uint8_t)(crc >> 8)) {
            *offset = pos;
            return true;
        }
    }
    return false;
}

bool nfcb_decode_iq(const uint8_t *samples, size_t count, size_t expected,
                    uint8_t *out, unsigned *offset) {
    return nfcb_decode_iq_checked(samples, count, expected, out, offset, NULL);
}

bool nfcb_decode_iq_checked(const uint8_t *samples, size_t count, size_t expected,
        uint8_t *out, unsigned *offset, bool *crc_error) {
    if (!samples || !count || count > 8192) return false;
    int32_t sum_i = 0, sum_q = 0;
    for (size_t n = 0; n < count; n++) {
        unsigned sample = samples[n];
        sum_i += (int8_t)sample >> 4;
        sum_q += (int)((sample & 15u) ^ 8u) - 8;
    }
    int32_t mean_i = mean_q10(sum_i, count), mean_q = mean_q10(sum_q, count);
    if (decode_iq(samples, count, expected, out, offset, conjugate_first, mean_i, mean_q,
                  crc_error, NULL))
        return true;
    if (!decode_iq(samples, count, expected, out, offset, !conjugate_first, mean_i, mean_q,
                   crc_error, NULL))
        return false;
    conjugate_first = !conjugate_first;
    return true;
}

bool nfcb_decode_iq_live(const uint8_t *samples, size_t count, size_t expected,
        uint8_t *out, unsigned *offset, unsigned (*available)(void)) {
    if (!samples || !available || count < LIVE_MEAN_SAMPLES || count > 8192) return false;
    while (available() < LIVE_MEAN_SAMPLES) {}
    int32_t sum_i = 0, sum_q = 0;
    for (size_t n = 0; n < LIVE_MEAN_SAMPLES; n++) {
        unsigned sample = samples[n];
        sum_i += (int8_t)sample >> 4;
        sum_q += (int)((sample & 15u) ^ 8u) - 8;
    }
    return decode_iq(samples, count, expected, out, offset, conjugate_first,
                     mean_q10(sum_i, LIVE_MEAN_SAMPLES), mean_q10(sum_q, LIVE_MEAN_SAMPLES),
                     NULL, available);
}
