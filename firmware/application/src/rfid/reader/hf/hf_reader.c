#include "hf_reader.h"
#include "st25tb_reader.h"
#include "hf_radio.h"
#include "st25tb_tearoff.h"
#include "app_status.h"
#include "data_cmd.h"
#include "nfca_session.h"
#include "rfid_main.h"
#include "ble_main.h"
#include "usb_main.h"
#include "utils/dataframe.h"

extern volatile bool m_is_field_on;

enum {
    NFCV_REQUEST_BLOCK_OFFSET = HF_READER_UID_BYTES,
    NFCV_REQUEST_COUNT_OFFSET = NFCV_REQUEST_BLOCK_OFFSET + 1,
    NFCV_DUMP_BLOCK_SIZE_OFFSET = NFCV_REQUEST_COUNT_OFFSET + 1,
    NFCV_READ_REQUEST_BYTES = NFCV_REQUEST_BLOCK_OFFSET + 1,
    NFCV_SECURITY_REQUEST_BYTES = NFCV_REQUEST_COUNT_OFFSET + 1,
    NFCV_DUMP_REQUEST_BYTES = NFCV_DUMP_BLOCK_SIZE_OFFSET + 1,
    NFCV_SECURITY_MAX_BLOCKS = 16,
    NFCV_ADDRESSABLE_BLOCKS = 256,
};

bool hf_reader_supports(uint16_t command) {
    switch (command) {
    case DATA_CMD_HF15_INVENTORY:
    case DATA_CMD_HF15_GET_SYSTEM_INFO:
    case DATA_CMD_HF15_READ_BLOCK:
    case DATA_CMD_HF14B_SCAN:
    case DATA_CMD_ST25TB_INFO:
    case DATA_CMD_ST25TB_READ_BLOCK:
    case DATA_CMD_ST25TB_WRITE_BLOCK:
    case DATA_CMD_ST25TB_DUMP:
    case DATA_CMD_ST25TB_TEAROFF:
    case DATA_CMD_HF15_DUMP:
    case DATA_CMD_SLIX_PRIVACY_SCAN:
    case DATA_CMD_HF15_WRITE_BLOCK:
    case DATA_CMD_HF15_RAW:
    case DATA_CMD_HF15_SECURITY: return true;
    default: return false;
    }
}

uint16_t hf_reader_command(uint16_t command, uint16_t length, const uint8_t *data,
        uint8_t *output, size_t capacity, uint16_t *size) {
    *size = 0;
    if (!hf_reader_supports(command)) return STATUS_INVALID_CMD;
    unsigned expected = 0;
    switch (command) {
    case DATA_CMD_SLIX_PRIVACY_SCAN:
        expected = 4;
        break;
    case DATA_CMD_HF15_WRITE_BLOCK:
        expected = 17;
        break;
    case DATA_CMD_HF15_RAW:
        expected = length;
        break;
    case DATA_CMD_HF15_GET_SYSTEM_INFO: expected = HF_READER_UID_BYTES; break;
    case DATA_CMD_ST25TB_DUMP: expected = ST25TB_UID_BYTES; break;
    case DATA_CMD_ST25TB_TEAROFF: expected = ST25TB_TEAROFF_REQUEST_BYTES; break;
    case DATA_CMD_ST25TB_WRITE_BLOCK: expected = ST25TB_WRITE_REQUEST_BYTES; break;
    case DATA_CMD_HF15_READ_BLOCK: expected = NFCV_READ_REQUEST_BYTES; break;
    case DATA_CMD_ST25TB_READ_BLOCK: expected = ST25TB_READ_REQUEST_BYTES; break;
    case DATA_CMD_HF15_SECURITY: expected = NFCV_SECURITY_REQUEST_BYTES; break;
    case DATA_CMD_HF15_DUMP: expected = NFCV_DUMP_REQUEST_BYTES; break;
    default: break;
    }
    bool authenticated = (command == DATA_CMD_HF15_GET_SYSTEM_INFO ||
        command == DATA_CMD_HF15_READ_BLOCK || command == DATA_CMD_HF15_DUMP ||
        command == DATA_CMD_HF15_SECURITY || command == DATA_CMD_HF15_WRITE_BLOCK) && length == expected + 4;
    if ((length != expected && !authenticated) || (length && !data) || !output ||
            capacity < HF_READER_OUTPUT_BYTES) {
        return STATUS_PAR_ERR;
    }
    if (command == DATA_CMD_HF15_RAW && (length < 3 || length > 277)) {
        return STATUS_PAR_ERR;
    }
    if (command == DATA_CMD_HF15_DUMP &&
            (data[NFCV_REQUEST_BLOCK_OFFSET] || !data[NFCV_REQUEST_COUNT_OFFSET] ||
             data[NFCV_REQUEST_COUNT_OFFSET] > HF_READER_MAX_BLOCKS ||
             data[NFCV_DUMP_BLOCK_SIZE_OFFSET] != HF_READER_BLOCK_BYTES))
        return STATUS_PAR_ERR;
    if (command == DATA_CMD_HF15_SECURITY &&
            (!data[NFCV_REQUEST_COUNT_OFFSET] || data[NFCV_REQUEST_COUNT_OFFSET] > NFCV_SECURITY_MAX_BLOCKS ||
             (unsigned)data[NFCV_REQUEST_BLOCK_OFFSET] + data[NFCV_REQUEST_COUNT_OFFSET] > NFCV_ADDRESSABLE_BLOCKS))
        return STATUS_PAR_ERR;
    if (command == DATA_CMD_ST25TB_READ_BLOCK &&
            data[ST25TB_REQUEST_BLOCK_OFFSET] >= ST25TB_BLOCK_COUNT &&
            data[ST25TB_REQUEST_BLOCK_OFFSET] != ST25TB_SYSTEM_BLOCK)
        return STATUS_PAR_ERR;
    if (command == DATA_CMD_ST25TB_WRITE_BLOCK &&
            data[ST25TB_REQUEST_BLOCK_OFFSET] >= ST25TB_BLOCK_COUNT &&
            data[ST25TB_REQUEST_BLOCK_OFFSET] != ST25TB_SYSTEM_BLOCK)
        return STATUS_PAR_ERR;
    if (command == DATA_CMD_ST25TB_TEAROFF)
        return st25tb_tearoff_command(data, output, size);
    if (get_device_mode() != DEVICE_MODE_READER) return STATUS_DEVICE_MODE_ERROR;
    if (m_is_field_on || nfca_session_active()) return STATUS_BUSY;
    bool vicinity = command == DATA_CMD_HF15_INVENTORY ||
        command == DATA_CMD_HF15_GET_SYSTEM_INFO || command == DATA_CMD_HF15_READ_BLOCK ||
        command == DATA_CMD_HF15_DUMP || command == DATA_CMD_HF15_SECURITY ||
        command == DATA_CMD_SLIX_PRIVACY_SCAN || command == DATA_CMD_HF15_WRITE_BLOCK || command == DATA_CMD_HF15_RAW;
    hf_radio_depth(false);
    uint16_t status = STATUS_HF_ERR_STAT;
    bool st25tb_read = command == DATA_CMD_ST25TB_INFO ||
        command == DATA_CMD_ST25TB_READ_BLOCK || command == DATA_CMD_ST25TB_DUMP;
    bool pause_advertising = st25tb_read && usb_hfclk_active();
    bool resume_advertising = pause_advertising && advertising_stop_checked();
    bool opened = hf_radio_open(vicinity);
    bool raw_complete = false;
    if (opened) {
        if (vicinity) {
            status = STATUS_HF_TAG_OK;
            if (authenticated || command == DATA_CMD_SLIX_PRIVACY_SCAN) {
                status = nfcv_unlock_privacy(command == DATA_CMD_SLIX_PRIVACY_SCAN ? data : data + expected);
            }
            if (status == STATUS_HF_TAG_OK) {
                if (command == DATA_CMD_HF15_WRITE_BLOCK) {
                    status = nfcv_write_block(data, output, size);
                } else if (command == DATA_CMD_HF15_RAW) {
                    status = nfcv_raw(length, data, output, size, &raw_complete);
                } else {
                    uint8_t inventory[1] = {0};
                    status = nfcv_reader_command(command == DATA_CMD_SLIX_PRIVACY_SCAN ? DATA_CMD_HF15_INVENTORY : command,
                        command == DATA_CMD_SLIX_PRIVACY_SCAN ? inventory : data, output, size);
                }
            }
        }
        else if (command == DATA_CMD_HF14B_SCAN) {
            status = nfcb_reader_command(command, data, output, size);
        } else {
            status = st25tb_reader_command(command, data, output, size);
        }
    }
    if (command == DATA_CMD_HF15_RAW && raw_complete &&
            status == STATUS_HF_TAG_OK && (data[0] & NFCV_RAW_KEEP_FIELD)) {
        hf_radio_keep();
    } else if (!hf_radio_close() && status == STATUS_HF_TAG_OK) {
        status = STATUS_HF_ERR_STAT;
    }
    if (resume_advertising) advertising_start(false);
    return status;
}
