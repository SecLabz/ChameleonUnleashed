#include "manchester.h"

#include <stdlib.h>
#include <string.h>

#define MANCHESTER_MIN_CONTRAST_ADC 512

bool manchester_sample_bit(const uint16_t *samples, uint16_t clock_cycles, bool *bit) {
    unsigned first = 0;
    unsigned second = 0;
    unsigned window = clock_cycles / 4;
    for (unsigned i = 0; i < window; i++) {
        first += samples[clock_cycles / 8 + i];
        second += samples[clock_cycles * 5 / 8 + i];
    }
    first /= window;
    second /= window;
    unsigned difference = first > second ? first - second : second - first;
    if (difference < MANCHESTER_MIN_CONTRAST_ADC) {
        return false;
    }
    *bit = first > second;
    return true;
}

void manchester_reset(manchester *m) {
    m->sync = true;
}

void manchester_feed(manchester *m, uint8_t interval, bool *bits, int8_t *bitlen) {
    // after the current interval is processed, is it on the judgment line
    uint8_t t = m->rp(interval);
    *bitlen = -1;
    if (t == 3) {
        return;
    }

    if (m->sync) {
        if (t == 0) {
            // 1T, add '0', still sync
            *bitlen = 1;
            bits[0] = 0;
        } else if (t == 1) {
            // 1.5T, add '1', switch to non-sync
            *bitlen = 1;
            bits[0] = 1;
            m->sync = false;
        } else if (t == 2) {
            // 2T, add '10', still sync
            *bitlen = 2;
            bits[0] = 1;
            bits[1] = 0;
        } else {
            return;
        }
    } else {
        if (t == 0) {
            // 1T, add '1', still non-sync
            *bitlen = 1;
            bits[0] = 1;
        } else if (t == 1) {
            // 1.5T, add '10', switch to sync
            *bitlen = 2;
            bits[0] = 1;
            bits[1] = 0;
            m->sync = true;
        } else {
            return;
        }
    }
}
