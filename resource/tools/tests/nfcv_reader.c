#include "nfcv_reader_codec.h"
#include "iso15693/frame.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t samples[NFCV_READER_IQ_SAMPLES];
static unsigned waveform_start = 421;
static void waveform(const uint8_t *payload, size_t length, bool reverse, bool corrupt) {
    uint8_t frame[24], halves[8 + 24 * 16 + 8];
    memcpy(frame, payload, length);
    iso15693_append_crc(frame, length);
    if (corrupt)
        frame[length] ^= 1;
    const uint8_t sof[] = {0, 0, 0, 1, 1, 1, 0, 1};
    const uint8_t eof[] = {1, 0, 1, 1, 1, 0, 0, 0};
    memcpy(halves, sof, 8);
    size_t used = 8;
    for (size_t b = 0; b < length + 2; b++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            bool one = (frame[b] >> bit) & 1;
            halves[used++] = !one;
            halves[used++] = one;
        }
    }
    memcpy(halves + used, eof, 8);
    used += 8;
    uint32_t random = 123456;
    for (unsigned n = 0; n < sizeof(samples); n++) {
        double half = ((double)n - waveform_start) / (256.0 / 13.56);
        double amplitude = half >= 0 && half < used && halves[(unsigned)half] ? 3 : 0;
        double phase = (reverse ? -1 : 1) * 2 * 3.141592653589793 * 0.42375 * n + 0.42;
        random = random * 1664525u + 1013904223u;
        double noise = ((int)(random >> 24) - 128) / 512.0;
        int i = lround(amplitude * cos(phase) + 1 + noise);
        int q = lround(amplitude * sin(phase) - 1 - noise);
        samples[n] = ((i & 15) << 4) | (q & 15);
    }
}

int main(void) {
    uint8_t levels[NFCV_READER_MAX_TICKS], byte = 0xe4;
    assert(nfcv_reader_encode(&byte, 1, levels, sizeof(levels)) == 44);
    for (unsigned i = 0; i < 44; i++)
        assert(levels[i] == (i == 0 || i == 5 || i == 9 || i == 19 || i == 29 ||
                             i == 39 || i == 42));
    assert(!nfcv_reader_encode(&byte, 1, levels, 43));
    assert(!nfcv_reader_encode(NULL, 1, levels, sizeof(levels)));
    const uint8_t inventory[] = {0, 0x12, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x04, 0xe0};
    const uint8_t block[] = {0, 0x12, 0x34, 0x56, 0x78};
    const uint8_t info[] = {0, 0x0f, 6, 5, 4, 3, 2, 1, 4, 0xe0, 0, 0, 63, 3, 0};
    uint8_t out[24];
    unsigned offset;
    for (unsigned direction = 0; direction < 2; direction++) {
        waveform(inventory, sizeof(inventory), direction, false);
        assert(nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset) == 12);
        assert(!memcmp(out, inventory, sizeof(inventory)));
        assert(offset >= 402 && offset <= 440);
        assert(!nfcv_reader_decode_iq(samples, sizeof(samples), out, 7, &offset));
        waveform(block, sizeof(block), direction, false);
        assert(nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset) == 7);
        assert(!memcmp(out, block, sizeof(block)));
        waveform(block, sizeof(block), direction, true);
        assert(!nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset));
    }
    waveform_start = 3100;
    waveform(info, sizeof(info), false, false);
    assert(nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset) == 17);
    assert(!memcmp(out, info, sizeof(info)));
    assert(offset >= 3081 && offset <= 3119);
    uint8_t multiple[21] = {0};
    for (unsigned i = 1; i < sizeof(multiple); i++) multiple[i] = i * 7;
    for (unsigned direction = 0; direction < 2; direction++) {
        waveform(multiple, sizeof(multiple), direction, false);
        assert(nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset) == 23);
        assert(!memcmp(out, multiple, sizeof(multiple)));
        assert(!nfcv_reader_decode_iq(samples, sizeof(samples), out, 22, &offset));
        waveform(multiple, sizeof(multiple), direction, true);
        assert(!nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset));
    }
    memset(samples, 0, sizeof(samples));
    assert(!nfcv_reader_decode_iq(samples, sizeof(samples), out, sizeof(out), &offset));
    assert(!nfcv_reader_decode_iq(samples, NFCV_READER_IQ_SAMPLES + 1, out, sizeof(out), &offset));
    assert(!nfcv_reader_decode_iq(samples, 100, out, sizeof(out), &offset));
    puts("NFC-V reader: PPM framing, both I/Q orientations, inventory/block CRCs and bounds passed");
}
