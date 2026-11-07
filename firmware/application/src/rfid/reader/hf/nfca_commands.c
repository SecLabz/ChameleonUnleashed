#include "nfca_commands.h"
#include "mf1_magic.h"
#include "mf1_uid.h"
#include "type2_commands.h"
#include "mf1_toolbox.h"
#include "nfca_session.h"
#include "hf_radio.h"
#include "mf1_capture_stats.h"
#include "rc522.h"
#include "rfid_main.h"
#include "hex_utils.h"
#include "data_cmd.h"
#include "app_status.h"
#include <string.h>

uint16_t nfca_prepare(uint16_t command, uint16_t length, const uint8_t *data) {
    if (get_device_mode() != DEVICE_MODE_READER) return STATUS_DEVICE_MODE_ERROR;
    if (!hf_radio_release()) {
        return STATUS_HF_ERR_STAT;
    }
    nfca_session_poll();
    if (command == DATA_CMD_MF1_MAGIC_PROBE || command == DATA_CMD_MF1_INSPECT || command == DATA_CMD_MF1_MAINTENANCE) {
        nfca_session_close();
    }
    if (command == DATA_CMD_MF1_READ_SECTOR && length != 8) {
        if (!nfca_session_reading()) nfca_session_read_open();
        if (nfca_session_ready()) return STATUS_SUCCESS;
    } else if (nfca_session_reading() || command == DATA_CMD_MF1_READ_SECTOR) {
        nfca_session_close();
    }
    if (command == DATA_CMD_MF1_DARKSIDE_STEP && length >= 3 && data[2] == 0) nfca_session_close();
    if (command == DATA_CMD_MF1_COLLECT_OBSERVATIONS) {
        mf1_capture_stats_start(length == 13 && data[12] == 1);
    }
    if ((command == DATA_CMD_MF1_COLLECT_OBSERVATIONS || command == DATA_CMD_TYPE2_READ ||
         command == DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK || command == DATA_CMD_MF1_DARKSIDE_STEP) && nfca_session_ready()) return STATUS_SUCCESS;
    uint32_t started = mf1_capture_clock();
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    mf1_capture_stats.setup_cycles += mf1_capture_elapsed(started);
    return STATUS_SUCCESS;
}

void nfca_finish(uint16_t command) {
    if ((command != DATA_CMD_MF1_COLLECT_OBSERVATIONS && command != DATA_CMD_TYPE2_READ &&
         command != DATA_CMD_MF1_READ_SECTOR &&
         command != DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK && command != DATA_CMD_MF1_DARKSIDE_STEP) || !nfca_session_finish()) {
        pcd_14a_reader_antenna_off();
    }
    mf1_capture_stats.enabled = false;
}

uint16_t mf1_collect_command(uint16_t length, uint8_t *data, uint8_t *output, uint16_t *output_length) {
    *output_length = 0;

    if ((length != 12 && length != 13) || (length == 13 && data[12] != 1) ||
        data[10] > 1 || data[11] == 0 || data[11] > 16) {
        nfca_session_close();
        return STATUS_PAR_ERR;
    }
    uint8_t offset = length == 13 ? MF1_CAPTURE_STATS_BYTES : 0;
    memset(output, 0, offset + data[11] * 12);
    uint16_t status = mf1_collect_observations(data[1], data[0], bytes_to_num(data + 2, 6),
        data[9], data[8], data[10] != 0, data[11], output + offset);
    if (status != STATUS_HF_TAG_OK) nfca_session_close();
    if (offset) { mf1_capture_stats_encode(output); *output_length = offset; }
    if (status == STATUS_HF_TAG_OK) *output_length = offset + data[11] * 12;
    return status;
}

/* Request: key type, sector (0..39), six key bytes. Response: big-endian
 * readable-block mask followed by 4 or 16 blocks; unread bytes are zero.
 * An optional UID length and UID enable checked reads with a retained RF lease.
 * Appending byte 1 to a checked request adds four diagnostic bytes after the
 * unchanged body: version 1, first failed offset (FF if complete), RF status BE16.
 * A read failure ends this authentication. Callers can retry missing blocks
 * independently when access permissions differ within a sector. */
static uint16_t read_sector(uint16_t length, uint8_t *data, uint8_t *output,
                            uint16_t *output_length) {
    *output_length = 0;
    bool diagnostic = length >= 9 && length == 10 + data[8] && data[length - 1] == 1;
    if (length < 8 || (data[0] != 0x60 && data[0] != 0x61) || data[1] >= 40 ||
        (length != 8 && ((data[8] != 4 && data[8] != 7 && data[8] != 10) ||
                         (length != 9 + data[8] && !diagnostic)))) {
        nfca_session_close();
        return STATUS_PAR_ERR;
    }
    uint8_t first = data[1] < 32 ? data[1] * 4 : 128 + (data[1] - 32) * 16;
    uint8_t count = data[1] < 32 ? 4 : 16;
    uint16_t status;
    if (length == 8) {
        status = auth_key_use_522_hw(first, data[0], data + 2);
    } else {
        picc_14a_tag_t tag;
        status = pcd_14a_reader_scan_auto(&tag);
        if (status == STATUS_HF_TAG_OK &&
            (tag.uid_len != data[8] || memcmp(tag.uid, data + 9, data[8])))
            status = STATUS_HF_TAG_NO;
        if (status == STATUS_HF_TAG_OK)
            status = pcd_14a_reader_mf1_auth(&tag, data[0], first, data + 2);
    }
    if (status != STATUS_HF_TAG_OK) {
        nfca_session_close();
        return status;
    }
    memset(output, 0, 2 + count * 16);
    uint16_t mask = 0;
    uint8_t failed = 0xff;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t block[16];
        status = pcd_14a_reader_mf1_read(first + i, block);
        if (status != STATUS_HF_TAG_OK) {
            failed = i;
            break;
        }
        memcpy(output + 2 + i * 16, block, 16);
        mask |= 1u << i;
    }
    output[0] = mask >> 8;
    output[1] = mask;
    *output_length = 2 + count * 16;
    if (diagnostic) {
        uint8_t *tail = output + *output_length;
        tail[0] = 1;
        tail[1] = failed;
        tail[2] = status >> 8;
        tail[3] = status;
        *output_length += 4;
    }
    if (status != STATUS_HF_TAG_OK || length == 8) {
        nfca_session_close();
    } else {
        /* Halt under the current cipher, then start the next WUPA in plaintext. */
        pcd_14a_reader_fast_halt_tag();
        pcd_14a_reader_mf1_unauth();
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t type2_model_check(uint8_t model, const uint8_t *uid) {
    if (model == 19) {
        uint8_t request[4] = {0x1a, 0};
        uint8_t response[11];
        uint16_t length = 0;
        uint16_t status = pcd_14a_reader_raw_cmd(false, true, true, false, true, true,
            25, 16, request, response, &length, sizeof(response) * 8);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (length != 9 || response[0] != 0xaf) {
            return STATUS_HF_ERR_STAT;
        }
        // End the identification challenge before READ or a full authentication.
        pcd_14a_reader_fast_halt_tag();
        picc_14a_tag_t tag;
        status = pcd_14a_reader_scan_auto(&tag);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (tag.sak != 0 || tag.uid_len != 7 || memcmp(tag.uid, uid, 7)) {
            return STATUS_HF_TAG_NO;
        }
        return STATUS_HF_TAG_OK;
    }
    static const uint8_t expected[][3] = {
        {11, 3, 0x0b}, {12, 3, 0x0e}, {20, 4, 0x0b},
        {21, 4, 0x0e}, {8, 4, 0x0f}, {2, 4, 0x11}, {9, 4, 0x13}
    };
    const uint8_t *profile = NULL;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (expected[i][0] == model) {
            profile = expected[i];
        }
    }
    if (!profile) {
        return STATUS_NOT_IMPLEMENTED;
    }
    uint8_t request[3] = {0x60};
    uint8_t version[10];
    uint16_t length = 0;
    uint16_t status = pcd_14a_reader_raw_cmd(false, true, true, false, true, true,
        25, 8, request, version, &length, sizeof(version) * 8);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t product = model == 11 || model == 12 || model == 20 || model == 21 ? 1 : 2;
    return length == 8 && version[0] == 0 && version[1] == 4 &&
           version[2] == profile[1] &&
           (version[3] == product || (profile[1] == 3 && version[3] == 2)) &&
           version[4] == 1 && version[5] == 0 && version[6] == profile[2] &&
           version[7] == 3 ? STATUS_HF_TAG_OK : STATUS_HF_TAG_NO;
}

static uint16_t type2_password_auth(const uint8_t *key) {
    uint8_t request[7] = {0x1b};
    uint8_t response[4];
    uint16_t length = 0;
    memcpy(request + 1, key, 4);
    uint16_t status = pcd_14a_reader_raw_cmd(false, true, true, false, true, true,
        25, 5 * 8, request, response, &length, sizeof(response) * 8);
    return status == STATUS_HF_TAG_OK && length != 2 ? STATUS_MF_ERR_AUTH : status;
}

static uint16_t type2_auth_read(uint16_t length, const uint8_t *data,
                                uint8_t *output, uint16_t *output_length) {
    if (length != 27 || data[1] != 7 ||
        (data[10] != 0 && data[10] != (data[0] == 19 ? 4 : 3))) {
        return STATUS_PAR_ERR;
    }
    uint8_t pages;
    switch (data[0]) {
    case 2:
        pages = 135;
        break;
    case 8:
        pages = 45;
        break;
    case 9:
        pages = 231;
        break;
    case 11:
    case 20:
        pages = 20;
        break;
    case 12:
    case 21:
        pages = 41;
        break;
    case 19:
        pages = 44;
        break;
    default:
        return STATUS_PAR_ERR;
    }
    if (data[9] >= pages) {
        return STATUS_PAR_ERR;
    }
    size_t credential_size = 0;
    if (data[10] == 4) {
        credential_size = 16;
    } else if (data[10] == 3) {
        credential_size = 4;
    }
    for (size_t i = credential_size; i < 16; i++) {
        if (data[11 + i]) {
            return STATUS_PAR_ERR;
        }
    }
    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.sak != 0 || tag.uid_len != 7 || memcmp(tag.uid, data + 2, 7)) {
        return STATUS_HF_TAG_NO;
    }
    status = type2_model_check(data[0], data + 2);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (data[10] == 4) {
        status = pcd_14a_reader_mf0_ulc_auth((uint8_t *)data + 11);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
    }
    if (data[10] == 3) {
        status = type2_password_auth(data + 11);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
    }
    status = pcd_14a_reader_mf1_read(data[9], output);
    if (status == STATUS_HF_TAG_OK) {
        *output_length = 16;
    }
    return status;
}

static uint16_t classic_model_check(uint8_t model, const picc_14a_tag_t *tag) {
    uint8_t sak;
    switch (model) {
    case 1: sak = 8; break;
    case 5: sak = 9; break;
    case 6:
        if (tag->sak == 8) {
            return mf1_classic_2k_sl1_check(tag->uid_len, tag->uid, tag->atqa, tag->sak);
        }
        sak = 0x10;
        break;
    case 7: sak = 0x18; break;
    default: return STATUS_PAR_ERR;
    }
    return (tag->sak & ~0x20u) == sak ? STATUS_HF_TAG_OK : STATUS_HF_TAG_NO;
}

/* One guarded write. The host must preflight every target unit first. */
static uint16_t write_user_unit(uint16_t length, const uint8_t *data,
                                uint8_t *output, uint16_t *output_length) {
    enum { REQUEST_BYTES = 59, UID_BYTE = 2, UNIT_BYTE = 9,
           KIND_BYTE = 10, CREDENTIAL_BYTE = 11, BEFORE_BYTE = 27,
           AFTER_BYTE = 43 };
    if (length != REQUEST_BYTES || (data[1] != 4 && data[1] != 7)) {
        return STATUS_PAR_ERR;
    }
    uint8_t model = data[0];
    uint8_t unit = data[UNIT_BYTE];
    uint8_t kind = data[KIND_BYTE];
    bool classic = model == 1 || model == 5 || model == 6 || model == 7;
    uint16_t count = 0;
    switch (model) {
    case 1:
        count = 64;
        break;
    case 5:
        count = 20;
        break;
    case 6:
        count = 128;
        break;
    case 7:
        count = 256;
        break;
    case 2:
        count = 130;  /* NTAG215: dynamic lock at 130. */
        break;
    case 8:
        count = 40;   /* NTAG213. */
        break;
    case 9:
        count = 226;  /* NTAG216. */
        break;
    case 10:
        count = 16;  /* Ultralight. */
        break;
    case 11:
        count = 16;  /* Ultralight EV1 48. */
        break;
    case 12:
        count = 36;  /* Ultralight EV1 128; page 36 locks data. */
        break;
    case 19:
        count = 40;  /* Ultralight C. */
        break;
    case 20:
        count = 16;  /* NTAG210. */
        break;
    case 21:
        count = 36;  /* NTAG212. */
        break;
    default:
        return STATUS_PAR_ERR;
    }
    if (unit >= count || (classic && (unit == 0 ||
        (unit < 128 ? unit % 4 == 3 : unit % 16 == 15))) ||
        (!classic && unit < 4) ||
        (classic && (kind != 1 && kind != 2)) ||
        (!classic && kind > 4) ||
        (!classic && kind == 1) || (!classic && kind == 2) ||
        (classic && data[1] != 4 && data[1] != 7) ||
        (!classic && data[1] != 7)) {
        return STATUS_PAR_ERR;
    }
    size_t credential_size = 0;
    if (kind == 1 || kind == 2) {
        credential_size = 6;
    } else if (kind == 3) {
        credential_size = 4;
    } else if (kind == 4) {
        credential_size = 16;
    }
    if ((kind == 4 && model != 19) || (kind == 3 && model == 19)) {
        return STATUS_PAR_ERR;
    }
    for (size_t i = credential_size; i < 16; i++) {
        if (data[CREDENTIAL_BYTE + i]) {
            return STATUS_PAR_ERR;
        }
    }
    if (!classic) {
        for (size_t i = 4; i < 16; i++) {
            if (data[BEFORE_BYTE + i] || data[AFTER_BYTE + i]) {
                return STATUS_PAR_ERR;
            }
        }
    }

    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != data[1] || memcmp(tag.uid, data + UID_BYTE, tag.uid_len) ||
        (!classic && tag.sak != 0)) {
        return STATUS_HF_TAG_NO;
    }
    if (classic) {
        status = classic_model_check(model, &tag);
        if (status != STATUS_HF_TAG_OK) return status;
    }
    if (!classic) {
        status = type2_model_check(model, data + UID_BYTE);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
    }
    if (classic) {
        status = pcd_14a_reader_mf1_auth(&tag, kind == 1 ? 0x60 : 0x61,
                                         unit, (uint8_t *)data + CREDENTIAL_BYTE);
    } else if (kind == 4) {
        status = pcd_14a_reader_mf0_ulc_auth((uint8_t *)data + CREDENTIAL_BYTE);
    } else if (kind == 3) {
        status = type2_password_auth(data + CREDENTIAL_BYTE);
    }
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }

    uint8_t before[16];
    status = pcd_14a_reader_mf1_read(unit, before);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (memcmp(before, data + BEFORE_BYTE, classic ? 16 : 4)) {
        return STATUS_CMD_ERR;
    }
    status = classic ? pcd_14a_reader_mf1_write(unit, (uint8_t *)data + AFTER_BYTE)
                     : pcd_14a_reader_mf0_ult_write_page(unit, (uint8_t *)data + AFTER_BYTE);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    status = pcd_14a_reader_mf1_read(unit, output);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (memcmp(output, data + AFTER_BYTE, classic ? 16 : 4)) {
        return STATUS_CMD_ERR;
    }
    *output_length = classic ? 16 : 4;
    return STATUS_HF_TAG_OK;
}

static uint16_t uid_set(uint16_t length, const uint8_t *data,
                         uint8_t *output, uint16_t *output_length) {
    if ((length != 15 && length != 22) || data[0] != 4 ||
        !memcmp(data + 1, data + 8, data[0])) return STATUS_PAR_ERR;
    if (length == 22 && data[15] != 1 && data[15] != 2) return STATUS_PAR_ERR;
    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) return status;
    if (tag.uid_len != data[0] || memcmp(tag.uid, data + 1, tag.uid_len) ||
        !(tag.sak == 8 || tag.sak == 9 || tag.sak == 0x10 || tag.sak == 0x18)) return STATUS_HF_TAG_NO;
    status = length == 15 ? pcd_14a_reader_gen1a_unlock() :
        pcd_14a_reader_mf1_auth(&tag, data[15] == 1 ? 0x60 : 0x61,
                               0, (uint8_t *)data + 16);
    if (status != STATUS_HF_TAG_OK) return status;
    uint8_t block[16];
    status = pcd_14a_reader_mf1_read(0, block);
    if (status != STATUS_HF_TAG_OK) return status;
    if (memcmp(block, data + 1, 4)) return STATUS_CMD_ERR;
    memcpy(block, data + 8, 4);
    block[4] = block[0] ^ block[1] ^ block[2] ^ block[3];
    status = pcd_14a_reader_mf1_write(0, block);
    if (status != STATUS_HF_TAG_OK) return status;
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) return status;
    if (tag.uid_len != data[0] || memcmp(tag.uid, data + 8, tag.uid_len))
        return STATUS_CMD_ERR;
    output[0] = tag.uid_len;
    memcpy(output + 1, tag.uid, tag.uid_len);
    *output_length = 1 + tag.uid_len;
    return STATUS_HF_TAG_OK;
}

/* Same guarded block layout as WRITE_USER_UNIT, restricted to confirmed
 * four-byte Gen1A/CUID targets. A final byte of 1 performs only the check. */
static uint16_t block0_set(uint16_t length, const uint8_t *data,
                            uint8_t *output, uint16_t *output_length) {
    enum { REQUEST_BYTES = 59, BEFORE_BYTE = 27, AFTER_BYTE = 43 };
    if ((length != REQUEST_BYTES && length != REQUEST_BYTES + 1) ||
        (length == REQUEST_BYTES + 1 && data[REQUEST_BYTES] != 1)) {
        return STATUS_PAR_ERR;
    }
    const uint8_t *before = data + BEFORE_BYTE;
    const uint8_t *after = data + AFTER_BYTE;
    if (data[1] != 4 || data[9] != 0 || data[10] > 2 ||
        (data[0] != 1 && data[0] != 5 && data[0] != 6 && data[0] != 7) ||
        memcmp(data + 2, before, 4) ||
        (after[0] ^ after[1] ^ after[2] ^ after[3]) != after[4]) {
        return STATUS_PAR_ERR;
    }
    for (unsigned i = 6; i < 9; i++) {
        if (data[i]) {
            return STATUS_PAR_ERR;
        }
    }
    for (unsigned i = data[10] ? 6 : 0; i < 16; i++) {
        if (data[11 + i]) {
            return STATUS_PAR_ERR;
        }
    }

    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != 4 || memcmp(tag.uid, data + 2, 4)) {
        return STATUS_HF_TAG_NO;
    }
    status = classic_model_check(data[0], &tag);
    if (status != STATUS_HF_TAG_OK) return status;
    picc_14a_tag_t expected = tag;
    uint8_t expected_sak = tag.sak & ~0x20u;
    uint8_t probe[21] = {4};
    memcpy(probe + 1, tag.uid, 4);
    memcpy(probe + 11, tag.atqa, 2);
    probe[13] = tag.sak;
    probe[14] = data[10];
    memcpy(probe + 15, data + 11, 6);
    uint8_t evidence[3];
    uint16_t evidence_length;
    status = mf1_magic_probe(data[10] ? 21 : 15, probe, evidence, &evidence_length);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (evidence_length != 3) {
        return STATUS_CMD_ERR;
    }
    uint16_t flags = (uint16_t)evidence[0] << 8 | evidence[1];
    uint16_t required = data[10] ? MF1_MAGIC_GEN2 : MF1_MAGIC_GEN1A;
    if (!(flags & required) ||
        (flags & ~(MF1_MAGIC_GEN1A | MF1_MAGIC_GEN2))) {
        return STATUS_CMD_ERR;
    }

    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != 4 || memcmp(tag.uid, data + 2, 4) ||
        tag.sak != expected.sak || memcmp(tag.atqa, expected.atqa, 2)) {
        return STATUS_HF_TAG_NO;
    }
    if (data[10]) {
        status = pcd_14a_reader_mf1_auth(&tag, data[10] == 1 ? 0x60 : 0x61,
                                        0, (uint8_t *)data + 11);
    } else {
        status = pcd_14a_reader_gen1a_unlock();
    }
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    status = pcd_14a_reader_mf1_read(0, output);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (memcmp(output, before, 16)) {
        return STATUS_CMD_ERR;
    }
    if (length == REQUEST_BYTES + 1) {
        *output_length = 16;
        return STATUS_HF_TAG_OK;
    }
    status = pcd_14a_reader_mf1_write(0, (uint8_t *)after);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != 4 || memcmp(tag.uid, after, 4) ||
        (tag.sak & ~0x20u) != expected_sak) {
        return STATUS_CMD_ERR;
    }
    if (data[10]) {
        status = pcd_14a_reader_mf1_auth(&tag, data[10] == 1 ? 0x60 : 0x61,
                                        0, (uint8_t *)data + 11);
    } else {
        status = pcd_14a_reader_gen1a_unlock();
    }
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    status = pcd_14a_reader_mf1_read(0, output);
    if (status != STATUS_HF_TAG_OK || memcmp(output, after, 16)) {
        return STATUS_CMD_ERR;
    }
    *output_length = 16;
    return STATUS_HF_TAG_OK;
}

static bool classic_access_valid(const uint8_t *bytes) {
    uint8_t inverted0 = ~((bytes[0] >> 4) | (bytes[0] << 4));
    uint8_t inverted1 = ~((bytes[1] >> 4) | (bytes[1] << 4));
    return ((inverted0 ^ bytes[1]) & 0xf0) == 0 &&
           ((inverted0 ^ bytes[2]) & 0x0f) == 0 &&
           ((inverted1 ^ bytes[2]) & 0xf0) == 0;
}

static uint16_t trailer_set(uint16_t length, const uint8_t *data,
                            uint8_t *output, uint16_t *output_length) {
    (void)output;
    // The optional final byte checks the same permissions without writing.
    if ((length != 37 && length != 38) || (length == 38 && data[37] != 1) ||
        (data[0] != 4 && data[0] != 7) ||
        (data[10] != 1 && data[10] != 2) ||
        !classic_access_valid(data + 33)) {
        return STATUS_PAR_ERR;
    }
    uint8_t model = data[8];
    uint8_t sector = data[9];
    uint8_t sectors;
    switch (model) {
    case 1:
        sectors = 16;
        break;
    case 5:
        sectors = 5;
        break;
    case 6:
        sectors = 32;
        break;
    case 7:
        sectors = 40;
        break;
    default:
        return STATUS_PAR_ERR;
    }
    if (sector >= sectors) {
        return STATUS_PAR_ERR;
    }
    uint8_t trailer = sector < 32 ? sector * 4 + 3 :
                      128 + (sector - 32) * 16 + 15;
    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != data[0] || memcmp(tag.uid, data + 1, tag.uid_len)) {
        return STATUS_HF_TAG_NO;
    }
    status = classic_model_check(model, &tag);
    if (status != STATUS_HF_TAG_OK) return status;
    status = pcd_14a_reader_mf1_auth(&tag, data[10] == 1 ? 0x60 : 0x61,
                                     trailer, (uint8_t *)data + 11);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t before[16];
    status = pcd_14a_reader_mf1_read(trailer, before);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (!classic_access_valid(before + 6) || memcmp(before + 6, data + 17, 4)) {
        return STATUS_CMD_ERR;
    }
    uint8_t old_condition = ((before[7] >> 7) & 1) << 2 |
                            ((before[8] >> 3) & 1) << 1 | (before[8] >> 7);
    bool key_b = data[10] == 2;
    bool keys_writable = key_b ? old_condition == 4 || old_condition == 3 :
                                 old_condition == 0 || old_condition == 1;
    bool access_writable = key_b ? old_condition == 3 : old_condition == 1;
    if (!keys_writable ||
        (memcmp(before + 6, data + 33, 4) && !access_writable)) {
        return STATUS_CMD_ERR;
    }
    if (length == 38) {
        *output_length = 0;
        return STATUS_HF_TAG_OK;
    }
    uint8_t after[16];
    memcpy(after, data + 21, 6);
    memcpy(after + 6, data + 33, 4);
    memcpy(after + 10, data + 27, 6);
    status = pcd_14a_reader_mf1_write(trailer, after);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (tag.uid_len != data[0] || memcmp(tag.uid, data + 1, tag.uid_len)) {
        return STATUS_HF_TAG_NO;
    }
    status = pcd_14a_reader_mf1_auth(&tag, 0x60, trailer, after);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t observed[16];
    status = pcd_14a_reader_mf1_read(trailer, observed);
    if (status != STATUS_HF_TAG_OK || memcmp(observed + 6, after + 6, 4)) {
        return STATUS_CMD_ERR;
    }
    uint8_t condition = ((observed[7] >> 7) & 1) << 2 |
                        ((observed[8] >> 3) & 1) << 1 | (observed[8] >> 7);
    if (condition <= 2) {
        if (memcmp(observed + 10, after + 10, 6)) {
            return STATUS_CMD_ERR;
        }
    } else {
        pcd_14a_reader_reset();
        pcd_14a_reader_antenna_on();
        bsp_delay_ms(8);
        status = pcd_14a_reader_scan_auto(&tag);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        if (tag.uid_len != data[0] || memcmp(tag.uid, data + 1, tag.uid_len)) {
            return STATUS_HF_TAG_NO;
        }
        status = pcd_14a_reader_mf1_auth(&tag, 0x61, trailer, after + 10);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
    }
    *output_length = 0;
    return STATUS_HF_TAG_OK;
}

/* Bounded maintenance: read(0), preflight(1), write+verify(2), initial read(3).
 * Header: op, generation(1A=1/1B=2/CUID=3), model, UID length,
 * seven padded UID bytes, block, credential(0/KeyA=1/KeyB=2), six key bytes.
 * Preflight/write append 16 observed bytes and 16 target bytes.
 * Read reply: UID length, seven padded UID bytes, ATQA[2], SAK, block[16].
 * Optional trailing 1 on authenticated model-6 requests requires fresh capacity
 * evidence. Legacy requests retain explicit known-model repair semantics.
 * Selection overrides belong to this call, never to the next reader operation. */
static uint16_t maintenance_open(const uint8_t *data, picc_14a_tag_t *tag, bool capacity_check) {
    if (data[1] != 3) {
        uint16_t status = pcd_14a_reader_magic_unlock(data[1] == 1);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        uint8_t block[16];
        status = pcd_14a_reader_mf1_read(0, block);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        memset(tag, 0, sizeof(*tag));
        tag->uid_len = 4;
        memcpy(tag->uid, block, 4);
        memcpy(tag->atqa, block + 6, 2);
        tag->sak = block[5];
    } else {
        uint16_t status = pcd_14a_reader_scan_auto(tag);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
    }
    if (tag->uid_len != data[3] ||
        (data[0] != 3 && memcmp(tag->uid, data + 4, data[3]))) {
        return STATUS_HF_TAG_NO;
    }
    if (data[1] == 3) {
        if (capacity_check) {
            uint16_t status = classic_model_check(data[2], tag);
            if (status != STATUS_HF_TAG_OK) return status;
        }
        return pcd_14a_reader_mf1_auth(tag, data[12] == 1 ? 0x60 : 0x61,
                                     data[11], (uint8_t *)data + 13);
    }
    return STATUS_HF_TAG_OK;
}

static void maintenance_reply(const picc_14a_tag_t *tag, const uint8_t *block,
                              uint8_t *output, uint16_t *length) {
    memset(output, 0, 27);
    output[0] = tag->uid_len;
    memcpy(output + 1, tag->uid, tag->uid_len);
    memcpy(output + 8, tag->atqa, 2);
    output[10] = tag->sak;
    memcpy(output + 11, block, 16);
    *length = 27;
}

static uint16_t maintenance_run(const uint8_t *data, uint8_t *output, uint16_t *length, bool capacity_check) {
    const uint8_t model = data[2];
    const uint8_t unit = data[11];
    const bool trailer = unit < 128 ? unit % 4 == 3 : unit % 16 == 15;
    picc_14a_tag_t tag;
    uint16_t status = maintenance_open(data, &tag, capacity_check);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t before[16];
    status = pcd_14a_reader_mf1_read(unit, before);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (data[0] == 0 || data[0] == 3) {
        maintenance_reply(&tag, before, output, length);
        return STATUS_HF_TAG_OK;
    }
    const uint8_t *after = data + 35;
    const bool hidden_keys = trailer && data[1] == 3;
    const uint8_t compare_offset = hidden_keys ? 6 : 0;
    const uint8_t compare_bytes = hidden_keys ? 4 : 16;
    if (memcmp(before + compare_offset, data + 19 + compare_offset, compare_bytes)) {
        return STATUS_CMD_ERR;
    }
    if (trailer && !classic_access_valid(after + 6)) {
        return STATUS_PAR_ERR;
    }
    if (unit == 0) {
        uint8_t sak = 8;
        if (model == 5) {
            sak = 9;
        } else if (model == 6) {
            sak = capacity_check ? tag.sak : after[data[3] == 4 ? 5 : 7];
            if (sak != 8 && sak != 0x10) return STATUS_PAR_ERR;
        } else if (model == 7) {
            sak = 0x18;
        }
        uint8_t offset = data[3] == 4 ? 5 : 7;
        uint8_t atqa = data[3] == 4 ? 4 : 0x44;
        if (model == 7) {
            atqa = data[3] == 4 ? 2 : 0x42;
        }
        if ((data[3] == 4 && (after[0] ^ after[1] ^ after[2] ^ after[3]) != after[4]) ||
            after[offset] != sak || after[offset + 1] != atqa || after[offset + 2] != 0) {
            return STATUS_PAR_ERR;
        }
    }
    if (data[1] == 3 && trailer) {
        uint8_t request[38] = {0};
        request[0] = data[3];
        memcpy(request + 1, tag.uid, tag.uid_len);
        request[8] = model;
        request[9] = unit < 128 ? unit / 4 : 32 + (unit - 128) / 16;
        request[10] = data[12];
        memcpy(request + 11, data + 13, 6);
        memcpy(request + 17, before + 6, 4);
        memcpy(request + 21, after, 6);
        memcpy(request + 27, after + 10, 6);
        memcpy(request + 33, after + 6, 4);
        request[37] = 1;
        return trailer_set(data[0] == 1 ? 38 : 37, request, output, length);
    }
    if (data[1] == 3) {
        uint8_t access[16];
        uint8_t trailer_block = unit < 128 ? (unit / 4) * 4 + 3 : (unit / 16) * 16 + 15;
        status = pcd_14a_reader_mf1_read(trailer_block, access);
        if (status != STATUS_HF_TAG_OK || !classic_access_valid(access + 6)) {
            return STATUS_CMD_ERR;
        }
        uint8_t group = unit < 128 ? unit % 4 : (unit % 16) / 5;
        uint8_t condition = ((access[8] >> (group + 4)) & 1) << 2 |
                            ((access[8] >> group) & 1) << 1 | ((~access[6] >> group) & 1);
        bool writable = data[12] == 1 ? condition == 0 :
            condition == 0 || condition == 1 || condition == 3 || condition == 6;
        if (!writable) {
            return STATUS_CMD_ERR;
        }
    }
    if (data[0] == 1) {
        return STATUS_HF_TAG_OK;
    }
    status = pcd_14a_reader_mf1_write(unit, (uint8_t *)after);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    // Verify persistence after RF loss. Block 0 additionally needs normal selection.
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    bsp_delay_ms(5);
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
    uint8_t request[19];
    memcpy(request, data, sizeof(request));
    if (unit == 0) {
        memcpy(request + 4, after, data[3]);
        const hf14a_config_t standard = {0, 0, 0, 0};
        set_hf14a_config(&standard);
        status = pcd_14a_reader_scan_auto(&tag);
        if (status != STATUS_HF_TAG_OK || tag.uid_len != data[3] ||
            memcmp(tag.uid, after, tag.uid_len)) {
            return STATUS_CMD_ERR;
        }
    }
    status = maintenance_open(request, &tag, capacity_check);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t observed[16];
    status = pcd_14a_reader_mf1_read(unit, observed);
    if (status != STATUS_HF_TAG_OK || memcmp(observed, after, 16)) {
        return STATUS_CMD_ERR;
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t maintenance(uint16_t size, const uint8_t *data,
                             uint8_t *output, uint16_t *length) {
    if (size < 19) return STATUS_PAR_ERR;
    uint16_t base = data[0] == 1 || data[0] == 2 ? 51 : 19;
    bool capacity_check = size == base + 1 && data[base] == 1;
    if (data[0] > 3 || (size != base && !capacity_check) ||
        (capacity_check && (data[1] != 3 || data[2] != 6)) ||
        (data[0] == 3 && data[11] != 0) ||
        data[1] < 1 || data[1] > 3 ||
        (data[3] != 4 && data[3] != 7) ||
        (data[1] != 3 && (data[3] != 4 || data[12] != 0)) ||
        (data[1] == 3 && data[12] != 1 && data[12] != 2)) {
        return STATUS_PAR_ERR;
    }
    uint16_t blocks;
    switch (data[2]) {
    case 5:
        blocks = 20;
        break;
    case 1:
        blocks = 64;
        break;
    case 6:
        blocks = 128;
        break;
    case 7:
        blocks = 256;
        break;
    default:
        return STATUS_PAR_ERR;
    }
    if (data[11] >= blocks) {
        return STATUS_PAR_ERR;
    }
    const hf14a_config_t saved = *get_hf14a_config();
    const hf14a_config_t recovery = {2, data[3] == 7 ? 1 : 2, 2, 2};
    set_hf14a_config(&recovery);
    uint16_t status = maintenance_run(data, output, length, capacity_check);
    set_hf14a_config(&saved);
    return status;
}

uint16_t nfca_command(uint16_t command, uint16_t length, uint8_t *data,
                     uint8_t *output, uint16_t *output_length) {
    *output_length = 0;
    switch (command) {
    case DATA_CMD_MF1_UID_SPECIAL:
        return mf1_uid_special(length, data, output, output_length);
    case DATA_CMD_MF1_INSPECT:
        return mf1_classic_inspect(length, data, output, output_length);
    case DATA_CMD_MF1_MAINTENANCE:
        return maintenance(length, data, output, output_length);
    case DATA_CMD_MF1_BLOCK0_SET:
        return block0_set(length, data, output, output_length);
    case DATA_CMD_MF1_TRAILER_SET:
        return trailer_set(length, data, output, output_length);
    case DATA_CMD_TYPE2_AUTH_READ:
        return type2_auth_read(length, data, output, output_length);
    case DATA_CMD_MF1_DARKSIDE_STEP:
        return mf1_darkside_step(length, data, output, output_length);
    case DATA_CMD_TYPE2_READ: {
        uint16_t status = type2_read_command(length, data, output, output_length);
        if (status == STATUS_HF_TAG_NO || status >= STATUS_PAR_ERR) nfca_session_close();
        return status;
    }
    case DATA_CMD_HF14A_SCAN: {
        picc_14a_tag_t tag;
        // Opt-in discovery polling; empty requests keep the full retry policy.
        uint16_t status = length == 1 && data[0] == 1
            ? pcd_14a_reader_scan_poll(&tag) : pcd_14a_reader_scan_auto(&tag);
        if (status != STATUS_HF_TAG_OK) return status;
        uint16_t offset = 0;
        output[offset++] = tag.uid_len;
        memcpy(output + offset, tag.uid, tag.uid_len); offset += tag.uid_len;
        memcpy(output + offset, tag.atqa, 2); offset += 2;
        output[offset++] = tag.sak;
        output[offset++] = tag.ats_len;
        memcpy(output + offset, tag.ats, tag.ats_len); offset += tag.ats_len;
        *output_length = offset;
        return status;
    }
    case DATA_CMD_MF1_DETECT_PRNG: {
        uint8_t type;
        uint16_t status = check_prng_type((mf1_prng_type_t *)&type);
        if (status != STATUS_HF_TAG_OK) return status;
        output[0] = type; *output_length = 1;
        return status;
    }
    case DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK: {
        if (length < 9 || data[2] * 6 + 3 != length) return STATUS_PAR_ERR;
        mf1_toolbox_check_keys_on_block_in_t in = {
            .block = data[0], .key_type = data[1], .keys_len = data[2],
            .keys = (mf1_key_t *)(data + 3)
        };
        mf1_toolbox_check_keys_on_block_out_t out;
        uint16_t status = mf1_toolbox_check_keys_on_block(&in, &out);
        memcpy(output, &out, sizeof(out)); *output_length = sizeof(out);
        return status;
    }
    case DATA_CMD_MF1_READ_ONE_BLOCK: {
        if (length != 8) return STATUS_PAR_ERR;
        uint16_t status = auth_key_use_522_hw(data[1], data[0], data + 2);
        if (status != STATUS_HF_TAG_OK) return status;
        status = pcd_14a_reader_mf1_read(data[1], output);
        if (status == STATUS_HF_TAG_OK) *output_length = 16;
        return status;
    }
    case DATA_CMD_NFCA_WRITE_USER_UNIT:
        return write_user_unit(length, data, output, output_length);
    case DATA_CMD_MF1_MAGIC_PROBE:
        return mf1_magic_probe(length, data, output, output_length);
    case DATA_CMD_MF1_UID_SET:
        return uid_set(length, data, output, output_length);
    case DATA_CMD_MF1_COLLECT_OBSERVATIONS:
        return mf1_collect_command(length, data, output, output_length);
    case DATA_CMD_MF1_READ_SECTOR:
        return read_sector(length, data, output, output_length);
    default: return STATUS_INVALID_CMD;
    }
}
