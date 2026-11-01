#include "nfc_desfire.h"
#include "desfire_protocol.h"
#include "nfc_14a.h"
#include "fds_util.h"
#include "tag_persistence.h"
#include "app_status.h"
#include "rfid_main.h"
#include <string.h>

enum {
    TAG_REQUEST_BYTES = 64,
    TAG_RETRANSMIT_BYTES = 64,
    TAG_RESPONSE_BYTES = 32,
    TAG_NATIVE_REQUEST_BYTES = 40,
    TAG_NATIVE_REPLY_BYTES = 26,
    TAG_RESPONSE_DATA_MAX = 58,
    IMAGE_TRANSFER_HEADER_BYTES = 4,
    IMAGE_READ_CHUNK_MAX = 500,
};

_Static_assert(TAG_NATIVE_REPLY_BYTES >= DESFIRE_NATIVE_DATA_MAX + 1, "Native response scratch is too small");
_Static_assert(TAG_RESPONSE_BYTES >= DESFIRE_NATIVE_DATA_MAX + 2, "Wrapped response scratch is too small");

static desfire_model_t model;
static nfc_tag_14a_coll_res_entity_t identity;
static nfc_tag_14a_coll_res_reference_t reference;
static uint8_t expected_block, request[TAG_REQUEST_BYTES], last_frame[TAG_RETRANSMIT_BYTES],
    response[TAG_RESPONSE_BYTES];
static unsigned request_len, last_frame_len, response_len, response_offset;
static uint8_t cid, cid_flag, reply_number;
/* Image upload is independent of the RF session: a protocol reset must not
 * discard a host's in-progress upload. Model pointers borrow the slot buffer. */
static unsigned upload_received, upload_size;

static nfc_tag_14a_coll_res_reference_t *coll_res(void) {
    return &reference;
}

static void reset(void) {
    expected_block = cid = cid_flag = request_len = last_frame_len = response_len = response_offset = 0;
    desfire_model_reset(&model);
}

static void send_response(void) {
    unsigned fsd = nfc_tag_14a_get_fsd();
    unsigned limit = fsd > 5u + cid_flag ? fsd - 3u - cid_flag : 8;
    if (limit > TAG_RESPONSE_DATA_MAX)
        limit = TAG_RESPONSE_DATA_MAX;
    unsigned left = response_len - response_offset, n = left > limit ? limit : left;
    last_frame[0] =
        ISO_DEP_I_BLOCK | reply_number | (cid_flag ? ISO_DEP_CID_PRESENT : 0) | (left > n ? ISO_DEP_CHAINING : 0);
    unsigned pos = 1;
    if (cid_flag)
        last_frame[pos++] = cid;
    memcpy(last_frame + pos, response + response_offset, n);
    last_frame_len = pos + n;
    response_offset += n;
    expected_block = reply_number ^ 1;
    nfc_tag_14a_tx_bytes(last_frame, last_frame_len, true);
}

/* ISO-DEP control/duplicate handling precedes request accumulation. Only a
 * complete request reaches APDU adaptation and the native command model. */
static void handle(uint8_t *data, uint16_t bits) {
    if (bits % 8 || bits < 24 || !nfc_tag_14a_checks_crc(data, bits / 8))
        return;
    unsigned size = bits / 8 - 2;
    uint8_t pcb = data[0];
    if ((pcb & ISO_DEP_DESELECT_MASK) == ISO_DEP_S_DESELECT) {
        nfc_tag_14a_tx_bytes(data, size, true);
        reset();
        return;
    }
    if ((pcb & ISO_DEP_BLOCK_TYPE_MASK) == ISO_DEP_R_ACK) {
        if ((pcb & ISO_DEP_R_NAK) || (pcb & ISO_DEP_BLOCK_NUMBER) != expected_block) {
            if (last_frame_len)
                nfc_tag_14a_tx_bytes(last_frame, last_frame_len, true);
        } else if (response_offset < response_len) {
            reply_number ^= 1;
            send_response();
        }
        return;
    }
    if ((pcb & ISO_DEP_BLOCK_TYPE_MASK) != ISO_DEP_I_BLOCK || (pcb & ISO_DEP_NAD_PRESENT))
        return;
    if ((pcb & ISO_DEP_BLOCK_NUMBER) != expected_block) {
        if (last_frame_len)
            nfc_tag_14a_tx_bytes(last_frame, last_frame_len, true);
        return;
    }
    unsigned pos = 1;
    cid_flag = (pcb & ISO_DEP_CID_PRESENT) != 0;
    if (cid_flag) {
        if (size < 3 || data[1] > ISO_DEP_CID_MAX)
            return;
        cid = data[pos++];
    }
    if (size <= pos || request_len + size - pos > sizeof(request)) {
        reset();
        return;
    }
    memcpy(request + request_len, data + pos, size - pos);
    request_len += size - pos;
    if (pcb & ISO_DEP_CHAINING) {
        expected_block ^= 1;
        last_frame[0] = ISO_DEP_R_ACK | expected_block | (cid_flag ? ISO_DEP_CID_PRESENT : 0);
        last_frame_len = 1;
        if (cid_flag)
            last_frame[last_frame_len++] = cid;
        nfc_tag_14a_tx_bytes(last_frame, last_frame_len, true);
        return;
    }
    if (!model.public_capture) {
        request_len = 0;
        return;
    }
    /* Accept native commands or short wrapped APDUs, preserving the optional
     * Le byte and returning wrapped status as 91 xx after the payload. */
    uint8_t native[TAG_NATIVE_REQUEST_BYTES], out[TAG_NATIVE_REPLY_BYTES];
    unsigned native_len = request_len;
    bool wrapped = request[0] == DESFIRE_APDU_CLA;
    if (wrapped) {
        if (request_len < DESFIRE_APDU_HEADER_BYTES || request[2] || request[3] ||
            request[4] > DESFIRE_APDU_PARAMS_MAX ||
            (request_len != (unsigned)DESFIRE_APDU_HEADER_BYTES + request[4] &&
             request_len != (unsigned)DESFIRE_APDU_HEADER_BYTES + 1u + request[4])) {
            reset();
            return;
        }
        native[0] = request[1];
        native_len = 1 + request[4];
        memcpy(native + 1, request + DESFIRE_APDU_HEADER_BYTES, request[4]);
    } else {
        if (native_len > sizeof(native)) {
            reset();
            return;
        }
        memcpy(native, request, native_len);
    }
    unsigned count = desfire_model_command(&model, native, native_len, out);
    response_offset = 0;
    reply_number = pcb & ISO_DEP_BLOCK_NUMBER;
    request_len = 0;
    if (wrapped) {
        memcpy(response, out + 1, count - 1);
        response[count - 1] = DESFIRE_APDU_STATUS_CLASS;
        response[count] = out[0];
        response_len = count + 1;
    } else {
        memcpy(response, out, count);
        response_len = count;
    }
    send_response();
}

int nfc_desfire_load(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    if (!desfire_model_load(&model, buffer->buffer, buffer->length))
        return -1;
    memset(&identity, 0, sizeof(identity));
    identity.size = model.uid_len;
    memcpy(identity.uid, model.uid, model.uid_len);
    memcpy(identity.atqa, model.atqa, 2);
    identity.sak[0] = model.sak;
    identity.ats.length = model.ats_len;
    memcpy(identity.ats.data, model.ats, model.ats_len);
    reference = (nfc_tag_14a_coll_res_reference_t){
        .uid = identity.uid, .size = &identity.size, .atqa = identity.atqa, .sak = identity.sak, .ats = &identity.ats};
    nfc_tag_14a_handler_t handler = {.get_coll_res = coll_res, .cb_state = handle, .cb_reset = reset};
    reset();
    nfc_tag_14a_set_handler(&handler);
    return desfire_image_size(buffer->buffer, buffer->length);
}

int nfc_desfire_save(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    return desfire_image_size(buffer->buffer, buffer->length);
}

bool nfc_desfire_factory(uint8_t slot, tag_specific_type_t type) {
    /* Explicit identity-only defaults, not invented public files or keys. */
    uint8_t bytes[24] = {7, 4, 1, 2, 3, 4, 5, 6, 0, 10, 0, 0, 'D', 'F', 'U', '1', 0x44, 3, 0x20, 2, 2, 0};
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_dump(slot, TAG_SENSE_HF, &map);
    return fds_write_sync(map.id, map.key, sizeof(bytes), bytes);
}

uint16_t nfc_desfire_image_command(bool write, uint16_t size, const uint8_t *data, uint8_t *output, uint16_t *length) {
    *length = 0;
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(tag_emulation_get_slot(), &types);
    if (get_device_mode() != DEVICE_MODE_READER || types.tag_hf != TAG_TYPE_DESFIRE)
        return STATUS_DEVICE_MODE_ERROR;
    tag_data_buffer_t *buffer = get_buffer_by_tag_type(TAG_TYPE_DESFIRE);
    if (size < IMAGE_TRANSFER_HEADER_BYTES)
        return STATUS_PAR_ERR;
    unsigned offset = data[0] << 8 | data[1], total = data[2] << 8 | data[3];
    if (!write) {
        unsigned actual = desfire_image_size(buffer->buffer, buffer->length);
        if (size != IMAGE_TRANSFER_HEADER_BYTES || !actual || total > IMAGE_READ_CHUNK_MAX || !total ||
            offset + total > actual)
            return STATUS_PAR_ERR;
        output[0] = actual >> 8;
        output[1] = actual;
        memcpy(output + 2, buffer->buffer + offset, total);
        *length = total + 2;
    } else {
        if (size <= IMAGE_TRANSFER_HEADER_BYTES || total > DESFIRE_SLOT_MAX || !total ||
            offset + size - IMAGE_TRANSFER_HEADER_BYTES > total)
            return STATUS_PAR_ERR;
        if (!offset) {
            upload_received = 0;
            upload_size = total;
        }
        if (upload_received != offset || upload_size != total)
            return STATUS_PAR_ERR;
        memcpy(buffer->buffer + offset, data + IMAGE_TRANSFER_HEADER_BYTES, size - IMAGE_TRANSFER_HEADER_BYTES);
        upload_received += size - IMAGE_TRANSFER_HEADER_BYTES;
        /* Preserve the previous persistent CRC: SAVE_SLOTS must detect and
         * persist this upload after the host has verified its full readback. */
        if (upload_received == total && nfc_desfire_load(TAG_TYPE_DESFIRE, buffer) <= 0)
            return STATUS_PAR_ERR;
    }
    return STATUS_SUCCESS;
}
