#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NORDIC_INIT_MAX 512u
#define NORDIC_HW_ULTRA 0u
#define NORDIC_SD_S140_7_2_0 0x100u
#define NORDIC_SHA256_BYTES 32u
#define NORDIC_ECDSA_P256_COMPONENT_BYTES 32u
#define NORDIC_ECDSA_P256_SIGNATURE_BYTES (2u * NORDIC_ECDSA_P256_COMPONENT_BYTES)
#define NORDIC_ECDSA_P256_PUBLIC_KEY_BYTES (2u * NORDIC_ECDSA_P256_COMPONENT_BYTES)
enum {
    NORDIC_FW_APPLICATION = 0,
    NORDIC_FW_SOFTDEVICE = 1,
    NORDIC_FW_BOOTLOADER = 2,
    NORDIC_FW_SOFTDEVICE_BOOTLOADER = 3
};
/* IEEE CRC32, two nibble lookups per byte. The recovery dispatcher places its
 * own table in flash, before RAM code has been loaded. */
#define UPDATE_CRC32_TABLE { \
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, \
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu, \
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu, \
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu }
static inline __attribute__((always_inline))
uint32_t update_crc32_byte(uint32_t crc, uint8_t byte, const uint32_t table[16]) {
    crc ^= byte;
    crc = (crc >> 4) ^ table[crc & 15u];
    return (crc >> 4) ^ table[crc & 15u];
}
typedef struct {
    uint32_t type, version, hardware, sd_size, bl_size, app_size;
    uint8_t hash[NORDIC_SHA256_BYTES];
    const uint8_t *command, *signature;
    size_t command_size;
    bool softdevice_compatible;
} nordic_init_t;

/* Decode the original Nordic signed init packet, retaining the exact signed
 * command bytes. Signatures and SHA-256 fields use Nordic's little-endian wire order. */
size_t nordic_packet_size(const uint8_t *packet, size_t available);
bool nordic_init_decode(const uint8_t *packet, size_t size, nordic_init_t *info);
bool nordic_init_verify(const uint8_t *packet, size_t size, nordic_init_t *info);
bool nordic_signature(const uint8_t *message, size_t size, const uint8_t signature[NORDIC_ECDSA_P256_SIGNATURE_BYTES]);
uint32_t update_crc32(const uint8_t *bytes, size_t size);
