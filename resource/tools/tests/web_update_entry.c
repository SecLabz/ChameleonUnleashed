#include "api.h"
#include "web_update.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The production RTC uses a wrapping 24-bit counter. */
#define RTC_COUNTER_MASK UINT32_C(0xffffff)
#define APP_TIMER_TICKS(ms) ((ms) * 16384u / 1000u)
#define NRF_PWR_MGMT_SHUTDOWN_RESET 1
#define APP_ERROR_CHECK(result) assert((result) == 0)
#define CRITICAL_REGION_ENTER() critical_enter()
#define CRITICAL_REGION_EXIT() critical_exit()

static bool entering, held, critical, inject_field;
static bool valid, session, tearoff, sniff, nfca;
static web_recovery_descriptor_t boot_abi;
static volatile bool m_is_field_on;
static bool g_is_tag_emulating;
static uint32_t requested_at, ticks, retention;
static unsigned verifications, pauses, stops, saves, resets, clock_reads;

static void critical_enter(void) {
    assert(!critical);
    /* Model an RF ISR arriving just before interrupts are masked. */
    if (inject_field) { g_is_tag_emulating = true; inject_field = false; }
    critical = true;
}
static void critical_exit(void) { assert(critical); critical = false; }
bool web_session_active(void) { return session || entering; }
static bool usb_cdc_tearoff_running(void) { return tearoff; }
static bool hf_sniff_active(void) { return sniff; }
static bool nfca_session_active(void) { return nfca; }
static uint32_t app_timer_cnt_get(void) { clock_reads++; return ticks; }
static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return (now - before) & RTC_COUNTER_MASK;
}
static void tag_emulation_sense_hold(bool hold) {
    if (hold) {
        assert(critical && !g_is_tag_emulating && !m_is_field_on);
        pauses++;
    } else {
        assert(!critical);
    }
    held = hold;
}
const uint8_t *web_update_flash(uint32_t address) {
    assert(address == WEB_RECOVERY_BOOT_ABI);
    return (const uint8_t *)&boot_abi;
}
bool web_update_bootloader_ready(void) {
    /* Long verification must never run with RF listening or IRQs masked. */
    assert(held && !critical);
    assert(!g_is_tag_emulating && !m_is_field_on && !tearoff && !sniff && !nfca);
    verifications++;
    return valid;
}
uint32_t web_update_version(void) { return 42; }
uint32_t web_update_u32(const uint8_t *address) {
    if (address == (const uint8_t *)WEB_RECOVERY_CRC) return 123;
    assert(address == (const uint8_t *)&boot_abi.layout);
    return boot_abi.layout;
}
static void tag_emulation_sense_end(void) { assert(held && !critical); stops++; }
static void tag_emulation_save(void) { assert(stops == 1 && held && !critical); saves++; }
static int sd_power_gpregret_clr(unsigned index, uint32_t mask) {
    assert(index == 0 && saves == 1); retention &= ~mask; return 0;
}
static int sd_power_gpregret_set(unsigned index, uint32_t value) {
    assert(index == 0); retention |= value; return 0;
}
static void nrf_pwr_mgmt_shutdown(int action) {
    assert(action == NRF_PWR_MGMT_SHUTDOWN_RESET && retention == WEB_RECOVERY_REQUEST);
    assert(saves == 1 && held && !critical); resets++;
}

/* PRODUCTION_ENTRY */

static void reset(void) {
    entering = held = critical = inject_field = false;
    session = tearoff = sniff = nfca = m_is_field_on = g_is_tag_emulating = false;
    memcpy(boot_abi.magic, WEB_RECOVERY_BOOT_MAGIC, sizeof(boot_abi.magic));
    boot_abi.layout = WEB_UPDATE_LAYOUT;
    valid = true;
    requested_at = ticks = retention = 0;
    verifications = pauses = stops = saves = resets = clock_reads = 0;
}
static unsigned request(const char *method, const char *uri, size_t size) {
    uint8_t output[256];
    api_response_t response = web_update_application_request(method, uri, NULL, size, output, sizeof(output));
    if (!strcmp(method, "GET") && response.status == 200) {
        assert(strstr((const char *)output, bootloader_present() ? "\"layout\":2" : "\"layout\":0"));
    }
    return response.status;
}
static unsigned enter(void) { return request("POST", "/api/update/enter", 0); }

int main(void) {
    reset();
    for (unsigned i = 0; i < 1000; i++) web_update_poll();
    assert(!clock_reads && !pauses && !verifications);
    g_is_tag_emulating = true;
    assert(request("GET", "/api/update", 0) == 200);
    boot_abi.magic[0] ^= 1;
    assert(!bootloader_present());
    assert(request("GET", "/api/update", 0) == 200);
    boot_abi.magic[0] ^= 1;
    boot_abi.layout++;
    assert(!bootloader_present());
    assert(request("GET", "/api/update", 0) == 200);
    assert(!pauses && !verifications && g_is_tag_emulating);

    for (unsigned busy = 0; busy < 7; busy++) {
        reset();
        switch (busy) {
            case 0: session = true; break;
            case 1: m_is_field_on = true; break;
            case 2: g_is_tag_emulating = true; break;
            case 3: tearoff = true; break;
            case 4: sniff = true; break;
            case 5: nfca = true; break;
            case 6: inject_field = true; break;
        }
        assert(enter() == 409);
        assert(!web_update_entering() && !verifications && !pauses && !held && !resets);
    }

    reset();
    valid = false; /* Includes a stale, superficially valid bootloader footer. */
    assert(enter() == 409);
    assert(verifications == 1 && !held && !web_update_entering());
    reset();
    assert(request("POST", "/api/update/enter", 1) == 400);
    assert(!verifications && !pauses);

    for (unsigned busy = 0; busy < 6; busy++) {
        reset();
        ticks = RTC_COUNTER_MASK - APP_TIMER_TICKS(WEB_UPDATE_RESTART_DELAY_MS) / 2;
        assert(enter() == 200 && web_update_entering());
        assert(verifications == 1 && pauses == 1 && !held);
        assert(enter() == 409 && verifications == 1); /* No duplicate transition. */
        ticks = (requested_at + APP_TIMER_TICKS(WEB_UPDATE_RESTART_DELAY_MS) - 1) & RTC_COUNTER_MASK;
        web_update_poll();
        assert(!stops && !resets && !held);
        ticks = (ticks + 1) & RTC_COUNTER_MASK;
        switch (busy) {
            case 0: m_is_field_on = true; break;
            case 1: g_is_tag_emulating = true; break;
            case 2: tearoff = true; break;
            case 3: sniff = true; break;
            case 4: nfca = true; break;
            case 5: inject_field = true; break;
        }
        web_update_poll();
        assert(!stops && !saves && !resets && !held && pauses == 1);
        m_is_field_on = g_is_tag_emulating = tearoff = sniff = nfca = false;
        web_update_poll();
        assert(stops == 1 && saves == 1 && resets == 1 && pauses == 2);
    }
    puts("Recovery entry preserves RF exchanges and verifies only on explicit entry.");
}
