/* SPDX-License-Identifier: GPL-3.0-only */
#include "hf_reader.h"
#include "st25tb_reader.h"
#include "hf_radio.h"
#include "app_status.h"
#include "bsp_wdt.h"
#include "data_cmd.h"
#include <string.h>

#define ST25TB_SHORT_IQ_SAMPLES 2048u
#define ST25TB_UID_IQ_SAMPLES 3072u
#define ST25TB_CHIP_ID_IQ_SAMPLES 896u
#define ST25TB_BLOCK_IQ_SAMPLES 1216u
#define ST25TB_FAST_UID_IQ_SAMPLES 1664u

enum {
    ST25TB_INITIATE = 0x06,
    ST25TB_SELECT = 0x0e,
    ST25TB_GET_UID = 0x0b,
    ST25TB_READ_BLOCK = 0x08,
    ST25TB_WRITE_BLOCK = 0x09,
};

unsigned st25tb_capture_samples(size_t reply_bytes, bool hardware_tx) {
    switch (reply_bytes) {
    case ST25TB_CHIP_ID_REPLY_BYTES:
        return hardware_tx ? ST25TB_CHIP_ID_IQ_SAMPLES : ST25TB_SHORT_IQ_SAMPLES;
    case ST25TB_BLOCK_REPLY_BYTES:
        return hardware_tx ? ST25TB_BLOCK_IQ_SAMPLES : ST25TB_SHORT_IQ_SAMPLES;
    case ST25TB_UID_REPLY_BYTES:
        return hardware_tx ? ST25TB_FAST_UID_IQ_SAMPLES : ST25TB_UID_IQ_SAMPLES;
    default: return 0;
    }
}

bool st25tb_reset_inventory(void) {
    const uint8_t command = ST25TB_RESET_INVENTORY;
    return hf_radio_send(&command, sizeof(command));
}

static uint16_t exchange(const uint8_t *frame, size_t length, unsigned expected,
        uint8_t reply[HF_READER_FRAME_BYTES]) {
    size_t received;
    return hf_radio_exchange(frame, length, expected, reply, HF_READER_FRAME_BYTES, &received);
}

static uint16_t select_once(uint8_t uid[ST25TB_UID_BYTES], uint8_t *chip_id,
        uint8_t *stage, bool retry_deeper) {
    uint8_t frame[] = {ST25TB_INITIATE, 0}, reply[HF_READER_FRAME_BYTES];
    if (stage) *stage = ST25TB_STAGE_INITIATE;
    uint16_t status = exchange(frame, sizeof(frame), ST25TB_CHIP_ID_REPLY_BYTES, reply);
    if (status == STATUS_HF_TAG_NO && retry_deeper) {
        hf_radio_depth(true);
        status = exchange(frame, sizeof(frame), ST25TB_CHIP_ID_REPLY_BYTES, reply);
    }
    if (status) return status;
    *chip_id = reply[0];
    frame[0] = ST25TB_SELECT;
    frame[1] = *chip_id;
    if (stage) *stage = ST25TB_STAGE_SELECT;
    status = exchange(frame, sizeof(frame), ST25TB_CHIP_ID_REPLY_BYTES, reply);
    if (status) return status;
    if (reply[0] != *chip_id) return STATUS_HF_ERR_STAT;
    if (!uid) return STATUS_HF_TAG_OK;
    frame[0] = ST25TB_GET_UID;
    if (stage) *stage = ST25TB_STAGE_UID;
    status = exchange(frame, 1, ST25TB_UID_REPLY_BYTES, reply);
    if (status) return status;
    for (unsigned i = 0; i < ST25TB_UID_BYTES; i++) uid[i] = reply[ST25TB_UID_BYTES - 1 - i];
    return STATUS_HF_TAG_OK;
}

static uint16_t select_tag(uint8_t uid[ST25TB_UID_BYTES], uint8_t *chip_id,
        uint8_t *stage, bool read_only) {
    if (read_only) hf_radio_depth(true);
    uint16_t status = select_once(uid, chip_id, stage, !read_only);
    if (!read_only || (status != STATUS_HF_TAG_NO && status != STATUS_HF_ERR_CRC &&
            status != STATUS_HF_ERR_STAT)) return status;
    if (!st25tb_reset_inventory()) return STATUS_HF_ERR_STAT;
    hf_radio_depth(false);
    uint16_t retried = select_once(uid, chip_id, stage, true);
    return retried == STATUS_HF_TAG_NO && status == STATUS_HF_ERR_CRC ? status : retried;
}

static uint16_t read_block(uint8_t block, uint8_t reply[HF_READER_FRAME_BYTES]) {
    uint8_t frame[] = {ST25TB_READ_BLOCK, block};
    uint16_t status = exchange(frame, sizeof(frame), ST25TB_BLOCK_REPLY_BYTES, reply);
    if (status == STATUS_HF_TAG_NO || status == STATUS_HF_ERR_CRC || status == STATUS_HF_ERR_STAT)
        status = exchange(frame, sizeof(frame), ST25TB_BLOCK_REPLY_BYTES, reply);
    return status;
}

uint16_t st25tb_tearoff_read(const uint8_t expected_uid[ST25TB_UID_BYTES], bool verify_uid,
        uint8_t block, uint8_t value[ST25TB_BLOCK_BYTES], uint8_t *stage) {
    uint8_t uid[ST25TB_UID_BYTES], chip, reply[HF_READER_FRAME_BYTES];
    uint16_t status = select_tag(verify_uid ? uid : NULL, &chip, stage, false);
    if (status) return status;
    if (verify_uid && memcmp(uid, expected_uid, ST25TB_UID_BYTES)) return STATUS_HF_TAG_NO;
    uint8_t frame[] = {ST25TB_READ_BLOCK, block};
    *stage = ST25TB_STAGE_BLOCK;
    status = exchange(frame, sizeof(frame), ST25TB_BLOCK_REPLY_BYTES, reply);
    if (status) return status;
    memcpy(value, reply, ST25TB_BLOCK_BYTES);
    return STATUS_HF_TAG_OK;
}

bool st25tb_tearoff_write_selected(uint8_t block, const uint8_t value[ST25TB_BLOCK_BYTES],
        uint32_t delay_us, uint32_t *request_to_write_cycles) {
    uint8_t frame[ST25TB_WRITE_FRAME_BYTES] = {ST25TB_WRITE_BLOCK, block, value[0], value[1], value[2], value[3]};
    return hf_radio_tearoff_write(frame, delay_us, request_to_write_cycles);
}

uint16_t st25tb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    uint8_t reply[HF_READER_FRAME_BYTES];
    uint8_t uid[ST25TB_UID_BYTES], chip;
    bool read_only = command == DATA_CMD_ST25TB_INFO ||
        command == DATA_CMD_ST25TB_READ_BLOCK || command == DATA_CMD_ST25TB_DUMP;
    uint16_t status = select_tag(uid, &chip, NULL, read_only);
    if (status) return status;
    /* SRI512 uses a six-bit product code; 0x1b also identifies ST25TB512-AC.
     * These and ST25TB512-AT read blocks 0..15 and system block 255. */
    bool supported = uid[0] == ST25TB_UID_FAMILY && uid[1] == ST25TB_UID_MANUFACTURER &&
        (uid[ST25TB_UID_PRODUCT_OFFSET] == ST25TB_PRODUCT_512_AT ||
         (uid[ST25TB_UID_PRODUCT_OFFSET] >> ST25TB_PRODUCT_SHIFT) == ST25TB_PRODUCT_SRI512);
    if (command == DATA_CMD_ST25TB_INFO) {
        memcpy(output, uid, ST25TB_UID_BYTES);
        output[ST25TB_INFO_CHIP_ID_OFFSET] = chip;
        output[ST25TB_INFO_BLOCK_COUNT_OFFSET] = supported ? ST25TB_BLOCK_COUNT : 0;
        output[ST25TB_INFO_BLOCK_SIZE_OFFSET] = ST25TB_BLOCK_BYTES;
        *size = ST25TB_INFO_BYTES;
        return STATUS_HF_TAG_OK;
    }
    if (memcmp(uid, data, ST25TB_UID_BYTES)) return STATUS_HF_ERR_STAT;
    if (!supported) return STATUS_NOT_IMPLEMENTED;
    if (command == DATA_CMD_ST25TB_READ_BLOCK) {
        status = read_block(data[ST25TB_REQUEST_BLOCK_OFFSET], reply);
        if (status) return status;
        memcpy(output, reply, ST25TB_BLOCK_BYTES);
        *size = ST25TB_BLOCK_BYTES;
        return STATUS_HF_TAG_OK;
    }
    if (command == DATA_CMD_ST25TB_WRITE_BLOCK) {
        if (uid[ST25TB_UID_PRODUCT_OFFSET] != ST25TB_PRODUCT_512_AT) return STATUS_NOT_IMPLEMENTED;
        uint8_t block = data[ST25TB_REQUEST_BLOCK_OFFSET];
        const uint8_t *expected = data + ST25TB_WRITE_EXPECTED_OFFSET;
        const uint8_t *value = data + ST25TB_WRITE_VALUE_OFFSET;
        uint8_t frame[] = {ST25TB_READ_BLOCK, block};
        status = exchange(frame, sizeof(frame), ST25TB_BLOCK_REPLY_BYTES, reply);
        if (status) return status;
        if (memcmp(reply, expected, ST25TB_BLOCK_BYTES)) return STATUS_HF_ERR_STAT;
        if ((block == ST25TB_COUNTER_FIRST_BLOCK || block == ST25TB_COUNTER_LAST_BLOCK) &&
                ((uint32_t)value[0] | (uint32_t)value[1] << 8 |
                 (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24) >
                ((uint32_t)expected[0] | (uint32_t)expected[1] << 8 |
                 (uint32_t)expected[2] << 16 | (uint32_t)expected[3] << 24))
            return STATUS_PAR_ERR;
        if (block == ST25TB_SYSTEM_BLOCK)
            for (unsigned i = 0; i < ST25TB_BLOCK_BYTES; i++)
                if ((value[i] | expected[i]) != expected[i]) return STATUS_PAR_ERR;
        uint8_t write[ST25TB_WRITE_FRAME_BYTES] = {ST25TB_WRITE_BLOCK, block, value[0], value[1], value[2], value[3]};
        if (!hf_radio_st25tb_write(write)) return STATUS_HF_ERR_STAT;
        status = exchange(frame, sizeof(frame), ST25TB_BLOCK_REPLY_BYTES, reply);
        if (status) return status;
        memcpy(output, reply, ST25TB_BLOCK_BYTES);
        *size = ST25TB_BLOCK_BYTES;
        return memcmp(reply, value, ST25TB_BLOCK_BYTES) ? STATUS_HF_ERR_STAT : STATUS_HF_TAG_OK;
    }
    hf_reader_snapshot(output, ST25TB_DUMP_FAMILY, uid, ST25TB_BLOCK_COUNT, size);
    output[ST25TB_DUMP_CHIP_ID_OFFSET] = chip;
    for (unsigned block = 0; block <= ST25TB_BLOCK_COUNT; block++) {
        if (hf_radio_expired()) {
            output[HF_READER_DUMP_RESULT_OFFSET] = HF_READER_DUMP_DEADLINE;
            break;
        }
        status = read_block(block == ST25TB_BLOCK_COUNT ? ST25TB_SYSTEM_BLOCK : block, reply);
        if (status) {
            output[HF_READER_DUMP_STATUS_OFFSET] = status;
            output[HF_READER_DUMP_RESULT_OFFSET] = status == STATUS_BUSY
                ? HF_READER_DUMP_DEADLINE : HF_READER_DUMP_PARTIAL;
            break;
        }
        if (block == ST25TB_BLOCK_COUNT) {
            memcpy(output + ST25TB_DUMP_SYSTEM_OFFSET, reply, ST25TB_BLOCK_BYTES);
            output[HF_READER_DUMP_KNOWN_FIELDS_OFFSET] |= ST25TB_DUMP_SYSTEM_KNOWN;
            output[HF_READER_DUMP_RESULT_OFFSET] = HF_READER_DUMP_COMPLETE;
        } else {
            uint8_t *unit = output + HF_READER_HEADER_BYTES + block * HF_READER_UNIT_BYTES;
            unit[0] = 1;
            memcpy(unit + HF_READER_UNIT_DATA_OFFSET, reply, ST25TB_BLOCK_BYTES);
        }
        bsp_wdt_feed();
    }
    return STATUS_HF_TAG_OK;
}
