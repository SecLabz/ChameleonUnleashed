#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ST25TB_UID_BYTES = 8,
    ST25TB_UID_FAMILY = 0xd0,
    ST25TB_UID_MANUFACTURER = 2,
    ST25TB_UID_PRODUCT_OFFSET = 2,
    ST25TB_PRODUCT_512_AT = 0x33,
    ST25TB_PRODUCT_SRI512 = 6,
    ST25TB_PRODUCT_SHIFT = 2,
    ST25TB_BLOCK_COUNT = 16,
    ST25TB_BLOCK_BYTES = 4,
    ST25TB_COUNTER_FIRST_BLOCK = 5,
    ST25TB_COUNTER_LAST_BLOCK = 6,
    ST25TB_SYSTEM_BLOCK = 255,
    ST25TB_RESET_INVENTORY = 0x0c,
    ST25TB_CHIP_ID_REPLY_BYTES = 3,
    ST25TB_BLOCK_REPLY_BYTES = 6,
    ST25TB_UID_REPLY_BYTES = 10,
    ST25TB_WRITE_FRAME_BYTES = 6,
};

/* Existing read-stage diagnostics returned by the bounded command. */
enum {
    ST25TB_STAGE_INITIATE = 1,
    ST25TB_STAGE_SELECT = 2,
    ST25TB_STAGE_UID = 3,
    ST25TB_STAGE_BLOCK = 4,
};

/* Existing host command layouts: UID, block, then expected/new block values. */
enum {
    ST25TB_REQUEST_BLOCK_OFFSET = ST25TB_UID_BYTES,
    ST25TB_WRITE_EXPECTED_OFFSET = ST25TB_REQUEST_BLOCK_OFFSET + 1,
    ST25TB_WRITE_VALUE_OFFSET = ST25TB_WRITE_EXPECTED_OFFSET + ST25TB_BLOCK_BYTES,
    ST25TB_READ_REQUEST_BYTES = ST25TB_REQUEST_BLOCK_OFFSET + 1,
    ST25TB_WRITE_REQUEST_BYTES = ST25TB_WRITE_VALUE_OFFSET + ST25TB_BLOCK_BYTES,
    ST25TB_INFO_CHIP_ID_OFFSET = ST25TB_UID_BYTES,
    ST25TB_INFO_BLOCK_COUNT_OFFSET = ST25TB_INFO_CHIP_ID_OFFSET + 1,
    ST25TB_INFO_BLOCK_SIZE_OFFSET = ST25TB_INFO_BLOCK_COUNT_OFFSET + 1,
    ST25TB_INFO_BYTES = ST25TB_INFO_BLOCK_SIZE_OFFSET + 1,
    ST25TB_DUMP_FAMILY = 2,
    ST25TB_DUMP_CHIP_ID_OFFSET = 19,
    ST25TB_DUMP_SYSTEM_OFFSET = 20,
    ST25TB_DUMP_SYSTEM_KNOWN = 8,
};

bool st25tb_reset_inventory(void);
/* Qualified receive windows include RF margin; unknown reply sizes return zero. */
unsigned st25tb_capture_samples(size_t reply_bytes, bool hardware_tx);
uint16_t st25tb_reader_command(uint16_t command, const uint8_t *data,
    uint8_t *output, uint16_t *size);
uint16_t st25tb_tearoff_read(const uint8_t expected_uid[ST25TB_UID_BYTES], bool verify_uid,
    uint8_t block, uint8_t value[ST25TB_BLOCK_BYTES], uint8_t *stage);
bool st25tb_tearoff_write_selected(uint8_t block, const uint8_t value[ST25TB_BLOCK_BYTES],
    uint32_t delay_us, uint32_t *request_to_write_cycles);
