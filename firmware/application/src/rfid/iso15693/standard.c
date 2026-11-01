#include "standard.h"

#include <string.h>

#include "frame.h"

void nfcv_std_reset_session(nfcv_std_t *standard) {
    standard->state = NFCV_STANDARD_READY;
}

void nfcv_std_quiet(nfcv_std_t *standard) {
    standard->state = NFCV_STANDARD_QUIET;
}

void nfcv_std_init(nfcv_std_t *standard, nfcv_mem_t *memory, const uint8_t uid[ISO15693_UID_SIZE],
                   uint8_t dsfid, uint8_t afi, uint8_t ic_ref, bool dsfid_locked, bool afi_locked) {
    standard->memory = memory;
    memcpy(standard->uid, uid, sizeof(standard->uid));
    standard->dsfid = dsfid;
    standard->afi = afi;
    standard->ic_ref = ic_ref;
    standard->dsfid_locked = dsfid_locked;
    standard->afi_locked = afi_locked;
    nfcv_std_reset_session(standard);
}

bool nfcv_std_prepare(const nfcv_std_t *standard, const iso15693_frame_t *frame,
                      nfcv_request_t *request) {
    if (frame->byte_count < (uint16_t)(2u + ISO15693_CRC_SIZE) ||
            frame->byte_count > sizeof(frame->bytes) || !frame->crc_ok) {
        return false;
    }

    const uint16_t payload_len = (uint16_t)(frame->byte_count - ISO15693_CRC_SIZE);
    const uint8_t *bytes = frame->bytes;
    uint16_t index = 2u;

    const uint8_t flags = bytes[0];
    request->command = bytes[1];
    request->inventory = (flags & ISO15693_REQ_INVENTORY) != 0u;
    request->addressed = false;
    request->selected = false;
    request->option = (flags & ISO15693_REQ_OPTION) != 0u;
    request->params = NULL;
    request->param_len = 0u;

    if (request->inventory) {
        if ((flags & ISO15693_REQ_AFI) != 0u) {
            if (index >= payload_len || bytes[index] != standard->afi) {
                return false;
            }
            index++;
        }
        request->params = &bytes[index];
        request->param_len = (uint8_t)(payload_len - index);
        return true;
    }

    if (request->command >= ISO15693_CMD_CUSTOM_MIN &&
            request->command <= ISO15693_CMD_CUSTOM_MAX) {
        if (index >= payload_len || bytes[index] != standard->uid[1]) {
            return false;
        }
        index++;
    }

    request->addressed = (flags & ISO15693_REQ_ADDRESS) != 0u;
    request->selected = (flags & ISO15693_REQ_SELECT) != 0u;
    if (request->addressed) {
        if (payload_len < index + ISO15693_UID_SIZE ||
                !iso15693_wire_uid_matches(&bytes[index], standard->uid)) {
            return false;
        }
        index = (uint16_t)(index + ISO15693_UID_SIZE);
    } else if (request->selected && standard->state != NFCV_STANDARD_SELECTED) {
        return false;
    }

    request->params = &bytes[index];
    request->param_len = (uint8_t)(payload_len - index);
    return true;
}

static bool inventory_matches(const nfcv_std_t *standard, const nfcv_request_t *request) {
    if (request->param_len < 1u) {
        return false;
    }
    uint8_t wire_uid[ISO15693_UID_SIZE];
    iso15693_uid_to_wire(wire_uid, standard->uid);
    return iso15693_inventory_mask_matches(wire_uid, request->params[0], &request->params[1],
                                           (uint8_t)(request->param_len - 1u));
}

static void reply_inventory(const nfcv_std_t *standard, nfcv_reply_t *response) {
    response->bytes[0] = 0x00u;
    response->bytes[1] = standard->dsfid;
    iso15693_uid_to_wire(&response->bytes[2], standard->uid);
    response->len = 10u;
}

static void reply_system_info(const nfcv_std_t *standard, nfcv_reply_t *response) {
    response->bytes[0] = 0x00u;
    response->bytes[1] = 0x0fu;
    iso15693_uid_to_wire(&response->bytes[2], standard->uid);
    response->bytes[10] = standard->dsfid;
    response->bytes[11] = standard->afi;
    response->bytes[12] = (uint8_t)(standard->memory->block_count - 1u);
    response->bytes[13] = (uint8_t)(NFCV_BLOCK_SIZE - 1u);
    response->bytes[14] = standard->ic_ref;
    response->len = 15u;
}

nfcv_std_result_t nfcv_std_handle(nfcv_std_t *standard, const nfcv_request_t *request,
                                  nfcv_reply_t *response) {
    if (standard->state == NFCV_STANDARD_QUIET) {
        if (request->command == ISO15693_CMD_RESET_TO_READY && request->addressed) {
            nfcv_std_reset_session(standard);
            nfcv_reply_ok(response);
            return NFCV_STANDARD_RESPONSE;
        }
        return NFCV_STANDARD_NO_RESPONSE;
    }

    switch (request->command) {
        case ISO15693_CMD_INVENTORY:
            if (request->inventory && inventory_matches(standard, request)) {
                reply_inventory(standard, response);
                return NFCV_STANDARD_RESPONSE;
            }
            return NFCV_STANDARD_NO_RESPONSE;
        case ISO15693_CMD_STAY_QUIET:
            if (request->addressed) {
                nfcv_std_quiet(standard);
            }
            return NFCV_STANDARD_NO_RESPONSE;
        case ISO15693_CMD_SELECT:
            if (request->addressed && !request->selected) {
                standard->state = NFCV_STANDARD_SELECTED;
                nfcv_reply_ok(response);
                return NFCV_STANDARD_RESPONSE;
            }
            return NFCV_STANDARD_NO_RESPONSE;
        case ISO15693_CMD_RESET_TO_READY:
            nfcv_std_reset_session(standard);
            nfcv_reply_ok(response);
            return NFCV_STANDARD_RESPONSE;
        case ISO15693_CMD_GET_SYSTEM_INFO:
            reply_system_info(standard, response);
            return NFCV_STANDARD_RESPONSE;
        case ISO15693_CMD_READ_SINGLE:
            if (request->param_len == 1u) {
                nfcv_mem_read(standard->memory, response, request->option, request->params[0], 1u);
                return NFCV_STANDARD_RESPONSE;
            }
            return NFCV_STANDARD_NO_RESPONSE;
        case ISO15693_CMD_READ_MULTIPLE:
            if (request->param_len == 2u) {
                nfcv_mem_read(standard->memory, response, request->option, request->params[0],
                              (uint8_t)(request->params[1] + 1u));
                return NFCV_STANDARD_RESPONSE;
            }
            return NFCV_STANDARD_NO_RESPONSE;
        case ISO15693_CMD_WRITE_SINGLE:
            nfcv_mem_write(standard->memory, response, request->params, request->param_len);
            break;
        case ISO15693_CMD_LOCK_BLOCK:
            nfcv_mem_lock(standard->memory, response, request->params, request->param_len);
            break;
        case ISO15693_CMD_GET_BLOCK_SECURITY:
            nfcv_mem_security(standard->memory, response, request->params, request->param_len);
            break;
        case ISO15693_CMD_WRITE_AFI:
            if (request->param_len == 1u && !standard->afi_locked) {
                standard->afi = request->params[0];
                nfcv_reply_ok(response);
            } else if (standard->afi_locked) {
                nfcv_reply_error(response, ISO15693_ERR_BLOCK_LOCKED);
            }
            break;
        case ISO15693_CMD_LOCK_AFI:
            standard->afi_locked = true;
            nfcv_reply_ok(response);
            break;
        case ISO15693_CMD_WRITE_DSFID:
            if (request->param_len == 1u && !standard->dsfid_locked) {
                standard->dsfid = request->params[0];
                nfcv_reply_ok(response);
            } else if (standard->dsfid_locked) {
                nfcv_reply_error(response, ISO15693_ERR_BLOCK_LOCKED);
            }
            break;
        case ISO15693_CMD_LOCK_DSFID:
            standard->dsfid_locked = true;
            nfcv_reply_ok(response);
            break;
        default:
            if (request->command >= ISO15693_CMD_CUSTOM_MIN &&
                    request->command <= ISO15693_CMD_CUSTOM_MAX) {
                return NFCV_STANDARD_UNHANDLED;
            }
            if (request->addressed) {
                nfcv_reply_error(response, ISO15693_ERR_CMD_NOT_SUPPORTED);
                return NFCV_STANDARD_RESPONSE;
            }
            return NFCV_STANDARD_NO_RESPONSE;
    }
    return response->len != 0u ? NFCV_STANDARD_RESPONSE : NFCV_STANDARD_NO_RESPONSE;
}
