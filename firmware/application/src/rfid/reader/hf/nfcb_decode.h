#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint16_t nfcb_crc_b(const uint8_t *bytes, size_t size);
bool nfcb_decode_iq(const uint8_t *samples, size_t count, size_t expected,
                    uint8_t *out, unsigned *offset);
bool nfcb_decode_iq_checked(const uint8_t *samples, size_t count, size_t expected,
    uint8_t *out, unsigned *offset, bool *crc_error);
/* Decode while samples arrive; available() returns how many are written. Uses the
 * learned orientation only: a false result calls for nfcb_decode_iq_checked(). */
bool nfcb_decode_iq_live(const uint8_t *samples, size_t count, size_t expected,
    uint8_t *out, unsigned *offset, unsigned (*available)(void));
