#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t configuration;
    uint16_t clock_cycles;
    uint16_t sample_offset;
    uint8_t modulation;
    uint16_t level_low;
    uint16_t level_high;
    bool inverted;
} t55xx_demod_t;

/* Samples are uniformly spaced at one sample per 125 kHz carrier cycle.
 * Keep the command-to-capture timing identical when reusing sample_offset. */
bool t55xx_detect_samples(const uint16_t *samples, size_t count, t55xx_demod_t *demod,
    void (*keepalive)(void));
bool t55xx_read_samples(const uint16_t *samples, size_t count,
    const t55xx_demod_t *demod, uint32_t *word);
bool t55xx_configuration_valid(uint32_t word, uint16_t clock, uint8_t modulation);
