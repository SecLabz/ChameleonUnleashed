/* Frame-to-frame NFC-V coordination: response rate and inventory slots. */
#include "session.h"

#include "slix.h"
#include <string.h>

static const uint8_t *original;
static uint8_t block_count, storage_blocks;
static bool product_slix_l;
static struct {
    nfcv_mem_t memory;
    nfcv_std_t standard;
    slix_t slix;
} product;

bool nfcv_image_valid(const uint8_t *image, bool slix_l) {
    if (!image || image[0] != 0xe0 || image[1] != 0x04 ||
            image[2] != (slix_l ? 0x03 : 0x01) ||
            (!slix_l && (image[3] & 0x18) != 0x10) ||
            (image[11] & (slix_l ? 0xe0 : 0xe8)))
        return false;
    unsigned locks = NFCV_IMAGE_HEADER_SIZE +
                     (slix_l ? NFCV_SLIX_L_BLOCKS : NFCV_SLIX_BLOCKS) * NFCV_BLOCK_SIZE;
    for (unsigned i = locks; i < NFCV_IMAGE_SIZE(slix_l ? NFCV_SLIX_L_BLOCKS : NFCV_SLIX_BLOCKS); i++)
        if (image[i] > 1) return false;
    return true;
}

bool nfcv_session_configure(const uint8_t *image, bool slix_l) {
    if (!nfcv_image_valid(image, slix_l)) return false;
    original = image;
    product_slix_l = slix_l;
    storage_blocks = slix_l ? NFCV_SLIX_L_BLOCKS : NFCV_SLIX_BLOCKS;
    block_count = slix_l ? 8 : 28;
    return true;
}

static void load_original(void) {
    uint32_t passwords[SLIX_PASSWORD_TYPE_COUNT];
    for (unsigned i = 0; i < SLIX_PASSWORD_TYPE_COUNT; i++) {
        passwords[i] = 0;
        for (unsigned j = 0; j < 4; j++) passwords[i] |= (uint32_t)original[12 + i * 4 + j] << (j * 8);
    }
    nfcv_mem_init(&product.memory, block_count, original + NFCV_IMAGE_HEADER_SIZE,
                  original + NFCV_IMAGE_HEADER_SIZE + storage_blocks * NFCV_BLOCK_SIZE);
    nfcv_std_init(&product.standard, &product.memory, original, original[8], original[9],
                  original[10], original[11] & 1, original[11] & 2);
    slix_init(&product.slix, &product.standard, original[11] & 4, original[11] & 8,
              passwords, product_slix_l);
    product.slix.eas_active = original[11] & 16;
}

void nfcv_session_save(uint8_t *image) {
    /* Keep legacy reserved blocks and locks at their existing offsets. */
    memmove(image, original, NFCV_IMAGE_SIZE(storage_blocks));
    memcpy(image, product.standard.uid, 8);
    image[8] = product.standard.dsfid;
    image[9] = product.standard.afi;
    image[10] = product.standard.ic_ref;
    image[11] = product.standard.dsfid_locked | (product.standard.afi_locked << 1) |
                (product.slix.eas_locked << 2) | (product.slix.privacy_mode << 3) |
                (product.slix.eas_active << 4);
    for (unsigned i = 0; i < SLIX_PASSWORD_TYPE_COUNT; i++)
        for (unsigned j = 0; j < 4; j++) image[12 + i * 4 + j] = product.slix.passwords[i] >> (j * 8);
    memcpy(image + NFCV_IMAGE_HEADER_SIZE, product.memory.memory, block_count * NFCV_BLOCK_SIZE);
    memcpy(image + NFCV_IMAGE_HEADER_SIZE + storage_blocks * NFCV_BLOCK_SIZE,
           product.memory.locks, block_count);
}

static bool pending, high_rate;
static uint8_t slot_index, slot_target;
static nfcv_reply_t inventory;
void nfcv_session_init(void) {
    load_original();
    pending = false;
    high_rate = true;
}

void nfcv_session_field_lost(void) {
    nfcv_std_reset_session(&product.standard);
    slix_reset_session(&product.slix);
    pending = false;
}

bool nfcv_session_slot_pending(void) {
    return pending;
}

bool nfcv_session_high_rate(void) {
    return high_rate;
}

bool nfcv_session_next_slot(nfcv_reply_t *response) {
    if (!pending)
        return false;
    if (++slot_index >= 16) {
        pending = false;
        return false;
    }
    if (slot_index != slot_target)
        return false;
    *response = inventory;
    pending = false;
    return true;
}

bool nfcv_session_handle(const iso15693_frame_t *frame, nfcv_reply_t *response) {
    pending = false;
    response->len = 0;
    if (!frame->crc_ok || frame->byte_count < 4 || !frame->single_subcarrier)
        return false;
    high_rate = frame->high_data_rate;
    bool slots =
        (frame->bytes[0] & ISO15693_REQ_INVENTORY) && !(frame->bytes[0] & ISO15693_REQ_SLOT1);
    nfcv_request_t request;
    if (!nfcv_std_prepare(&product.standard, frame, &request) ||
            !slix_allows_command(&product.slix, request.command))
        return false;
    nfcv_std_result_t result = nfcv_std_handle(&product.standard, &request, response);
    if (result == NFCV_STANDARD_UNHANDLED)
        slix_handle(&product.slix, &request, response);
    if (slots && response->len && request.command == ISO15693_CMD_INVENTORY) {
        unsigned mask = request.params[0];
        uint8_t wire[8];
        iso15693_uid_to_wire(wire, product.standard.uid);
        slot_target = 0;
        for (unsigned b = 0; b < 4; b++)
            if (mask + b < 64)
                slot_target |= ((wire[(mask + b) / 8] >> ((mask + b) % 8)) & 1u) << b;
        slot_index = 0;
        inventory = *response;
        pending = slot_target != 0;
        if (pending)
            response->len = 0;
    }
    return response->len != 0;
}
