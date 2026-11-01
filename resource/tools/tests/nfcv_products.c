#include "iso15693/session.h"
#include "iso15693/commands.h"
#include "iso15693/waveform.h"
#include <assert.h>
#include <string.h>

static iso15693_frame_t frame;
static nfcv_reply_t reply;
static uint8_t image[NFCV_IMAGE_MAX_SIZE];

static void waveform(const uint8_t *bytes, unsigned length, unsigned rate) {
    uint8_t expected[ISO15693_RESPONSE_MAX_BYTES * 16u + 8u];
    unsigned count = 0;
    expected[count++] = 24 * rate;
    expected[count++] = 128 | (24 * rate);
    expected[count++] = 8 * rate;
    expected[count++] = 128 | (8 * rate);
    for (unsigned i = 0; i < length; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            bool one = (bytes[i] >> bit) & 1u;
            expected[count++] = (8 * rate) | (one ? 0 : 128);
            expected[count++] = (8 * rate) | (one ? 128 : 0);
        }
    }
    expected[count++] = 128 | (8 * rate);
    expected[count++] = 8 * rate;
    expected[count++] = 128 | (24 * rate);
    expected[count++] = 24 * rate;
    assert(count == length * NFCV_WAVEFORM_SEGMENTS_PER_BYTE + NFCV_WAVEFORM_FRAMING_SEGMENTS);
    for (unsigned segment = 0; segment < count; segment++)
        assert(nfcv_waveform_segment(bytes, length, segment, rate) == expected[segment]);
}

static void waveforms(void) {
    uint8_t bytes[ISO15693_RESPONSE_MAX_BYTES];
    for (unsigned rate = 1; rate <= 4; rate += 3) {
        for (unsigned first = 0; first < 256; first++) {
            bytes[0] = first;
            waveform(bytes, 1, rate);
            for (unsigned second = 0; second < 256; second++) {
                bytes[1] = second;
                waveform(bytes, 2, rate);
            }
        }
        for (unsigned pattern = 0; pattern < 4; pattern++) {
            for (unsigned i = 0; i < sizeof(bytes); i++)
                bytes[i] = pattern < 2 ? (pattern ? 0xff : 0) : pattern == 2 ? i : (i & 1 ? 0xaa : 0x55);
            waveform(bytes, sizeof(bytes), rate);
        }
    }
}

static bool command(uint8_t flags, uint8_t code, const uint8_t *params, unsigned count) {
    frame = (iso15693_frame_t){.high_data_rate = true, .single_subcarrier = true};
    frame.bytes[0] = flags;
    frame.bytes[1] = code;
    unsigned length = 2;
    if (code >= ISO15693_CMD_CUSTOM_MIN && code <= ISO15693_CMD_CUSTOM_MAX)
        frame.bytes[length++] = 4;
    if ((flags & ISO15693_REQ_ADDRESS) && !(flags & ISO15693_REQ_INVENTORY)) {
        iso15693_uid_to_wire(frame.bytes + length, image);
        length += ISO15693_UID_SIZE;
    }
    if (count) memcpy(frame.bytes + length, params, count);
    length += count;
    iso15693_append_crc(frame.bytes, length);
    frame.byte_count = length + ISO15693_CRC_SIZE;
    frame.crc_ok = true;
    return nfcv_session_handle(&frame, &reply);
}

static void product(bool slix_l) {
    memset(image, 0, sizeof(image));
    image[0] = 0xe0;
    image[1] = 4;
    image[2] = slix_l ? 3 : 1;
    image[3] = slix_l ? 0 : 0x10;
    image[7] = 0x0d;
    image[10] = 3;
    image[11] = 16;
    memset(image + 24, 0xa5, 8);
    unsigned blocks = slix_l ? NFCV_SLIX_L_BLOCKS : NFCV_SLIX_BLOCKS;
    unsigned storage_blocks = blocks;
    blocks = slix_l ? 8 : 28;
    unsigned size = NFCV_IMAGE_SIZE(storage_blocks);
    memset(image + NFCV_IMAGE_HEADER_SIZE + blocks * 4, 0xa6, (storage_blocks - blocks) * 4);
    assert(size == (slix_l ? 112u : 192u));
    assert(nfcv_image_valid(image, slix_l));
    assert(!nfcv_image_valid(image, !slix_l));
    assert(nfcv_session_configure(image, slix_l));
    nfcv_session_init();

    uint8_t mask = 0;
    assert(command(ISO15693_REQ_INVENTORY | ISO15693_REQ_HIGH_RATE | ISO15693_REQ_SLOT1,
                   ISO15693_CMD_INVENTORY, &mask, 1));
    assert(reply.len == 10 && reply.bytes[2] == 0x0d && reply.bytes[9] == 0xe0);
    assert(command(ISO15693_REQ_INVENTORY | ISO15693_REQ_HIGH_RATE,
                   ISO15693_CMD_INVENTORY, &mask, 1) == false);
    assert(nfcv_session_slot_pending());
    for (unsigned slot = 1; slot < 16; slot++) {
        bool answered = nfcv_session_next_slot(&reply);
        assert(answered == (slot == 13));
    }
    assert(!nfcv_session_slot_pending());
    assert(command(ISO15693_REQ_ADDRESS | ISO15693_REQ_HIGH_RATE,
                   ISO15693_CMD_GET_SYSTEM_INFO, NULL, 0));
    assert(reply.bytes[12] == blocks - 1);

    uint8_t last = blocks - 1;
    uint8_t write[] = {last, 0xde, 0xad, 0xbe, 0xef};
    assert(command(0x22, ISO15693_CMD_WRITE_SINGLE, write, sizeof(write)));
    assert(reply.bytes[0] == 0);
    assert(command(0x22, ISO15693_CMD_READ_SINGLE, &last, 1));
    assert(reply.len == 5 && !memcmp(reply.bytes + 1, write + 1, 4));
    assert(command(0x22, ISO15693_CMD_LOCK_BLOCK, &last, 1));
    assert(command(0x22, ISO15693_CMD_WRITE_SINGLE, write, sizeof(write)));
    assert(reply.bytes[0] == 1);
    uint8_t missing = blocks;
    assert(command(0x22, ISO15693_CMD_READ_SINGLE, &missing, 1));
    assert(reply.bytes[0] == 1);

    uint8_t range[] = {0, last};
    for (unsigned option = 0; option < 2; option++) {
        assert(command(0x22 | (option ? ISO15693_REQ_OPTION : 0),
                       ISO15693_CMD_READ_MULTIPLE, range, sizeof(range)));
        assert(reply.len == 1 + blocks * (4 + option) && reply.bytes[0] == 0);
        unsigned offset = 1;
        for (unsigned block = 0; block < blocks; block++) {
            if (option) assert(reply.bytes[offset++] == (block == last));
            assert(!memcmp(reply.bytes + offset, block == last ? write + 1 :
                           (uint8_t[]){0, 0, 0, 0}, 4));
            offset += 4;
        }
        iso15693_append_crc(reply.bytes, reply.len);
        assert(iso15693_check_crc(reply.bytes, reply.len + ISO15693_CRC_SIZE));
    }

    uint8_t saved[NFCV_IMAGE_MAX_SIZE];
    nfcv_session_save(saved);
    assert(!memcmp(saved + 24, image + 24, 8));
    assert(!memcmp(saved + NFCV_IMAGE_HEADER_SIZE + blocks * 4,
                   image + NFCV_IMAGE_HEADER_SIZE + blocks * 4, (storage_blocks - blocks) * 4));
    assert(!memcmp(saved + NFCV_IMAGE_HEADER_SIZE + storage_blocks * 4 + blocks,
                   image + NFCV_IMAGE_HEADER_SIZE + storage_blocks * 4 + blocks, storage_blocks - blocks));
    assert(!memcmp(saved + NFCV_IMAGE_HEADER_SIZE + last * 4, write + 1, 4));
    assert(saved[NFCV_IMAGE_HEADER_SIZE + storage_blocks * 4 + last] == 1);
    /* Saving into the configured source must retain reserved blocks and locks. */
    nfcv_session_save(image);
    assert(!memcmp(image, saved, size));
    assert(nfcv_session_configure(image, slix_l));
    nfcv_session_init();
    nfcv_session_field_lost();
    assert(command(0x22, ISO15693_CMD_READ_SINGLE, &last, 1));
    assert(!memcmp(reply.bytes + 1, write + 1, 4));
    assert(command(0x22, ISO15693_CMD_SELECT, NULL, 0));
    assert(command(ISO15693_REQ_SELECT | ISO15693_REQ_HIGH_RATE,
                   ISO15693_CMD_READ_SINGLE, &last, 1));
    assert(command(0x22, ISO15693_CMD_STAY_QUIET, NULL, 0) == false);
    assert(!command(ISO15693_REQ_INVENTORY | ISO15693_REQ_HIGH_RATE | ISO15693_REQ_SLOT1,
                    ISO15693_CMD_INVENTORY, &mask, 1));
    assert(command(0x22, ISO15693_CMD_RESET_TO_READY, NULL, 0));

    assert(command(0x22, ISO15693_CMD_NXP_SET_EAS, NULL, 0));
    assert(reply.bytes[0] == 0);
    bool privacy_reply = command(0x22, ISO15693_CMD_NXP_ENABLE_PRIVACY,
                                 (uint8_t[]){0, 0, 0, 0}, 4);
    assert(privacy_reply == !slix_l);
    if (!slix_l) assert(reply.bytes[0] == 1);
    saved[NFCV_IMAGE_HEADER_SIZE + storage_blocks * 4] = 2;
    assert(!nfcv_image_valid(saved, slix_l));
}

int main(void) {
    product(true);
    product(false);
    waveforms();
}
