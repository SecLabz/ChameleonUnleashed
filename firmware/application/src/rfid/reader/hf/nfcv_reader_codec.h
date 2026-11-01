#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NFCV_READER_MAX_FRAME 24u
#define NFCV_READER_IQ_SAMPLES 11264u
#define NFCV_READER_MAX_TICKS (12u + 32u * NFCV_READER_MAX_FRAME)
/* One level per 128 carrier cycles: 1 is an ASK pause, 0 is carrier. */
size_t nfcv_reader_encode(const uint8_t *frame, size_t length, uint8_t *levels, size_t capacity);
/* Single-subcarrier, high-rate ISO15693 responses, including CRC bytes. */
size_t nfcv_reader_decode_iq(const uint8_t *samples, size_t count, uint8_t *frame,
                             size_t capacity, unsigned *offset);
size_t nfcv_reader_decode_iq_checked(const uint8_t *samples, size_t count, uint8_t *frame,
    size_t capacity, unsigned *offset, bool *crc_error);
