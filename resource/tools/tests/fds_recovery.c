#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* SDK_PAGE_CONSTANTS */
#define FDS_VIRTUAL_PAGES 4u
#define FDS_VIRTUAL_PAGE_SIZE 8u
#define FDS_VIRTUAL_PAGES_RESERVED 1u
#define NRF_SUCCESS 0
_Alignas(32) static uint32_t flash[5][8];
static uint32_t saved[5][8];
#define BOOTLOADER_ADDRESS ((uintptr_t)(flash + 5))
static struct { unsigned CODEPAGESIZE; } ficr = {16};
#define NRF_FICR (&ficr)
static bool sd_enabled, fail_erase, fail_write;
static unsigned erases, writes;
static bool nrf_sdh_is_enabled(void) { return sd_enabled; }
static unsigned sd_flash_page_erase(uintptr_t number) {
    uint32_t *page = (uint32_t *)(number * ficr.CODEPAGESIZE);
    assert(page >= flash[0] && page < flash[4]);
    assert(page[0] == FDS_PAGE_TAG_MAGIC && page[1] == FDS_PAGE_TAG_DATA);
    for (unsigned i = 2; i < 8; i++) assert(page[i] == FDS_ERASED_WORD);
    erases++;
    if (fail_erase) return 1;
    memset(page, 255, ficr.CODEPAGESIZE);
    return 0;
}
static unsigned sd_flash_write(uint32_t *dest, const uint32_t *src, unsigned words) {
    assert(words == 2 && dest >= flash[0] && dest < flash[4]);
    assert(dest[0] == FDS_ERASED_WORD && dest[1] == FDS_ERASED_WORD);
    writes++;
    if (fail_write) return 1;
    memcpy(dest, src, words * sizeof(*src));
    return 0;
}
/* PRODUCTION_REPAIR */
typedef unsigned ret_code_t;
static struct { bool ignore_pm, waiting; } fds_operation_info;
static unsigned registrations, initializations;
static void fds_evt_handler(void) {}
static ret_code_t fds_register(void (*handler)(void)) {
    assert(handler == fds_evt_handler);
    registrations++;
    return 0;
}
static ret_code_t fds_init(void) {
    /* The SDK scan needs its swap before it fills the data-page array. */
    unsigned swaps = 0;
    for (unsigned i = 0; i < 4; i++)
        swaps += flash[i][0] == FDS_PAGE_TAG_MAGIC && flash[i][1] == FDS_PAGE_TAG_SWAP;
    assert(swaps == 1);
    initializations++;
    return 0;
}
#define APP_ERROR_CHECK(value) assert((value) == NRF_SUCCESS)
/* PRODUCTION_INIT */
static void reset(void) {
    memset(flash, 255, sizeof(flash));
    for (unsigned i = 0; i < 4; i++) {
        flash[i][0] = FDS_PAGE_TAG_MAGIC;
        flash[i][1] = FDS_PAGE_TAG_DATA;
    }
    /* Existing records, including a dirty record, must remain byte-identical. */
    flash[0][2] = 0x00010001;
    flash[0][3] = 0x12345678;
    flash[1][2] = 0x00010000;
    flash[1][3] = 0xabcdef01;
    flash[4][0] = 0x11223344; /* Reserved pages are never inspected or changed. */
    sd_enabled = fail_erase = fail_write = false;
    erases = writes = 0;
    memcpy(saved, flash, sizeof(saved));
}
int main(void) {
    reset();
    assert(fds_recover_empty_swap() && erases == 1 && writes == 1);
    assert(flash[3][0] == FDS_PAGE_TAG_MAGIC && flash[3][1] == FDS_PAGE_TAG_SWAP);
    assert(!memcmp(flash, saved, sizeof(flash[0]) * 3));
    assert(!memcmp(flash[4], saved[4], sizeof(flash[4])));
    assert(!fds_recover_empty_swap() && erases == 1); /* Existing swap. */
    reset();
    flash[0][0] = 0x3349f240; flash[1][0] = 0xf8b5002d;
    memcpy(saved, flash, sizeof(saved));
    assert(fds_recover_empty_swap() && erases == 1 && writes == 1);
    assert(!memcmp(flash, saved, sizeof(flash[0]) * 3)); /* Legacy DFU scratch left untouched. */
    assert(flash[3][1] == FDS_PAGE_TAG_SWAP);
    reset(); sd_enabled = true;
    assert(!fds_recover_empty_swap() && !erases && !writes);
    reset(); memset(flash[3], 255, sizeof(flash[3]));
    assert(!fds_recover_empty_swap() && !erases); /* SDK can create swap normally. */
    flash[3][0] = FDS_PAGE_TAG_MAGIC;
    assert(!fds_recover_empty_swap() && !erases); /* Interrupted tag write. */
    reset(); flash[2][2] = 0; flash[3][2] = 0;
    assert(!fds_recover_empty_swap() && !erases && !writes); /* No empty data page. */
    reset(); flash[2][0] = 0x12345678; flash[3][1] = 0x12345678;
    assert(!fds_recover_empty_swap() && !erases); /* Unknown pages. */
    reset(); flash[0][0] = flash[1][0] = flash[2][0] = 0;
    assert(!fds_recover_empty_swap() && !erases); /* Need another data page. */
    reset(); fail_erase = true;
    assert(!fds_recover_empty_swap() && erases == 1 && !writes);
    assert(!memcmp(flash, saved, sizeof(flash)));
    reset(); fail_write = true;
    assert(!fds_recover_empty_swap() && erases == 1 && writes == 1);
    assert(!memcmp(flash, saved, sizeof(flash[0]) * 3));
    for (unsigned i = 0; i < 8; i++) assert(flash[3][i] == FDS_ERASED_WORD);
    assert(!memcmp(flash[4], saved[4], sizeof(flash[4])));
    reset();
    fds_util_init();
    assert(registrations == 1 && initializations == 1 && erases == 1 && writes == 1);
    assert(fds_operation_info.ignore_pm && !fds_operation_info.waiting);
    assert(!memcmp(flash, saved, sizeof(flash[0]) * 3));
}
