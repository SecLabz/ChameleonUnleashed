/* Only this small dispatcher executes from flash. The installer, its constants,
 * interrupt handlers and vectors execute from RAM before any flash is changed. */
#include "web_update.h"
#include "recovery.h"
#include "nrf.h"
#include "nrf_sdm.h"

#define BOOT __attribute__((section(".boot.text"), noinline))
uint32_t recovery_vectors[RECOVERY_VECTOR_COUNT]
    __attribute__((aligned(RECOVERY_VECTOR_ALIGNMENT), section(".bss.recovery_vectors")));
extern uint32_t __ram_text_load, __ram_text_start, __ram_text_end;
extern uint32_t __data_load, __data_start__, __data_end__, __bss_start__, __bss_end__;
extern void SystemInit(void);
extern int main(void);
static const uint32_t crc_table[16] __attribute__((section(".boot.rodata"))) = UPDATE_CRC32_TABLE;

static BOOT bool cached_rf_image(void) {
    const web_update_cache_t *cache = (const void *)WEB_UPDATE_METADATA;
    if (*(const uint32_t *)WEB_UPDATE_COMMIT != WEB_UPDATE_COMMITTED || cache->magic != WEB_UPDATE_COMMITTED ||
        !cache->packet_size || cache->packet_size > NORDIC_INIT_MAX || cache->app_size > WEB_UPDATE_MAX ||
        cache->app_size < WEB_UPDATE_START - WEB_RECOVERY_START + WEB_IMAGE_VECTOR_BYTES) return false;
    uint32_t sp = *(const uint32_t *)WEB_UPDATE_START, pc = *(const uint32_t *)(WEB_UPDATE_START + sizeof(uint32_t));
    if (sp <= WEB_RAM_START || sp > WEB_RAM_END || (sp & (WEB_STACK_ALIGNMENT - 1u)) || !(pc & 1u) ||
        (pc & ~1u) < WEB_UPDATE_START || (pc & ~1u) >= WEB_RECOVERY_START + cache->app_size) return false;
    uint32_t crc = UINT32_MAX;
    const uint8_t *p = (const uint8_t *)cache;
    for (unsigned i = 0; i < sizeof(*cache); i++) {
        crc = update_crc32_byte(crc, i >= offsetof(web_update_cache_t, crc) && i < offsetof(web_update_cache_t, crc) + sizeof(cache->crc) ? 0 : p[i], crc_table);
    }
    return ~crc == cache->crc;
}
static __attribute__((naked, section(".boot.text"))) uint32_t vectors_set(uint32_t address __attribute__((unused))) {
    __asm volatile("svc %0\n bx lr" :: "I"(SD_SOFTDEVICE_VECTOR_TABLE_BASE_SET));
}
static BOOT void fast_start(void) {
    for (unsigned i = 0; i < sizeof(NRF_ACL->ACL) / sizeof(NRF_ACL->ACL[0]); i++) if (!NRF_ACL->ACL[i].SIZE) {
        NRF_ACL->ACL[i].ADDR = WEB_RECOVERY_START;
        NRF_ACL->ACL[i].SIZE = WEB_UPDATE_END - WEB_RECOVERY_START;
        NRF_ACL->ACL[i].PERM = ACL_ACL_PERM_WRITE_Disable << ACL_ACL_PERM_WRITE_Pos;
        SCB->VTOR = 0;
        if (vectors_set(WEB_UPDATE_START)) return;
        uint32_t sp = *(const uint32_t *)WEB_UPDATE_START, pc = *(const uint32_t *)(WEB_UPDATE_START + sizeof(uint32_t));
        __asm volatile("msr msp, %0\n bx %1" :: "r"(sp), "r"(pc) : "memory");
        __builtin_unreachable();
    }
}
BOOT void recovery_boot_entry(void) {
    uint32_t reason = NRF_POWER->RESETREAS;
    if ((reason & (POWER_RESETREAS_NFC_Msk | POWER_RESETREAS_LPCOMP_Msk)) &&
        !(reason & (POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk | POWER_RESETREAS_SREQ_Msk)) &&
        !(NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) &&
        NRF_POWER->GPREGRET != WEB_RECOVERY_REQUEST && cached_rf_image()) fast_start();

    NRF_POWER->GPREGRET2 = (NRF_POWER->GPREGRET2 & WEB_RECOVERY_APP_STATE_MASK) | WEB_RECOVERY_STARTING;
    volatile uint32_t *destination = &__ram_text_start;
    const volatile uint32_t *source = &__ram_text_load;
    while (destination < &__ram_text_end) *destination++ = *source++;
    destination = &__data_start__; source = &__data_load;
    while (destination < &__data_end__) *destination++ = *source++;
    destination = &__bss_start__;
    while (destination < &__bss_end__) *destination++ = 0;
    for (unsigned i = 0; i < RECOVERY_VECTOR_COUNT; i++) recovery_vectors[i] = ((const uint32_t *)WEB_RECOVERY_START)[i];
    __DSB(); __ISB();
    SystemInit();
    main();
    for (;;) {}
}
