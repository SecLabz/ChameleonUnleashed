#include "nfcb_decode.h"
#include "hf_iq.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t samples[8192];
static unsigned live_ready, live_count;

/* The ADC DMA exposes a few samples per poll, as a running capture would. */
static unsigned live_available(void) {
    live_ready = live_ready + 7 < live_count ? live_ready + 7 : live_count;
    return live_ready;
}

static bool decode_live(unsigned count, size_t expected, uint8_t *out, unsigned *offset) {
    live_ready = 0;
    live_count = count;
    return nfcb_decode_iq_live(samples, count, expected, out, offset, live_available);
}

static void waveform_at(const uint8_t *data, unsigned length, bool corrupt_crc,
                        bool conjugate, unsigned start) {
    uint8_t frame[14];
    memcpy(frame, data, length);
    uint16_t crc = nfcb_crc_b(data, length);
    frame[length] = crc;
    frame[length + 1] = (crc >> 8) ^ (corrupt_crc ? 1 : 0);
    unsigned nbits = 12 + (length + 2) * 10 + 10;
    uint32_t random = 12345;
    for (unsigned n = 0; n < sizeof(samples); n++) {
        double t = ((double)n - start) / (128.0 / 13.56);
        double amplitude = 0;
        if (t >= -16 && t < nbits) {
            bool bit = true;
            if (t >= 0 && t < 10)
                bit = false;
            else if (t >= 12 && t < 12 + (length + 2) * 10) {
                unsigned symbol = (unsigned)t - 12, pos = symbol % 10;
                bit = pos == 9 || (pos && ((frame[symbol / 10] >> (pos - 1)) & 1));
            } else if (t >= 12 + (length + 2) * 10)
                bit = false;
            amplitude = bit ? 3.0 : -3.0;
        }
        random = random * 1664525u + 1013904223u;
        double noise = ((int)(random >> 24) - 128) / 512.0;
        double phase = 2 * 3.141592653589793 * 0.1525 * n + 0.71;
        int i = (int)lround(amplitude * cos(phase) + 1 + noise);
        int q = (int)lround(amplitude * sin(phase) - 1 - noise);
        if (conjugate) q = -q;
        samples[n] = ((i & 15) << 4) | (q & 15);
    }
}

static void waveform(const uint8_t *data, unsigned length, bool corrupt_crc, bool conjugate) {
    waveform_at(data, length, corrupt_crc, conjugate, 311);
}

int main(void) {
    /* Quantization rounds half away from zero and wraps the prefix sum. */
    assert(hf_iq_accumulate(0, 0.0f) == 0);
    assert(hf_iq_accumulate(0, -0.0f) == 0);
    assert(hf_iq_accumulate(0, 0.031f) == 0);
    assert(hf_iq_accumulate(0, -0.031f) == 0);
    assert(hf_iq_accumulate(0, 0.03125f) == 1);
    assert(hf_iq_accumulate(0, -0.03125f) == UINT16_MAX);
    assert(hf_iq_accumulate(UINT16_MAX, 0.0625f) == 0);
    assert(hf_iq_accumulate(0, -0.0625f) == UINT16_MAX);
    const uint8_t initiate[] = {0x06, 0x00}, uid_command[] = {0x0b};
    assert(nfcb_crc_b(initiate, 2) == 0x5b97);
    assert(nfcb_crc_b(uid_command, 1) == 0x4eab);
    const uint8_t payload[] = {0x50, 0x91, 0xa6, 0x00, 0xff, 0x52, 0x12, 0xd0, 1, 2, 3, 4};
    const unsigned sizes[] = {1, 4, 8, 12};
    for (unsigned test = 0; test < 8; test++) {
        unsigned n = sizes[test % 4], offset;
        uint8_t out[14];
        waveform(payload, n, false, test >= 4);
        assert(nfcb_decode_iq(samples, sizeof(samples), n + 2, out, &offset));
        assert(!memcmp(payload, out, n));
        assert(offset >= 306 && offset <= 316);
        waveform(payload, n, true, test >= 4);
        assert(!nfcb_decode_iq(samples, sizeof(samples), n + 2, out, &offset));
        bool crc_error = false;
        assert(!nfcb_decode_iq_checked(samples, sizeof(samples), n + 2, out, &offset, &crc_error));
        assert(crc_error);
    }
    for (unsigned conjugate = 0; conjugate < 2; conjugate++) {
        uint8_t out[14];
        unsigned offset;
        waveform_at(payload, 4, false, conjugate, 1200);
        assert(nfcb_decode_iq(samples, 2048, 6, out, &offset));
        assert(!memcmp(payload, out, 4));
        assert(offset >= 1195 && offset <= 1205);
        waveform_at(payload, 8, false, conjugate, 1600);
        assert(nfcb_decode_iq(samples, 3072, 10, out, &offset));
        assert(!memcmp(payload, out, 8));
        assert(offset >= 1595 && offset <= 1605);
        waveform_at(payload, 12, false, conjugate, 6500);
        assert(nfcb_decode_iq(samples, sizeof(samples), 14, out, &offset));
        assert(!memcmp(payload, out, 12));
        assert(offset >= 6495 && offset <= 6505);
    }
    /* A decimated lane can prefer another valid frame in a colliding capture. */
    for (unsigned conjugate = 0; conjugate < 2; conjugate++) {
        const uint8_t first_payload[] = {0x50, 0x91, 0xa6, 0};
        const uint8_t second_payload[] = {1, 2, 3, 4};
        uint8_t first[sizeof(samples)], out[14];
        unsigned offset;
        waveform_at(first_payload, 4, false, conjugate, 311);
        memcpy(first, samples, sizeof(first));
        waveform_at(second_payload, 4, false, conjugate, 320);
        for (unsigned n = 0; n < 1216; n++) {
            if (n % 2) { samples[n] = first[n]; continue; }
            int i = samples[n] >> 4, q = samples[n] & 15;
            i = i < 8 ? i : i - 16;
            q = q < 8 ? q : q - 16;
            int dc_q = conjugate ? 1 : -1;
            i = (i - 1) / 2 + 1;
            q = (q - dc_q) / 2 + dc_q;
            samples[n] = ((i & 15) << 4) | (q & 15);
        }
        assert(nfcb_decode_iq(samples, 1216, 6, out, &offset));
        assert(!memcmp(first_payload, out, sizeof(first_payload)) && offset == 310);
    }
    /* Live decoding follows the stream and matches the complete-buffer frame. */
    for (unsigned conjugate = 0; conjugate < 2; conjugate++) {
        uint8_t out[14], live[14];
        unsigned offset, live_offset;
        waveform_at(payload, 4, false, conjugate, 311);
        assert(nfcb_decode_iq(samples, 1216, 6, out, &offset)); /* Learn orientation. */
        assert(decode_live(1216, 6, live, &live_offset) && live_ready < 1216);
        assert(!memcmp(out, live, 6) && live_offset == offset);
        waveform_at(payload, 4, true, conjugate, 311);
        assert(!decode_live(1216, 6, live, &live_offset) && live_ready == 1216);
    }
    uint8_t out[10];
    unsigned offset;
    memset(samples, 0, sizeof(samples));
    assert(!decode_live(sizeof(samples), 3, out, &offset));
    assert(!nfcb_decode_iq_live(samples, 100, 6, out, &offset, live_available));
    assert(!nfcb_decode_iq_live(samples, sizeof(samples), 6, out, &offset, NULL));
    assert(!nfcb_decode_iq(samples, sizeof(samples), 3, out, &offset));
    assert(!nfcb_decode_iq(samples, 100, 10, out, &offset));
    assert(!nfcb_decode_iq(samples, 8193, 3, out, &offset));
    assert(!nfcb_decode_iq(samples, sizeof(samples), 4, out, &offset));
    assert(!nfcb_decode_iq(NULL, sizeof(samples), 3, out, &offset));
    puts("NFC-B IQ: Chip_ID, block and UID frames, CRC rejection, silence and bounds passed");
    return 0;
}
