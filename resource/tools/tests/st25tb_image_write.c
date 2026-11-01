#define PROJECT_CHAMELEON_ULTRA
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "st25tb/model.h"
#include "nfctag/tag_base_type.h"

#define ST25TB_RECORD_SIZE 80u
#define NFCV_SLIX_L_BLOCKS 8u
#define NFCV_SLIX_BLOCKS 32u
#define NFCV_IMAGE_SIZE(blocks) (16u + (blocks) * 4u)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
enum { DEVICE_MODE_READER, DEVICE_MODE_TAG };
typedef struct {
    uint16_t length;
    uint8_t *buffer;
    uint16_t *crc;
} tag_data_buffer_t;
typedef int (*tag_datas_loadcb_t)(tag_specific_type_t type, tag_data_buffer_t *buffer);
typedef int (*tag_datas_savecb_t)(tag_specific_type_t type, tag_data_buffer_t *buffer);
typedef struct {
    uint16_t id;
    uint16_t key;
} fds_slot_record_map_t;
static struct {
    tag_slot_specific_type_t slots[1];
} slotConfig = {{{TAG_TYPE_ST25TB512_AT, TAG_TYPE_EM410X}}};
static uint8_t hf_ram[128];
static uint8_t lf_ram[20];
static uint8_t hf_flash[ST25TB_RECORD_SIZE];
static uint8_t lf_flash[20];
static uint8_t lf_emulated[20];
static uint16_t hf_crc;
static uint16_t lf_crc;
static tag_data_buffer_t hf_buffer = {sizeof(hf_ram), hf_ram, &hf_crc};
static tag_data_buffer_t lf_buffer = {sizeof(lf_ram), lf_ram, &lf_crc};
static bool no_hf_buffer;
static bool fail_write;
static bool sensing;
static bool lf_sensing = true;
static unsigned sense_changes;
static unsigned writes;
static unsigned reads;
static unsigned lf_loads;
static int mode = DEVICE_MODE_TAG;

static uint16_t checksum(const uint8_t *bytes, size_t length) {
    uint16_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum = sum * 31u + bytes[i];
    }
    return sum;
}

static void calc_14a_crc_lut(const uint8_t *bytes, size_t length, uint8_t *result) {
    uint16_t sum = checksum(bytes, length);
    memcpy(result, &sum, sizeof(sum));
}

static tag_sense_type_t get_sense_type_from_tag_type(tag_specific_type_t type) {
    return type == TAG_TYPE_ST25TB512_AT ? TAG_SENSE_HF : TAG_SENSE_LF;
}

static tag_data_buffer_t *get_buffer_by_tag_type(tag_specific_type_t type) {
    if (type == TAG_TYPE_ST25TB512_AT) {
        return no_hf_buffer ? NULL : &hf_buffer;
    }
    assert(type == TAG_TYPE_EM410X);
    return &lf_buffer;
}

static uint8_t tag_emulation_get_slot(void) {
    return 0;
}

static void tag_emulation_get_specific_types_by_slot(uint8_t slot, tag_slot_specific_type_t *types) {
    assert(slot == 0);
    *types = slotConfig.slots[slot];
}

static void get_fds_map_by_slot_sense_type_for_dump(uint8_t slot, tag_sense_type_t sense,
                                                  fds_slot_record_map_t *map) {
    assert(slot == 0);
    *map = (fds_slot_record_map_t){.id = 1, .key = sense};
}

static bool fds_write_sync(uint16_t id, uint16_t key, uint16_t length, void *bytes) {
    assert(id == 1);
    writes++;
    if (fail_write) {
        return false;
    }
    if (key == TAG_SENSE_HF) {
        assert(length == sizeof(hf_flash));
        memcpy(hf_flash, bytes, length);
    } else {
        assert(key == TAG_SENSE_LF && length == sizeof(lf_flash));
        memcpy(lf_flash, bytes, length);
    }
    return true;
}

static bool fds_read_sync(uint16_t id, uint16_t key, uint16_t *length, uint8_t *bytes) {
    assert(id == 1);
    reads++;
    if (key == TAG_SENSE_HF) {
        assert(*length >= sizeof(hf_flash));
        *length = sizeof(hf_flash);
        memcpy(bytes, hf_flash, *length);
    } else {
        assert(key == TAG_SENSE_LF && *length >= sizeof(lf_flash));
        *length = sizeof(lf_flash);
        memcpy(bytes, lf_flash, *length);
    }
    return true;
}

static int lf_load(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    assert(type == TAG_TYPE_EM410X && buffer->length >= sizeof(lf_emulated));
    lf_loads++;
    memcpy(lf_emulated, buffer->buffer, sizeof(lf_emulated));
    return sizeof(lf_emulated);
}

static int lf_save(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    assert(type == TAG_TYPE_EM410X && buffer == &lf_buffer);
    return sizeof(lf_emulated);
}

static void nfc_tag_15_prepare_load(void) {
    assert(!"unexpected NFC-V load");
}

static void nfc_st25tb_sense_switch(bool enable) {
    sensing = enable;
    sense_changes++;
}

static void tag_emulation_sense_switch(tag_sense_type_t sense, bool enable) {
    assert(sense == TAG_SENSE_HF);
    nfc_st25tb_sense_switch(enable);
}

static int get_device_mode(void) {
    return mode;
}

/* ST25TB CALLBACKS */

static tag_datas_loadcb_t get_data_loadcb_from_tag_type(tag_specific_type_t type) {
    return type == TAG_TYPE_ST25TB512_AT ? nfc_st25tb_data_loadcb : lf_load;
}

static tag_datas_savecb_t get_data_savecb_from_tag_type(tag_specific_type_t type) {
    return type == TAG_TYPE_ST25TB512_AT ? nfc_st25tb_data_savecb : lf_save;
}

/* SLOT STORAGE */
/* ST25TB SETTER */

static void assert_lf_unchanged(unsigned loads_before, uint16_t crc_before) {
    assert(lf_loads == loads_before && lf_crc == crc_before && lf_sensing);
    for (unsigned i = 0; i < sizeof(lf_ram); i++) {
        assert(lf_ram[i] == 0x42 && lf_emulated[i] == 0x42 && lf_flash[i] == 0x11);
    }
}

int main(void) {
    uint8_t image[ST25TB_IMAGE_SIZE] = {0};
    image[5] = 0x33;
    image[6] = 2;
    image[7] = 0xd0;
    memset(hf_flash, 0, sizeof(hf_flash));
    memcpy(hf_flash, image, sizeof(image));
    memset(lf_flash, 0x11, sizeof(lf_flash));
    tag_emulation_load_data();
    uint16_t saved_lf_crc = lf_crc;
    // Existing LF setters update the RAM and emulator without saving the slot.
    memset(lf_ram, 0x42, sizeof(lf_ram));
    (void)tag_emulation_load_by_buffer(TAG_TYPE_EM410X, false);
    unsigned loads_before = lf_loads;
    image[8] = 0xab;
    image[76] = 7;
    sensing = true;
    unsigned reads_before = reads;
    assert(nfc_st25tb_set_image(image));
    assert_lf_unchanged(loads_before, saved_lf_crc);
    assert(reads == reads_before && sensing);
    assert(memcmp(hf_ram, image, sizeof(image)) == 0);
    assert(memcmp(hf_flash, hf_ram, sizeof(hf_flash)) == 0);
    assert(hf_ram[77] == 0 && hf_ram[78] == 0 && hf_ram[79] == 0);
    assert(hf_crc == checksum(hf_ram, ST25TB_RECORD_SIZE));
    uint8_t loaded_image[ST25TB_IMAGE_SIZE];
    assert(st25tb_save(loaded_image) && memcmp(loaded_image, image, sizeof(image)) == 0);
    // The persisted HF CRC suppresses a redundant save; LF remains dirty.
    unsigned writes_before = writes;
    assert(save_data_by_tag_type(0, TAG_TYPE_ST25TB512_AT));
    assert(writes == writes_before);
    for (unsigned resume = 0; resume < 2; resume++) {
        sensing = resume;
        uint16_t saved_hf_crc = hf_crc;
        uint8_t saved_hf_ram[sizeof(hf_ram)];
        memcpy(saved_hf_ram, hf_ram, sizeof(hf_ram));
        fail_write = true;
        image[8]++;
        assert(!nfc_st25tb_set_image(image));
        assert(sensing == (bool)resume && hf_crc == saved_hf_crc);
        assert(memcmp(hf_ram, saved_hf_ram, sizeof(hf_ram)) == 0);
        assert(st25tb_save(image) && memcmp(image, loaded_image, sizeof(image)) == 0);
        assert_lf_unchanged(loads_before, saved_lf_crc);
        fail_write = false;
    }
    // Reject missing/short buffers and invalid images before flash or sensing.
    writes_before = writes;
    unsigned changes_before = sense_changes;
    no_hf_buffer = true;
    assert(!nfc_st25tb_set_image(image));
    no_hf_buffer = false;
    hf_buffer.length = ST25TB_RECORD_SIZE - 1;
    assert(!nfc_st25tb_set_image(image));
    hf_buffer.length = sizeof(hf_ram);
    assert(!nfc_st25tb_set_image(NULL));
    image[5] = 0;
    assert(!nfc_st25tb_set_image(image));
    image[5] = 0x33;
    assert(writes == writes_before && sense_changes == changes_before);
    // Reader mode must not restart HF emulation.
    mode = DEVICE_MODE_READER;
    sensing = false;
    assert(nfc_st25tb_set_image(image) && !sensing);
    assert_lf_unchanged(loads_before, saved_lf_crc);
    writes_before = writes;
    assert(save_data_by_tag_type(0, TAG_TYPE_EM410X));
    assert(writes == writes_before + 1 && lf_crc == checksum(lf_ram, sizeof(lf_ram)));
    assert(memcmp(lf_flash, lf_ram, sizeof(lf_ram)) == 0);
    return 0;
}
