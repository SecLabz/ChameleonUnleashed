#pragma once

#include "protocols.h"

extern const protocol viking;

uint8_t viking_t55xx_writer(uint8_t* uid, uint32_t* blks);

/* Uniform 14-bit ADC samples, one per carrier cycle. Two identical complete
 * RF/32 frames are required; data remains unchanged on failure. */
bool viking_decode_samples(const uint16_t *samples, size_t count, uint8_t *data);
