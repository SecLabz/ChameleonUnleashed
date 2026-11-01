#include "desfire_reader.h"
#include "desfire_protocol.h"
#include "nfca_session.h"
#include "hf_radio.h"
#include "rc522.h"
#include "rfid_main.h"
#include "app_status.h"
#include <string.h>

enum {
    READER_FRAME_BYTES = 64,
    READER_RESPONSE_BYTES = 512,
    READER_COMMAND_MAX_BYTES = 40,
    READER_FRAME_TIMEOUT_MS = 200,
    READER_FIELD_SETTLE_MS = 8,
    ISO_DEP_CHAIN_MAX_FRAMES = 32,
    NATIVE_CHAIN_MAX_FRAMES = 32,
    READER_END = 0,
    READER_BEGIN = 1,
    READER_EXCHANGE = 2,
};

static bool active;
static uint8_t block_number;

void desfire_reader_invalidate(void) {
    active = false;
    block_number = 0;
}

/* The RF field and selected tag belong to the current capture lease. Each
 * exchange waits for a response, appends/checks CRC, and keeps that field on. */
static uint8_t exchange_frame(uint8_t *tx, unsigned sent, uint8_t *rx, uint16_t *count, unsigned capacity) {
    return pcd_14a_reader_raw_cmd(false, /* Do not open a new field. */
                                  true,  /* Wait for the response. */
                                  true,  /* Append the request CRC. */
                                  false, /* Keep the existing selection. */
                                  true,  /* Keep the field on. */
                                  true,  /* Check the response CRC. */
                                  READER_FRAME_TIMEOUT_MS, sent * 8, tx, rx, count, capacity * 8);
}

/* Reassemble ISO-DEP I-blocks; the caller separately aggregates native AF
 * responses. Block-number changes belong to this lower transport layer. */
static uint16_t exchange(uint8_t command, const uint8_t *params, unsigned size, uint8_t *output, uint16_t *length) {
    uint8_t tx[READER_FRAME_BYTES], rx[READER_FRAME_BYTES];
    tx[0] = ISO_DEP_I_BLOCK | block_number;
    tx[1] = command;
    if (size)
        memcpy(tx + 2, params, size);
    unsigned sent = size + 2, used = 0;
    /* Both ISO-DEP chaining and WTX are bounded; malformed chains never return
     * successful truncated data. FSDI=4 in RATS keeps frames in the RC522 FIFO. */
    for (unsigned frame = 0; frame < ISO_DEP_CHAIN_MAX_FRAMES; frame++) {
        uint16_t count = 0;
        uint16_t status = exchange_frame(tx, sent, rx, &count, sizeof(rx));
        if (status != STATUS_HF_TAG_OK)
            return status;
        if (count == 2 && rx[0] == ISO_DEP_S_WTX && rx[1] >= ISO_DEP_WTX_MULTIPLIER_MIN &&
            rx[1] <= ISO_DEP_WTX_MULTIPLIER_MAX) {
            memcpy(tx, rx, count);
            sent = count;
            continue;
        }
        if (count < 2 || (rx[0] & ISO_DEP_READER_I_BLOCK_MASK) != ISO_DEP_I_BLOCK ||
            (rx[0] & ISO_DEP_BLOCK_NUMBER) != block_number || used + count - 1 > READER_RESPONSE_BYTES)
            return STATUS_HF_ERR_STAT;
        memcpy(output + used, rx + 1, count - 1);
        used += count - 1;
        block_number ^= 1;
        if (!(rx[0] & ISO_DEP_CHAINING)) {
            *length = used;
            return STATUS_HF_TAG_OK;
        }
        tx[0] = ISO_DEP_R_ACK | block_number;
        sent = 1;
    }
    return STATUS_HF_ERR_STAT;
}

uint16_t desfire_reader_command(uint16_t size, const uint8_t *data, uint8_t *output, uint16_t *length) {
    *length = 0;
    if (!size)
        return STATUS_PAR_ERR;
    if (data[0] == READER_END && size == 1) {
        nfca_session_close();
        return STATUS_SUCCESS;
    }
    if (get_device_mode() != DEVICE_MODE_READER)
        return STATUS_DEVICE_MODE_ERROR;
    nfca_session_poll();
    uint16_t status = STATUS_PAR_ERR;
    if (data[0] == READER_BEGIN &&
        (size == 1 || (size >= 2 && (data[1] == 4 || data[1] == 7 || data[1] == 10) && size == 2u + data[1]))) {
        if (!hf_radio_release()) {
            return STATUS_HF_ERR_STAT;
        }
        nfca_session_open();
        pcd_14a_reader_reset();
        pcd_14a_reader_antenna_on();
        bsp_delay_ms(READER_FIELD_SETTLE_MS);
        picc_14a_tag_t tag;
        status = pcd_14a_reader_scan_auto(&tag);
        if (status == STATUS_HF_TAG_OK &&
            (!(tag.sak & DESFIRE_SAK_ISO_DEP) || tag.ats_len < DESFIRE_ATS_MIN_BYTES ||
             tag.ats_len > DESFIRE_ATS_MAX_BYTES ||
             (size > 1 && (tag.uid_len != data[1] || memcmp(tag.uid, data + 2, tag.uid_len)))))
            status = STATUS_HF_ERR_ATS;
        if (status == STATUS_HF_TAG_OK) {
            output[0] = tag.uid_len;
            memcpy(output + 1, tag.uid, tag.uid_len);
            unsigned pos = 1 + tag.uid_len;
            memcpy(output + pos, tag.atqa, 2);
            pos += 2;
            output[pos++] = tag.sak;
            output[pos++] = tag.ats_len;
            memcpy(output + pos, tag.ats, tag.ats_len);
            *length = pos + tag.ats_len;
            active = true;
            block_number = 0;
        }
    } else if (data[0] == READER_EXCHANGE && size >= 2 && size <= READER_COMMAND_MAX_BYTES && active &&
               nfca_session_ready()) {
        /* This endpoint intentionally exposes only public discovery/reads. */
        switch (data[1]) {
        case DESFIRE_CMD_GET_VERSION:
        case DESFIRE_CMD_FREE_MEMORY:
        case DESFIRE_CMD_GET_KEY_SETTINGS:
        case DESFIRE_CMD_GET_KEY_VERSION:
        case DESFIRE_CMD_GET_APPLICATION_IDS:
        case DESFIRE_CMD_SELECT_APPLICATION:
        case DESFIRE_CMD_GET_FILE_IDS:
        case DESFIRE_CMD_GET_FILE_SETTINGS:
        case DESFIRE_CMD_READ_DATA:
        case DESFIRE_CMD_READ_RECORDS:
        case DESFIRE_CMD_GET_VALUE:
            break;
        default:
            nfca_session_close();
            return STATUS_PAR_ERR;
        }
        uint8_t part[READER_RESPONSE_BYTES];
        uint16_t count = 0;
        status = exchange(data[1], data + 2, size - 2, part, &count);
        unsigned used = 1;
        for (unsigned frame = 0; status == STATUS_HF_TAG_OK && frame < NATIVE_CHAIN_MAX_FRAMES; frame++) {
            if (!count || used + count - 1 > READER_RESPONSE_BYTES) {
                status = STATUS_HF_ERR_STAT;
                break;
            }
            output[0] = part[0];
            memcpy(output + used, part + 1, count - 1);
            used += count - 1;
            if (part[0] != DESFIRE_STATUS_ADDITIONAL_FRAME) {
                *length = used;
                break;
            }
            if (frame == NATIVE_CHAIN_MAX_FRAMES - 1) {
                status = STATUS_HF_ERR_STAT;
                break;
            }
            status = exchange(DESFIRE_CMD_ADDITIONAL_FRAME, NULL, 0, part, &count);
        }
    }
    if (status == STATUS_HF_TAG_OK)
        nfca_session_finish();
    else {
        *length = 0;
        nfca_session_close();
    }
    return status;
}
