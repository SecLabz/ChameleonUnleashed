#include "st25tb/model.h"
#include "iso15693/frame.h"
#include <assert.h>
#include <string.h>

static unsigned command(uint8_t *frame, unsigned length, uint8_t *reply) {
    iso15693_append_crc(frame, length);
    unsigned received = st25tb_reply(frame, length + 2, reply);
    assert(!received || iso15693_check_crc(reply, received));
    return received;
}

static void select_tag(uint8_t *frame, uint8_t *reply) {
    frame[0] = 0x0e;
    frame[1] = 0x5a;
    assert(command(frame, 2, reply) == 3 && reply[0] == 0x5a);
}

static void write(uint8_t *frame, uint8_t *reply, uint8_t address, const void *data) {
    frame[0] = 9;
    frame[1] = address;
    memcpy(frame + 2, data, 4);
    assert(!command(frame, 6, reply));
}

static void read(uint8_t *frame, uint8_t *reply, uint8_t address, const void *expected) {
    frame[0] = 8;
    frame[1] = address;
    assert(command(frame, 2, reply) == 6 && !memcmp(reply, expected, 4));
}

int main(void) {
    uint8_t image[77];
    uint8_t saved[77];
    uint8_t frame[8];
    uint8_t reply[10];
    assert(!st25tb_loaded() && !st25tb_save(saved));
    memset(image, 255, sizeof(image));
    image[5] = 0x33;
    image[6] = 0x02;
    image[7] = 0xd0;
    image[76] = 0x5a;
    assert(st25tb_load(image) && st25tb_loaded());
    assert(!st25tb_load((uint8_t[77]){0}));
    write(frame, reply, 1, "abcd"); /* Unselected writes are ignored. */
    assert(st25tb_save(saved) && !memcmp(saved, image, 77));

    frame[0] = 6;
    frame[1] = 0;
    assert(command(frame, 2, reply) == 3);
    frame[1] = 4;
    assert(!command(frame, 2, reply)); /* Chip_ID low nibble selects slot 10. */
    frame[0] = 0xa6;
    assert(command(frame, 1, reply) == 3);
    select_tag(frame, reply);
    frame[0] = 0x0b;
    assert(command(frame, 1, reply) == 10 && !memcmp(reply, image, 8));
    /* PM3 dumps re-initiate after UID discovery without a field reset. */
    for (unsigned run = 0; run < 10; run++) {
        frame[0] = 6;
        frame[1] = 0;
        assert(command(frame, 2, reply) == 3 && reply[0] == 0x5a);
        select_tag(frame, reply);
        for (unsigned address = 0; address < 17; address++) {
            read(frame, reply, address == 16 ? 255 : address, image + 8 + 4 * address);
        }
    }
    write(frame, reply, 1, "abcd");
    read(frame, reply, 1, "abcd");

    const uint8_t small[] = {100, 0, 0, 0};
    const uint8_t large[] = {200, 0, 0, 0};
    for (unsigned address = 5; address <= 6; address++) {
        write(frame, reply, address, small);
        write(frame, reply, address, large);
        read(frame, reply, address, small);
    }
    const uint8_t locks[] = {255, 255, 0xfd, 255};
    write(frame, reply, 255, locks);
    write(frame, reply, 1, "efgh"); /* Lock takes effect only at SELECT. */
    select_tag(frame, reply);
    write(frame, reply, 1, "ijkl");
    read(frame, reply, 1, "efgh");
    const uint8_t zero[] = {0, 0, 0, 0};
    const uint8_t ones[] = {255, 255, 255, 255};
    const uint8_t reserved[] = {0, 255, 0, 0};
    write(frame, reply, 255, zero);
    read(frame, reply, 255, reserved);
    write(frame, reply, 255, ones);
    read(frame, reply, 255, reserved); /* OTP/lock bits cannot be set again. */

    frame[0] = 0x0e;
    frame[1] = 0x59;
    assert(!command(frame, 2, reply));
    frame[0] = 6;
    frame[1] = 0;
    assert(!command(frame, 2, reply));
    select_tag(frame, reply); /* A deselected tag can be reselected. */
    frame[0] = 0x0c;
    assert(!command(frame, 1, reply));
    frame[0] = 8;
    frame[1] = 0;
    assert(!command(frame, 2, reply));
    select_tag(frame, reply);
    frame[0] = 0x0f;
    assert(!command(frame, 1, reply));
    frame[0] = 6;
    frame[1] = 0;
    assert(!command(frame, 2, reply));
    assert(st25tb_save(saved));
    st25tb_field_lost();
    assert(st25tb_loaded() && st25tb_save(image) && !memcmp(image, saved, sizeof(image)));
    frame[0] = 0x0e;
    frame[1] = 0x5a;
    assert(!command(frame, 2, reply));
    frame[0] = 8;
    frame[1] = 1;
    assert(!command(frame, 2, reply));
    frame[0] = 0x0b;
    assert(!command(frame, 1, reply));
    frame[0] = 6;
    frame[1] = 0;
    assert(command(frame, 2, reply) == 3);
    select_tag(frame, reply);
    read(frame, reply, 1, "efgh");
    read(frame, reply, 5, small);
    read(frame, reply, 255, reserved);
    write(frame, reply, 1, "mnop");
    read(frame, reply, 1, "efgh");
    frame[0] = 8;
    frame[1] = 16;
    assert(!command(frame, 2, reply)); /* Out-of-range reads are silent. */
    frame[0] = 0x0b;
    iso15693_append_crc(frame, 1);
    frame[2] ^= 1;
    assert(!st25tb_reply(frame, 3, reply));
    assert(!st25tb_reply(frame, 0, reply) && !st25tb_reply(frame, 9, reply));
    st25tb_unload();
    assert(!st25tb_loaded() && !st25tb_save(saved));
    assert(!command(frame, 1, reply));
}
