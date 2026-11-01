#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ISO15693_UID_SIZE 8u
#define ISO15693_CRC_SIZE 2u
#define ISO15693_FRAME_MAX_BYTES 64u
/* Full SLIX option read: status + 28 * (lock + 4 data) + CRC. */
#define ISO15693_RESPONSE_MAX_BYTES 143u

typedef struct {
    uint8_t bytes[ISO15693_FRAME_MAX_BYTES];
    uint16_t byte_count;
    bool crc_ok;
    bool high_data_rate;
    bool single_subcarrier;
} iso15693_frame_t;

/*
 * Calculates ISO15693 CRC over len bytes.
 */
uint16_t iso15693_crc16(const uint8_t *data, uint16_t len);

/*
 * Appends ISO15693 CRC after len payload bytes.
 */
void iso15693_append_crc(uint8_t *data, uint16_t len);

/*
 * Returns true when the final two bytes contain the expected CRC.
 */
bool iso15693_check_crc(const uint8_t *data, uint16_t len);

/*
 * Reverses the display-order UID into ISO15693 wire order.
 */
void iso15693_uid_to_wire(uint8_t *dst, const uint8_t *uid);

/*
 * Returns true when a wire-order UID matches a display-order UID.
 */
bool iso15693_wire_uid_matches(const uint8_t *wire_uid, const uint8_t *uid);

/*
 * Tests an inventory mask against a wire-order UID.
 */
bool iso15693_inventory_mask_matches(const uint8_t *wire_uid, uint8_t mask_bits,
                                     const uint8_t *mask_value, uint8_t mask_value_len);
