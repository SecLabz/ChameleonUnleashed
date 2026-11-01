#include "hf_reader.h"
#include "hf_radio.h"
#include "app_status.h"
#include "data_cmd.h"
#include <string.h>

enum {
    NFCB_REQB = 0x05,
    NFCB_WUPB = 0x08,
    NFCB_ATQB = 0x50,
    NFCB_PARAM_OFFSET = 2,
};

uint16_t nfcb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)data;
    if (command != DATA_CMD_HF14B_SCAN) return STATUS_INVALID_CMD;
    uint8_t reply[HF_READER_FRAME_BYTES];
    size_t received;
    uint8_t request[] = {NFCB_REQB, 0, 0};
    uint16_t status = hf_radio_exchange(request, sizeof(request), NFCB_ATQB_BYTES,
                                                reply, sizeof(reply), &received);
    if (status == STATUS_HF_TAG_NO) {
        request[NFCB_PARAM_OFFSET] = NFCB_WUPB;
        status = hf_radio_exchange(request, sizeof(request), NFCB_ATQB_BYTES,
                                            reply, sizeof(reply), &received);
    }
    if (status) return status;
    if (reply[0] != NFCB_ATQB) return STATUS_HF_ERR_STAT;
    memcpy(output, reply, NFCB_ATQB_BYTES);
    *size = NFCB_ATQB_BYTES;
    return STATUS_HF_TAG_OK;
}
