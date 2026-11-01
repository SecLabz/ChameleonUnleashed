#include "nfc_15.h"
#include "nfcv_radio.h"
#include "tag_persistence.h"
#include "fds_util.h"
#include "rfid_main.h"
#include "app_status.h"
#include "syssleep.h"
#include "lf_tag_em.h"
#include "app_util_platform.h"
#include <string.h>

static bool sensing, loaded, field_active;

static unsigned image_blocks(tag_specific_type_t type) {
    if (type == TAG_TYPE_SLIX_L) return NFCV_SLIX_L_BLOCKS;
    if (type == TAG_TYPE_SLIX) return NFCV_SLIX_BLOCKS;
    return 0;
}

static void update_field(bool present) {
    CRITICAL_REGION_ENTER();
    bool was_emulating = g_is_tag_emulating;
    g_is_tag_emulating = present || lf_tag_125khz_is_emulating();
    if (present && (!field_active || !was_emulating)) sleep_timer_stop();
    else if (!present && field_active) sleep_timer_start(SLEEP_DELAY_MS_FIELD_NFC_LOST);
    field_active = present;
    CRITICAL_REGION_EXIT();
}

void nfc_tag_15_prepare_load(void) { loaded = false; }

int nfc_tag_15_load(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    loaded = false;
    unsigned blocks = image_blocks(type);
    if (!blocks || buffer->length < NFCV_IMAGE_SIZE(blocks) ||
            !nfcv_session_configure(buffer->buffer, type == TAG_TYPE_SLIX_L)) return 0;
    nfcv_session_init();
    loaded = true;
    return NFCV_IMAGE_SIZE(blocks);
}

int nfc_tag_15_save(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    unsigned blocks = image_blocks(type);
    if (!loaded || !blocks || buffer->length < NFCV_IMAGE_SIZE(blocks)) return 0;
    nfcv_session_save(buffer->buffer);
    return NFCV_IMAGE_SIZE(blocks);
}

bool nfc_tag_15_factory(uint8_t slot, tag_specific_type_t type) {
    unsigned blocks = image_blocks(type);
    if (!blocks) return false;
    uint8_t image[NFCV_IMAGE_MAX_SIZE] = {0xe0, 4, 3, 0, 3, 4, 5, 6};
    if (type == TAG_TYPE_SLIX) {
        image[2] = 1;
        image[3] = 0x10;
    }
    image[10] = 0x03;
    image[11] = 16;
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_dump(slot, TAG_SENSE_HF, &map);
    return fds_write_sync(map.id, map.key, NFCV_IMAGE_SIZE(blocks), image);
}

void nfc_tag_15_sense_switch(bool enable) {
    if (enable == sensing) return;
    if (!enable) {
        nfcv_radio_stop();
        sensing = false;
        update_field(false);
        return;
    }
    if (loaded) sensing = nfcv_radio_start();
}

bool nfc_tag_15_active(void) { return sensing && loaded; }

void nfc_tag_15_poll(void) {
    if (!loaded) nfc_tag_15_sense_switch(false);
    if (!sensing) return;
    nfcv_radio_poll();
    update_field(nfcv_radio_field_present());
}

uint16_t nfc_tag_15_image_command(bool write, uint16_t length, const uint8_t *data,
                                  uint8_t *output, uint16_t *output_length) {
    *output_length = 0;
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(tag_emulation_get_slot(), &types);
    unsigned blocks = image_blocks(types.tag_hf);
    if (!blocks || !loaded) return STATUS_INVALID_SLOT_TYPE;
    if (length != (write ? NFCV_IMAGE_SIZE(blocks) : 0) ||
            (write && !nfcv_image_valid(data, types.tag_hf == TAG_TYPE_SLIX_L)))
        return STATUS_PAR_ERR;
    tag_data_buffer_t *buffer = get_buffer_by_tag_type(types.tag_hf);
    if (!buffer) return STATUS_INVALID_SLOT_TYPE;
    if (write) {
        if (nfcv_radio_field_present()) return STATUS_BUSY;
        bool resume = sensing;
        nfc_tag_15_sense_switch(false);
        memcpy(buffer->buffer, data, NFCV_IMAGE_SIZE(blocks));
        nfc_tag_15_load(types.tag_hf, buffer);
        if (resume) nfc_tag_15_sense_switch(true);
    } else {
        nfcv_session_save(output);
        *output_length = NFCV_IMAGE_SIZE(blocks);
    }
    return STATUS_SUCCESS;
}
