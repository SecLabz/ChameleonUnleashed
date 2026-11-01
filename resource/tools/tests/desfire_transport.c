#include "desfire_model.h"
#include "app_status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef unsigned tag_specific_type_t;
enum { TAG_TYPE_DESFIRE = 1300, TAG_SENSE_HF = 2, DEVICE_MODE_READER = 1 };
typedef struct {
    uint8_t *buffer;
    unsigned length;
} tag_data_buffer_t;
typedef struct {
    unsigned tag_hf;
} tag_slot_specific_type_t;
typedef struct {
    unsigned id, key;
} fds_slot_record_map_t;
typedef struct {
    uint8_t data[255], length;
} nfc_14a_ats_t;
typedef struct {
    unsigned size;
    uint8_t atqa[2], sak[1], uid[10];
    nfc_14a_ats_t ats;
} nfc_tag_14a_coll_res_entity_t;
typedef struct {
    unsigned *size;
    uint8_t *atqa, *sak, *uid;
    nfc_14a_ats_t *ats;
} nfc_tag_14a_coll_res_reference_t;
typedef struct {
    nfc_tag_14a_coll_res_reference_t *(*get_coll_res)(void);
    void (*cb_state)(uint8_t *, uint16_t);
    void (*cb_reset)(void);
} nfc_tag_14a_handler_t;

static uint8_t image[4500], slot[4500], sent[64];
static uint8_t factory_image[24];
static unsigned image_size, sent_size, transmissions, frame_size = 64, device_mode = 1;
static unsigned slot_type = TAG_TYPE_DESFIRE, factory_writes;
static bool valid_crc = true;
static tag_data_buffer_t slot_buffer = {slot, sizeof(slot)};
static nfc_tag_14a_handler_t registered;

static unsigned nfc_tag_14a_get_fsd(void) {
    return frame_size;
}

static bool nfc_tag_14a_checks_crc(uint8_t *data, uint16_t length) {
    assert(length >= 3);
    return valid_crc;
}

static void nfc_tag_14a_tx_bytes(uint8_t *data, unsigned length, bool crc) {
    assert(crc && length <= sizeof(sent));
    memcpy(sent, data, length);
    sent_size = length;
    transmissions++;
}

static void nfc_tag_14a_set_handler(nfc_tag_14a_handler_t *handler) {
    registered = *handler;
}

static unsigned tag_emulation_get_slot(void) {
    return 3;
}

static void tag_emulation_get_specific_types_by_slot(unsigned index, tag_slot_specific_type_t *types) {
    assert(index == 3);
    types->tag_hf = slot_type;
}

static unsigned get_device_mode(void) {
    return device_mode;
}

static tag_data_buffer_t *get_buffer_by_tag_type(tag_specific_type_t type) {
    assert(type == TAG_TYPE_DESFIRE);
    return &slot_buffer;
}

static void get_fds_map_by_slot_sense_type_for_dump(unsigned index, unsigned sense, fds_slot_record_map_t *map) {
    assert(index == 3 && sense == TAG_SENSE_HF);
    map->id = 4;
    map->key = 5;
}

static bool fds_write_sync(unsigned id, unsigned key, unsigned length, const uint8_t *data) {
    assert(id == 4 && key == 5 && length == 24 && data[0] == 7);
    assert(!memcmp(data + 12, "DFU1", 4));
    memcpy(factory_image, data, length);
    factory_writes++;
    return true;
}

/* IMPLEMENTATION */

static void begin(void) {
    memcpy(slot, image, image_size);
    assert(nfc_desfire_load(TAG_TYPE_DESFIRE, &slot_buffer) == (int)image_size);
    frame_size = 64;
    sent_size = transmissions = 0;
    valid_crc = true;
    nfc_tag_14a_coll_res_reference_t *reference = registered.get_coll_res();
    assert(*reference->size == image[0] && !memcmp(reference->uid, image + 1, image[0]));
    assert(reference->ats->length >= 2);
}

static void deliver(const uint8_t *data, unsigned length) {
    uint8_t frame[128];
    assert(length + 2 <= sizeof(frame));
    memcpy(frame, data, length);
    memset(frame + length, 0, 2); /* CRC acceptance belongs to the NFC-A layer. */
    registered.cb_state(frame, (length + 2) * 8);
}

static void assert_frame(const uint8_t *expected, unsigned length) {
    assert(sent_size == length && !memcmp(sent, expected, length));
}

static void test_native_and_retries(void) {
    begin();
    const uint8_t version[] = {2, 0x60}, more1[] = {3, 0xaf}, more2[] = {2, 0xaf};
    deliver(version, sizeof(version));
    assert(transmissions == 1 && sent_size == 9 && sent[0] == 2 && sent[1] == 0xaf);
    uint8_t first[64];
    unsigned first_size = sent_size;
    memcpy(first, sent, sent_size);
    deliver(version, sizeof(version));
    assert(transmissions == 2);
    assert_frame(first, first_size);
    const uint8_t nack[] = {0xb3}, wrong_ack[] = {0xa2};
    deliver(nack, sizeof(nack));
    assert_frame(first, first_size);
    deliver(wrong_ack, sizeof(wrong_ack));
    assert_frame(first, first_size);
    deliver(more1, sizeof(more1));
    assert(sent_size == 9 && sent[0] == 3 && sent[1] == 0xaf);
    deliver(more2, sizeof(more2));
    assert(sent_size == 16 && sent[0] == 2 && sent[1] == 0);
    const uint8_t deselect[] = {0xc2};
    deliver(deselect, sizeof(deselect));
    assert_frame(deselect, sizeof(deselect));
    deliver(version, sizeof(version));
    assert_frame(first, first_size);
}

static void test_apdu_cid_and_chaining(void) {
    begin();
    const uint8_t wrapped[] = {2, 0x90, 0x60, 0, 0, 0};
    deliver(wrapped, sizeof(wrapped));
    assert(sent_size == 10 && sent[0] == 2 && sent[8] == 0x91 && sent[9] == 0xaf);
    uint8_t expected[64];
    unsigned expected_size = sent_size;
    memcpy(expected, sent, sent_size);
    begin();
    const uint8_t wrapped_le[] = {2, 0x90, 0x60, 0, 0, 0, 0};
    deliver(wrapped_le, sizeof(wrapped_le));
    assert_frame(expected, expected_size);
    begin();
    const uint8_t first[] = {0x12, 0x90, 0x60}, last[] = {3, 0, 0, 0};
    const uint8_t ack[] = {0xa3};
    deliver(first, sizeof(first));
    assert_frame(ack, sizeof(ack));
    deliver(first, sizeof(first)); /* Duplicate must not append parameters twice. */
    assert_frame(ack, sizeof(ack));
    deliver(last, sizeof(last));
    expected[0] = 3;
    assert_frame(expected, expected_size);
    begin();
    const uint8_t cid_request[] = {0x0a, 14, 0x60};
    deliver(cid_request, sizeof(cid_request));
    assert(sent_size == 10 && sent[0] == 0x0a && sent[1] == 14 && sent[2] == 0xaf);
    begin();
    frame_size = 8;
    const uint8_t version[] = {2, 0x60};
    deliver(version, sizeof(version));
    assert(sent_size == 6 && sent[0] == 0x12 && sent[1] == 0xaf);
    uint8_t fragment[64];
    memcpy(fragment, sent, sent_size);
    const uint8_t nack[] = {0xb3};
    deliver(nack, sizeof(nack));
    assert_frame(fragment, 6);
    deliver(ack, sizeof(ack));
    assert(sent_size == 4 && sent[0] == 3 && transmissions == 3);
    const uint8_t done_ack[] = {0xa2};
    deliver(done_ack, sizeof(done_ack));
    assert(transmissions == 3);
}

static void test_malformed_frames(void) {
    const uint8_t malformed[][9] = {
        {2, 0x90, 0x60, 1, 0, 0}, /* Nonzero P1. */
        {2, 0x90, 0x60, 0, 0, 2}, /* Truncated Lc. */
        {0x0a, 15, 0x60},         /* Invalid CID. */
        {6, 0x60},                /* Unsupported NAD. */
        {0xe2, 0x60},             /* Unsupported PCB. */
    };
    const unsigned lengths[] = {6, 6, 3, 2, 2};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        begin();
        deliver(malformed[i], lengths[i]);
        assert(!transmissions);
    }
    begin();
    const uint8_t version[] = {2, 0x60};
    valid_crc = false;
    deliver(version, sizeof(version));
    assert(!transmissions);
    valid_crc = true;
    uint8_t short_frame[3] = {2, 0, 0};
    registered.cb_state(short_frame, 23);
    registered.cb_state(short_frame, 25);
    assert(!transmissions);
    uint8_t oversized[42] = {2};
    deliver(oversized, sizeof(oversized));
    assert(!transmissions);
    uint8_t chain[41] = {0x12};
    deliver(chain, sizeof(chain));
    assert(transmissions == 1);
    chain[0] = 3;
    deliver(chain, sizeof(chain)); /* Accumulated request exceeds 64 bytes. */
    assert(transmissions == 1);
    deliver(version, sizeof(version)); /* Reset must admit the next reader. */
    assert(transmissions == 2);
}

static void test_image_transfer(void) {
    begin();
    uint8_t header[] = {0, 0, 0, 16}, output[512];
    uint16_t length = 0;
    assert(nfc_desfire_image_command(false, 4, header, output, &length) == STATUS_SUCCESS);
    assert(length == 18 && ((unsigned)output[0] << 8 | output[1]) == image_size);
    assert(!memcmp(output + 2, image, 16));
    device_mode = 0;
    assert(nfc_desfire_image_command(false, 4, header, output, &length) == STATUS_DEVICE_MODE_ERROR);
    device_mode = 1;
    slot_type = 0;
    assert(nfc_desfire_image_command(false, 4, header, output, &length) == STATUS_DEVICE_MODE_ERROR);
    slot_type = TAG_TYPE_DESFIRE;
    header[2] = 2;
    assert(nfc_desfire_image_command(false, 4, header, output, &length) == STATUS_PAR_ERR);
    for (unsigned offset = 0; offset < image_size;) {
        uint8_t packet[36] = {offset >> 8, offset, image_size >> 8, image_size};
        unsigned bytes = image_size - offset;
        if (bytes > 32)
            bytes = 32;
        memcpy(packet + 4, image + offset, bytes);
        if (offset) {
            packet[1]++;
            assert(nfc_desfire_image_command(true, bytes + 4, packet, output, &length) == STATUS_PAR_ERR);
            packet[1]--;
        }
        assert(nfc_desfire_image_command(true, bytes + 4, packet, output, &length) == STATUS_SUCCESS);
        assert(!length);
        offset += bytes;
    }
    assert(!memcmp(slot, image, image_size));
    assert(nfc_desfire_save(TAG_TYPE_DESFIRE, &slot_buffer) == (int)image_size);
    assert(nfc_desfire_factory(3, TAG_TYPE_DESFIRE) && factory_writes == 1);
    memset(slot, 0, sizeof(slot));
    memcpy(slot, factory_image, sizeof(factory_image));
    assert(nfc_desfire_load(TAG_TYPE_DESFIRE, &slot_buffer) == 24);
    transmissions = 0;
    const uint8_t version[] = {2, 0x60};
    deliver(version, sizeof(version));
    assert(!transmissions); /* An identity-only image has no invented native data. */
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    image_size = fread(image, 1, sizeof(image), file);
    fclose(file);
    assert(image_size > 16 && image_size < sizeof(image));
    test_native_and_retries();
    test_apdu_cid_and_chaining();
    test_malformed_frames();
    test_image_transfer();
    return 0;
}
