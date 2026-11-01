/* Wrap the pinned TinyUSB driver so USB and LF share SoftDevice clock ownership. */
#include "nrf_soc.h"
#include "nrf_drv_power.h"
#include "app_error.h"
#include "utils/hfclk.h"
static uint32_t usb_clock_request(void) {
    uint32_t result = app_hfclk_acquire(HFCLK_USB);
    APP_ERROR_CHECK(result);
    return result;
}
static uint32_t usb_clock_release(void) {
    uint32_t result = app_hfclk_release(HFCLK_USB);
    APP_ERROR_CHECK(result);
    return result;
}
#define sd_clock_hfclk_request usb_clock_request
#define sd_clock_hfclk_release usb_clock_release
#define tusb_hal_nrf_power_event nordic_power_event
#include "portable/nordic/nrf5x/dcd_nrf5x.c"
#undef tusb_hal_nrf_power_event
#undef sd_clock_hfclk_release
#undef sd_clock_hfclk_request

void tusb_hal_nrf_power_event(uint32_t event) {
    /* Acquisition must not depend on the oscillator already running for LF
     * or BLE. The upstream helper otherwise skips this ownership entirely. */
    if (event == NRF_DRV_POWER_USB_EVT_DETECTED && usb_clock_request() != NRF_SUCCESS) return;
    if (event == NRF_DRV_POWER_USB_EVT_READY && (!(app_hfclk_owners() & HFCLK_USB) || !NRF_USBD->ENABLE)) return;
    nordic_power_event(event);
    /* Also covers repeated REMOVED with an already-disabled controller. */
    if (event == NRF_DRV_POWER_USB_EVT_REMOVED && !NRF_USBD->ENABLE) (void)usb_clock_release();
}
