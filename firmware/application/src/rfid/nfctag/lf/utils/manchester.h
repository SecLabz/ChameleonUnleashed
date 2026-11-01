#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t (*period)(uint8_t interval);

typedef struct {
    bool sync;
    period rp;
} manchester;

extern void manchester_reset(manchester *m);
extern void manchester_feed(manchester *m, uint8_t interval, bool *bits, int8_t *bitlen);

/* One uniformly sampled 14-bit ADC bit, at one sample per carrier cycle.
 * The caller supplies at least clock_cycles samples and clock_cycles >= 8. */
bool manchester_sample_bit(const uint16_t *samples, uint16_t clock_cycles, bool *bit);
