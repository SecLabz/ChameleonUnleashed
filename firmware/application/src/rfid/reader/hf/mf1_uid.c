#include "mf1_uid.h"
#include "rc522.h"
#include "app_status.h"
#include "bsp_delay.h"
#include <string.h>

static uint16_t select_expected(const uint8_t *uid, uint8_t length,
        const picc_14a_tag_t *expected, picc_14a_tag_t *tag) {
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    bsp_delay_ms(5);
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    uint16_t status = pcd_14a_reader_scan_auto(tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag->uid_len != length || memcmp(tag->uid, uid, length) ||
            (expected && (tag->sak != expected->sak || memcmp(tag->atqa, expected->atqa, 2)))) {
        return STATUS_HF_TAG_NO;
    }
    if (tag->sak != 8 && tag->sak != 9 && tag->sak != 0x10 && tag->sak != 0x18) {
        return STATUS_HF_TAG_NO;
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t exchange(const uint8_t *bytes, uint8_t length, uint8_t *reply, uint16_t *size) {
    uint8_t frame[DEF_FIFO_LENGTH];
    memcpy(frame, bytes, length);
    return pcd_14a_reader_raw_cmd(false, true, true, false, true, true,
        100, length * 8, frame, reply, size, DEF_FIFO_LENGTH * 8);
}

static bool wakeup(uint8_t value, bool short_frame) {
    uint8_t reply = 0;
    uint16_t bits = 0;
    uint16_t status;
    if (short_frame) {
        status = pcd_14a_reader_bits_transfer(&value, 7, NULL, &reply, NULL, &bits, 8);
    } else {
        status = pcd_14a_reader_bytes_transfer(PCD_TRANSCEIVE, &value, 1, &reply, &bits, 8);
    }
    return status == STATUS_HF_TAG_OK && bits == 4 && (reply & 15) == 0x0a;
}

static uint16_t direct_block(uint8_t method, const uint8_t uid[4], const uint8_t replacement[4], bool check) {
    pcd_14a_reader_halt_tag();
    uint8_t first = method == MF1_UID_GDM20 ? 0x20 : 0x40;
    if (!wakeup(first, true) || (method != MF1_UID_GEN1B && !wakeup(method == MF1_UID_GDM20 ? 0x23 : 0x43, false))) {
        return STATUS_MF_ERR_AUTH;
    }
    uint8_t block[16];
    uint16_t status = pcd_14a_reader_mf1_read(0, block);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (memcmp(block, uid, 4)) {
        return STATUS_CMD_ERR;
    }
    if (check) {
        return STATUS_HF_TAG_OK;
    }
    memcpy(block, replacement, 4);
    block[4] = block[0] ^ block[1] ^ block[2] ^ block[3];
    return pcd_14a_reader_mf1_write(0, block);
}

static uint16_t special_write(const uint8_t *data) {
    uint8_t method = data[0];
    uint8_t uid_length = data[1];
    const uint8_t *old_uid = data + 2;
    const uint8_t *uid = data + 9;
    const uint8_t *password = data + 16;
    bool check = data[20] != 0;
    if (method == MF1_UID_GEN1B || method == MF1_UID_GDM20 || method == MF1_UID_GDM40 || method == MF1_UID_GDM40_ZUID) {
        return direct_block(method, old_uid, uid, check);
    }
    uint8_t reply[DEF_FIFO_LENGTH];
    uint16_t size = 0;
    uint8_t command[23] = {0};
    uint8_t length;
    if (method == MF1_UID_GEN3) {
        const uint8_t read[] = {0x30, 0};
        uint16_t status = exchange(read, sizeof(read), reply, &size);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (size != 16) {
            return STATUS_HF_ERR_STAT;
        }
        if (check) {
            return STATUS_HF_TAG_OK;
        }
        command[0] = 0x90;
        command[1] = 0xfb;
        command[2] = 0xcc;
        command[3] = 0xcc;
        command[4] = uid_length;
        memcpy(command + 5, uid, uid_length);
        length = 5 + uid_length;
    } else if (method == MF1_UID_GEN4) {
        command[0] = 0xcf;
        memcpy(command + 1, password, 4);
        command[5] = 0xce;
        uint16_t status = exchange(command, 7, reply, &size);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (size != 16 || memcmp(reply, old_uid, 4)) {
            return STATUS_HF_TAG_NO;
        }
        if (check) {
            return STATUS_HF_TAG_OK;
        }
        command[5] = 0xcd;
        memcpy(command + 7, reply, 16);
        memcpy(command + 7, uid, 4);
        command[11] = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
        length = 23;
    } else {
        const uint8_t query[] = {0x0a, 0, 0, 0xa6, 0xb0, 0, 0x10};
        uint16_t status = exchange(query, sizeof(query), reply, &size);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (size < 4 || reply[size - 2] != 0x90 || reply[size - 1] != 0) {
            return STATUS_HF_ERR_STAT;
        }
        if (check) {
            return STATUS_HF_TAG_OK;
        }
        memcpy(command, (const uint8_t[]){0x0a, 0, 0, 0xa6, 0xa0, 0, 5}, 7);
        memcpy(command + 7, uid, 4);
        length = 12;
    }
    uint16_t status = exchange(command, length, reply, &size);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (method == MF1_UID_SUPER_GEN1) {
        return size == 4 && (reply[0] & 0xfe) == 0x0a && reply[2] == 0x90 && reply[3] == 0 ? STATUS_HF_TAG_OK : STATUS_HF_ERR_STAT;
    }
    return size == 2 && reply[0] == 0x90 && reply[1] == 0 ? STATUS_HF_TAG_OK : STATUS_HF_ERR_STAT;
}

uint16_t mf1_uid_special(uint16_t length, const uint8_t *data, uint8_t *output, uint16_t *output_length) {
    *output_length = 0;
    if (length != 21 || (data[1] != 4 && data[1] != 7) || data[20] > 1 ||
            (data[1] != 4 && data[0] != MF1_UID_GEN3) ||
            !memcmp(data + 2, data + 9, data[1])) {
        return STATUS_PAR_ERR;
    }
    switch (data[0]) {
    case MF1_UID_GEN1B:
    case MF1_UID_GEN3:
    case MF1_UID_GEN4:
    case MF1_UID_GDM20:
    case MF1_UID_GDM40:
    case MF1_UID_GDM40_ZUID:
    case MF1_UID_SUPER_GEN1:
        break;
    default:
        return STATUS_PAR_ERR;
    }
    for (unsigned i = data[1]; i < 7; i++) {
        if (data[2 + i] || data[9 + i]) {
            return STATUS_PAR_ERR;
        }
    }
    if (data[0] != MF1_UID_GEN4 && (data[16] || data[17] || data[18] || data[19])) {
        return STATUS_PAR_ERR;
    }
    uint16_t timeout = pcd_14a_reader_timeout_get();
    hf14a_config_t saved = *get_hf14a_config();
    hf14a_config_t config = saved;
    config.forcerats = 2;
    set_hf14a_config(&config);
    picc_14a_tag_t before;
    uint16_t status = select_expected(data + 2, data[1], NULL, &before);
    if (status == STATUS_HF_TAG_OK) {
        status = special_write(data);
    }
    if (status == STATUS_HF_TAG_OK && !data[20]) {
        picc_14a_tag_t after;
        status = select_expected(data + 9, data[1], &before, &after);
    }
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    pcd_14a_reader_timeout_set(timeout);
    set_hf14a_config(&saved);
    if (status == STATUS_HF_TAG_OK) {
        output[0] = data[1];
        memcpy(output + 1, data + (data[20] ? 2 : 9), data[1]);
        *output_length = 1 + data[1];
    }
    return status;
}
