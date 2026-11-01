#include "iso15693/sniff_decode.h"
#include "iso15693/ppm.h"
#include "iso15693/commands.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

static nfcv_sniff_tag_t tag;
static float integrals[4 * NFCV_SNIFF_LOW_INTEGRALS];

static bool carrier(const uint8_t *data, unsigned length, unsigned half) {
    if (half < 8) {
        return (0x1du & (0x80u >> half)) != 0;
    }
    half -= 8;
    if (half < length * 16u) {
        unsigned bit = half / 2u;
        bool one = (data[bit / 8u] & (1u << (bit % 8u))) != 0;
        return one == ((half & 1u) != 0);
    }
    half -= length * 16u;
    return half < 8 && (0xb8u & (0x80u >> half)) != 0;
}

static uint8_t iq_sample(unsigned n, const uint8_t *data, unsigned length, unsigned rate, double phase) {
    double ratio = phase > 1 ? 1.0001 : 0.9999;
    double time = n - 301.0;
    unsigned half = time < 0 ? 0 : time / (256.0 / 13.56 * rate * ratio);
    bool on = time >= 0 && carrier(data, length, half);
    double angle = n * 6.283185307179586 * 0.42375 / ratio + phase;
    int dc_i = phase > 1 ? 6 : 1;
    int dc_q = phase > 1 ? -6 : 0;
    double amplitude = phase > 1 ? 1 : 3;
    int i = dc_i + (on ? lround(amplitude * cos(angle)) : 0);
    int q = dc_q + (on ? lround(amplitude * sin(angle)) : 0);
    return ((i & 15) << 4) | (q & 15);
}

static void response(unsigned length, unsigned rate, double phase, unsigned chunk) {
    uint8_t data[ISO15693_RESPONSE_MAX_BYTES];
    for (unsigned i = 0; i < length - 2u; i++) {
        data[i] = i * 59u;
    }
    iso15693_append_crc(data, length - 2u);
    memset(&tag, 0xa5, sizeof(tag));
    nfcv_sniff_tag_reset(&tag, rate == 1, integrals);
    bool done = false;
    unsigned used = 0;
    uint8_t samples[32];
    for (unsigned n = 0; n < 200000u && !done; n++) {
        if (n % 4097u == 4096u) {
            if (used) {
                done = nfcv_sniff_tag_feed(&tag, samples, used);
                used = 0;
            }
            if (!done) {
                done = nfcv_sniff_tag_gap(&tag, 1, iq_sample(n - 1, data, length, rate, phase),
                    iq_sample(n + 1, data, length, rate, phase));
            }
        } else {
            samples[used++] = iq_sample(n, data, length, rate, phase);
            if (used == chunk) {
                done = nfcv_sniff_tag_feed(&tag, samples, used);
                used = 0;
            }
        }
    }
    if (!done || tag.truncated || tag.length != length) {
        fprintf(stderr, "length=%u rate=%u phase=%f done=%d truncated=%d decoded=%u\n",
            length, rate, phase, done, tag.truncated, tag.length);
    }
    assert(done && !tag.truncated && tag.length == length);
    assert(!memcmp(tag.data, data, length));
    assert(iso15693_check_crc(tag.data, tag.length));
}

static uint32_t reader_cycles(unsigned tick, int ppm) {
    uint64_t cycles = (uint64_t)tick * NFCV_PPM_TICK_CYCLES_NUMERATOR / NFCV_PPM_TICK_CYCLES_DENOMINATOR;
    return (uint32_t)(100000u + cycles * (1000000 + ppm) / 1000000);
}

static nfcv_sniff_reader_event_t reader_pulse(nfcv_sniff_reader_t *reader, unsigned tick, int ppm) {
    uint32_t cycles = reader_cycles(tick, ppm);
    nfcv_sniff_reader_event_t event = nfcv_sniff_reader_feed(reader, false, cycles);
    assert(!nfcv_sniff_reader_feed(reader, true, cycles + 1208));
    return event;
}

static uint32_t reader_request(nfcv_sniff_reader_t *reader, const uint8_t *data,
        unsigned length, bool slow, int ppm, unsigned first_tick) {
    assert(!reader_pulse(reader, first_tick, ppm));
    assert(!reader_pulse(reader, first_tick + (slow ? 7 : 5), ppm));
    unsigned symbols = 0;
    for (unsigned i = 0; i < length; i++) {
        if (slow) {
            assert(!reader_pulse(reader, first_tick + 8 + symbols * 512 + data[i] * 2 + 1, ppm));
            symbols++;
        } else {
            for (unsigned pair = 0; pair < 4; pair++) {
                symbols++;
                assert(!reader_pulse(reader, first_tick + symbols * 8 + ((data[i] >> (pair * 2)) & 3) * 2 + 1, ppm));
            }
        }
    }
    unsigned eof_tick = first_tick + (slow ? 8 + symbols * 512 + 2 : (symbols + 1) * 8 + 2);
    assert(reader_pulse(reader, eof_tick, ppm) == NFCV_SNIFF_READER_FRAME);
    return reader_cycles(eof_tick, ppm);
}

static void request(bool slow, int ppm, bool good_crc) {
    nfcv_sniff_reader_t reader = {.previous_lock = true};
    uint8_t data[] = {0x22, 0x2b, 0x41, 0x87, 0xfb, 0x19, 0x50, 3, 4, 0xe0, 0, 0};
    iso15693_append_crc(data, sizeof(data) - 2);
    if (!good_crc) {
        data[10] ^= 1;
    }
    reader_request(&reader, data, sizeof(data), slow, ppm, 0);
    assert(reader.frame.crc_ok == good_crc);
    assert(reader.frame.byte_count == sizeof(data));
    assert(!memcmp(reader.frame.bytes, data, sizeof(data)));
}

static void inventory_slots(bool slow, bool high_rate, bool single_slot, bool good_crc) {
    nfcv_sniff_reader_t reader = {.previous_lock = true};
    uint8_t data[] = {ISO15693_REQ_INVENTORY, ISO15693_CMD_INVENTORY, 0, 0, 0};
    if (high_rate) {
        data[0] |= ISO15693_REQ_HIGH_RATE;
    }
    if (single_slot) {
        data[0] |= ISO15693_REQ_SLOT1;
    }
    iso15693_append_crc(data, sizeof(data) - 2);
    if (!good_crc) {
        data[3] ^= 1;
    }
    uint32_t cycles = reader_request(&reader, data, sizeof(data), slow, 0, 0);
    unsigned slots = !single_slot && good_crc ? 15 : 0;
    assert(reader.inventory_slots == slots);
    cycles += 10000u;
    assert(nfcv_sniff_reader_feed(&reader, true, cycles) == NFCV_SNIFF_READER_NONE);
    assert(reader.inventory_slots == slots);
    for (unsigned slot = 1; slot <= 16; slot++) {
        cycles += 100000u;
        assert(nfcv_sniff_reader_feed(&reader, false, cycles) == NFCV_SNIFF_READER_NONE);
        assert(nfcv_sniff_reader_feed(&reader, true, cycles + 1208u) == NFCV_SNIFF_READER_NONE);
        assert(nfcv_sniff_reader_feed(&reader, true, cycles + NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES) == NFCV_SNIFF_READER_NONE);
        nfcv_sniff_reader_event_t event = nfcv_sniff_reader_feed(&reader, true,
            cycles + NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES + 1u);
        assert(event == (slot <= slots ? NFCV_SNIFF_READER_SLOT : NFCV_SNIFF_READER_NONE));
        assert(!memcmp(reader.frame.bytes, data, sizeof(data)));
        assert(reader.frame.high_data_rate == high_rate);
        assert(reader.frame.single_subcarrier);
        assert(nfcv_sniff_reader_feed(&reader, true,
            cycles + NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES + 2u) == NFCV_SNIFF_READER_NONE);
    }
    assert(reader.inventory_slots == 0);
}

static void new_command_cancels_inventory(void) {
    nfcv_sniff_reader_t reader = {.previous_lock = true};
    uint8_t inventory[] = {0x06, ISO15693_CMD_INVENTORY, 0, 0, 0};
    iso15693_append_crc(inventory, sizeof(inventory) - 2);
    reader_request(&reader, inventory, sizeof(inventory), false, 0, 0);
    assert(reader.inventory_slots == 15);
    uint8_t other[] = {0x02, ISO15693_CMD_GET_SYSTEM_INFO, 0, 0};
    iso15693_append_crc(other, sizeof(other) - 2);
    reader_request(&reader, other, sizeof(other), false, 0, 1024);
    assert(reader.inventory_slots == 0);
}

static void partial_reader(void) {
    nfcv_sniff_reader_t reader = {.previous_lock = true};
    assert(!reader_pulse(&reader, 0, 0));
    assert(!reader_pulse(&reader, 5, 0));
    const uint8_t bytes[] = {0x22, 0x20};
    unsigned symbols = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        for (unsigned pair = 0; pair < 4; pair++) {
            symbols++;
            assert(!reader_pulse(&reader, symbols * 8 + ((bytes[i] >> (pair * 2)) & 3) * 2 + 1, 0));
        }
    }
    assert(nfcv_sniff_reader_stop(&reader));
    assert(reader.truncated && !reader.frame.crc_ok && reader.frame.byte_count == sizeof(bytes));
    assert(!memcmp(reader.frame.bytes, bytes, sizeof(bytes)));
    assert(!nfcv_sniff_reader_stop(&reader));
}

int main(void) {
    partial_reader();
    new_command_cancels_inventory();
    for (unsigned slow = 0; slow < 2; slow++) {
        for (unsigned high_rate = 0; high_rate < 2; high_rate++) {
            inventory_slots(slow, high_rate, false, true);
            inventory_slots(slow, high_rate, true, true);
            inventory_slots(slow, high_rate, false, false);
        }
    }
    for (int ppm = -75; ppm <= 75; ppm += 75) {
        for (unsigned slow = 0; slow < 2; slow++) {
            request(slow, ppm, true);
            request(slow, ppm, false);
        }
    }
    for (unsigned rate = 1; rate <= 4; rate += 3) {
        for (unsigned length = 3; length <= 143; length += length < 12 ? 1 : 131) {
            for (unsigned chunk = 1; chunk <= 32; chunk += 31) {
                response(length, rate, 0.1, chunk);
                response(length, rate, 1.5, chunk);
            }
        }
    }
    nfcv_sniff_tag_reset(&tag, true, integrals);
    for (unsigned n = 0; n < 10000; n++) {
        uint8_t silence = 0x22;
        assert(!nfcv_sniff_tag_feed(&tag, &silence, 1));
    }
}
