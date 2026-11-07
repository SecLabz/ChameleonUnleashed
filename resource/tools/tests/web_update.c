#include "web_update.h"
#include "update-fixtures.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX hash_context;
#define HASH_INIT CC_SHA256_Init
#define HASH_UPDATE CC_SHA256_Update
#define HASH_FINAL CC_SHA256_Final
#else
#include <openssl/sha.h>
typedef SHA256_CTX hash_context;
#define HASH_INIT SHA256_Init
#define HASH_UPDATE SHA256_Update
#define HASH_FINAL SHA256_Final
#endif

static uint8_t flash[0x100000], baseline[0x100000];
static unsigned operations, fail_at;
static bool cut;
static uint32_t crc_before;
static bool operation(void) {
    operations++;
    if (fail_at && operations == fail_at) { cut = true; return false; }
    return true;
}
const uint8_t *web_update_flash(uint32_t address) {
    assert(address < sizeof(flash)); return flash + address;
}
bool web_update_erase(uint32_t address) {
    assert(address >= WEB_RECOVERY_START && address < WEB_UPDATE_END && !(address & 4095));
    if (!operation()) { memset(flash + address, 255, 2048); return false; }
    memset(flash + address, 255, 4096); return true;
}
bool web_update_store(uint32_t address, const uint8_t *data, size_t size) {
    assert(!(address & 3));
    assert((address >= WEB_RECOVERY_START && address + ((size + 3) & ~3u) <= WEB_UPDATE_END) ||
           (address >= WEB_INSTALL_JOURNAL && address + ((size + 3) & ~3u) <= WEB_INSTALL_JOURNAL + sizeof(web_install_journal_t)));
    for (size_t i = 0; i < size; i += 4) {
        if (!operation()) return false;
        for (size_t j = 0; j < 4; j++) {
            uint8_t byte = i + j < size ? data[i + j] : 255;
            assert((flash[address + i + j] & byte) == byte);
            flash[address + i + j] &= byte;
        }
    }
    return true;
}
static bool known_signature(const uint8_t *packet, size_t size, const uint8_t *message, size_t message_size, const uint8_t *signature) {
    nordic_init_t info;
    return nordic_init_decode(packet, size, &info) && info.command_size == message_size &&
        !memcmp(info.command, message, message_size) && !memcmp(info.signature, signature, 64);
}
bool nordic_signature(const uint8_t *message, size_t size, const uint8_t signature[64]) {
    /* Cryptographic primitives are independently checked with Python and on ARM.
     * Here only the exact signed fixture messages are trusted by the flash model. */
#define KNOWN(name) known_signature(name##_packet, sizeof(name##_packet), message, size, signature)
    return KNOWN(old) || KNOWN(normal) || KNOWN(changed) || KNOWN(legacy) || KNOWN(system);
#undef KNOWN
}
void web_update_hash(uint32_t address, size_t size, uint8_t hash[32]) {
    assert(address + size <= sizeof(flash));
    hash_context context; HASH_INIT(&context); HASH_UPDATE(&context, flash + address, size); HASH_FINAL(hash, &context);
}
void web_update_image_hash(uint32_t size, const uint8_t *prefix, uint8_t hash[32]) {
    hash_context context; HASH_INIT(&context);
    if (!prefix) prefix = flash + WEB_RECOVERY_START;
    HASH_UPDATE(&context, prefix, WEB_RECOVERY_END - WEB_RECOVERY_START);
    uint8_t padding[4096]; memset(padding, 255, sizeof(padding));
    HASH_UPDATE(&context, padding, sizeof(padding));
    HASH_UPDATE(&context, flash + WEB_UPDATE_START, size - (WEB_UPDATE_START - WEB_RECOVERY_START));
    HASH_FINAL(hash, &context);
}
void web_update_system_hash(uint32_t sd_size, uint32_t bl_size, uint8_t hash[32]) {
    hash_context context; HASH_INIT(&context);
    HASH_UPDATE(&context, flash + 0x1000, sd_size);
    HASH_UPDATE(&context, flash + WEB_SYSTEM_STAGE, bl_size);
    HASH_FINAL(hash, &context);
}
static size_t plan(uint8_t out[1024], const uint8_t *packet, size_t size, bool restore, bool wipe) {
    size_t system_size = restore ? sizeof(system_packet) : 0;
    memset(out, 0, 8); out[0] = restore | (wipe ? 2 : 0);
    out[2] = size; out[3] = size >> 8; out[4] = system_size; out[5] = system_size >> 8;
    memcpy(out + 8, packet, size);
    if (restore) memcpy(out + 8 + size, system_packet, system_size);
    return 8 + size + system_size;
}
static void reset_flash(unsigned failure) {
    memcpy(flash, baseline, sizeof(flash)); operations = 0; fail_at = failure; cut = false;
}
static unsigned install(const uint8_t *payload, size_t size, const uint8_t *packet, size_t packet_size, bool restore) {
    uint8_t body[1024]; size_t body_size = plan(body, packet, packet_size, restore, false);
    unsigned result = web_update_begin(body, body_size);
    if (result != 200) return result;
    uint32_t id = web_update_session();
    if (restore) for (size_t offset = 0; offset < sizeof(system_payload); offset += 1024) {
        size_t count = sizeof(system_payload) - offset < 1024 ? sizeof(system_payload) - offset : 1024;
        result = web_update_system_write(id, offset, system_payload + offset, count);
        if (result != 200) return result;
    }
    for (size_t offset = 0; offset < size; offset += 1024) {
        size_t count = size - offset < 1024 ? size - offset : 1024;
        result = web_update_write(id, offset, payload + offset, count);
        if (result != 200) return result;
    }
    return web_update_commit(id);
}
static void protected_regions_unchanged(void) {
    assert(!memcmp(flash, baseline, WEB_RECOVERY_START));
    assert(!memcmp(flash + WEB_UPDATE_END, baseline + WEB_UPDATE_END, 0xff800 - WEB_UPDATE_END));
    assert(!memcmp(flash + WEB_INSTALL_JOURNAL + sizeof(web_install_journal_t),
                   baseline + WEB_INSTALL_JOURNAL + sizeof(web_install_journal_t),
                   sizeof(flash) - WEB_INSTALL_JOURNAL - sizeof(web_install_journal_t)));
}
static bool journal_valid(void) {
    const web_install_journal_t *journal = (const void *)(flash + WEB_INSTALL_JOURNAL);
    return journal->magic == WEB_INSTALL_COMMITTED && journal->commit == WEB_INSTALL_COMMITTED &&
        update_crc32((const uint8_t *)journal, offsetof(web_install_journal_t, crc)) == journal->crc;
}
int main(int argc, char **argv) {
    (void)argv;
    assert(!web_update_startup_pending(0xa9)); /* Nordic's post-update CRC-skip token. */
    assert(!web_update_startup_pending(0xa8));
    assert(!web_update_startup_pending(2));    /* RFID application's full-restart flag. */
    for (unsigned value = 0; value < 4; value++) assert(web_update_startup_pending(WEB_RECOVERY_STARTING | value));
    memset(baseline, 0xa5, sizeof(baseline));
    memset(baseline + WEB_RECOVERY_START, 255, WEB_UPDATE_END - WEB_RECOVERY_START);
    memcpy(baseline + WEB_RECOVERY_START, old_payload, sizeof(old_payload));
    memcpy(baseline + 0x1000, system_payload, 516);
    memset(baseline + WEB_INSTALL_JOURNAL, 255, sizeof(web_install_journal_t));
    memcpy(baseline + WEB_RECOVERY_BOOT_ABI, "CUWEBBL2\2\0\0\0", 12);
    uint32_t boot_crc = update_crc32(baseline + 0xf3000, WEB_RECOVERY_BOOT_ABI - 0xf3000u);
    memcpy(baseline + WEB_RECOVERY_BOOT_ABI + 12, &boot_crc, 4);
    if (argc > 1) {
        memcpy(flash, baseline, sizeof(flash)); flash[0xf3100] ^= 1;
        assert(!web_update_bootloader_ready());
        puts("Stale recovery ABI on an upstream bootloader correctly rejected.");
        return 0;
    }
    web_update_cache_t cache = {.magic = WEB_UPDATE_COMMITTED, .app_size = sizeof(old_payload), .packet_size = sizeof(old_packet)};
    memcpy(cache.packet, old_packet, sizeof(old_packet)); cache.crc = update_crc32((const uint8_t *)&cache, sizeof(cache));
    memcpy(baseline + WEB_UPDATE_METADATA, &cache, sizeof(cache));
    uint32_t magic = WEB_UPDATE_COMMITTED;
    memcpy(baseline + WEB_UPDATE_COMMIT, &magic, 4);
    reset_flash(0);
    assert(web_update_bootloader_ready());
    assert(web_update_application_valid() && web_update_cache_valid());
    assert(web_update_cache_committed());
    assert(web_update_version() == 0x1000000);
    flash[WEB_UPDATE_METADATA + 12] ^= 1;
    assert(web_update_cache_committed() && !web_update_cache_valid());
    /* The bootloader only checks commit; the RF dispatcher must check CRC. */
    reset_flash(0);

    /* A classic DFU BIN contains an erased cache page. Bootstrap must use the
     * retained original SDK init packet, authenticate it, and commit last. */
    memset(flash + WEB_UPDATE_METADATA, 255, WEB_UPDATE_PAGE);
    memset(flash + 0xff000u + 92u, 255, NORDIC_INIT_MAX);
    assert(!web_update_recover_installation() && operations == 0);
    memcpy(flash + 0xff000u + 92u, normal_packet, sizeof(normal_packet));
    assert(!web_update_recover_installation() && operations == 0); /* Wrong image hash. */
    memcpy(flash + 0xff000u + 92u, old_packet, sizeof(old_packet));
    assert(web_update_recover_installation());
    unsigned bootstrap_operations = operations;
    assert(web_update_application_valid() && web_update_version() == 0x1000000);
    assert(!memcmp(flash + 0xff000u + 92u, old_packet, sizeof(old_packet)));
    for (unsigned failure = 1; failure <= bootstrap_operations; failure++) {
        reset_flash(failure);
        memset(flash + WEB_UPDATE_METADATA, 255, WEB_UPDATE_PAGE);
        memcpy(flash + 0xff000u + 92u, old_packet, sizeof(old_packet));
        assert(!web_update_recover_installation() && cut);
        assert(!web_update_cache_valid());
        assert(!web_update_cache_committed());
    }
    reset_flash(0);
    crc_before = update_crc32(flash + WEB_RECOVERY_START, WEB_RECOVERY_FOOTER - WEB_RECOVERY_START);

    assert(install(normal_payload, sizeof(normal_payload), normal_packet, sizeof(normal_packet), false) == 200);
    unsigned normal_operations = operations;
    assert(web_update_application_valid() && web_update_version() == 0x1000001);
    assert(web_update_commit(web_update_session()) == 200);
    protected_regions_unchanged();
    for (unsigned failure = 1; failure <= normal_operations; failure++) {
        reset_flash(failure);
        assert(install(normal_payload, sizeof(normal_payload), normal_packet, sizeof(normal_packet), false) != 200 && cut);
        assert(!web_update_cache_valid());
        assert(!memcmp(flash + WEB_RECOVERY_START, baseline + WEB_RECOVERY_START, WEB_RECOVERY_END - WEB_RECOVERY_START));
        assert(update_crc32(flash + WEB_RECOVERY_START, WEB_RECOVERY_FOOTER - WEB_RECOVERY_START) == crc_before);
        protected_regions_unchanged();
    }

    reset_flash(0);
    assert(install(legacy_payload, sizeof(legacy_payload), legacy_packet, sizeof(legacy_packet), true) == 200);
    unsigned restore_operations = operations;
    assert(journal_valid() && !web_update_cache_valid());
    const web_install_journal_t *journal = (const void *)(flash + WEB_INSTALL_JOURNAL);
    assert(journal->app_packet_size == sizeof(legacy_packet) && journal->system_packet_size == sizeof(system_packet) && !journal->wipe);
    assert(!memcmp(journal->app, legacy_packet, sizeof(legacy_packet)));
    assert(!memcmp(journal->system, system_packet, sizeof(system_packet)));
    for (unsigned failure = 1; failure <= restore_operations; failure++) {
        reset_flash(failure);
        assert(install(legacy_payload, sizeof(legacy_payload), legacy_packet, sizeof(legacy_packet), true) != 200 && cut);
        assert(!journal_valid());
        protected_regions_unchanged(); /* MBR + DFU survive every cut, including after recovery is erased. */
    }

    reset_flash(0);
    assert(install(changed_payload, sizeof(changed_payload), changed_packet, sizeof(changed_packet), false) == 200);
    unsigned changed_operations = operations;
    assert(web_update_application_valid());
    /* Probe both sides of each erase/write boundary during supervisor replacement. */
    for (unsigned failure = 1; failure <= changed_operations; failure += 127) {
        reset_flash(failure);
        assert(install(changed_payload, sizeof(changed_payload), changed_packet, sizeof(changed_packet), false) != 200 && cut);
        assert(!web_update_cache_valid()); protected_regions_unchanged();
    }

    reset_flash(0);
    uint8_t body[1024]; size_t size = plan(body, normal_packet, sizeof(normal_packet), false, false);
    body[size - 1] ^= 1;
    assert(web_update_begin(body, size) == 422 && operations == 0);
    size = plan(body, normal_packet, sizeof(normal_packet), false, false);
    body[6] = 1;
    assert(web_update_begin(body, size) == 422 && operations == 0);
    size = plan(body, normal_packet, sizeof(normal_packet), false, false);
    assert(web_update_begin(body, size) == 200);
    uint32_t id = web_update_session();
    assert(web_update_write(id, 0, normal_payload, 1024) == 200);
    assert(web_update_write(id, 0, normal_payload, 1024) == 200);
    uint8_t bad[1024]; memcpy(bad, normal_payload, 1024); bad[0] ^= 1;
    assert(web_update_write(id, 0, bad, 1024) == 409);
    assert(web_update_write(id - 1, 1024, normal_payload + 1024, 1024) == 409);
    assert(web_update_commit(id) == 409);
    reset_flash(0);
    size = plan(body, legacy_packet, sizeof(legacy_packet), true, true);
    assert(web_update_begin(body, size) == 200);
    id = web_update_session();
    assert(web_update_write(id, 0, legacy_payload, 1024) == 409); /* System must be verified first. */
    memcpy(bad, system_payload, 1024); bad[0] ^= 1;
    unsigned before = operations;
    assert(web_update_system_write(id, 0, bad, 1024) == 422 && operations == before);
    protected_regions_unchanged();
    printf("Nordic installer: %u application cuts, %u restore cuts, supervisor boundaries, signatures and retries passed.\n", normal_operations, restore_operations);
    return 0;
}
