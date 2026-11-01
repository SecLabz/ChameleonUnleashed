#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HF_READER_MAX_BLOCKS 64u
#define HF_READER_UID_BYTES 8u
#define HF_READER_BLOCK_BYTES 4u
#define HF_READER_UNIT_BYTES 6u
#define HF_READER_FRAME_BYTES 24u
#define HF_READER_CRC_BYTES 2u
#define NFCB_ATQB_BYTES 14u
#define HF_READER_HEADER_BYTES 24u
#define HF_READER_OUTPUT_BYTES (HF_READER_HEADER_BYTES + HF_READER_UNIT_BYTES * HF_READER_MAX_BLOCKS)
#define HF_READER_DEADLINE_MS 1500u
#define HF_READER_CYCLES_PER_MS 64000u
#define HF_READER_CLEANUP_RESERVE_MS 400u
#define HF_READER_DEADLINE_CYCLES (HF_READER_DEADLINE_MS * HF_READER_CYCLES_PER_MS)
#define NFCV_RAW_KEEP_FIELD 0x80u
#define NFCV_RAW_COUNT_MASK 0x7fu

enum {
    HF_READER_DUMP_RESULT_OFFSET = 2,
    HF_READER_DUMP_STATUS_OFFSET = 3,
    HF_READER_DUMP_UID_OFFSET = 4,
    HF_READER_DUMP_BLOCK_COUNT_OFFSET = 12,
    HF_READER_DUMP_BLOCK_SIZE_OFFSET = 14,
    HF_READER_DUMP_KNOWN_FIELDS_OFFSET = 15,
    HF_READER_UNIT_DATA_OFFSET = 2,
    HF_READER_DUMP_COMPLETE = 0,
    HF_READER_DUMP_PARTIAL = 1,
    HF_READER_DUMP_DEADLINE = 2,
};

bool hf_reader_supports(uint16_t command);
uint16_t hf_reader_command(uint16_t command, uint16_t length, const uint8_t *data,
    uint8_t *output, size_t capacity, uint16_t *size);
uint16_t nfcv_raw(uint16_t length, const uint8_t *data, uint8_t *output, uint16_t *size, bool *complete);
uint16_t nfcv_unlock_privacy(const uint8_t password[4]);
uint16_t nfcv_write_block(const uint8_t *data, uint8_t *output, uint16_t *size);
uint16_t nfcv_reader_command(uint16_t command, const uint8_t *data,
    uint8_t *output, uint16_t *size);
uint16_t nfcb_reader_command(uint16_t command, const uint8_t *data,
    uint8_t *output, uint16_t *size);
/* Dump v1: fixed 24-byte header followed by [known, lock, data(4)] per block.
 * lock: 0 unknown, 1 unlocked, 2 locked. Header byte 2: 0 complete, 1 partial,
 * 2 deadline; byte 3 holds the terminating existing device status.
 */
static inline void hf_reader_snapshot(uint8_t *out, uint8_t family,
        const uint8_t uid[HF_READER_UID_BYTES], unsigned blocks, uint16_t *size) {
    *size = HF_READER_HEADER_BYTES + blocks * HF_READER_UNIT_BYTES;
    for (unsigned i = 0; i < *size; i++) out[i] = 0;
    out[0] = 1;
    out[1] = family;
    out[HF_READER_DUMP_RESULT_OFFSET] = HF_READER_DUMP_PARTIAL;
    for (unsigned i = 0; i < HF_READER_UID_BYTES; i++) out[HF_READER_DUMP_UID_OFFSET + i] = uid[i];
    out[HF_READER_DUMP_BLOCK_COUNT_OFFSET] = blocks >> 8;
    out[HF_READER_DUMP_BLOCK_COUNT_OFFSET + 1] = blocks;
    out[HF_READER_DUMP_BLOCK_SIZE_OFFSET] = HF_READER_BLOCK_BYTES;
}
