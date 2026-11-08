#include "web_update.h"
#include "nrf.h"
#include "nrf_nvmc.h"
#include "ocrypto_sha256.h"
#include "ocrypto_ecdsa_p256.h"
#include <string.h>

/* Hash erased metadata without putting a full flash page on the stack. */
#define HASH_PADDING_CHUNK_BYTES 64u

extern const uint8_t pk[NORDIC_ECDSA_P256_PUBLIC_KEY_BYTES];
const uint8_t *web_update_flash(uint32_t address) { return (const uint8_t *)address; }

static void feed(void) {
    if (NRF_WDT->RUNSTATUS) {
        for (unsigned i = 0; i < sizeof(NRF_WDT->RR) / sizeof(NRF_WDT->RR[0]); i++)
            if (NRF_WDT->RREN & (1u << i)) NRF_WDT->RR[i] = WDT_RR_RR_Reload;
    }
}

bool web_update_erase(uint32_t address) {
    if ((address & (WEB_UPDATE_PAGE - 1u)) ||
        address < WEB_RECOVERY_START || address >= WEB_UPDATE_END) return false;
    feed();
    nrf_nvmc_page_erase(address);
    return true;
}

bool web_update_store(uint32_t address, const uint8_t *data, size_t size) {
    uint32_t limit = address >= WEB_INSTALL_JOURNAL ? WEB_INSTALL_JOURNAL + sizeof(web_install_journal_t) : WEB_UPDATE_END;
    if ((address & WEB_FLASH_WORD_MASK) || address < WEB_RECOVERY_START ||
        (address >= WEB_UPDATE_END && address < WEB_INSTALL_JOURNAL) || address >= limit ||
        size > limit - address || ((size + WEB_FLASH_WORD_MASK) & ~WEB_FLASH_WORD_MASK) > limit - address) return false;
    for (size_t i = 0; i < size; i += sizeof(uint32_t)) {
        uint32_t word = UINT32_MAX;
        size_t count = size - i < sizeof(word) ? size - i : sizeof(word);
        memcpy(&word, data + i, count);
        nrf_nvmc_write_word(address + i, word);
    }
    feed();
    return !memcmp((const void *)address, data, size);
}

bool nordic_signature(const uint8_t *message, size_t size, const uint8_t signature[NORDIC_ECDSA_P256_SIGNATURE_BYTES]) {
    uint8_t public_key[NORDIC_ECDSA_P256_PUBLIC_KEY_BYTES], big_signature[NORDIC_ECDSA_P256_SIGNATURE_BYTES];
    /* Nordic's DFU key uses little-endian coordinates; Oberon uses big-endian. */
    for (unsigned i = 0; i < NORDIC_ECDSA_P256_COMPONENT_BYTES; i++) {
        public_key[i] = pk[NORDIC_ECDSA_P256_COMPONENT_BYTES - 1u - i];
        public_key[NORDIC_ECDSA_P256_COMPONENT_BYTES + i] = pk[NORDIC_ECDSA_P256_PUBLIC_KEY_BYTES - 1u - i];
        big_signature[i] = signature[NORDIC_ECDSA_P256_COMPONENT_BYTES - 1u - i];
        big_signature[NORDIC_ECDSA_P256_COMPONENT_BYTES + i] = signature[NORDIC_ECDSA_P256_SIGNATURE_BYTES - 1u - i];
    }
    feed();
    bool valid = ocrypto_ecdsa_p256_verify(big_signature, message, size, public_key) == 0;
    feed();
    return valid;
}

void web_update_image_hash(uint32_t size, const uint8_t *prefix, uint8_t hash[NORDIC_SHA256_BYTES]) {
    ocrypto_sha256_ctx context;
    ocrypto_sha256_init(&context);
    if (!prefix) prefix = (const uint8_t *)WEB_RECOVERY_START;
    ocrypto_sha256_update(&context, prefix, WEB_RECOVERY_END - WEB_RECOVERY_START);
    uint8_t padding[HASH_PADDING_CHUNK_BYTES]; memset(padding, WEB_FLASH_ERASED_BYTE, sizeof(padding));
    for (unsigned i = 0; i < WEB_UPDATE_PAGE / sizeof(padding); i++)
        ocrypto_sha256_update(&context, padding, sizeof(padding));
    uint32_t address = WEB_UPDATE_START;
    size -= WEB_UPDATE_START - WEB_RECOVERY_START;
    while (size) {
        size_t count = size > WEB_UPDATE_PAGE ? WEB_UPDATE_PAGE : size;
        ocrypto_sha256_update(&context, (const void *)address, count);
        address += count; size -= count; feed();
    }
    ocrypto_sha256_final(&context, hash);
}

void web_update_system_hash(uint32_t sd_size, uint32_t bl_size, uint8_t hash[NORDIC_SHA256_BYTES]) {
    ocrypto_sha256_ctx context;
    ocrypto_sha256_init(&context);
    for (uint32_t i = 0; i < sd_size; i += WEB_UPDATE_PAGE) {
        uint32_t count = sd_size - i > WEB_UPDATE_PAGE ? WEB_UPDATE_PAGE : sd_size - i;
        ocrypto_sha256_update(&context, (const void *)(WEB_SOFTDEVICE_START + i), count); feed();
    }
    ocrypto_sha256_update(&context, (const void *)WEB_SYSTEM_STAGE, bl_size);
    ocrypto_sha256_final(&context, hash); feed();
}

void web_update_hash(uint32_t address, size_t size, uint8_t hash[NORDIC_SHA256_BYTES]) {
    ocrypto_sha256_ctx context;
    ocrypto_sha256_init(&context);
    while (size) {
        size_t count = size > WEB_UPDATE_PAGE ? WEB_UPDATE_PAGE : size;
        ocrypto_sha256_update(&context, (const uint8_t *)address, count);
        address += count;
        size -= count;
        feed();
    }
    ocrypto_sha256_final(&context, hash);
}
