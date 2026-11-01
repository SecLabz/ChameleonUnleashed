/* SPDX-License-Identifier: GPL-3.0-only */
#include "hf_reader.h"
#include "hf_radio.h"
#include "iso15693/frame.h"
#include "app_status.h"
#include "bsp_wdt.h"
#include "data_cmd.h"
#include "nrf.h"
#include <string.h>

static uint16_t exchange(uint8_t command, const uint8_t uid[8], unsigned first,
        unsigned count, uint8_t reply[24], size_t *size) {
    uint8_t frame[12] = {0x22, command};
    size_t length = 10;
    if (command == 1) {
        frame[0] = 0x26;
        frame[2] = 0;
        length = 3;
    } else {
        iso15693_uid_to_wire(frame + 2, uid);
        if (command == 0x20 || command == 0x23 || command == 0x2c) frame[length++] = first;
        if (command == 0x23 || command == 0x2c) frame[length++] = count - 1;
    }
    return hf_radio_exchange(frame, length, 0, reply, 24, size);
}

static bool geometry(const uint8_t *reply, size_t size, const uint8_t uid[8],
        uint8_t *snapshot, unsigned *blocks, unsigned *block_size) {
    if (size < 12 || reply[0] || (reply[1] & 0xf0) ||
            !iso15693_wire_uid_matches(reply + 2, uid)) return false;
    unsigned at = 10;
    for (unsigned field = 0; field < 4; field++) {
        if (!(reply[1] & (1u << field))) continue;
        unsigned length = field == 2 ? 2 : 1;
        if (at + length > size - 2) return false;
        if (field == 2) {
            *blocks = reply[at] + 1u;
            *block_size = (reply[at + 1] & 31u) + 1u;
        } else if (snapshot) {
            unsigned target = field == 3 ? 18 : 16 + field;
            snapshot[target] = reply[at];
            snapshot[15] |= 1u << (field == 3 ? 2 : field);
        }
        at += length;
    }
    return at == size - 2;
}

/* Passwords are four hexadecimal bytes in RF order, matching capture secrets.
 * Authentication and the requested operation share one RF field; no host-side
 * session is retained. */
uint16_t nfcv_unlock_privacy(const uint8_t password[4]) {
    uint8_t random_request[] = {0x02, 0xb2, 0x04};
    uint8_t reply[24];
    size_t size;
    uint16_t status = hf_radio_exchange(random_request, sizeof(random_request), 0, reply, sizeof(reply), &size);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (size != 5 || reply[0]) {
        return STATUS_HF_ERR_STAT;
    }
    uint8_t request[] = {0x02, 0xb3, 0x04, 0x04, 0, 0, 0, 0};
    for (unsigned index = 0; index < 4; index++) {
        request[4 + index] = password[index] ^ reply[1 + index % 2];
    }
    status = hf_radio_exchange(request, sizeof(request), 0, reply, sizeof(reply), &size);
    if (status == STATUS_HF_TAG_NO) {
        return STATUS_MF_ERR_AUTH;
    }
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    return size == 3 && !reply[0] ? STATUS_HF_TAG_OK : STATUS_MF_ERR_AUTH;
}

/* A bounded sequence shares one RF field, optionally retained between commands.
 * Request: count (bit 7 keeps the field after success), then [frame length, frame without CRC]. Reply: count, then
 * [status BE16, reply length, reply including CRC]. Stop at the first failure. */
uint16_t nfcv_raw(uint16_t length, const uint8_t *data, uint8_t *output, uint16_t *size, bool *complete) {
    *complete = false;
    if (length < 3) {
        return STATUS_PAR_ERR;
    }
    unsigned count = data[0] & NFCV_RAW_COUNT_MASK;
    if (!count || count > 12) {
        return STATUS_PAR_ERR;
    }
    unsigned offset = 1;
    for (unsigned index = 0; index < count; index++) {
        if (offset >= length || !data[offset] || data[offset] > 22 || offset + 1u + data[offset] > length) {
            return STATUS_PAR_ERR;
        }
        offset += 1u + data[offset];
    }
    if (offset != length) {
        return STATUS_PAR_ERR;
    }
    offset = 1;
    *size = 1;
    output[0] = 0;
    for (unsigned index = 0; index < count; index++) {
        uint8_t reply[24];
        size_t received;
        uint8_t frame_length = data[offset++];
        uint16_t status = hf_radio_exchange(data + offset, frame_length, 0, reply, sizeof(reply), &received);
        output[(*size)++] = status >> 8;
        output[(*size)++] = status;
        output[(*size)++] = received;
        memcpy(output + *size, reply, received);
        *size += received;
        output[0]++;
        if (status != STATUS_HF_TAG_OK) {
            break;
        }
        offset += frame_length;
    }
    *complete = output[0] == count && offset == length;
    return STATUS_HF_TAG_OK;
}

/* Option-flag READ returns the block security byte on SLIX and SLIX-L.
 * SLIX-L does not implement GET MULTIPLE BLOCK SECURITY STATUS. */
static uint16_t read_security(const uint8_t uid[8], uint8_t block, uint8_t reply[24], size_t *size) {
    uint8_t frame[11] = {0x62, 0x20};
    iso15693_uid_to_wire(frame + 2, uid);
    frame[10] = block;
    uint16_t status = hf_radio_exchange(frame, sizeof(frame), 0, reply, 24, size);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (*size != 8 || reply[0] || (reply[1] & ~1u)) {
        return STATUS_HF_ERR_STAT;
    }
    return STATUS_HF_TAG_OK;
}

uint16_t nfcv_write_block(const uint8_t *data, uint8_t *output, uint16_t *size) {
    uint8_t reply[24];
    size_t received;
    unsigned blocks = 0;
    unsigned width = 0;
    uint16_t status = exchange(0x2b, data, 0, 1, reply, &received);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (!geometry(reply, received, data, NULL, &blocks, &width) || width != 4 || data[8] >= blocks) {
        return STATUS_PAR_ERR;
    }
    status = read_security(data, data[8], reply, &received);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (reply[1] || memcmp(reply + 2, data + 9, 4)) {
        return STATUS_CMD_ERR;
    }
    if (!memcmp(data + 9, data + 13, 4)) {
        memcpy(output, data + 13, 4);
        *size = 4;
        return STATUS_HF_TAG_OK;
    }
    uint8_t frame[15] = {0x22, 0x21};
    iso15693_uid_to_wire(frame + 2, data);
    frame[10] = data[8];
    memcpy(frame + 11, data + 13, 4);
    status = hf_radio_exchange(frame, sizeof(frame), 0, reply, sizeof(reply), &received);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (received != 3 || reply[0]) {
        return STATUS_HF_ERR_STAT;
    }
    status = exchange(0x20, data, data[8], 1, reply, &received);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (received != 7 || reply[0] || memcmp(reply + 1, data + 13, 4)) {
        return STATUS_HF_ERR_STAT;
    }
    memcpy(output, reply + 1, 4);
    *size = 4;
    return STATUS_HF_TAG_OK;
}

uint16_t nfcv_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    uint8_t reply[24];
    size_t received;
    if (command != DATA_CMD_HF15_DUMP) {
        uint8_t rf_command = command == DATA_CMD_HF15_INVENTORY ? 1 :
            command == DATA_CMD_HF15_GET_SYSTEM_INFO ? 0x2b :
            command == DATA_CMD_HF15_SECURITY ? 0x2c : 0x20;
        uint16_t status = exchange(rf_command, data,
            rf_command == 0x20 || rf_command == 0x2c ? data[8] : 0,
            rf_command == 0x2c ? data[9] : 1, reply, &received);
        if (status) return status;
        if (received < 3) return STATUS_HF_ERR_STAT;
        if (reply[0]) {
            memcpy(output, reply, received);
            *size = received;
            return STATUS_HF_ERR_STAT;
        }
        if (rf_command == 1 && received != 12) return STATUS_HF_ERR_STAT;
        if (rf_command == 0x20 && received < 4) return STATUS_HF_ERR_STAT;
        if (rf_command == 0x2c && received != (size_t)data[9] + 3) return STATUS_HF_ERR_STAT;
        if (rf_command == 0x2b) {
            unsigned blocks = 0, width = 0;
            if (!geometry(reply, received, data, NULL, &blocks, &width)) return STATUS_HF_ERR_STAT;
        }
        memcpy(output, reply, received);
        *size = received;
        return STATUS_HF_TAG_OK;
    }
    unsigned blocks = data[9], width = data[10];
    hf_reader_snapshot(output, 3, data, blocks, size);
    uint16_t status = exchange(0x2b, data, 0, 1, reply, &received);
    if (status) { *size = 0; return status; }
    if (received >= 4 && (reply[0] & 1u)) {
        /* An unsupported optional command permits an explicitly supplied geometry. */
        if (reply[1] != 1) { *size = 0; return STATUS_HF_ERR_STAT; }
    } else {
        unsigned actual_blocks = 0, actual_width = 0;
        if (!geometry(reply, received, data, output, &actual_blocks, &actual_width) ||
                (actual_blocks && (actual_blocks != blocks || actual_width != width)))
            { *size = 0; return STATUS_HF_ERR_STAT; }
    }
    /* Small tags fit the budget without probing an optional command. */
    bool multiple = blocks > 8;
    for (unsigned block = 0; block < blocks;) {
        if (hf_radio_expired()) {
            output[2] = 2;
            break;
        }
        unsigned count = multiple ? blocks - block : 1;
        if (count > (sizeof(reply) - 3) / width) count = (sizeof(reply) - 3) / width;
        status = exchange(count > 1 ? 0x23 : 0x20, data, block, count, reply, &received);
        if (!status && count > 1 && received == 4 && (reply[0] & 1) && reply[1] == 1) {
            multiple = false;
            continue;
        }
        if (status || received != count * width + 3 || reply[0]) {
            output[3] = status ? status : STATUS_HF_ERR_STAT;
            if (status == STATUS_BUSY) output[2] = 2;
            if (!status && received >= 4 && (reply[0] & 1)) output[19] = reply[1];
            break;
        }
        for (unsigned i = 0; i < count; i++) {
            uint8_t *unit = output + HF_READER_HEADER_BYTES + (block + i) * 6;
            unit[0] = 1;
            memcpy(unit + 2, reply + 1 + i * width, width);
        }
        block += count;
        if (block == blocks) output[2] = 0;
        bsp_wdt_feed();
    }
    for (unsigned first = 0; first < blocks && !hf_radio_expired(); first += 16) {
        unsigned count = blocks - first < 16 ? blocks - first : 16;
        status = exchange(0x2c, data, first, count, reply, &received);
        if (!status && received == 4 && (reply[0] & 1u) && reply[1] == 1) {
            for (unsigned block = first; block < blocks && !hf_radio_expired(); block++) {
                status = read_security(data, block, reply, &received);
                if (status != STATUS_HF_TAG_OK) {
                    break;
                }
                output[HF_READER_HEADER_BYTES + block * 6 + 1] = 1 + reply[1];
                bsp_wdt_feed();
            }
            break;
        }
        if (status || received != count + 3 || reply[0]) break;
        for (unsigned i = 0; i < count; i++) {
            if (reply[i + 1] & ~1u) continue;
            output[HF_READER_HEADER_BYTES + (first + i) * 6 + 1] = 1 + (reply[i + 1] & 1u);
        }
        bsp_wdt_feed();
    }
    return STATUS_HF_TAG_OK;
}
