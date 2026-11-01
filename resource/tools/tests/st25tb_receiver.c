#include "st25tb/receiver.h"
#include "iso15693/frame.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static unsigned clock_scale = 1000;
static unsigned clock_origin;
static unsigned offset_quarters;
static unsigned channel_mode;
static unsigned chunk_size = 2;
static unsigned eof_bits = 10;
static unsigned eof_noise;
/* Start 100 or 132 us before the frame with the ADC offset left by load modulation. */
static unsigned recovery_start;
static bool after_reply;
static unsigned start_erasures[ST25TB_REQUEST_MAX];
static bool continue_stream;
static unsigned crc_calls;

/* Only receiver.c redirects its production CRC calls to this test counter. */
uint16_t st25tb_test_crc16(const uint8_t *data, uint16_t length) {
    crc_calls++;
    return iso15693_crc16(data, length);
}

static unsigned encode(uint8_t *bits, const uint8_t *frame, unsigned size, unsigned low, unsigned high,
                       unsigned end, unsigned bad_stop_byte) {
    unsigned count = 0;
    for (unsigned n = 0; n < low; n++) {
        bits[count++] = 0;
    }
    for (unsigned n = 0; n < high; n++) {
        bits[count++] = 1;
    }
    for (unsigned n = 0; n < size; n++) {
        bits[count++] = 0;
        for (unsigned b = 0; b < 8; b++) {
            bits[count++] = (frame[n] >> b) & 1;
        }
        bits[count++] = !(bad_stop_byte && n + 1 == bad_stop_byte);
    }
    for (unsigned n = 0; n < end; n++) {
        bits[count++] = 0;
    }
    return count;
}

static int quantize(double value) {
    int sample = (int)round(value - 0.5);
    if (sample < -8) {
        sample = -8;
    }
    if (sample > 7) {
        sample = 7;
    }
    return sample;
}

static void receive_samples(const uint8_t *samples, unsigned size, const uint8_t *expected, unsigned expected_size,
                            unsigned batch_bytes) {
    uint8_t result[ST25TB_REQUEST_MAX];
    unsigned found = 0;
    for (unsigned n = 0; n < size;) {
        unsigned bytes = size - n;
        unsigned length = 0;
        unsigned consumed;
        if (!batch_bytes) {
            st25tb_receiver_pair(samples[n], samples[n + 1], result, &length);
            consumed = 2;
        } else {
            if (bytes > batch_bytes) {
                bytes = batch_bytes;
            }
            consumed = st25tb_receiver_batch(samples + n, bytes, result, &length);
        }
        assert(consumed && consumed <= bytes && !(consumed & 1));
        n += consumed;
        if (length) {
            assert(length == expected_size && !memcmp(expected, result, length));
            found++;
        }
    }
    assert(found == (unsigned)(expected_size != 0));
}

static void receive(const uint8_t *payload, unsigned size, int polarity, unsigned tau, unsigned low, unsigned high,
                    bool bad_crc, unsigned bad_stop_byte) {
    uint8_t frame[8];
    uint8_t bits[150];
    memcpy(frame, payload, size);
    iso15693_append_crc(frame, size);
    size += 2;
    if (bad_crc) {
        frame[size - 1] ^= 1;
    }
    unsigned count = encode(bits, frame, size, low, high, eof_bits, bad_stop_byte);
    for (unsigned n = 0; n < 40; n++) {
        bits[count++] = 1;
    }
    bool expected = low == 10 && high == 2 && eof_bits == 10 && !bad_crc && !bad_stop_byte;
    double baseline = 1;
    uint8_t samples[3000];
    uint8_t previous[2] = {0, 0};
    if (!continue_stream) {
        st25tb_receiver_reset();
        st25tb_receiver_time(clock_origin);
    }
    assert(st25tb_receiver_idle());
    if (after_reply) {
        st25tb_receiver_settle();
    }
    for (unsigned t = 0; t < 3000; t++) {
        int quarters = ((int)t - 1000) * 4 - (int)offset_quarters;
        unsigned bit = quarters < 0 ? count : (uint64_t)quarters * 1356 * 1000 / (51200 * clock_scale);
        double level = bit < count ? bits[bit] : 1;
        baseline += (level - baseline) / tau;
        double recovery = after_reply && t >= recovery_start ? -6 * exp(-((double)t - recovery_start) / 12) : 0;
        int sample = quantize(polarity * (level - baseline) * 10 + recovery);
        /* Suppress a character's first rising transient, as in the captured GET_UID. */
        if (bit >= low + high && bit < low + high + size * 10) {
            unsigned character = (bit - low - high) / 10;
            unsigned offset = (bit - low - high) % 10;
            if (start_erasures[character] && offset == start_erasures[character]) {
                sample = -1;
            }
        }
        uint8_t adc = (sample & 15) << 4;
        uint8_t q = channel_mode == 4 ? previous[t % 2] : adc;
        previous[t % 2] = adc;
        /* I only, Q only, equal axes, opposite axes, or Q delayed by 2 us. */
        if (channel_mode == 0) {
            samples[t] = adc;
        } else if (channel_mode == 1) {
            samples[t] = adc >> 4;
        } else if (channel_mode == 3) {
            samples[t] = adc | ((-sample) & 15);
        } else {
            samples[t] = adc | (q >> 4);
        }
        if (eof_noise && t >= 1586 && t < 1588) {
            samples[t] |= 7;
        }
        if (eof_noise == 2) {
            /* A previous false SOF on Q must not qualify I's later frame. */
            if ((t >= 200 && t < 202) || (t >= 313 && t < 315)) {
                samples[t] |= 9;
            } else if (t >= 294 && t < 296) {
                samples[t] |= 7;
            }
        }
    }
    unsigned start = after_reply ? recovery_start : 0;
    receive_samples(samples + start, sizeof(samples) - start, frame, expected ? size : 0,
                    channel_mode ? chunk_size : 0);
}

/* A malformed three-byte prefix has no EOF. Its following SOF must remain
 * detectable after a rejected EOF or repair of a weak axis from its peer. */
static void receive_after_invalid_prefix(unsigned low, unsigned clock, bool delayed_q, bool erased_start) {
    const uint8_t prefix[] = {0, 0, 0};
    const struct {
        bool prefix;
        bool target;
        bool bad_crc;
        unsigned end;
    } cases[] = {{false, true, false, 10}, {true, false, false, 10}, {true, true, true, 10},
                 {true, true, false, 9}, {true, true, false, 10}};
    assert(!iso15693_check_crc(prefix, sizeof(prefix)));
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t frame[4] = {0x06, 0x00};
        iso15693_append_crc(frame, 2);
        if (cases[c].bad_crc) {
            frame[3] ^= 1;
        }
        uint8_t bits[200];
        unsigned count = encode(bits, prefix, sizeof(prefix), 10, 2, 0, 0);
        if (!cases[c].prefix) {
            /* Keep the isolated control's SOF at the same sampling phase. */
            memset(bits, 1, count);
        }
        if (cases[c].target) {
            count += encode(bits + count, frame, sizeof(frame), low, 2, cases[c].end, 0);
        }
        for (unsigned n = 0; n < 40; n++) {
            bits[count++] = 1;
        }
        double baseline[2] = {1, 1};
        uint8_t history[2] = {0, 0};
        uint8_t samples[3600];
        for (unsigned t = 0; t < sizeof(samples); t++) {
            int quarters = ((int)t - 1000) * 4;
            unsigned bit = quarters < 0 ? count : (uint64_t)quarters * 1356 * 1000 / (51200 * clock);
            double level = bit < count ? bits[bit] : 1;
            int values[2];
            for (unsigned axis = 0; axis < 2; axis++) {
                /* One continuous NRZ/AC waveform, independently quantized on
                 * both axes before saturation. */
                baseline[axis] += (level - baseline[axis]) / 3;
                values[axis] = quantize(-(level - baseline[axis]) * 10);
            }
            /* Erase only I's third prefix start fall. The continuous analog
             * baselines still see every transmitted NRZ level. */
            if (erased_start && cases[c].prefix && bit == 12 + 2 * 10) {
                values[0] = -1;
            }
            uint8_t q = delayed_q ? history[t % 2] : values[1] & 15;
            history[t % 2] = values[1] & 15;
            samples[t] = ((values[0] & 15) << 4) | q;
        }
        const unsigned chunks[] = {2, 64, 127};
        bool expected = cases[c].target && !cases[c].bad_crc && cases[c].end == 10;
        for (unsigned orientation = 0; orientation < 2; orientation++) {
            for (unsigned n = 0; n < sizeof(chunks) / sizeof(chunks[0]); n++) {
                st25tb_receiver_reset();
                receive_samples(samples, sizeof(samples), frame, expected ? sizeof(frame) : 0, chunks[n]);
            }
            for (unsigned n = 0; n < sizeof(samples); n++) {
                samples[n] = (samples[n] << 4) | (samples[n] >> 4);
            }
        }
    }
}

static void run(bool weak_edges) {
    const unsigned decay[] = {3, 5, 10, 20};
    for (unsigned address = 0; address < 17; address++) {
        uint8_t read[] = {8, address == 16 ? 255 : address};
        for (int polarity = -1; polarity <= 1; polarity += 2) {
            for (unsigned n = weak_edges ? 0 : 1; n < 4; n++) {
                receive(read, 2, polarity, decay[n], 10, 2, false, false);
            }
        }
    }
    /* In the last WRITE, 00's long low run leaves a SOF search anchor.
     * The following ff's long high run must remain data before byte02. */
    const uint8_t commands[][6] = {{6, 0}, {14, 90}, {11}, {9, 0, 18, 52, 86, 120}, {9, 0, 0xff, 2, 0x34, 0x56}};
    const unsigned lengths[] = {2, 2, 1, 6, 6};
    for (unsigned n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
        for (int polarity = -1; polarity <= 1; polarity += 2) {
            for (unsigned d = weak_edges ? 0 : 1; d < 4; d++) {
                receive(commands[n], lengths[n], polarity, decay[d], 10, 2, false, false);
            }
        }
    }
    uint8_t read[] = {8, 8};
    receive(read, 2, 1, 5, 6, 2, false, false);
    receive(read, 2, 1, 5, 14, 2, false, false);
    receive(read, 2, 1, 5, 10, 1, false, false);
    receive(read, 2, 1, 5, 10, 4, false, false);
    receive(read, 2, 1, 5, 10, 2, true, false);
    receive(read, 2, 1, 5, 10, 2, false, true);
}

static void receive_start_erasure(void) {
    const uint8_t uid[] = {0x0b};
    start_erasures[1] = 1;
    for (channel_mode = 0; channel_mode < 5; channel_mode++) {
        for (int polarity = -1; polarity <= 1; polarity += 2) {
            receive(uid, sizeof(uid), polarity, 5, 10, 2, false, 0);
            receive(uid, sizeof(uid), polarity, 5, 10, 2, true, 0);
            receive(uid, sizeof(uid), polarity, 5, 10, 2, false, 2);
            const unsigned invalid_eof[] = {0, 9, 11};
            for (unsigned n = 0; n < sizeof(invalid_eof) / sizeof(invalid_eof[0]); n++) {
                eof_bits = invalid_eof[n];
                receive(uid, sizeof(uid), polarity, 5, 10, 2, false, 0);
            }
            eof_bits = 10;
        }
    }
    channel_mode = 2;
    for (start_erasures[1] = 1; start_erasures[1] <= 7; start_erasures[1]++) {
        uint8_t read[] = {0x08, 1u << (start_erasures[1] - 1)};
        for (int polarity = -1; polarity <= 1; polarity += 2) {
            receive(read, sizeof(read), polarity, 5, 10, 2, false, 0);
            receive(read, sizeof(read), polarity, 5, 10, 2, true, 0);
            receive(read, sizeof(read), polarity, 5, 10, 2, false, 2);
        }
    }
    /* An unambiguous first-byte repair must not hide the next byte's CRC choice. */
    const uint8_t two_gaps[] = {0x01, 0x02};
    start_erasures[0] = 1;
    start_erasures[1] = 2;
    receive(two_gaps, sizeof(two_gaps), 1, 5, 10, 2, false, 0);
    receive(two_gaps, sizeof(two_gaps), 1, 5, 10, 2, true, 0);
    /* Both erased rises require a CRC-selected low-run split. */
    for (start_erasures[0] = 2; start_erasures[0] <= 7; start_erasures[0]++) {
        for (start_erasures[1] = 2; start_erasures[1] <= 7; start_erasures[1]++) {
            const uint8_t two_ambiguous[] = {1u << (start_erasures[0] - 1), 1u << (start_erasures[1] - 1)};
            const uint8_t long_ambiguous[] = {two_ambiguous[0], two_ambiguous[1], 0x12, 0x34, 0x56, 0x78};
            for (channel_mode = 0; channel_mode < 5; channel_mode++) {
                for (int polarity = -1; polarity <= 1; polarity += 2) {
                    receive(two_ambiguous, sizeof(two_ambiguous), polarity, 5, 10, 2, false, 0);
                    receive(long_ambiguous, sizeof(long_ambiguous), polarity, 5, 10, 2, false, 0);
                    receive(two_ambiguous, sizeof(two_ambiguous), polarity, 5, 10, 2, true, 0);
                    receive(two_ambiguous, sizeof(two_ambiguous), polarity, 5, 10, 2, false, 2);
                }
            }
        }
    }
    start_erasures[0] = 0;
    start_erasures[1] = 0;
}

/* Many ambiguous bytes with a corrupt CRC must leave the receiver ready for
 * the next complete request, without resetting its streaming state. */
static void receive_ambiguous_noise(void) {
    const uint8_t noise[] = {0x40, 0x40, 0x40, 0x40, 0x40, 0x40};
    const uint8_t read[] = {0x08, 0x02};
    channel_mode = 2;
    for (unsigned i = 0; i < sizeof(noise); i++) {
        start_erasures[i] = 7;
    }
    crc_calls = 0;
    receive(noise, sizeof(noise), 1, 5, 10, 2, true, 0);
    /* Both axes reach EOF; each has at most 16 CRC evaluations. */
    assert(crc_calls && crc_calls <= 32);
    memset(start_erasures, 0, sizeof(start_erasures));
    continue_stream = true;
    receive(read, sizeof(read), 1, 5, 10, 2, false, 0);
    continue_stream = false;
}

/* Before the real SOF rise at 143, noise acquires a false frame on Q at 98
 * and I at 100. I's 43-tick run still fits nine bits, while Q rejects 45 ticks.
 * The peer has completed no character, so it must not hide the new SOF on Q. */
static void receive_after_false_frame(void) {
    const uint8_t expected[] = {0x08, 0x02, 0x95, 0xe2};
    uint8_t samples[1200] = {0};
    const struct {
        unsigned tick;
        uint8_t value;
    } edges[] = {{43, 0x99}, {90, 0x77}, {98, 0x09}, {100, 0x90}, {143, 0x77}, {153, 0x99}};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        samples[2 * edges[i].tick] = edges[i].value;
        samples[2 * edges[i].tick + 1] = edges[i].value;
    }
    bool previous = false;
    for (unsigned bit = 0; bit <= 50; bit++) {
        bool level = false;
        if (bit < 40) {
            unsigned offset = bit % 10;
            if (offset == 9) {
                level = true;
            } else if (offset) {
                level = (expected[bit / 10] >> (offset - 1)) & 1;
            }
        } else if (bit == 50) {
            level = true;
        }
        if (level != previous) {
            unsigned tick = 153 + (bit * 1600 + 169) / 339;
            uint8_t value = level ? 0x77 : 0x99;
            samples[2 * tick] = value;
            samples[2 * tick + 1] = value;
            previous = level;
        }
    }
    uint8_t frame[ST25TB_REQUEST_MAX];
    unsigned length;
    st25tb_receiver_reset();
    st25tb_receiver_batch(samples, sizeof(samples), frame, &length);
    assert(length == sizeof(expected) && !memcmp(frame, expected, sizeof(expected)));
}

/* Captured Proxmark3 requests: ADC pairs with I in the high and Q in the low nibble. */
/* SELECT 0e a1: I keeps every data edge but misses EOF; Q sees EOF but loses byte 0. */
static const uint8_t select_split[] = {
    0x0f, 0x00, 0x00, 0x0f, 0xff, 0xff, 0x0f, 0x0f, 0x00, 0x0f, 0xf1, 0xfd, 0xf0, 0x00, 0x0f, 0x0f, 0xf0, 0x00, 0xfe,
    0xf0, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x0f, 0xff, 0xff, 0x0f, 0x02, 0x0e, 0xf0, 0x00, 0x78, 0x78, 0x78, 0x60,
    0x20, 0x01, 0xf1, 0xf2, 0xf1, 0xf1, 0xe0, 0xff, 0xf0, 0xf0, 0xf1, 0xf0, 0x0f, 0xf0, 0xff, 0x0f, 0x00, 0x0e, 0xf1,
    0xfe, 0x00, 0x0f, 0x0f, 0xf0, 0xf1, 0x0f, 0xff, 0x00, 0x00, 0x0f, 0x0f, 0xf1, 0x0f, 0x0f, 0xff, 0xf0, 0xf0, 0x00,
    0x0f, 0x0f, 0xf0, 0x0f, 0xf0, 0x00, 0xf1, 0x0e, 0x00, 0x00, 0xff, 0xff, 0xf0, 0x00, 0x0f, 0xf0, 0x0f, 0x0f, 0x00,
    0x0f, 0x0f, 0x0f, 0x0f, 0xf0, 0xf0, 0xf0, 0xf1, 0x0f, 0x00, 0xff, 0xff, 0x00, 0x00, 0x0f, 0xff, 0x0f, 0xf0, 0xf0,
    0x0f, 0x00, 0x00, 0xf0, 0xff, 0xff, 0xff, 0x0e, 0xf1, 0xff, 0x00, 0x00, 0x0f, 0x87, 0x87, 0x84, 0x80, 0xe0, 0x0f,
    0x1e, 0x10, 0x1e, 0x1e, 0x1f, 0x1e, 0x0f, 0x00, 0x0e, 0x0f, 0x0f, 0x00, 0x0e, 0x78, 0x7e, 0x7f, 0x70, 0x31, 0x11,
    0x01, 0xf1, 0xf1, 0xe2, 0xff, 0xf0, 0xe0, 0xf0, 0xf0, 0xff, 0xff, 0xf0, 0xf0, 0x83, 0x81, 0x80, 0x9f, 0xef, 0x0f,
    0x1f, 0x1f, 0x10, 0x1d, 0x1f, 0x1f, 0x0f, 0x0f, 0x0f, 0x01, 0x0f, 0x00, 0x0f, 0x00, 0xf0, 0x00, 0x0f, 0x0f, 0xf0,
    0x0f, 0x00, 0x78, 0x74, 0x72, 0x71, 0x31, 0x11, 0x00, 0xf0, 0xe1, 0xff, 0xe0, 0xe0, 0xf0, 0xf0, 0xff, 0xe1, 0xff,
    0xff, 0xff, 0x00, 0x00, 0x0f, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x0f, 0xf0, 0xf0, 0x00, 0x00, 0x0f, 0x0f,
    0xff, 0xff, 0x88, 0x8a, 0x8d, 0x80, 0xf1, 0x01, 0x10, 0x10, 0x11, 0x51, 0x77, 0x76, 0x72, 0x61, 0x30, 0x10, 0x1e,
    0x0f, 0x0f, 0x88, 0x88, 0x8d, 0xb1, 0xff, 0x00, 0x10, 0x20, 0x10, 0x74, 0x77, 0x77, 0x72, 0x51, 0x20, 0x1f, 0x0f,
    0x0f, 0x0f, 0x0f, 0xff, 0xff, 0x0f, 0xf0, 0x0f, 0xff, 0x00, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0xff, 0x00, 0xf0,
    0xf0, 0x0e, 0x0f, 0x00, 0x0f, 0x0f, 0xf0, 0xf0, 0x0f, 0xf0, 0x00, 0x88, 0x88, 0x8b, 0xbf, 0xe0, 0x01, 0x20, 0x10,
    0x63, 0x77, 0x77, 0x75, 0x52, 0x20, 0x10, 0x1f, 0x0f, 0x0f, 0x88, 0x88, 0x89, 0xde, 0xf0, 0x01, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x0f, 0x77, 0x77, 0x77, 0x42, 0x11, 0x00, 0xff, 0xef, 0xef,
    0xef, 0xef, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xe0, 0x0f, 0xf0, 0x00, 0xf0, 0x0f, 0x0f, 0xf0, 0xf0, 0x0f, 0x00, 0xf0,
    0x88, 0x88, 0x88, 0xcd, 0xf0, 0x00, 0x11, 0x11, 0x11, 0x65, 0x77, 0x77, 0x77, 0x43, 0x21, 0x11, 0x00, 0x0f, 0xff,
    0x88, 0x88, 0x88, 0xdd, 0xff, 0x00, 0x20, 0x11, 0x10, 0x77, 0x77, 0x77, 0x77, 0x33, 0x11, 0x00, 0x00, 0x0f, 0xff,
    0x88, 0x88, 0x88, 0xed, 0x0f, 0x00, 0x20, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xf0, 0x77, 0x77, 0x77, 0x37, 0x12, 0x00, 0xff, 0xef, 0xa8, 0x88,
    0x88, 0xc8, 0xed, 0xff, 0x0f, 0xf0, 0x00, 0x00, 0x77, 0x77, 0x77, 0x26, 0x01, 0x00, 0xff, 0xef, 0xee, 0xee, 0xee,
    0xfe, 0xee, 0xff, 0xff, 0xff, 0xff, 0x0f, 0xff, 0xf0, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0xf0,
    0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x88, 0x88, 0xe8, 0x0d, 0x1f, 0x10, 0x21, 0x11, 0x11, 0x77, 0x77,
    0x77, 0x37, 0x24, 0x12, 0x01, 0x01, 0x00, 0xef, 0x0f, 0xff, 0x0f, 0x0f, 0xff, 0xff, 0xff, 0x00, 0xff, 0x88, 0x88,
    0xd8, 0xfc, 0x2f, 0x10, 0x11, 0x11, 0x11, 0x77, 0x77, 0x77, 0x37, 0x25, 0x02, 0x01, 0x00, 0xff, 0x0f, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x0f, 0x00, 0xff, 0x0f, 0xf0, 0xf0, 0x00, 0x0f, 0x00, 0xf0, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0x00, 0xf0, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xf0, 0x00, 0xff, 0xf0, 0x00, 0x0f, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xf0, 0x00, 0x00, 0xf0, 0xff, 0xff, 0x00,
    0xf0, 0xf0, 0x0f, 0x00, 0x01, 0xff, 0x00, 0xff, 0xff, 0xfe, 0x00, 0x00, 0xff, 0x00, 0xf0, 0xf0, 0xf0, 0x0f, 0x00,
    0xf0, 0xff, 0xff, 0xf0, 0xff, 0x00, 0xe8, 0xf8, 0xf8, 0x0c, 0x0f, 0x02, 0x01, 0x01, 0x02, 0x01,
};
/* INITIATE 06 00: Q loses byte 0, I loses the last byte before EOF. */
static const uint8_t initiate_split[] = {
    0xff, 0x00, 0xf0, 0xfe, 0xf0, 0x00, 0xf0, 0xf0, 0x0f, 0xff, 0xff, 0xff, 0x00, 0xf0, 0x00, 0xf0, 0x0e, 0xf0, 0xf0,
    0xff, 0xff, 0x0f, 0xff, 0xff, 0x00, 0x0f, 0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0xf0, 0xf0, 0x87, 0x87, 0xb6, 0xf2,
    0x00, 0x1f, 0x1f, 0x1e, 0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0xff, 0xff, 0xf0, 0xf0, 0x00,
    0x00, 0xff, 0xff, 0xf0, 0x00, 0x0f, 0xf0, 0x00, 0xff, 0xff, 0x0f, 0xf0, 0xf0, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xff,
    0xff, 0xf0, 0xf0, 0xf0, 0x00, 0xf0, 0xf0, 0x0f, 0xf0, 0xf0, 0xf0, 0xff, 0xf0, 0xf0, 0xff, 0x0f, 0x00, 0xf0, 0x0e,
    0xf0, 0xf0, 0x00, 0xff, 0x00, 0xf0, 0x0e, 0xf0, 0x0f, 0xf0, 0xf0, 0xff, 0xf0, 0x0f, 0xf0, 0xf0, 0x0f, 0xf0, 0x00,
    0x0f, 0xff, 0xff, 0xf0, 0x0f, 0x00, 0x00, 0xf0, 0x00, 0x00, 0xff, 0xf0, 0x00, 0x78, 0x7b, 0x7f, 0x30, 0x01, 0xf1,
    0xf1, 0xf0, 0xe1, 0xe0, 0xe0, 0xe0, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xf0, 0x87, 0x80, 0x80, 0xe0, 0x0f, 0x0f,
    0x1f, 0x1e, 0x00, 0x0f, 0x0f, 0x0f, 0x00, 0x00, 0x0e, 0xf0, 0xf0, 0xf0, 0xff, 0x7e, 0x7e, 0x7e, 0x20, 0x0f, 0xff,
    0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xff, 0xef, 0xe0, 0xf0, 0x0e, 0xf0, 0xf0, 0x0f, 0x8f, 0x8f, 0x8f, 0x0f, 0x1f, 0x1f,
    0x1f, 0x10, 0x10, 0x0f, 0x00, 0x00, 0x0f, 0x0f, 0x0f, 0x0f, 0xf0, 0xf0, 0x0f, 0xff, 0x0f, 0x0f, 0xf0, 0xf0, 0xff,
    0xff, 0xf0, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xf0, 0xf0, 0xf0, 0xe0, 0x0f, 0xff, 0xff, 0xf0, 0xff, 0x0f, 0x0f, 0xf0,
    0x00, 0xf0, 0x77, 0x73, 0x70, 0x3f, 0x0f, 0xff, 0xef, 0xff, 0xfe, 0x80, 0x88, 0x8e, 0x80, 0xd1, 0xf1, 0x01, 0x00,
    0x00, 0x00, 0xf0, 0xf0, 0xf0, 0xff, 0xf0, 0xf0, 0xff, 0xf0, 0xf0, 0x00, 0x00, 0xf0, 0x0f, 0xff, 0xf0, 0xf0, 0x0f,
    0xf0, 0x0f, 0xf0, 0xf0, 0xf0, 0xf0, 0xff, 0xf0, 0xf0, 0x0f, 0xf0, 0x0f, 0x0f, 0xf0, 0x00, 0xff, 0xff, 0xf0, 0xf0,
    0xff, 0xff, 0xf0, 0xf0, 0xf0, 0x00, 0x0f, 0xf0, 0xf0, 0x00, 0xff, 0xff, 0x0f, 0x00, 0xf0, 0xf0, 0xff, 0xf0, 0x00,
    0x0f, 0x0f, 0x00, 0x00, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0x0f, 0xff, 0xf0, 0xf0, 0xf0, 0xf0, 0x0f, 0x0f, 0xff, 0x00,
    0x77, 0x77, 0x76, 0x40, 0x00, 0xff, 0xef, 0xee, 0xef, 0xee, 0x88, 0x88, 0x8e, 0xd0, 0xf1, 0x00, 0x00, 0x00, 0xf0,
    0x77, 0x77, 0x75, 0x31, 0x1f, 0xff, 0xef, 0xee, 0xee, 0xef, 0xef, 0xef, 0xef, 0xef, 0xef, 0xff, 0xf0, 0x0f, 0xe0,
    0xf0, 0xf0, 0xe0, 0xf0, 0x0f, 0xf0, 0xf0, 0xff, 0xf0, 0xbc, 0x88, 0x88, 0xce, 0xf0, 0x11, 0x01, 0x11, 0x11, 0x10,
    0x77, 0x77, 0x77, 0x75, 0x20, 0x10, 0x0f, 0xff, 0xff, 0x88, 0x88, 0x88, 0xde, 0xf0, 0x01, 0x01, 0x10, 0x10, 0x10,
    0x10, 0x00, 0x00, 0x0f, 0x00, 0x0f, 0x0f, 0xff, 0xf0, 0x77, 0x77, 0x77, 0x31, 0x10, 0xff, 0xef, 0xfe, 0xee, 0xef,
    0xee, 0xee, 0xef, 0xff, 0xdf, 0xff, 0xf0, 0xff, 0xff, 0x88, 0x88, 0xc8, 0xef, 0x11, 0x11, 0x01, 0x11, 0x10, 0x24,
    0x77, 0x77, 0x34, 0x11, 0x10, 0x0f, 0xff, 0xff, 0xef, 0xff, 0xff, 0x0f, 0x0f, 0xf0, 0xe0, 0xff, 0xf0, 0x88, 0x88,
    0xd8, 0x0e, 0x00, 0x11, 0x11, 0x11, 0x11, 0x10, 0x77, 0x77, 0x36, 0x11, 0x10, 0x00, 0x0f, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x0f, 0x00, 0xff, 0xf0, 0x0f, 0x00, 0xe0, 0xb8, 0xe8, 0x0b, 0x00, 0x11, 0x11, 0x11, 0x11, 0x10, 0x27, 0x37,
    0x27, 0x13, 0x01, 0x00, 0xff, 0xff, 0xff, 0xfc, 0xf8, 0xf8, 0xfb, 0xff, 0x00, 0x11, 0x00, 0x00, 0x00, 0xd7, 0x07,
    0x07, 0x07, 0x00, 0x00, 0xff, 0xfe, 0xfe, 0xd8, 0x28, 0x08, 0x0b, 0xff, 0xf0, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0xf0,
    0xf0, 0x00, 0x00, 0x0f, 0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xfe, 0xf0, 0x00, 0x00,
    0xff, 0xf0, 0x00, 0x0f, 0xff, 0x00, 0xf0, 0x00, 0xfe, 0xf0, 0x00, 0x00, 0xff, 0xff, 0xff, 0x01, 0xff, 0xf0, 0x00,
    0xff, 0xff, 0x0f, 0x01, 0xff, 0xf0, 0xff, 0xff, 0xff, 0x01, 0x00, 0xff, 0xf0, 0x00, 0x00, 0x01, 0xfe, 0xf0, 0x00,
    0xff, 0xff, 0x0f, 0x00, 0xff, 0xff, 0xf0, 0xff, 0x0f, 0x01, 0xfe, 0xf0, 0x0f, 0x00, 0xff, 0x00, 0xfe, 0x00, 0xff,
    0xf0, 0xff, 0xff, 0x01, 0x0f, 0x00, 0x87, 0x87, 0xd7, 0xf1, 0x00, 0x1e, 0x0e, 0x1e, 0x0e, 0x0e,
};

static void receive_captured(const uint8_t *samples, unsigned size, const uint8_t *expected) {
    uint8_t frame[ST25TB_REQUEST_MAX];
    unsigned length = 0;
    st25tb_receiver_reset();
    for (unsigned n = 0; n + 1 < size && !length;) {
        unsigned bytes = (size - n) & ~1u;
        n += st25tb_receiver_batch(samples + n, bytes > 64 ? 64 : bytes, frame, &length);
    }
    assert(length == 4 && !memcmp(frame, expected, 4));
}

/* A truncated I frame must retain its good byte after a failed repair,
 * so Q can recover when its longer frame reaches EOF. */
static void receive_truncated_axis(bool erased_starts) {
    uint8_t data[2][4] = {{0x06, 0x00, 0x97, 0x5b}, {0x07, 0x00, 0x97, 0x5b}};
    if (erased_starts) {
        data[0][0] = 0x02;
        data[0][1] = 0x04;
        iso15693_append_crc(data[0], 2);
        memcpy(data[1], data[0], sizeof(data[0]));
        data[1][0] ^= 1;
    }
    uint8_t bits[2][150];
    uint8_t samples[3000];
    uint8_t frame[ST25TB_REQUEST_MAX];
    unsigned counts[2] = {0};
    unsigned length = 0;
    double baseline[2] = {1, 1};
    for (unsigned axis = 0; axis < 2; axis++) {
        unsigned n = 0;
        for (unsigned b = 0; b < 10; b++) {
            bits[axis][n++] = 0;
        }
        for (unsigned b = 0; b < 2; b++) {
            bits[axis][n++] = 1;
        }
        for (unsigned byte = 0; byte < 3 + axis; byte++) {
            bits[axis][n++] = 0;
            for (unsigned b = 0; b < 8; b++) {
                bits[axis][n++] = (data[axis][byte] >> b) & 1;
            }
            bits[axis][n++] = 1;
        }
        for (unsigned b = 0; b < 10; b++) {
            bits[axis][n++] = 0;
        }
        counts[axis] = n;
    }
    for (unsigned t = 0; t < sizeof(samples); t++) {
        samples[t] = 0;
        for (unsigned axis = 0; axis < 2; axis++) {
            int elapsed = (int)t - 1000 + (axis ? 2 : 0);
            unsigned bit = elapsed < 0 ? counts[axis] : (unsigned)elapsed * 1356 / 12800;
            /* Delay the I EOF edge by 1 us, within the framing tolerance. */
            if (!axis && elapsed >= 0 && bit >= 42) {
                bit = (unsigned)(elapsed - 1) * 1356 / 12800;
            }
            double level = bit < counts[axis] ? bits[axis][bit] : 1;
            baseline[axis] += (level - baseline[axis]) / 5;
            int value = (int)round((level - baseline[axis]) * 10 - 0.5);
            if (erased_starts && bit >= 12 && bit < 12 + 10 * (3 + axis)) {
                unsigned character = (bit - 12) / 10;
                unsigned offset = (bit - 12) % 10;
                if ((!axis && character == 0 && offset == 2) || (character == 1 && offset == 3)) {
                    value = -1;
                }
            }
            if (value < -8) {
                value = -8;
            }
            if (value > 7) {
                value = 7;
            }
            samples[t] |= (value & 15) << (axis ? 0 : 4);
        }
    }
    st25tb_receiver_reset();
    for (unsigned n = 0; n < sizeof(samples) && !length;) {
        n += st25tb_receiver_batch(samples + n, 2, frame, &length);
    }
    assert(length == 4 && !memcmp(frame, data[0], 4));
}

int main(void) {
    receive_start_erasure();
    receive_ambiguous_noise();
    receive_after_false_frame();
    receive_after_invalid_prefix(10, 990, true, false);
    /* A nine-ETU SOF must survive peer repair of an erased prefix start. */
    receive_after_invalid_prefix(9, 1010, false, true);
    receive_truncated_axis(false);
    receive_truncated_axis(true);
    const unsigned chunks[] = {2, 6, 32, 64, 127};
    uint8_t sample = 0;
    uint8_t frame[ST25TB_REQUEST_MAX];
    unsigned length = 1;
    assert(!st25tb_receiver_batch(&sample, 0, frame, &length) && !length);
    length = 1;
    assert(!st25tb_receiver_batch(&sample, 1, frame, &length) && !length);
    receive_captured(select_split, sizeof(select_split), (const uint8_t[]){0x0e, 0xa1, 0xd4, 0x21});
    receive_captured(initiate_split, sizeof(initiate_split), (const uint8_t[]){0x06, 0x00, 0x97, 0x5b});
    const uint8_t read[] = {8, 8};
    eof_bits = 11;
    channel_mode = 0;
    for (eof_noise = 1; eof_noise <= 2; eof_noise++) {
        receive(read, sizeof(read), 1, 5, 10, 2, false, false);
    }
    eof_noise = 0;
    eof_bits = 10;
    for (channel_mode = 0; channel_mode < 5; channel_mode++) {
        chunk_size = chunks[channel_mode];
        clock_scale = 1000;
        offset_quarters = 0;
        run(true);
        /* An absent, short or long EOF must not deliver even a CRC-valid payload. */
        const unsigned invalid_eof[] = {0, 9, 11};
        for (unsigned n = 0; n < sizeof(invalid_eof) / sizeof(invalid_eof[0]); n++) {
            eof_bits = invalid_eof[n];
            receive(read, sizeof(read), 1, 5, 10, 2, false, false);
        }
        eof_bits = 10;
        after_reply = true;
        for (recovery_start = 868; recovery_start <= 900; recovery_start += 32) {
            run(false);
        }
        after_reply = false;
        /* Preserve framing across sampling phase and +/-1% clocks on both axes. */
        for (offset_quarters = 0; offset_quarters < 8; offset_quarters++) {
            for (clock_scale = 990; clock_scale <= 1010; clock_scale += 5) {
                run(false);
            }
        }
        /* Cross the pair-clock wrap during data, including recovery split
         * across scalar, short, odd and partially consumed batches. */
        clock_origin = ~0u - 600;
        clock_scale = 1000;
        offset_quarters = 0;
        receive(read, sizeof(read), 1, 5, 10, 2, false, false);
        after_reply = true;
        receive(read, sizeof(read), 1, 5, 10, 2, false, false);
        after_reply = false;
        clock_origin = 0;
    }
}
