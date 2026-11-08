#include "classic_batch.h"
#include "nfca_commands.h"
#include "nfca_session.h"
#include "data_cmd.h"
#include "app_status.h"
#include <string.h>

uint16_t classic_batch_request(const uint8_t *body, size_t size,
    uint8_t *output, size_t capacity, uint16_t *length) {
    *length = 0;
    if (!body || size < 8 || body[0] != 1 || body[1] > 1 ||
        (body[2] != 4 && body[2] != 7 && body[2] != 10)) return STATUS_PAR_ERR;
    unsigned head = 4 + body[2];
    if (size < head) return STATUS_PAR_ERR;
    unsigned count = body[head - 1];
    if (!count || count > CLASSIC_BATCH_MAX_ENTRIES || size != head + count * 8) return STATUS_PAR_ERR;
    size_t required = 3;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = body + head + 8 * i;
        if ((entry[1] != 0x60 && entry[1] != 0x61) || (body[1] && entry[0] >= 40)) return STATUS_PAR_ERR;
        required += 4 + (body[1] ? 2 + (entry[0] < 32 ? 4 : 16) * 16 : 7);
    }
    if (required > capacity) return STATUS_PAR_ERR;
    uint8_t result[NFCA_COMMAND_OUTPUT_BYTES];
    uint16_t result_size = 0, status;
    if (!body[1]) {
        /* Check UID on the live RF field. A scan still runs for every batch;
         * avoid resetting and repowering a prepared authentication lease. */
        if (nfca_session_reading()) nfca_session_close();
        if (!nfca_session_active()) nfca_session_open();
        status = nfca_prepare(DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK, 0, NULL);
        if (status == STATUS_SUCCESS) {
            status = nfca_command(DATA_CMD_HF14A_SCAN, 0, NULL, result, &result_size);
            nfca_finish(DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK);
        }
        if (status != STATUS_HF_TAG_OK) { nfca_session_close(); return status; }
        if (result_size < 1u + body[2] || result[0] != body[2] || memcmp(result + 1, body + 3, body[2])) {
            nfca_session_close();
            return STATUS_HF_TAG_NO;
        }
    }
    output[0] = 1; output[1] = body[1]; output[2] = count;
    unsigned offset = 3;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = body + head + 8 * i;
        uint8_t request[19] = {0};
        uint16_t command, request_size;
        if (body[1]) {
            command = DATA_CMD_MF1_READ_SECTOR;
            request[0] = entry[1]; request[1] = entry[0];
            memcpy(request + 2, entry + 2, 6);
            request[8] = body[2]; memcpy(request + 9, body + 3, body[2]);
            request_size = 9 + body[2];
        } else {
            command = DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK;
            request[0] = entry[0]; request[1] = entry[1]; request[2] = 1;
            memcpy(request + 3, entry + 2, 6);
            request_size = 9;
        }
        result_size = 0;
        status = nfca_prepare(command, request_size, request);
        if (status == STATUS_SUCCESS) {
            status = nfca_command(command, request_size, request, result, &result_size);
            nfca_finish(command);
        }
        if (result_size > sizeof(result) || offset + 4u + result_size > capacity) {
            nfca_session_close(); return STATUS_PAR_ERR;
        }
        output[offset++] = status >> 8; output[offset++] = status;
        output[offset++] = result_size >> 8; output[offset++] = result_size;
        memcpy(output + offset, result, result_size); offset += result_size;
        if (status != STATUS_HF_TAG_OK && status != STATUS_MF_ERR_AUTH) {
            /* Preserve completed records and stop touching a lost target. */
            output[2] = i + 1; break;
        }
    }
    *length = offset;
    return STATUS_HF_TAG_OK;
}
