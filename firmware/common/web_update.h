/* Layout 2: standard Nordic signed packets; no custom firmware file format. */
#pragma once
#include "nordic_update.h"
#define WEB_FLASH_ERASED_BYTE UINT8_MAX
#define WEB_SOFTDEVICE_START 0x1000u
#define WEB_BOOTLOADER_START 0xf3000u
#define WEB_BOOTLOADER_END 0xfe000u
#define WEB_DFU_SETTINGS_START 0xff000u
/* Nordic SDK settings ABI; the bootloader checks this against offsetof(). */
#define WEB_DFU_INIT_PACKET_OFFSET 92u
#define WEB_RAM_START 0x20000000u
#define WEB_RAM_END 0x20040000u
#define WEB_STACK_ALIGNMENT 8u
#define WEB_FLASH_WORD_BYTES sizeof(uint32_t)
#define WEB_FLASH_WORD_MASK (WEB_FLASH_WORD_BYTES - 1u)
#define WEB_IMAGE_VECTOR_BYTES (2u * sizeof(uint32_t))
#define WEB_RECOVERY_MAGIC "CURECOV2"
#define WEB_RECOVERY_BOOT_MAGIC "CUWEBBL2"
typedef struct {
    char magic[8];
    uint32_t layout, crc;
} web_recovery_descriptor_t;
#define WEB_RECOVERY_START 0x27000u
#define WEB_RECOVERY_END 0x3b000u
#define WEB_RECOVERY_FOOTER (WEB_RECOVERY_END - sizeof(web_recovery_descriptor_t))
#define WEB_RECOVERY_CRC (WEB_RECOVERY_FOOTER + offsetof(web_recovery_descriptor_t, crc))
#define WEB_UPDATE_METADATA 0x3b000u
#define WEB_UPDATE_START 0x3c000u
#define WEB_UPDATE_END 0xc7000u
#define WEB_UPDATE_MAX (WEB_UPDATE_END - WEB_RECOVERY_START)
#define WEB_UPDATE_PAGE 4096u
#define WEB_UPDATE_COMMIT (WEB_UPDATE_METADATA + WEB_UPDATE_PAGE - sizeof(uint32_t))
#define WEB_UPDATE_COMMITTED 0x43555732u
#define WEB_UPDATE_LAYOUT 2u
#define WEB_RECOVERY_REQUEST 0x40u
#define WEB_RECOVERY_STARTING 0x5cu /* GPREGRET2: distinct from Nordic's 0xa9 CRC token. */
#define WEB_RECOVERY_STARTING_MASK 0xfcu
#define WEB_RECOVERY_APP_STATE_MASK 0x03u
#define WEB_RECOVERY_BOOT_ABI 0xfdff0u
#define WEB_SYSTEM_STAGE 0xbc000u
#define WEB_SYSTEM_MAX (WEB_BOOTLOADER_END - WEB_BOOTLOADER_START)
#define WEB_INSTALL_JOURNAL 0xff800u
#define WEB_INSTALL_COMMITTED 0x43555232u
#define WEB_UPDATE_CHUNK_MAX 1024u
#define WEB_UPDATE_BEGIN_SIZE 8u
#define WEB_UPDATE_BEGIN_RESTORE 0x01u
#define WEB_UPDATE_BEGIN_WIPE 0x02u
#define WEB_UPDATE_RESTART_DELAY_MS 500u
#define WEB_CACHE_SIZE sizeof(web_update_cache_t)
static inline bool web_update_startup_pending(uint32_t value) {
    return (value & WEB_RECOVERY_STARTING_MASK) == WEB_RECOVERY_STARTING;
}

typedef struct {
    uint32_t magic, app_size, packet_size, crc;
    uint8_t packet[NORDIC_INIT_MAX];
} web_update_cache_t;
typedef struct {
    uint32_t magic, app_packet_size, system_packet_size, wipe;
    uint8_t app[NORDIC_INIT_MAX], system[NORDIC_INIT_MAX];
    uint32_t crc, commit;
} web_install_journal_t;

uint32_t web_update_u32(const uint8_t *p);
bool web_update_bootloader_ready(void);
bool web_update_cache_committed(void);
bool web_update_cache_valid(void);
bool web_update_application_valid(void);
bool web_update_recover_installation(void);
uint32_t web_update_version(void);
unsigned web_update_begin(const uint8_t *header, size_t size);
unsigned web_update_write(uint32_t session, uint32_t offset, const uint8_t *data, size_t size);
unsigned web_update_system_write(uint32_t session, uint32_t offset, const uint8_t *data, size_t size);
unsigned web_update_commit(uint32_t session);
uint32_t web_update_session(void);
uint32_t web_update_received(void);
uint32_t web_update_size(void);
bool web_update_active(void);
bool web_update_restoring(void);
const uint8_t *web_update_flash(uint32_t address);
bool web_update_erase(uint32_t address);
bool web_update_store(uint32_t address, const uint8_t *data, size_t size);
void web_update_hash(uint32_t address, size_t size, uint8_t hash[NORDIC_SHA256_BYTES]);
void web_update_image_hash(uint32_t size, const uint8_t *prefix, uint8_t hash[NORDIC_SHA256_BYTES]);
void web_update_system_hash(uint32_t sd_size, uint32_t bl_size, uint8_t hash[NORDIC_SHA256_BYTES]);
