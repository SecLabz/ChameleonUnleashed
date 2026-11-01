#include "lf_commands.h"
#include "lf_reader_data.h"
#include "lf_tag_em.h"
#include "lf_t55xx.h"
#include "data_cmd.h"
#include "app_status.h"
#include <string.h>

#define LF_SCAN_TIMEOUT_MS 500

uint16_t web_lf_command(uint16_t command, uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length) {
    *length = 0;
    if (get_device_mode() != DEVICE_MODE_READER) {
        return STATUS_DEVICE_MODE_ERROR;
    }
    if (command == DATA_CMD_LF_T55XX) {
        return lf_t55xx_command(data, size, output, length);
    }
    bool hinted = command == DATA_CMD_HIDPROX_SCAN || command == DATA_CMD_IOPROX_SCAN;
    if (size != (hinted ? 1u : 0u)) {
        return STATUS_PAR_ERR;
    }
    memset(output, 0, 16);
    bool found;
    unsigned bytes;
    // HTTP owns this fixed bound; native clients retain their configured timeout.
    switch (command) {
    case DATA_CMD_EM410X_SCAN:
        found = em410x_read(output, 150);
        bytes = 2 + (output[1] == TAG_TYPE_EM410X_ELECTRA ? LF_EM410X_ELECTRA_TAG_ID_SIZE : LF_EM410X_TAG_ID_SIZE);
        break;
    case DATA_CMD_HIDPROX_SCAN:
        found = hidprox_read(output, data[0], LF_SCAN_TIMEOUT_MS);
        bytes = 16;
        break;
    case DATA_CMD_IOPROX_SCAN:
        found = ioprox_read(output, data[0], LF_SCAN_TIMEOUT_MS);
        bytes = 16;
        break;
    case DATA_CMD_PAC_SCAN:
        found = pac_read(output, LF_SCAN_TIMEOUT_MS);
        bytes = LF_PAC_TAG_ID_SIZE;
        break;
    case DATA_CMD_VIKING_SCAN:
        found = viking_read(output, LF_SCAN_TIMEOUT_MS);
        bytes = LF_VIKING_TAG_ID_SIZE;
        break;
    case DATA_CMD_JABLOTRON_SCAN:
        found = jablotron_read(output, LF_SCAN_TIMEOUT_MS);
        bytes = LF_JABLOTRON_TAG_ID_SIZE;
        break;
    default:
        return STATUS_PAR_ERR;
    }
    if (!found) {
        return STATUS_LF_TAG_NO_FOUND;
    }
    *length = bytes;
    return STATUS_LF_TAG_OK;
}
