#pragma once
#include <stddef.h>
#include <stdint.h>

/* Web-only read operations. v1, kind (0 check / 1 read), UID length, UID,
 * entry count, entries of [block-or-sector, key type, six key bytes].
 * Reply: v1, kind, count, then [RF status BE16, size BE16, result] per entry.
 * Every request performs fresh RF work; no observations or memory are cached. */
#define CLASSIC_BATCH_MAX_ENTRIES 8u
uint16_t classic_batch_request(const uint8_t *body, size_t size,
    uint8_t *output, size_t capacity, uint16_t *length);
