/* One SoftDevice HFXO request is shared by USB, LF and NFC. Future
 * clients must join this manager rather than issue raw SVCs or maintain
 * a second independent software reference count. */
#include "hfclk.h"
#include "nrf_soc.h"
#include "nrf_nvic.h"
#include "nrf_delay.h"
static volatile uint8_t owners;
enum { HFCLK_START_TIMEOUT_US = 10000, HFCLK_POLL_INTERVAL_US = 10 };

uint8_t app_hfclk_owners(void) { return owners; }

static uint32_t change(uint8_t owner, uint8_t acquire) {
    if (owner != HFCLK_USB && owner != HFCLK_LF && owner != HFCLK_NFC) return NRF_ERROR_INVALID_PARAM;
    uint8_t nested;
    uint32_t result = sd_nvic_critical_region_enter(&nested);
    if (result) return result;
    uint8_t next = acquire ? owners | owner : owners & ~owner;
    if (!owners && next) result = sd_clock_hfclk_request();
    if (owners && !next) result = sd_clock_hfclk_release();
    if (!result) owners = next;
    uint32_t exit_result = sd_nvic_critical_region_exit(nested);
    return result ? result : exit_result;
}

uint32_t app_hfclk_acquire(uint8_t owner) { return change(owner, 1); }
uint32_t app_hfclk_release(uint8_t owner) { return change(owner, 0); }

uint32_t app_hfclk_wait(void) {
    /* Wait outside the ownership critical region; no unbounded clock wait. */
    for (unsigned i = 0; i < HFCLK_START_TIMEOUT_US / HFCLK_POLL_INTERVAL_US; i++) {
        uint32_t running;
        uint32_t result = sd_clock_hfclk_is_running(&running);
        if (result || running) return result;
        nrf_delay_us(HFCLK_POLL_INTERVAL_US);
    }
    return NRF_ERROR_TIMEOUT;
}
