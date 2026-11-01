/* Keep the SDK unchanged. Only the clock configuration passed to its SVC is
 * selected here; all SoftDevice observers and initialization keep their order. */
#include "nrf.h"
#include "nrf_sdm.h"

enum { NFC_RC_CALIBRATION_INTERVAL = 16, NFC_RC_TEMPERATURE_INTERVAL = 2 };

static uint32_t field_clock_softdevice_enable(nrf_clock_lf_cfg_t const *config,
                                             nrf_fault_handler_t handler) {
    /* No retained token and no effect on button/VBUS/cold starts. The
     * SoftDevice owns RC calibration; its nRF52 recommendation guarantees
     * calibration at least every 8 s and checks temperature every 4 s.
     * System OFF still stops the oscillator entirely. */
    nrf_clock_lf_cfg_t rc;
    if (config && NRF_POWER->RESETREAS == POWER_RESETREAS_NFC_Msk) {
        rc = *config;
        rc.source = NRF_CLOCK_LF_SRC_RC;
        rc.rc_ctiv = NFC_RC_CALIBRATION_INTERVAL;
        rc.rc_temp_ctiv = NFC_RC_TEMPERATURE_INTERVAL;
        rc.accuracy = NRF_CLOCK_LF_ACCURACY_500_PPM;
        config = &rc;
    }
    return sd_softdevice_enable(config, handler);
}
#define sd_softdevice_enable field_clock_softdevice_enable
#include "nrf_sdh.c"
#undef sd_softdevice_enable
