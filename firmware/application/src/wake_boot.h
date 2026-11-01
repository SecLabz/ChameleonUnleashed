#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nrf_soc.h"
#include "nrf_bootloader_info.h"
static bool m_wake_boot_token_owned;

/* The stock bootloader consumes this once before validating its images. It
 * skips CRC validation (including an installed SD CRC), never SHA/signatures.
 * Do not overwrite another application's retained marker. No flash writes. */
static inline bool wake_boot_prepare_off(void) {
    uint32_t value;
    if (sd_power_gpregret_get(1, &value) != NRF_SUCCESS || value != 0) return false;
    if (sd_power_gpregret_set(1, BOOTLOADER_DFU_SKIP_CRC) != NRF_SUCCESS) return false;
    m_wake_boot_token_owned = true;
    return true;
}

static inline bool wake_boot_cancel_off(void) {
    if (!m_wake_boot_token_owned) return true;
    uint32_t value;
    if (sd_power_gpregret_get(1, &value) != NRF_SUCCESS) return false;
    if ((value & BOOTLOADER_DFU_SKIP_CRC_MASK) == BOOTLOADER_DFU_SKIP_CRC &&
        sd_power_gpregret_clr(1, BOOTLOADER_DFU_SKIP_CRC) != NRF_SUCCESS) return false;
    m_wake_boot_token_owned = false;
    return true;
}

/* An older bootloader may ignore the token. It must never masquerade as the
 * application's retained LF-wake bit when the application clears GPREGRET2. */
static inline uint32_t wake_boot_app_regret(uint32_t value) {
    if ((value & BOOTLOADER_DFU_SKIP_CRC_MASK) == BOOTLOADER_DFU_SKIP_CRC)
        value &= ~BOOTLOADER_DFU_SKIP_CRC;
    return value;
}
