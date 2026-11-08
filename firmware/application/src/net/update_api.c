#include "update_api.h"
#include "web_update.h"
#include "app_timer.h"
#include "app_error.h"
#include "app_util_platform.h"
#include "nrf_soc.h"
#include "nrf_pwr_mgmt.h"
#include "tag_emulation.h"
#include "app_status.h"
#include "usb_main.h"
#include "hf_sniff.h"
#include "nfca_session.h"
#include <stdio.h>
#include <string.h>

extern volatile bool m_is_field_on;
static bool entering;
static uint32_t requested_at;
const uint8_t *web_update_flash(uint32_t address) { return (const uint8_t *)address; }
bool web_update_entering(void) { return entering; }

static bool bootloader_present(void) {
    /* Status is a capability hint; explicit entry still verifies the full CRC. */
    const uint8_t *abi = web_update_flash(WEB_RECOVERY_BOOT_ABI);
    return !memcmp(abi, WEB_RECOVERY_BOOT_MAGIC, sizeof(WEB_RECOVERY_BOOT_MAGIC) - 1u) &&
        web_update_u32(abi + offsetof(web_recovery_descriptor_t, layout)) == WEB_UPDATE_LAYOUT;
}

static bool rf_busy(void) {
    return m_is_field_on || g_is_tag_emulating || usb_cdc_tearoff_running() ||
        hf_sniff_active() || nfca_session_active();
}

static bool pause_emulation(void) {
    bool busy;
    /* A field callback must not start an exchange between the check and pause. */
    CRITICAL_REGION_ENTER();
    busy = rf_busy();
    if (!busy) tag_emulation_sense_hold(true);
    CRITICAL_REGION_EXIT();
    return !busy;
}

api_response_t web_update_info(uint8_t *out, size_t capacity) {
    int length = snprintf((char *)out, capacity,
        "{\"mode\":\"application\",\"layout\":%u,\"version\":%lu,\"recovery_crc32\":%lu}",
        bootloader_present() ? WEB_UPDATE_LAYOUT : 0,
        (unsigned long)web_update_version(),
        (unsigned long)web_update_u32((const uint8_t *)(WEB_RECOVERY_CRC)));
    return (api_response_t){length > 0 && (size_t)length < capacity ? 200 : 500,
        "application/json", length > 0 && (size_t)length < capacity ? (size_t)length : 0};
}

api_response_t web_update_application_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *out, size_t capacity) {
    (void)body;
    if (!strcmp(method, "GET") && !strcmp(uri, "/api/update")) return web_update_info(out, capacity);
    if (!strcmp(method, "POST") && !strcmp(uri, "/api/update/enter")) {
        if (size) return (api_response_t){400, "application/octet-stream", 0};
        if (web_session_active() || !pause_emulation())
            return (api_response_t){409, "application/octet-stream", 0};
        /* Verify only on explicit entry, with sensing paused and IRQs enabled. */
        bool ready = web_update_bootloader_ready();
        tag_emulation_sense_hold(false);
        if (!ready) return (api_response_t){409, "application/octet-stream", 0};
        if (!entering) { requested_at = app_timer_cnt_get(); entering = true; }
        return (api_response_t){200, "application/octet-stream", 0};
    }
    return (api_response_t){404, "application/octet-stream", 0};
}

void web_update_poll(void) {
    if (!entering || app_timer_cnt_diff_compute(app_timer_cnt_get(), requested_at) < APP_TIMER_TICKS(WEB_UPDATE_RESTART_DELAY_MS)) return;
    if (!pause_emulation()) return;
    /* Stop emulation and persist its latest bytes before leaving the app. */
    tag_emulation_sense_end();
    tag_emulation_save();
    APP_ERROR_CHECK(sd_power_gpregret_clr(0, UINT32_MAX));
    APP_ERROR_CHECK(sd_power_gpregret_set(0, WEB_RECOVERY_REQUEST));
    nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_RESET);
}
