#include "lf_t55xx.h"

#include <stdbool.h>
#include <string.h>
#include "app_status.h"
#include "hex_utils.h"
#include "t55xx_radio.h"
#include "protocols/em410x.h"
#include "protocols/hidprox.h"
#include "protocols/idteck.h"
#include "protocols/ioprox.h"
#include "protocols/jablotron.h"
#include "protocols/pac.h"
#include "protocols/t55xx.h"
#include "protocols/t55xx_decode.h"
#include "protocols/viking.h"
#include "tag_base_type.h"

static bool snapshot(uint8_t *output,
    t55xx_demod_t *demod) {
    memset(output, 0, LF_T55XX_SNAPSHOT_BYTES);
    output[0] = LF_T55XX_VERSION;
    if (!t55xx_radio_detect(demod)) {
        return false;
    }
    uint32_t confirmation;
    if (!t55xx_radio_read(0, demod, &confirmation) ||
        confirmation != demod->configuration) {
        return false;
    }
    output[2] = 1;
    num_to_bytes(confirmation, 4, output + 4);
    bool distinct = false;
    /* Block 7 may contain a password. It is never exported or changed here. */
    for (uint8_t block = 1; block < 7; block++) {
        uint32_t word;
        if (t55xx_radio_read(block, demod, &word)) {
            output[2] |= 1u << block;
            num_to_bytes(word, 4, output + 4 + block * 4);
            distinct |= word != demod->configuration;
        }
    }
    /* A read-only broadcast tag ignores direct-access commands. Its unchanged
     * stream must not be mistaken for successful reads of several blocks. */
    if (!distinct || (output[2] & 7) != 7) {
        memset(output + 1, 0, LF_T55XX_SNAPSHOT_BYTES - 1);
        return false;
    }
    output[1] = 1;
    return true;
}

static uint32_t fingerprint(const uint8_t *snapshot_data, unsigned count) {
    uint32_t hash = 2166136261u;
    /* Unused words may be unreadable (for example a constant NRZ reply).
     * Bind only words the plan needs, so optional read failures cannot expire it. */
    for (unsigned i = 0; i < 4 + count * 4; i++) {
        uint8_t value = i == 3 ? 0 : snapshot_data[i];
        if (i == 2) {
            value &= (1u << count) - 1;
        }
        hash = (hash ^ value) * 16777619u;
    }
    return hash;
}

static uint8_t encode_image(uint16_t type, uint8_t clock, const uint8_t *data,
    unsigned length, uint32_t *blocks) {
    switch (type) {
        case TAG_TYPE_EM410X:
            if (length != 5 || (clock != 0 && clock != 16 && clock != 32 && clock != 64)) {
                return 0;
            }
            em410x_t55xx_writer((uint8_t *)data, blocks);
            blocks[0] &= ~(7u << 18);
            if (clock == 16) {
                blocks[0] |= T5577_BITRATE_RF_16;
            } else if (clock == 32) {
                blocks[0] |= T5577_BITRATE_RF_32;
            } else {
                blocks[0] |= T5577_BITRATE_RF_64;
            }
            return 3;
        case TAG_TYPE_EM410X_ELECTRA:
            return length == 13 ? em410x_electra_t55xx_writer((uint8_t *)data, blocks) : 0;
        case TAG_TYPE_VIKING:
            return length == 4 ? viking_t55xx_writer((uint8_t *)data, blocks) : 0;
        case TAG_TYPE_PAC:
            if (length != 8) {
                return 0;
            }
            for (unsigned i = 0; i < length; i++) {
                if (data[i] < 33 || data[i] > 126) {
                    return 0;
                }
            }
            return pac_t55xx_writer((uint8_t *)data, blocks);
        case TAG_TYPE_JABLOTRON:
            return length == 5 ? jablotron_t55xx_writer((uint8_t *)data, blocks) : 0;
        case TAG_TYPE_IDTECK:
            return length == 8 && !memcmp(data, "IDTK", 4) ?
                idteck_t55xx_writer((uint8_t *)data, blocks) : 0;
        case TAG_TYPE_IOPROX: {
            uint8_t decoded[16];
            if (length != 16 || !ioprox_decode_raw_to_data(data + 4, decoded) ||
                memcmp(decoded, data, 12)) {
                return 0;
            }
            return ioprox_t55xx_writer((uint8_t *)data, blocks);
        }
        case TAG_TYPE_HID_PROX: {
            if (length != 13) {
                return 0;
            }
            wiegand_card_t card = {
                .format = data[0],
                .facility_code = bytes_to_num(data + 1, 4),
                .card_number = bytes_to_num(data + 5, 5),
                .issue_level = data[10],
                .oem = bytes_to_num(data + 11, 2)
            };
            return wiegand_card_valid(&card) ? hidprox_t55xx_writer(&card, blocks) : 0;
        }
        default:
            return 0;
    }
}

uint16_t lf_t55xx_command(const uint8_t *request, size_t size,
    uint8_t *output, uint16_t *length) {
    *length = 0;
    if (size < 7 || request[0] != LF_T55XX_VERSION || request[1] > 2 || request[2] != 0 || bytes_to_num(request + 3, 4) != 0) {
        return STATUS_PAR_ERR;
    }
    unsigned operation = request[1];
    uint32_t desired[8] = {0};
    uint8_t count = 0;
    unsigned data_length = 0;
    if (operation == 0) {
        if (size != 7) {
            return STATUS_PAR_ERR;
        }
    } else {
        if (size < 11) {
            return STATUS_PAR_ERR;
        }
        data_length = request[10];
        if (size != 11 + data_length + (operation == 2 ? 4u : 0u)) {
            return STATUS_PAR_ERR;
        }
        count = encode_image(bytes_to_num(request + 7, 2), request[9], request + 11, data_length, desired);
        if (count < 2 || count > 7) {
            return STATUS_PAR_ERR;
        }
    }
    t55xx_demod_t demod;
    bool detected = snapshot(output, &demod);
    *length = LF_T55XX_SNAPSHOT_BYTES;
    if (operation == 0) {
        return STATUS_LF_TAG_OK;
    }
    if (!detected) {
        return STATUS_LF_TAG_NO_FOUND;
    }
    /* Only the format fields change. Password and POR state are preserved;
     * OTP, extended and answer-on-request configurations require separate tools. */
    if (demod.configuration & (0x20000u | 0x200u | 0x100u)) {
        return STATUS_NOT_IMPLEMENTED;
    }
    if (demod.configuration & T5577_PWD) {
        return STATUS_LF_TAG_LOGIN_REQUIRED;
    }
    const uint32_t preserved = 0xf0000000u | T5577_PWD | T5577_POR_DELAY;
    desired[0] = (desired[0] & ~preserved) | (demod.configuration & preserved);
    unsigned required = (1u << count) - 1;
    if ((output[2] & required) != required) {
        return STATUS_CMD_ERR;
    }
    if (demod.modulation <= 1 || ((desired[0] >> 12) & 31) <= 1) {
        for (unsigned block = 1; block < count; block++) {
            if (desired[block] == 0 || desired[block] == UINT32_MAX) {
                /* Verify data both before and after the configuration changes.
                 * A constant NRZ/PSK1 envelope has no verifiable polarity. */
                return STATUS_NOT_IMPLEMENTED;
            }
        }
    }
    uint32_t target_fingerprint = fingerprint(output, count);
    bool unchanged = true;
    for (unsigned block = 0; block < count; block++) {
        unchanged &= bytes_to_num(output + 4 + block * 4, 4) == desired[block];
    }
    output[3] = unchanged ? 1 : 0;
    output[36] = count;
    for (unsigned block = 0; block < 7; block++) {
        num_to_bytes(desired[block], 4, output + 37 + block * 4);
    }
    num_to_bytes(target_fingerprint, 4, output + 65);
    *length = LF_T55XX_PLAN_BYTES;
    if (operation == 1) {
        return STATUS_LF_TAG_OK;
    }
    if (bytes_to_num(request + 11 + data_length, 4) != target_fingerprint) {
        return STATUS_BUSY;
    }
    if (unchanged) {
        return STATUS_LF_TAG_OK;
    }
    for (uint8_t block = 1; block < count; block++) {
        if (bytes_to_num(output + 4 + block * 4, 4) == desired[block]) {
            continue;
        }
        t55xx_radio_write(block, desired[block]);
        uint32_t actual;
        bool matches = false;
        for (unsigned attempt = 0; attempt < 2 && !matches; attempt++) {
            matches = t55xx_radio_read(block, &demod, &actual) && actual == desired[block];
        }
        if (!matches) {
            return STATUS_CMD_ERR;
        }
    }
    /* Commit the new modulation last, after checking every data block under
     * the old configuration. Never repeat a write after an uncertain result. */
    if (desired[0] != demod.configuration) {
        t55xx_radio_write(0, desired[0]);
    }
    uint8_t verified[LF_T55XX_SNAPSHOT_BYTES];
    bool complete = false;
    for (unsigned attempt = 0; attempt < 2 && !complete; attempt++) {
        complete = snapshot(verified, &demod);
        for (unsigned block = 0; block < count && complete; block++) {
            complete = (verified[2] & (1u << block)) &&
                bytes_to_num(verified + 4 + block * 4, 4) == desired[block];
        }
    }
    if (!complete) {
        return STATUS_CMD_ERR;
    }
    memcpy(output, verified, sizeof(verified));
    output[3] = 2;
    return STATUS_LF_TAG_OK;
}
