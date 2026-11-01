#include "frame.h"

#include <stddef.h>

static inline __attribute__((always_inline)) uint16_t crc16(const uint8_t *data, size_t length) {
    static const uint16_t table[16] = {
        0x0000u, 0x1081u, 0x2102u, 0x3183u, 0x4204u, 0x5285u, 0x6306u, 0x7387u,
        0x8408u, 0x9489u, 0xa50au, 0xb58bu, 0xc60cu, 0xd68du, 0xe70eu, 0xf78fu,
    };
    uint16_t crc = 0xffffu;
    for (size_t i = 0u; i < length; i++) {
        crc ^= data[i];
        crc = (uint16_t)((crc >> 4u) ^ table[crc & 0x0fu]);
        crc = (uint16_t)((crc >> 4u) ^ table[crc & 0x0fu]);
    }
    return (uint16_t)~crc;
}

uint16_t iso15693_crc16(const uint8_t *data, uint16_t len) {
    return crc16(data, len);
}

void iso15693_append_crc(uint8_t *data, uint16_t len) {
    const uint16_t crc = iso15693_crc16(data, len);
    data[len] = (uint8_t)crc;
    data[len + 1u] = (uint8_t)(crc >> 8u);
}

bool iso15693_check_crc(const uint8_t *data, uint16_t len) {
    if (data == NULL || len < ISO15693_CRC_SIZE)
        return false;
    const size_t payload_length = len - ISO15693_CRC_SIZE;
    const uint16_t crc = crc16(data, payload_length);
    return data[payload_length] == (uint8_t)crc &&
           data[payload_length + 1u] == (uint8_t)(crc >> 8u);
}

void iso15693_uid_to_wire(uint8_t *dst, const uint8_t *uid) {
    for (uint8_t i = 0u; i < ISO15693_UID_SIZE; i++) {
        dst[i] = uid[ISO15693_UID_SIZE - 1u - i];
    }
}

bool iso15693_wire_uid_matches(const uint8_t *wire_uid, const uint8_t *uid) {
    for (uint8_t i = 0u; i < ISO15693_UID_SIZE; i++) {
        if (wire_uid[i] != uid[ISO15693_UID_SIZE - 1u - i]) {
            return false;
        }
    }
    return true;
}

bool iso15693_inventory_mask_matches(const uint8_t *wire_uid, uint8_t mask_bits,
                                     const uint8_t *mask_value, uint8_t mask_value_len) {
    const uint8_t full_bytes = (uint8_t)(mask_bits / 8u);
    const uint8_t spare_bits = (uint8_t)(mask_bits % 8u);
    const uint8_t needed = (uint8_t)(full_bytes + (spare_bits != 0u ? 1u : 0u));

    if (mask_bits > 64u || mask_value_len < needed) {
        return false;
    }

    for (uint8_t i = 0u; i < full_bytes; i++) {
        if (mask_value[i] != wire_uid[i]) {
            return false;
        }
    }

    if (spare_bits != 0u) {
        const uint8_t mask = (uint8_t)((1u << spare_bits) - 1u);
        return (mask_value[full_bytes] & mask) == (wire_uid[full_bytes] & mask);
    }

    return true;
}
