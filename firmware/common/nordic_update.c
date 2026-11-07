#include "nordic_update.h"
#include <string.h>

/* Field numbers and enum values from Nordic dfu-cc.proto. */
enum {
    WIRE_VARINT = 0, WIRE_BYTES = 2,
    PACKET_SIGNED_COMMAND = 2,
    SIGNED_COMMAND_COMMAND = 1, SIGNED_COMMAND_SIGNATURE_TYPE = 2, SIGNED_COMMAND_SIGNATURE = 3,
    COMMAND_OPCODE = 1, COMMAND_INIT = 2,
    INIT_FW_VERSION = 1, INIT_HW_VERSION = 2, INIT_SD_REQ = 3, INIT_TYPE = 4,
    INIT_SD_SIZE = 5, INIT_BL_SIZE = 6, INIT_APP_SIZE = 7, INIT_HASH = 8,
    INIT_DEBUG = 9, INIT_BOOT_VALIDATION = 10,
    HASH_TYPE = 1, HASH_BYTES = 2,
    OP_INIT = 1, SIGNATURE_ECDSA_P256_SHA256 = 0, HASH_SHA256 = 3
};

typedef struct { const uint8_t *p, *end; } reader_t;
static bool integer(reader_t *r, uint32_t *value) {
    *value = 0;
    for (unsigned shift = 0; shift < 35 && r->p < r->end; shift += 7) {
        uint8_t c = *r->p++;
        if (shift == 28 && (c & 0xf0)) return false;
        *value |= (uint32_t)(c & 127) << shift;
        if (!(c & 128)) return true;
    }
    return false;
}
static bool bytes(reader_t *r, reader_t *out) {
    uint32_t size;
    if (!integer(r, &size) || size > (size_t)(r->end - r->p)) return false;
    *out = (reader_t){r->p, r->p + size}; r->p += size;
    return true;
}
static bool field(reader_t *r, unsigned *number, unsigned *wire) {
    uint32_t tag;
    if (!integer(r, &tag) || !(tag >> 3)) return false;
    *number = tag >> 3; *wire = tag & 7;
    return *wire == WIRE_VARINT || *wire == WIRE_BYTES;
}
size_t nordic_packet_size(const uint8_t *packet, size_t available) {
    reader_t r = {packet, packet + available}, data;
    uint32_t tag;
    if (!integer(&r, &tag) || tag != ((PACKET_SIGNED_COMMAND << 3) | WIRE_BYTES) || !bytes(&r, &data)) return 0;
    return (size_t)(r.p - packet);
}
bool nordic_init_decode(const uint8_t *packet, size_t size, nordic_init_t *info) {
    if (!packet || !info || !size || size > NORDIC_INIT_MAX || nordic_packet_size(packet, size) != size) return false;
    memset(info, 0, sizeof(*info));
    reader_t outer = {packet, packet + size}, signed_command, command = {0}, signature = {0}, init = {0};
    uint32_t value; unsigned number, wire, seen = 0;
    if (!integer(&outer, &value) || !bytes(&outer, &signed_command)) return false;
    while (signed_command.p < signed_command.end) {
        if (!field(&signed_command, &number, &wire) || number > SIGNED_COMMAND_SIGNATURE || (seen & (1u << number))) return false;
        seen |= 1u << number;
        if (number == SIGNED_COMMAND_COMMAND && wire == WIRE_BYTES) { if (!bytes(&signed_command, &command)) return false; }
        else if (number == SIGNED_COMMAND_SIGNATURE_TYPE && wire == WIRE_VARINT) { if (!integer(&signed_command, &value) || value != SIGNATURE_ECDSA_P256_SHA256) return false; }
        else if (number == SIGNED_COMMAND_SIGNATURE && wire == WIRE_BYTES) { if (!bytes(&signed_command, &signature)) return false; }
        else return false;
    }
    if (seen != ((1u << SIGNED_COMMAND_COMMAND) | (1u << SIGNED_COMMAND_SIGNATURE_TYPE) | (1u << SIGNED_COMMAND_SIGNATURE)) ||
        (size_t)(signature.end - signature.p) != NORDIC_ECDSA_P256_SIGNATURE_BYTES) return false;
    info->signature = signature.p;
    seen = 0;
    while (command.p < command.end) {
        if (!field(&command, &number, &wire) || number > COMMAND_INIT || (seen & (1u << number))) return false;
        seen |= 1u << number;
        if (number == COMMAND_OPCODE && wire == WIRE_VARINT) { if (!integer(&command, &value) || value != OP_INIT) return false; }
        else if (number == COMMAND_INIT && wire == WIRE_BYTES) { if (!bytes(&command, &init)) return false; }
        else return false;
    }
    if (!(seen & (1u << COMMAND_INIT))) return false;
    info->command = init.p; info->command_size = (size_t)(init.end - init.p);
    seen = 0;
    while (init.p < init.end) {
        reader_t data;
        if (!field(&init, &number, &wire) || number > INIT_BOOT_VALIDATION || (number != INIT_SD_REQ && number != INIT_BOOT_VALIDATION && (seen & (1u << number)))) return false;
        seen |= 1u << number;
        if (wire == WIRE_VARINT) {
            if (!integer(&init, &value)) return false;
            switch (number) {
            case INIT_FW_VERSION: info->version = value; break;
            case INIT_HW_VERSION: info->hardware = value; break;
            case INIT_SD_REQ: info->softdevice_compatible |= value == NORDIC_SD_S140_7_2_0; break;
            case INIT_TYPE: info->type = value; break;
            case INIT_SD_SIZE: info->sd_size = value; break;
            case INIT_BL_SIZE: info->bl_size = value; break;
            case INIT_APP_SIZE: info->app_size = value; break;
            case INIT_DEBUG: if (value) return false; break; /* Never accept debug packets. */
            default: return false;
            }
        } else {
            if (!bytes(&init, &data)) return false;
            if (number == INIT_SD_REQ) {
                while (data.p < data.end) {
                    if (!integer(&data, &value)) return false;
                    info->softdevice_compatible |= value == NORDIC_SD_S140_7_2_0;
                }
            } else if (number == INIT_HASH) {
                unsigned hash_seen = 0;
                while (data.p < data.end) {
                    unsigned n, w; reader_t hash;
                    if (!field(&data, &n, &w) || n > HASH_BYTES || (hash_seen & (1u << n))) return false;
                    hash_seen |= 1u << n;
                    if (n == HASH_TYPE && w == WIRE_VARINT) { if (!integer(&data, &value) || value != HASH_SHA256) return false; }
                    else if (n == HASH_BYTES && w == WIRE_BYTES) {
                        if (!bytes(&data, &hash) || (size_t)(hash.end - hash.p) != sizeof(info->hash)) return false;
                        for (unsigned i = 0; i < sizeof(info->hash); i++) info->hash[i] = hash.p[sizeof(info->hash) - 1u - i];
                    } else return false;
                }
                if (hash_seen != ((1u << HASH_TYPE) | (1u << HASH_BYTES))) return false;
            } else if (number != INIT_BOOT_VALIDATION) return false; /* Boot validation remains covered by the signature. */
        }
    }
    const unsigned required = (1u << INIT_FW_VERSION) | (1u << INIT_HW_VERSION) | (1u << INIT_TYPE) | (1u << INIT_HASH);
    return (seen & required) == required &&
           info->hardware == NORDIC_HW_ULTRA && info->softdevice_compatible && info->type <= NORDIC_FW_SOFTDEVICE_BOOTLOADER;
}
bool nordic_init_verify(const uint8_t *packet, size_t size, nordic_init_t *info) {
    return nordic_init_decode(packet, size, info) && nordic_signature(info->command, info->command_size, info->signature);
}
uint32_t update_crc32(const uint8_t *bytes, size_t size) {
    static const uint32_t table[16] = UPDATE_CRC32_TABLE;
    uint32_t crc = UINT32_MAX;
    while (size--) crc = update_crc32_byte(crc, *bytes++, table);
    return ~crc;
}
