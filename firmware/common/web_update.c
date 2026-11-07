#include "web_update.h"
#include <stddef.h>
#include <string.h>

static uint32_t session, received, system_received;
static bool active, restoring, wipe, complete;
static nordic_init_t app, system_info;
static uint8_t app_packet[NORDIC_INIT_MAX], system_packet[NORDIC_INIT_MAX];
static size_t app_packet_size, system_packet_size;
/* Kept in RAM until the signed image is fully verified. Recovery is replaced
 * last, only when its bytes actually changed. Ordinary app-only updates never
 * erase the independent supervisor. */
static uint8_t prefix[WEB_RECOVERY_END - WEB_RECOVERY_START];

uint32_t web_update_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool web_update_bootloader_ready(void) {
    static bool checked, ready;
    if (checked) return ready;
    const uint8_t *abi = web_update_flash(WEB_RECOVERY_BOOT_ABI);
    /* Short upstream bootloaders can leave our old footer behind. Bind the ABI
     * to the actual bootloader bytes before offering any web flash operation. */
    ready = !memcmp(abi, WEB_RECOVERY_BOOT_MAGIC, sizeof(WEB_RECOVERY_BOOT_MAGIC) - 1u) &&
        web_update_u32(abi + offsetof(web_recovery_descriptor_t, layout)) == WEB_UPDATE_LAYOUT &&
        update_crc32(web_update_flash(WEB_BOOTLOADER_START), WEB_RECOVERY_BOOT_ABI - WEB_BOOTLOADER_START) ==
        web_update_u32(abi + offsetof(web_recovery_descriptor_t, crc));
    checked = true;
    return ready;
}
static bool vectors(uint32_t address, uint32_t start, uint32_t size) {
    const uint8_t *p = web_update_flash(address);
    uint32_t sp = web_update_u32(p), pc = web_update_u32(p + sizeof(uint32_t));
    return sp > WEB_RAM_START && sp <= WEB_RAM_END && !(sp & (WEB_STACK_ALIGNMENT - 1u)) && (pc & 1u) &&
           (pc & ~1u) >= start && (pc & ~1u) < start + size;
}
static bool app_bounds(const nordic_init_t *info, bool legacy) {
    return info->type == NORDIC_FW_APPLICATION && !info->sd_size && !info->bl_size &&
           info->app_size >= (legacy ? WEB_IMAGE_VECTOR_BYTES : WEB_UPDATE_START - WEB_RECOVERY_START + WEB_IMAGE_VECTOR_BYTES) &&
           info->app_size <= (legacy ? WEB_SYSTEM_STAGE - WEB_RECOVERY_START : WEB_UPDATE_MAX);
}
bool web_update_cache_committed(void) {
    const web_update_cache_t *cache = (const void *)web_update_flash(WEB_UPDATE_METADATA);
    return web_update_u32(web_update_flash(WEB_UPDATE_COMMIT)) == WEB_UPDATE_COMMITTED &&
        cache->magic == WEB_UPDATE_COMMITTED && cache->packet_size && cache->packet_size <= NORDIC_INIT_MAX &&
        cache->app_size >= WEB_UPDATE_START - WEB_RECOVERY_START + WEB_IMAGE_VECTOR_BYTES && cache->app_size <= WEB_UPDATE_MAX;
}
bool web_update_cache_valid(void) {
    if (!web_update_cache_committed()) return false;
    const web_update_cache_t *cache = (const void *)web_update_flash(WEB_UPDATE_METADATA);
    web_update_cache_t copy;
    memcpy(&copy, cache, sizeof(copy)); copy.crc = 0;
    return update_crc32((const uint8_t *)&copy, sizeof(copy)) == cache->crc &&
           vectors(WEB_UPDATE_START, WEB_UPDATE_START,
                   cache->app_size - (WEB_UPDATE_START - WEB_RECOVERY_START));
}
uint32_t web_update_version(void) {
    const web_update_cache_t *cache = (const void *)web_update_flash(WEB_UPDATE_METADATA);
    nordic_init_t info;
    return web_update_cache_valid() && nordic_init_decode(cache->packet, cache->packet_size, &info) ? info.version : 0;
}
bool web_update_application_valid(void) {
    if (!web_update_cache_valid()) return false;
    const web_update_cache_t *cache = (const void *)web_update_flash(WEB_UPDATE_METADATA);
    nordic_init_t info; uint8_t hash[NORDIC_SHA256_BYTES];
    if (!nordic_init_verify(cache->packet, cache->packet_size, &info) || !app_bounds(&info, false) || info.app_size != cache->app_size) return false;
    web_update_image_hash(info.app_size, NULL, hash);
    return !memcmp(hash, info.hash, sizeof(hash));
}
static bool cache_save(const uint8_t *packet, size_t size, uint32_t image_size) {
    web_update_cache_t cache = {.magic = WEB_UPDATE_COMMITTED, .app_size = image_size, .packet_size = size};
    memcpy(cache.packet, packet, size);
    cache.crc = update_crc32((const uint8_t *)&cache, sizeof(cache));
    return web_update_erase(WEB_UPDATE_METADATA) &&
           web_update_store(WEB_UPDATE_METADATA, (const uint8_t *)&cache, sizeof(cache)) &&
           web_update_store(WEB_UPDATE_COMMIT, (const uint8_t *)&cache.magic, 4);
}
bool web_update_recover_installation(void) {
    /* Our DFU bootloader retains the original signed init command in settings
     * after resetting transfer progress. A first boot builds the protected cache. */
    const uint8_t *packet = web_update_flash(WEB_DFU_SETTINGS_START + WEB_DFU_INIT_PACKET_OFFSET);
    size_t size = nordic_packet_size(packet, NORDIC_INIT_MAX);
    nordic_init_t info; uint8_t hash[NORDIC_SHA256_BYTES];
    if (!size || !nordic_init_verify(packet, size, &info) || !app_bounds(&info, false)) return false;
    web_update_image_hash(info.app_size, NULL, hash);
    return !memcmp(hash, info.hash, sizeof(hash)) && cache_save(packet, size, info.app_size);
}
unsigned web_update_begin(const uint8_t *body, size_t size) {
    if (size < WEB_UPDATE_BEGIN_SIZE || (body[0] & ~(WEB_UPDATE_BEGIN_RESTORE | WEB_UPDATE_BEGIN_WIPE)) ||
        body[1] || body[6] || body[7]) return 422;
    size_t a = body[2] | (size_t)body[3] << 8, s = body[4] | (size_t)body[5] << 8;
    bool restore = body[0] & WEB_UPDATE_BEGIN_RESTORE;
    nordic_init_t ai, si;
    if (!a || a > NORDIC_INIT_MAX || s > NORDIC_INIT_MAX || size != WEB_UPDATE_BEGIN_SIZE + a + s ||
        !nordic_init_verify(body + WEB_UPDATE_BEGIN_SIZE, a, &ai) || !app_bounds(&ai, restore) ||
        (restore != (s != 0)) || (!restore && (body[0] & WEB_UPDATE_BEGIN_WIPE))) return 422;
    if (restore && (!nordic_init_verify(body + WEB_UPDATE_BEGIN_SIZE + a, s, &si) ||
        (si.type != NORDIC_FW_BOOTLOADER && si.type != NORDIC_FW_SOFTDEVICE_BOOTLOADER) ||
        si.bl_size < WEB_IMAGE_VECTOR_BYTES || si.bl_size > WEB_SYSTEM_MAX ||
        si.sd_size > WEB_RECOVERY_START - WEB_SOFTDEVICE_START || si.app_size ||
        (si.type == NORDIC_FW_BOOTLOADER && si.sd_size) || (si.type == NORDIC_FW_SOFTDEVICE_BOOTLOADER && !si.sd_size))) return 422;
    if (restore) {
        const uint8_t *journal = web_update_flash(WEB_INSTALL_JOURNAL);
        for (size_t i = 0; i < sizeof(web_install_journal_t); i++) if (journal[i] != WEB_FLASH_ERASED_BYTE) return 409;
    }
    active = false; complete = false;
    if (!web_update_erase(WEB_UPDATE_METADATA)) return 500;
    memcpy(app_packet, body + WEB_UPDATE_BEGIN_SIZE, a); app_packet_size = a;
    if (!nordic_init_verify(app_packet, a, &app)) return 422;
    system_packet_size = s;
    if (restore) {
        memcpy(system_packet, body + WEB_UPDATE_BEGIN_SIZE + a, s);
        if (!nordic_init_verify(system_packet, s, &system_info)) return 422;
    }
    received = system_received = 0; restoring = restore; wipe = body[0] & WEB_UPDATE_BEGIN_WIPE;
    if (!++session) ++session;
    active = true;
    return 200;
}
static bool chunk_valid(uint32_t offset, size_t size, uint32_t maximum) {
    return size && size <= WEB_UPDATE_CHUNK_MAX && !(offset & WEB_FLASH_WORD_MASK) && offset <= maximum && size <= maximum - offset &&
           (!(size & WEB_FLASH_WORD_MASK) || offset + size == maximum) && offset % WEB_UPDATE_PAGE + size <= WEB_UPDATE_PAGE;
}
unsigned web_update_system_write(uint32_t id, uint32_t offset, const uint8_t *data, size_t size) {
    if (!active || !restoring || id != session || received) return 409;
    uint32_t total = system_info.sd_size + system_info.bl_size;
    if (!chunk_valid(offset, size, total) || offset > system_received) return 400;
    bool duplicate = offset < system_received;
    if (duplicate && size > system_received - offset) return 409;
    for (size_t i = 0; i < size; ) {
        uint32_t position = offset + i;
        size_t count = size - i;
        if (position < system_info.sd_size) {
            if (count > system_info.sd_size - position) count = system_info.sd_size - position;
            /* A complete upstream package carries S140. Reuse it byte-for-byte;
             * never erase MBR or a running SoftDevice from the web installer. */
            if (memcmp(web_update_flash(WEB_SOFTDEVICE_START + position), data + i, count)) return 422;
        } else {
            uint32_t address = WEB_SYSTEM_STAGE + position - system_info.sd_size;
            if (count > WEB_UPDATE_PAGE - address % WEB_UPDATE_PAGE) count = WEB_UPDATE_PAGE - address % WEB_UPDATE_PAGE;
            if (duplicate) { if (memcmp(web_update_flash(address), data + i, count)) return 409; }
            else if ((!(address % WEB_UPDATE_PAGE) && !web_update_erase(address)) ||
                     !web_update_store(address, data + i, count)) return 500;
        }
        i += count;
    }
    if (!duplicate) system_received += size;
    return 200;
}
unsigned web_update_write(uint32_t id, uint32_t offset, const uint8_t *data, size_t size) {
    if (!active || id != session) return 409;
    if (!chunk_valid(offset, size, app.app_size) || offset > received) return 400;
    if (restoring) {
        if (system_received != system_info.sd_size + system_info.bl_size) return 409;
        if (!received) {
            uint8_t hash[NORDIC_SHA256_BYTES]; web_update_system_hash(system_info.sd_size, system_info.bl_size, hash);
            if (memcmp(hash, system_info.hash, sizeof(hash)) || !vectors(WEB_SYSTEM_STAGE, WEB_BOOTLOADER_START, system_info.bl_size)) return 422;
        }
    }
    uint32_t address = WEB_RECOVERY_START + offset;
    bool duplicate = offset < received;
    if (duplicate && size > received - offset) return 409;
    if (!restoring && address < WEB_RECOVERY_END) {
        if (address + size > WEB_RECOVERY_END) return 400;
        if (duplicate && memcmp(prefix + offset, data, size)) return 409;
        if (!duplicate) memcpy(prefix + offset, data, size);
    } else if (!restoring && address < WEB_UPDATE_START) {
        if (address + size > WEB_UPDATE_START) return 400;
        for (size_t i = 0; i < size; i++) if (data[i] != WEB_FLASH_ERASED_BYTE) return 422;
    } else if (duplicate) {
        if (memcmp(web_update_flash(address), data, size)) return 409;
    } else if ((!(address % WEB_UPDATE_PAGE) && !web_update_erase(address)) ||
               !web_update_store(address, data, size)) return 500;
    if (!duplicate) received += size;
    return 200;
}
unsigned web_update_commit(uint32_t id) {
    if (complete && id == session) return 200;
    if (!active || id != session || received != app.app_size) return 409;
    uint8_t hash[NORDIC_SHA256_BYTES];
    if (restoring) web_update_hash(WEB_RECOVERY_START, app.app_size, hash);
    else web_update_image_hash(app.app_size, prefix, hash);
    if (memcmp(hash, app.hash, sizeof(hash))) return 422;
    if (restoring) {
        if (!vectors(WEB_RECOVERY_START, WEB_RECOVERY_START, app.app_size)) return 422;
        web_install_journal_t journal = {.magic = WEB_INSTALL_COMMITTED, .app_packet_size = app_packet_size,
            .system_packet_size = system_packet_size, .wipe = wipe};
        memcpy(journal.app, app_packet, app_packet_size); memcpy(journal.system, system_packet, system_packet_size);
        journal.crc = update_crc32((const uint8_t *)&journal, offsetof(web_install_journal_t, crc));
        if (!web_update_store(WEB_INSTALL_JOURNAL, (const uint8_t *)&journal, offsetof(web_install_journal_t, commit)) ||
            !web_update_store(WEB_INSTALL_JOURNAL + offsetof(web_install_journal_t, commit), (const uint8_t *)&journal.magic, sizeof(journal.magic))) return 500;
    } else {
        uint32_t pc = web_update_u32(prefix + sizeof(uint32_t));
        const uint8_t *footer = prefix + sizeof(prefix) - sizeof(web_recovery_descriptor_t);
        if (!(pc & 1) || (pc & ~1u) < WEB_RECOVERY_START || (pc & ~1u) >= WEB_RECOVERY_END ||
            memcmp(footer, WEB_RECOVERY_MAGIC, sizeof(WEB_RECOVERY_MAGIC) - 1u) ||
            web_update_u32(footer + offsetof(web_recovery_descriptor_t, layout)) != WEB_UPDATE_LAYOUT ||
            update_crc32(prefix, sizeof(prefix) - sizeof(web_recovery_descriptor_t)) !=
                web_update_u32(footer + offsetof(web_recovery_descriptor_t, crc)) ||
            !vectors(WEB_UPDATE_START, WEB_UPDATE_START, app.app_size - (WEB_UPDATE_START - WEB_RECOVERY_START))) return 422;
        for (size_t offset = 0; offset < sizeof(prefix); offset += WEB_UPDATE_PAGE) {
            if (!memcmp(web_update_flash(WEB_RECOVERY_START + offset), prefix + offset, WEB_UPDATE_PAGE)) continue;
            if (!web_update_erase(WEB_RECOVERY_START + offset) ||
                !web_update_store(WEB_RECOVERY_START + offset, prefix + offset, WEB_UPDATE_PAGE)) return 500;
        }
        if (!cache_save(app_packet, app_packet_size, app.app_size)) return 500;
    }
    active = false; complete = true;
    return 200;
}
uint32_t web_update_session(void) { return session; }
uint32_t web_update_received(void) { return received; }
uint32_t web_update_size(void) { return app.app_size; }
bool web_update_active(void) { return active; }
bool web_update_restoring(void) { return restoring; }
