#pragma once
#include <stdint.h>

enum {
    MF1_UID_GEN1B = 1,
    MF1_UID_GEN3 = 3,
    MF1_UID_GEN4 = 4,
    MF1_UID_GDM20 = 6,
    MF1_UID_GDM40 = 7,
    MF1_UID_GDM40_ZUID = 8,
    MF1_UID_SUPER_GEN1 = 9,
};

/* Command 2041: method, UID length, old UID[7], new UID[7], password[4],
 * check-only (0/1). Padding and unused password bytes must be zero.
 * Only Gen3 supports seven-byte UIDs here.
 * Reply: UID length and checked old UID or verified new UID. */
uint16_t mf1_uid_special(uint16_t length, const uint8_t *data,
    uint8_t *output, uint16_t *output_length);
