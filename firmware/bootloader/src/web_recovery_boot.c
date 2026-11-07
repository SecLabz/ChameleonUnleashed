/* Recovery ABI and authenticated handover to an unchanged Nordic bootloader. */
#include "web_update.h"
#include "crc32.h"
#include "nrf_dfu_settings.h"
#include "nrf_dfu_flash.h"
#include "nrf_dfu_validation.h"
#include "nrf_dfu_req_handler.h"
#include "nrf_bootloader_info.h"
#include "nrf_bootloader_wdt.h"
#include "nrf_crypto.h"
#include "nrf_nvmc.h"
#include <stddef.h>
#include <string.h>

__attribute__((section(".web_recovery_abi"), used))
const web_recovery_descriptor_t web_recovery_abi = {WEB_RECOVERY_BOOT_MAGIC, WEB_UPDATE_LAYOUT, 0};
STATIC_ASSERT(offsetof(nrf_dfu_settings_t, init_command) == WEB_DFU_INIT_PACKET_OFFSET);
STATIC_ASSERT(sizeof(web_install_journal_t) <= BOOTLOADER_SETTINGS_ADDRESS + CODE_PAGE_SIZE - WEB_INSTALL_JOURNAL);
static web_install_journal_t pending;
static bool pending_present;
const uint8_t *web_update_flash(uint32_t address) { return (const uint8_t *)address; }

bool web_recovery_present(void) {
    const uint8_t *footer = (const uint8_t *)WEB_RECOVERY_FOOTER;
    return !memcmp(footer, WEB_RECOVERY_MAGIC, sizeof(WEB_RECOVERY_MAGIC) - 1u) &&
        web_update_u32(footer + offsetof(web_recovery_descriptor_t, layout)) == WEB_UPDATE_LAYOUT;
}
bool web_recovery_boot_valid(void) {
    return web_recovery_present() &&
        crc32_compute((const uint8_t *)WEB_RECOVERY_START,
                      WEB_RECOVERY_FOOTER - WEB_RECOVERY_START, NULL) ==
        web_update_u32((const uint8_t *)WEB_RECOVERY_CRC);
}
uint32_t update_crc32(const uint8_t *bytes, size_t size) { return crc32_compute(bytes, size, NULL); }
static bool image_hash(uint32_t address, uint32_t size, uint32_t second, uint32_t second_size, const uint8_t expected[NORDIC_SHA256_BYTES]) {
    nrf_crypto_hash_context_t context = {0};
    uint8_t hash[NORDIC_SHA256_BYTES]; size_t hash_size = sizeof(hash);
    if (nrf_crypto_hash_init(&context, &g_nrf_crypto_hash_sha256_info)) return false;
    for (unsigned part = 0; part < 2; part++) {
        while (size) {
            uint32_t count = size > CODE_PAGE_SIZE ? CODE_PAGE_SIZE : size;
            if (nrf_crypto_hash_update(&context, (const uint8_t *)address, count)) return false;
            address += count; size -= count; nrf_bootloader_wdt_feed();
        }
        address = second; size = second_size;
    }
    return !nrf_crypto_hash_finalize(&context, hash, &hash_size) && !memcmp(hash, expected, sizeof(hash));
}
void web_recovery_pending_capture(void) {
    pending_present = *(const uint32_t *)WEB_INSTALL_JOURNAL != UINT32_MAX;
    if (pending_present) memcpy(&pending, (const void *)WEB_INSTALL_JOURNAL, sizeof(pending));
}
void web_recovery_pending_apply(void) {
    if (!pending_present) return;
    nordic_init_t app, system;
    bool valid = pending.magic == WEB_INSTALL_COMMITTED && pending.commit == WEB_INSTALL_COMMITTED && pending.wipe <= 1 &&
        pending.app_packet_size <= NORDIC_INIT_MAX && pending.system_packet_size <= NORDIC_INIT_MAX &&
        crc32_compute((const uint8_t *)&pending, offsetof(web_install_journal_t, crc), NULL) == pending.crc &&
        nordic_init_verify(pending.app, pending.app_packet_size, &app) &&
        nordic_init_verify(pending.system, pending.system_packet_size, &system) &&
        app.type == NORDIC_FW_APPLICATION && !app.sd_size && !app.bl_size &&
        app.app_size >= WEB_IMAGE_VECTOR_BYTES && app.app_size <= WEB_SYSTEM_STAGE - WEB_RECOVERY_START &&
        (system.type == NORDIC_FW_BOOTLOADER || system.type == NORDIC_FW_SOFTDEVICE_BOOTLOADER) &&
        system.bl_size >= WEB_IMAGE_VECTOR_BYTES && system.bl_size <= WEB_SYSTEM_MAX &&
        !system.app_size && system.sd_size <= WEB_RECOVERY_START - WEB_SOFTDEVICE_START &&
        ((system.type == NORDIC_FW_BOOTLOADER && !system.sd_size) ||
         (system.type == NORDIC_FW_SOFTDEVICE_BOOTLOADER && system.sd_size)) &&
        image_hash(WEB_RECOVERY_START, app.app_size, 0, 0, app.hash) &&
        image_hash(WEB_SOFTDEVICE_START, system.sd_size, WEB_SYSTEM_STAGE, system.bl_size, system.hash);
    if (valid) {
        if (pending.wipe) for (uint32_t address = WEB_UPDATE_END; address < BOOTLOADER_START_ADDR; address += CODE_PAGE_SIZE) {
            nrf_bootloader_wdt_feed();
            if (nrf_dfu_flash_erase(address, 1, NULL) != NRF_SUCCESS) return;
        }
        uint32_t crc = crc32_compute((const uint8_t *)WEB_RECOVERY_START, app.app_size, NULL);
        s_dfu_settings.app_version = app.version;
        s_dfu_settings.bootloader_version = system.version;
        s_dfu_settings.bank_current = NRF_DFU_CURRENT_BANK_0;
        s_dfu_settings.bank_0 = (nrf_dfu_bank_t){app.app_size, crc, NRF_DFU_BANK_VALID_APP};
        s_dfu_settings.bank_1 = (nrf_dfu_bank_t){system.bl_size,
            crc32_compute((const uint8_t *)WEB_SYSTEM_STAGE, system.bl_size, NULL), NRF_DFU_BANK_VALID_BL};
        memset(&s_dfu_settings.progress, 0, sizeof(s_dfu_settings.progress));
        s_dfu_settings.progress.update_start_address = WEB_SYSTEM_STAGE;
        s_dfu_settings.write_offset = 0;
        s_dfu_settings.boot_validation_app.type = VALIDATE_CRC;
        memcpy(s_dfu_settings.boot_validation_app.bytes, &crc, sizeof(crc));
        s_dfu_settings.boot_validation_bootloader.type = NO_VALIDATION;
    }
    /* The backup stays intact until the new settings have been committed. A
     * torn journal cannot arm a bootloader replacement. MBR owns the actual copy. */
    if (nrf_dfu_flash_erase(BOOTLOADER_SETTINGS_ADDRESS, 1, NULL) == NRF_SUCCESS)
        (void)nrf_dfu_settings_write_and_backup(NULL);
}
