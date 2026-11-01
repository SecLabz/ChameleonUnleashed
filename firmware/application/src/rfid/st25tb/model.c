/* SPDX-License-Identifier: GPL-3.0-only */
/* ST25TB512-AT model.
 * RF writes affect RAM only. Counter and lock behavior follows ST25TB512-AT. */
#include "model.h"
#include "iso15693/frame.h"
#include <string.h>

typedef enum { READY, INVENTORY, SELECTED, DESELECTED, DEACTIVATED } state_t;
static uint8_t image[ST25TB_IMAGE_SIZE];
static state_t state;
static uint16_t active_locks;
static bool loaded;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool st25tb_loaded(void) { return loaded; }
void st25tb_unload(void) { loaded = false; state = READY; }
void st25tb_field_lost(void) { state = READY; }
bool st25tb_save(uint8_t data[ST25TB_IMAGE_SIZE]) {
    if (!loaded) return false;
    memcpy(data, image, sizeof(image));
    return true;
}
bool st25tb_load(const uint8_t data[ST25TB_IMAGE_SIZE]) {
    /* Product 0x33 is the 512-bit AT variant; other models need other rules. */
    if (!data || data[7] != 0xd0 || data[6] != 0x02 || data[5] != 0x33)
        return false;
    memcpy(image, data, sizeof(image));
    state = READY;
    active_locks = read32(image + 72) >> 16;
    loaded = true;
    return true;
}
static void write_block(unsigned address, const uint8_t *data) {
    uint32_t value = read32(data);
    if (address < 16 && (active_locks & (1u << address))) {
        uint8_t *block = image + 8 + 4 * address;
        if ((address != 5 && address != 6) || value < read32(block))
            memcpy(block, data, 4);
        return;
    }
    if (address != 255) return;
    /* Factory-reserved bits 15..8 stay unchanged; OTP and locks only clear. */
    value = read32(image + 72) & (value | 0x0000ff00u);
    for (unsigned b = 0; b < 4; b++) image[72 + b] = value >> (8 * b);
}
unsigned st25tb_reply(const uint8_t *frame, unsigned length,
                      uint8_t reply[ST25TB_RESPONSE_MAX]) {
    /* CRC-B and ISO15693 share the same CRC parameters and wire order. */
    if (!loaded || length < 3 || length > ST25TB_REQUEST_MAX ||
            !iso15693_check_crc(frame, length)) return 0;
    unsigned n = 0, payload = length - 2;
    uint8_t command = frame[0], chip = image[76];
    if (payload == 2 && command == 6) {
        if (frame[1] == 0 && (state == READY || state == INVENTORY || state == SELECTED)) {
            state = INVENTORY;
            reply[n++] = chip;
        } else if (frame[1] == 4 && state == INVENTORY && !(chip & 15)) {
            reply[n++] = chip;
        }
    } else if (payload == 1 && (command & 15) == 6 && state == INVENTORY) {
        if ((command >> 4) == (chip & 15)) reply[n++] = chip;
    } else if (payload == 2 && command == 0x0e) {
        if (frame[1] == chip && (state == INVENTORY || state == SELECTED || state == DESELECTED)) {
            state = SELECTED;
            active_locks = read32(image + 72) >> 16;
            reply[n++] = chip;
        } else if (state == SELECTED && frame[1] != chip) {
            state = DESELECTED;
        }
    } else if (state == SELECTED) {
        if (payload == 1 && command == 0x0b) {
            memcpy(reply, image, 8);
            n = 8;
        } else if (payload == 2 && command == 8) {
            unsigned address = frame[1];
            if (address < 16 || address == 255) {
                memcpy(reply, image + 8 + (address == 255 ? 16 : address) * 4, 4);
                n = 4;
            }
        } else if (payload == 1 && command == 0x0c) {
            state = INVENTORY;
        } else if (payload == 1 && command == 0x0f) {
            state = DEACTIVATED;
        } else if (payload == 6 && command == 9) {
            write_block(frame[1], frame + 2);
        }
    }
    if (!n) return 0;
    iso15693_append_crc(reply, n);
    return n + 2;
}
