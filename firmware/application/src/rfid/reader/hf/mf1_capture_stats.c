#include "mf1_capture_stats.h"
#include "hex_utils.h"
#include "nrf.h"
#include <string.h>

mf1_capture_stats_t mf1_capture_stats;

void mf1_capture_stats_start(bool enabled) {
    memset(&mf1_capture_stats, 0, sizeof(mf1_capture_stats));
    mf1_capture_stats.enabled = enabled;
    if (!enabled) return;
    // Do not reset the shared counter or mask interrupts. Batch intervals are
    // shorter than its 67-second wrap period at the nRF52840's 64 MHz clock.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t mf1_capture_clock(void) {
    return mf1_capture_stats.enabled ? DWT->CYCCNT : 0;
}

uint32_t mf1_capture_elapsed(uint32_t started) {
    return mf1_capture_stats.enabled ? DWT->CYCCNT - started : 0;
}

void mf1_capture_stats_encode(uint8_t output[MF1_CAPTURE_STATS_BYTES]) {
    uint32_t cycles_per_us = SystemCoreClock / 1000000;
    uint32_t values[] = {
        1, // Profile format version.
        mf1_capture_stats.setup_cycles / cycles_per_us,
        mf1_capture_stats.select_cycles / cycles_per_us,
        mf1_capture_stats.auth_cycles / cycles_per_us,
        mf1_capture_stats.target_cycles / cycles_per_us,
        mf1_capture_stats.housekeeping_cycles / cycles_per_us,
        mf1_capture_stats.wait_cycles / cycles_per_us,
        mf1_capture_stats.timeouts,
        mf1_capture_stats.wakeups,
        mf1_capture_stats.observations,
    };
    for (uint8_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        num_to_bytes(values[index], 4, output + index * 4);
    }
}
