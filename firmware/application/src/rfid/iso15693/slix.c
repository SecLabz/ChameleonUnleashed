#include "slix.h"

#include <string.h>



static uint32_t read_password(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) |
           bytes[3];
}

void slix_reset_session(slix_t *slix) {
    memset(slix->password_match, 0, sizeof(slix->password_match));
}

void slix_init(slix_t *slix, nfcv_std_t *standard, bool eas_locked, bool privacy_mode,
               const uint32_t passwords[SLIX_PASSWORD_TYPE_COUNT], bool slix_l) {
    slix->standard = standard;
    slix->slix_l = slix_l;
    slix->eas_active = true;
    slix->eas_locked = eas_locked;
    slix->privacy_mode = privacy_mode;
    slix->pseudo_random = 0x1569u;
    memcpy(slix->passwords, passwords, sizeof(slix->passwords));
    slix_reset_session(slix);
}

bool slix_allows_command(const slix_t *slix, uint8_t command) {
    return !slix->privacy_mode || command == ISO15693_CMD_NXP_GET_RANDOM_NUMBER ||
           command == ISO15693_CMD_NXP_SET_PASSWORD;
}

static bool password_type(uint8_t id, bool slix_l, slix_password_type_t *type) {
    switch (id) {
        case SLIX_PASSWORD_PRIVACY:
            if (!slix_l) return false;
            *type = SLIX_PASSWORD_TYPE_PRIVACY;
            return true;
        case SLIX_PASSWORD_DESTROY:
            if (!slix_l) return false;
            *type = SLIX_PASSWORD_TYPE_DESTROY;
            return true;
        case SLIX_PASSWORD_EAS_AFI:
            *type = SLIX_PASSWORD_TYPE_EAS_AFI;
            return true;
        default:
            return false;
    }
}

static uint32_t unmask_password(const slix_t *slix, uint32_t masked_password) {
    const uint16_t r = __builtin_bswap16(slix->pseudo_random);
    const uint32_t mask = ((uint32_t)r << 16u) | r;
    return masked_password ^ mask;
}

static bool set_password_from_request(slix_t *slix, const nfcv_request_t *request) {
    if (request->param_len != 5u) {
        return false;
    }
    slix_password_type_t type;
    if (!password_type(request->params[0], slix->slix_l, &type)) {
        return false;
    }
    const uint32_t password = unmask_password(slix, read_password(&request->params[1]));
    if (password != slix->passwords[type]) {
        return false;
    }
    slix->password_match[type] = true;
    if (type == SLIX_PASSWORD_TYPE_PRIVACY) {
        slix->privacy_mode = false;
    }
    return true;
}

static bool enable_privacy_from_request(slix_t *slix, const nfcv_request_t *request) {
    if (request->param_len != 4u) {
        return false;
    }
    const uint32_t password = unmask_password(slix, read_password(request->params));
    if (password != slix->passwords[SLIX_PASSWORD_TYPE_PRIVACY]) {
        return false;
    }
    slix->password_match[SLIX_PASSWORD_TYPE_PRIVACY] = true;
    slix->privacy_mode = true;
    return true;
}

static bool write_password_from_request(slix_t *slix, const nfcv_request_t *request) {
    if (request->param_len != 5u) {
        return false;
    }
    slix_password_type_t type;
    if (!password_type(request->params[0], slix->slix_l, &type) || !slix->password_match[type]) {
        return false;
    }
    slix->passwords[type] = read_password(&request->params[1]);
    slix->password_match[type] = false;
    return true;
}

static void reply_eas_alarm(const slix_t *slix, nfcv_reply_t *response) {
    response->bytes[0] = 0x00u;
    for (uint8_t index = 0u; index < 32u; index++) {
        response->bytes[1u + index] = slix->eas_active ? (uint8_t)(0xa5u ^ index) : 0x00u;
    }
    response->len = 33u;
}

void slix_handle(slix_t *slix, const nfcv_request_t *request, nfcv_reply_t *response) {
    if (!slix->slix_l &&
            (request->command == ISO15693_CMD_NXP_ENABLE_PRIVACY ||
             request->command == ISO15693_CMD_NXP_DESTROY)) {
        if (request->addressed) nfcv_reply_error(response, ISO15693_ERR_CMD_NOT_SUPPORTED);
        return;
    }
    switch (request->command) {
        case ISO15693_CMD_NXP_SET_EAS:
            if (!slix->eas_locked) {
                slix->eas_active = true;
            }
            nfcv_reply_ok(response);
            break;
        case ISO15693_CMD_NXP_RESET_EAS:
            if (!slix->eas_locked) {
                slix->eas_active = false;
            }
            nfcv_reply_ok(response);
            break;
        case ISO15693_CMD_NXP_LOCK_EAS:
            slix->eas_locked = true;
            nfcv_reply_ok(response);
            break;
        case ISO15693_CMD_NXP_EAS_ALARM:
            reply_eas_alarm(slix, response);
            break;
        case ISO15693_CMD_NXP_GET_RANDOM_NUMBER:
            slix->pseudo_random = (uint16_t)(slix->pseudo_random * 109u + 1021u);
            response->bytes[0] = 0x00u;
            response->bytes[1] = slix->pseudo_random;
            response->bytes[2] = slix->pseudo_random >> 8;
            response->len = 3u;
            break;
        case ISO15693_CMD_NXP_SET_PASSWORD:
            if (set_password_from_request(slix, request)) {
                nfcv_reply_ok(response);
            }
            break;
        case ISO15693_CMD_NXP_ENABLE_PRIVACY:
            if (enable_privacy_from_request(slix, request)) {
                nfcv_reply_ok(response);
            }
            break;
        case ISO15693_CMD_NXP_PASSWORD_PROTECT_EAS_AFI:
            if (slix->password_match[SLIX_PASSWORD_TYPE_EAS_AFI]) {
                nfcv_reply_ok(response);
            }
            break;
        case ISO15693_CMD_NXP_WRITE_PASSWORD:
            if (write_password_from_request(slix, request)) {
                nfcv_reply_ok(response);
            }
            break;
        case ISO15693_CMD_NXP_DESTROY:
            if (slix->password_match[SLIX_PASSWORD_TYPE_DESTROY]) {
                nfcv_std_quiet(slix->standard);
                nfcv_reply_silent(response);
            }
            break;
        default:
            if (request->addressed) {
                nfcv_reply_error(response, ISO15693_ERR_CMD_NOT_SUPPORTED);
            }
            break;
    }
}
