#include "slot_image.h"
#include "iso15693/session.h"
#include "lf_tag_em.h"
#include "nfc_mf1.h"
#include "nfc_mf0_ntag.h"
#include "nfc_desfire.h"
#include <stddef.h>
#include <string.h>

static unsigned read16(const uint8_t *bytes) { return (bytes[0] << 8) | bytes[1]; }
static void write16(uint8_t *bytes, unsigned value) { bytes[0] = value >> 8; bytes[1] = value; }

bool slot_image_shape(tag_specific_type_t type, slot_image_shape_t *shape) {
    *shape = (slot_image_shape_t){.type = type};
    if (type == TAG_TYPE_DESFIRE) { shape->sense = TAG_SENSE_HF; shape->unit_bytes = 1; return true; }
    if (type >= TAG_TYPE_MIFARE_Mini && type <= TAG_TYPE_MIFARE_4096) {
        static const uint16_t blocks[] = {20, 64, 128, 256};
        shape->sense = TAG_SENSE_HF;
        shape->unit_bytes = 16;
        shape->memory_bytes = blocks[type - TAG_TYPE_MIFARE_Mini] * 16;
        shape->native_bytes = (offsetof(nfc_tag_mf1_information_t, memory) + shape->memory_bytes + 3) & ~3u;
        shape->option_bytes = 7;
        return true;
    }
    if (type == TAG_TYPE_ST25TB512_AT) {
        shape->sense = TAG_SENSE_HF;
        shape->unit_bytes = 4;
        shape->memory_bytes = 64;
        shape->native_bytes = 80; /* Existing 77-byte image, FDS word padding. */
        return true;
    }
    if (type == TAG_TYPE_SLIX_L || type == TAG_TYPE_SLIX) {
        shape->sense = TAG_SENSE_HF;
        shape->unit_bytes = 4;
        shape->memory_bytes = (type == TAG_TYPE_SLIX_L ? 8 : 28) * 4;
        shape->native_bytes = NFCV_IMAGE_SIZE(type == TAG_TYPE_SLIX_L ? NFCV_SLIX_L_BLOCKS : NFCV_SLIX_BLOCKS);
        return true;
    }
    int pages = nfc_tag_mf0_ntag_get_nr_pages_by_tag_type(type);
    if (pages > 0) {
        int size = nfc_tag_mf0_ntag_image_size(type);
        if (size < (int)offsetof(nfc_tag_mf0_ntag_information_t, memory) || size > SLOT_IMAGE_MAX_BYTES) return false;
        shape->sense = TAG_SENSE_HF;
        shape->unit_bytes = 4;
        shape->native_bytes = (size + 3) & ~3u;
        shape->memory_bytes = size - offsetof(nfc_tag_mf0_ntag_information_t, memory);
        shape->option_bytes = 5;
        return true;
    }
    unsigned size;
    switch (type) {
    case TAG_TYPE_EM410X: size = LF_EM410X_TAG_ID_SIZE; break;
    case TAG_TYPE_EM410X_ELECTRA: size = LF_EM410X_ELECTRA_TAG_ID_SIZE; break;
    case TAG_TYPE_HID_PROX: size = LF_HIDPROX_TAG_ID_SIZE; break;
    case TAG_TYPE_IOPROX: size = LF_IOPROX_TAG_ID_SIZE; break;
    case TAG_TYPE_PAC: size = LF_PAC_TAG_ID_SIZE; break;
    case TAG_TYPE_VIKING: size = LF_VIKING_TAG_ID_SIZE; break;
    case TAG_TYPE_JABLOTRON: size = LF_JABLOTRON_TAG_ID_SIZE; break;
    case TAG_TYPE_IDTECK: size = LF_IDTECK_TAG_ID_SIZE; break;
    default: return false;
    }
    shape->sense = TAG_SENSE_LF;
    shape->unit_bytes = size;
    shape->memory_bytes = size;
    shape->native_bytes = (size + 3) & ~3u;
    return true;
}

static bool st25tb_identity(const uint8_t *image) {
    return image[7] == 0xd0 && image[6] == 2 && image[5] == 0x33;
}

/* Version 1: version, type BE16, unit bytes, memory bytes BE16, UID length,
 * ATS length, option length; then UID, ATQA in wire order, SAK (HF only),
 * ATS, explicit emulator options and memory. Native structs never cross HTTP. */
bool slot_image_encode(tag_specific_type_t type, const uint8_t *native, size_t native_size,
    uint8_t *wire, size_t capacity, uint16_t *size) {
    slot_image_shape_t shape;
    if (!slot_image_shape(type, &shape) || native_size < shape.native_bytes) return false;
    if (type == TAG_TYPE_DESFIRE) {
        unsigned n = desfire_image_size(native, native_size);
        if (!n || capacity < n + 5) return false;
        wire[0] = 2; write16(wire + 1, type); write16(wire + 3, n);
        memcpy(wire + 5, native, n); *size = n + 5; return true;
    }
    if (type == TAG_TYPE_ST25TB512_AT) {
        /* Version 2: type BE16, payload length BE16, existing explicit image. */
        if (capacity < 82 || !st25tb_identity(native)) return false;
        wire[0] = 2; write16(wire + 1, type); write16(wire + 3, 77);
        memcpy(wire + 5, native, 77);
        *size = 82;
        return true;
    }
    if (type == TAG_TYPE_SLIX_L || type == TAG_TYPE_SLIX) {
        if (capacity < shape.native_bytes + 5u || !nfcv_image_valid(native, type == TAG_TYPE_SLIX_L)) return false;
        wire[0] = 2; write16(wire + 1, type); write16(wire + 3, shape.native_bytes);
        memcpy(wire + 5, native, shape.native_bytes);
        *size = shape.native_bytes + 5;
        return true;
    }
    const nfc_tag_14a_coll_res_entity_t *identity = shape.sense == TAG_SENSE_HF ? (const void *)native : NULL;
    unsigned uid_size = identity ? identity->size : 0;
    if (identity && uid_size != 4 && uid_size != 7 && uid_size != 10) return false;
    unsigned ats_size = identity ? identity->ats.length : 0;
    unsigned total = 9 + (identity ? uid_size + 3 : 0) + ats_size + shape.option_bytes + shape.memory_bytes;
    if (total > capacity || total > SLOT_IMAGE_MAX_BYTES) return false;
    wire[0] = 1; write16(wire + 1, type); wire[3] = shape.unit_bytes;
    write16(wire + 4, shape.memory_bytes);
    wire[6] = uid_size; wire[7] = ats_size; wire[8] = shape.option_bytes;
    unsigned offset = 9;
    if (identity) {
        memcpy(wire + offset, identity->uid, uid_size); offset += uid_size;
        memcpy(wire + offset, identity->atqa, 2); offset += 2;
        wire[offset++] = identity->sak[0];
        memcpy(wire + offset, identity->ats.data, ats_size); offset += ats_size;
    }
    const uint8_t *memory = native;
    if (shape.unit_bytes == 16 && shape.sense == TAG_SENSE_HF) {
        const nfc_tag_mf1_information_t *image = (const void *)native;
        const nfc_tag_mf1_configure_t *config = &image->config;
        uint8_t options[] = {config->mode_block_write, config->mode_gen1a_magic, config->mode_gen2_magic,
            config->use_mf1_coll_res, config->detection_enable, config->field_off_do_reset, config->prng_type};
        memcpy(wire + offset, options, sizeof(options));
        memory = (const uint8_t *)image->memory;
    } else if (shape.sense == TAG_SENSE_HF) {
        const nfc_tag_mf0_ntag_information_t *image = (const void *)native;
        wire[offset] = image->config.mode_uid_magic;
        wire[offset + 1] = image->config.mode_block_write;
        wire[offset + 2] = image->config.detection_enable;
        write16(wire + offset + 3, nfc_tag_mf0_ntag_get_nr_pages_by_tag_type(type));
        memory = (const uint8_t *)image->memory;
    }
    offset += shape.option_bytes;
    memcpy(wire + offset, memory, shape.memory_bytes);
    *size = total;
    return true;
}

bool slot_image_decode(const uint8_t *wire, size_t size, uint8_t *native,
    size_t capacity, slot_image_shape_t *shape) {
    if (size >= 5 && wire[0] == 2 && read16(wire + 1) == TAG_TYPE_DESFIRE) {
        unsigned n = read16(wire + 3);
        desfire_model_t validation;
        if (n + 5 != size || n > capacity || desfire_image_size(wire + 5, n) != n ||
            !desfire_model_load(&validation, wire + 5, n)) return false;
        slot_image_shape(TAG_TYPE_DESFIRE, shape); shape->native_bytes = n;
        memcpy(native, wire + 5, n); return true;
    }
    if (size == 82 && wire[0] == 2 && read16(wire + 1) == TAG_TYPE_ST25TB512_AT &&
        read16(wire + 3) == 77 && st25tb_identity(wire + 5)) {
        if (!slot_image_shape(TAG_TYPE_ST25TB512_AT, shape) || capacity < shape->native_bytes) return false;
        memset(native, 0, shape->native_bytes);
        memcpy(native, wire + 5, 77);
        return true;
    }
    if (size >= 5 && wire[0] == 2 &&
            (read16(wire + 1) == TAG_TYPE_SLIX_L || read16(wire + 1) == TAG_TYPE_SLIX)) {
        if (!slot_image_shape(read16(wire + 1), shape) || capacity < shape->native_bytes ||
                size != shape->native_bytes + 5u || read16(wire + 3) != shape->native_bytes ||
                !nfcv_image_valid(wire + 5, shape->type == TAG_TYPE_SLIX_L)) return false;
        memcpy(native, wire + 5, shape->native_bytes);
        return true;
    }
    if (size < 9 || wire[0] != 1 || !slot_image_shape(read16(wire + 1), shape) ||
        (shape->type == TAG_TYPE_DESFIRE || shape->type == TAG_TYPE_ST25TB512_AT || shape->type == TAG_TYPE_SLIX_L || shape->type == TAG_TYPE_SLIX) || shape->native_bytes > capacity || wire[3] != shape->unit_bytes ||
        read16(wire + 4) != shape->memory_bytes || wire[8] != shape->option_bytes) return false;
    unsigned uid_size = wire[6], ats_size = wire[7];
    if (shape->sense == TAG_SENSE_HF ? (uid_size != 4 && uid_size != 7 && uid_size != 10) : (uid_size || ats_size)) return false;
    unsigned offset = 9 + (uid_size ? uid_size + 3 : 0) + ats_size;
    if (offset + shape->option_bytes + shape->memory_bytes != size) return false;
    const uint8_t *options = wire + offset;
    memset(native, 0, shape->native_bytes);
    if (uid_size) {
        nfc_tag_14a_coll_res_entity_t *identity = (void *)native;
        identity->size = uid_size;
        memcpy(identity->uid, wire + 9, uid_size);
        memcpy(identity->atqa, wire + 9 + uid_size, 2);
        identity->sak[0] = wire[11 + uid_size];
        identity->ats.length = ats_size;
        memcpy(identity->ats.data, wire + 12 + uid_size, ats_size);
    }
    uint8_t *memory = native;
    if (shape->unit_bytes == 16 && shape->sense == TAG_SENSE_HF) {
        if (options[0] > 4 || options[6] > 2) return false;
        for (unsigned i = 1; i < 6; i++) if (options[i] > 1) return false;
        nfc_tag_mf1_information_t *image = (void *)native;
        image->config.mode_block_write = options[0];
        image->config.mode_gen1a_magic = options[1]; image->config.mode_gen2_magic = options[2];
        image->config.use_mf1_coll_res = options[3]; image->config.detection_enable = options[4];
        image->config.field_off_do_reset = options[5]; image->config.prng_type = options[6];
        memory = (uint8_t *)image->memory;
    } else if (shape->sense == TAG_SENSE_HF) {
        if (options[0] > 1 || options[1] > 4 || options[2] > 1 ||
            read16(options + 3) != nfc_tag_mf0_ntag_get_nr_pages_by_tag_type(shape->type)) return false;
        nfc_tag_mf0_ntag_information_t *image = (void *)native;
        image->config.mode_uid_magic = options[0]; image->config.mode_block_write = options[1];
        image->config.detection_enable = options[2];
        memory = (uint8_t *)image->memory;
    }
    memcpy(memory, wire + offset + shape->option_bytes, shape->memory_bytes);
    return true;
}
