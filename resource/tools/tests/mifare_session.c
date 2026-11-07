#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_status.h"
#include "data_cmd.h"
#include "net/ble_api.h"
#include "mf1_capture_stats.h"
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;
#define APP_TIMER_TICKS(ms) (((ms) * 32768u + 1000) / 2000)
static bool release_ok = true;
static bool hf_radio_release(void) { return release_ok; }
static void mf1_darkside_reset(void) {}
static void desfire_reader_invalidate(void) {}
static void hf_sniff_stop(void) {
}
#define DEVICE_MODE_READER 1
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define NRF_LOG_INFO(...) ((void)0)

static uint32_t rtc_ticks;
static int mode = DEVICE_MODE_READER, resets, field_on, field_off;
static uint8_t capture_status;
static bool web_owned;
enum { DATA_SOURCE_BLE = 1 };
static unsigned frame_source, ble_calls;
static unsigned data_frame_source(void) { return frame_source; }
uint16_t ble_api_request(const uint8_t *request, size_t size, uint8_t *out,
    size_t capacity, uint16_t *length) {
    assert(web_owned && capacity == BLE_API_RESPONSE_MAX);
    ble_calls++; *length = 0; return STATUS_SUCCESS;
}
static bool web_session_active(void) { return web_owned; }
static uint32_t app_timer_cnt_get(void) { return rtc_ticks; }
static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return (now - before) & 0xffffff;
}

static void pcd_14a_reader_antenna_off(void) { field_off++; }
static void pcd_14a_reader_antenna_on(void) { field_on++; }
static void pcd_14a_reader_reset(void) { resets++; DWT->CYCCNT += 20 * 64000; }
static void bsp_delay_ms(uint32_t ms) { assert(ms == 8); DWT->CYCCNT += ms * 64000; }
typedef int device_mode_t;
static device_mode_t get_device_mode(void) { return mode; }
typedef struct { uint16_t status, length; uint8_t bytes[232]; } data_frame_tx_t;
typedef data_frame_tx_t *(*cmd_processor)(uint16_t, uint16_t, uint16_t, uint8_t *);
typedef struct { uint16_t cmd; cmd_processor cmd_before, cmd_processor, cmd_after; } cmd_data_map_t;
static data_frame_tx_t frame;
static data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status, uint16_t length, uint8_t *bytes) {
    assert(length <= sizeof(frame.bytes));
    frame.status = status; frame.length = length;
    if (length) memcpy(frame.bytes, bytes, length);
    return &frame;
}

static void auto_response_data(data_frame_tx_t *response) { assert(response == &frame); }
static uint64_t bytes_to_num(uint8_t *bytes, uint8_t length) { return 0; }
static uint8_t mf1_collect_observations(uint8_t source_block, uint8_t source_type,
    uint64_t key, uint8_t target_block, uint8_t target_type, bool chained, uint8_t count, uint8_t *output) {
    if (!capture_status) mf1_capture_stats.observations += count;
    return capture_status;
}
/* SESSION FUNCTIONS */
/* COMMAND FUNCTIONS */
static data_frame_tx_t *type2_stub(uint16_t cmd,uint16_t status,uint16_t size,uint8_t *bytes) {
    return data_frame_make(cmd,STATUS_HF_TAG_OK,0,NULL);
}
static data_frame_tx_t *darkside_stub(uint16_t cmd,uint16_t status,uint16_t size,uint8_t *bytes) {
    if (bytes[2] == 0) nfca_session_open();
    return data_frame_make(cmd,STATUS_HF_TAG_OK,0,NULL);
}
static data_frame_tx_t *hf_reader_stub(uint16_t cmd, uint16_t status, uint16_t size, uint8_t *bytes) {
    assert(active && prepared);
    return data_frame_make(cmd, STATUS_BUSY, 0, NULL);
}
static cmd_data_map_t m_data_cmd_map[] = {
    {DATA_CMD_ST25TB_INFO, NULL, hf_reader_stub, NULL},
    {DATA_CMD_GET_APP_VERSION, NULL, type2_stub, NULL},
    {DATA_CMD_GET_GIT_VERSION, NULL, type2_stub, NULL},
    {DATA_CMD_GET_DEVICE_MODEL, NULL, type2_stub, NULL},
    {DATA_CMD_GET_DEVICE_CAPABILITIES, NULL, type2_stub, NULL},
    {DATA_CMD_ENTER_BOOTLOADER, NULL, type2_stub, NULL},
    {2028, before_hf_reader_run, type2_stub, after_hf_reader_run},
    {2027, before_hf_reader_run, type2_stub, after_hf_reader_run},
    {2008, before_hf_reader_run, type2_stub, after_hf_reader_run},
    {2029, before_hf_reader_run, darkside_stub, after_hf_reader_run},
    {2023, before_hf_reader_run, cmd_processor_mf1_collect_observations, after_hf_reader_run},
    {2026, NULL, cmd_processor_nfca_session, NULL},
};
/* DISPATCH FUNCTION */

static void control(uint8_t action) { on_data_frame_received(2026, 0, 1, &action); }
static void capture(void) {
    uint8_t bytes[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 4, 0, 1, 1};
    on_data_frame_received(2023, 0, sizeof(bytes), bytes);
}

int main(void) {
    release_ok = false;
    assert(nfca_prepare(2015, 0, NULL) == STATUS_HF_ERR_STAT && resets == 0);
    release_ok = true;
    mode = 0;
    control(1); assert(frame.status == STATUS_DEVICE_MODE_ERROR && !active);
    mode = DEVICE_MODE_READER;
    control(1); assert(active && !prepared);
    capture();
    assert(frame.status == 0 && frame.length == 52 && active && prepared);
    assert(resets == 1 && field_on == 1 && mf1_capture_stats.setup_cycles == 28 * 64000);
    int before = field_off;
    on_data_frame_received(DATA_CMD_ST25TB_INFO, 0, 0, NULL);
    assert(frame.status == STATUS_BUSY && active && prepared && field_off == before);
    capture();
    assert(active && resets == 1 && field_on == 1 && field_off == before);
    assert(mf1_capture_stats.setup_cycles == 0);
    control(2); assert(frame.length == 1 && frame.bytes[0] == 1);
    control(0); assert(!active && !prepared && field_off == before + 1);
    control(0); assert(field_off == before + 1);
    capture(); assert(!active && resets == 2 && field_off == before + 2);

    control(1); capture_status = STATUS_MF_ERR_AUTH; capture();
    assert(frame.status == STATUS_MF_ERR_AUTH && frame.length == 40 && !active);
    capture_status = 0;
    control(1); capture();
    on_data_frame_received(65535, 0, 0, NULL);
    assert(!active && frame.status == STATUS_INVALID_CMD);
    control(1); capture();
    on_data_frame_received(2023, 0, 0, NULL);
    assert(!active && frame.status == STATUS_PAR_ERR);
    control(1); control(3); assert(!active && frame.status == STATUS_PAR_ERR);

    rtc_ticks = 0xffff00;
    control(1); capture();
    rtc_ticks = (rtc_ticks + APP_TIMER_TICKS(2000) - 1) & 0xffffff;
    nfca_session_poll(); assert(active);
    rtc_ticks = (rtc_ticks + 1) & 0xffffff;
    before = field_off;
    nfca_session_poll(); assert(!active && field_off == before + 1);
    control(2); assert(frame.bytes[0] == 0);
    capture(); assert(!active && frame.status == 0);
    control(1); capture();
    // The reader mode transition calls close before uninitializing SPI.
    nfca_session_close(); assert(!active && !prepared);
    int previous_resets=resets, previous_on=field_on, previous_off=field_off;
    control(1);
    for (unsigned page=0;page<40;page++) {
        on_data_frame_received(2028,0,0,NULL);
        assert(active && prepared && frame.status==STATUS_HF_TAG_OK);
        assert(resets==previous_resets+1 && field_on==previous_on+1 && field_off==previous_off);
    }
    control(0);assert(!active && field_off==previous_off+1);
    on_data_frame_received(2028,0,0,NULL);
    assert(!active && resets==previous_resets+2 && field_off==previous_off+2);
    // Opt-in HTTP key checks reuse RF preparation; ordinary checks still close.
    previous_resets=resets; previous_off=field_off;
    assert(nfca_prepare(2015,0,NULL)==STATUS_SUCCESS);
    nfca_finish(2015);
    assert(!active && resets==previous_resets+1 && field_off==previous_off+1);
    control(1);
    for(unsigned check=0;check<4;check++) {
        assert(nfca_prepare(2015,0,NULL)==STATUS_SUCCESS);
        nfca_finish(2015);
        assert(active && prepared && resets==previous_resets+2);
        rtc_ticks=(rtc_ticks+APP_TIMER_TICKS(1000))&0xffffff;
        nfca_session_poll(); assert(active);
    }
    rtc_ticks=(rtc_ticks+APP_TIMER_TICKS(1000))&0xffffff;
    nfca_session_poll(); assert(!active);
    assert(nfca_prepare(2015,0,NULL)==STATUS_SUCCESS);
    nfca_finish(2015);
    assert(!active && resets==previous_resets+3);
    control(1); capture();
    web_owned = true;
    previous_off = field_off;
    const uint16_t passive[] = {DATA_CMD_GET_APP_VERSION, DATA_CMD_GET_GIT_VERSION,
        DATA_CMD_GET_DEVICE_MODEL, DATA_CMD_GET_DEVICE_CAPABILITIES};
    for (unsigned i=0;i<sizeof(passive)/sizeof(passive[0]);i++) {
        on_data_frame_received(passive[i],0,0,NULL);
        assert(frame.status==STATUS_HF_TAG_OK && active && prepared && field_off==previous_off);
    }
    capture(); assert(frame.status==STATUS_BUSY && active && prepared && field_off==previous_off);
    on_data_frame_received(65535,0,0,NULL);
    assert(frame.status==STATUS_BUSY && active && field_off==previous_off);
    on_data_frame_received(DATA_CMD_ENTER_BOOTLOADER,0,0,NULL);
    assert(!active && field_off==previous_off+1);
    web_owned = false;
    control(1); capture();
    on_data_frame_received(DATA_CMD_GET_APP_VERSION,0,0,NULL);
    assert(!active && field_off==previous_off+2);
    // Sector reads share a lease; individual block reads prepare RF separately.
    nfca_session_close();
    previous_resets = resets; previous_off = field_off;
    for (unsigned sector = 0; sector < 40; sector++) {
        on_data_frame_received(2027, 0, 0, NULL);
        assert(active && prepared && reading && resets == previous_resets + 1);
        assert(field_off == previous_off);
    }
    rtc_ticks = (rtc_ticks + APP_TIMER_TICKS(2000)) & 0xffffff;
    nfca_session_poll();
    assert(!active && !reading && field_off == previous_off + 1);
    on_data_frame_received(2027, 0, 0, NULL);
    assert(active && reading && resets == previous_resets + 2);
    on_data_frame_received(2008, 0, 0, NULL);
    assert(!active && !reading && resets == previous_resets + 3);
    on_data_frame_received(2027, 0, 0, NULL);
    control(0); assert(!active && !reading);
    // Original sector requests keep their per-command RF lifecycle.
    uint8_t legacy_sector[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    previous_resets = resets;
    on_data_frame_received(2027, 0, sizeof(legacy_sector), legacy_sector);
    assert(!active && !reading && resets == previous_resets + 1);
    on_data_frame_received(2027, 0, 0, NULL);
    assert(active && reading);
    on_data_frame_received(2027, 0, sizeof(legacy_sector), legacy_sector);
    assert(!active && !reading && resets == previous_resets + 3);
    control(1); capture(); previous_resets = resets;
    on_data_frame_received(2027, 0, sizeof(legacy_sector), legacy_sector);
    assert(!active && !reading && resets == previous_resets + 1);
    // Optional diagnostics retain the checked-read lifecycle without new resets.
    uint8_t detailed_sector[] = {0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                4, 1, 2, 3, 4, 1};
    previous_resets = resets;
    on_data_frame_received(2027, 0, sizeof(detailed_sector), detailed_sector);
    assert(active && reading && resets == previous_resets + 1);
    on_data_frame_received(2027, 0, sizeof(detailed_sector), detailed_sector);
    assert(active && reading && resets == previous_resets + 1);
    control(0);
    // Capture-to-read transitions must prepare a new RF lease.
    control(1); capture(); previous_resets = resets;
    on_data_frame_received(2027, 0, 0, NULL);
    assert(reading && resets == previous_resets + 1);
    capture(); assert(active == false && reading == false);
    previous_resets=resets; previous_off=field_off;
    uint8_t request[]={0x60,0,0,4,1,2,3,4};
    on_data_frame_received(2029,0,sizeof(request),request);
    assert(active && prepared && resets==previous_resets+1);
    request[2]=1;
    for(unsigned chunk=0;chunk<4;chunk++) {
        on_data_frame_received(2029,0,sizeof(request),request);
        assert(active && prepared && resets==previous_resets+1 && field_off==previous_off);
    }
    rtc_ticks=(rtc_ticks+APP_TIMER_TICKS(2000))&0xffffff;
    nfca_session_poll();
    assert(!active && field_off==previous_off+1);
    control(1);
    capture();
    previous_resets = resets;
    assert(nfca_prepare(DATA_CMD_MF1_MAGIC_PROBE, 0, NULL) == STATUS_SUCCESS);
    assert(!active && !prepared && resets == previous_resets + 1);
    nfca_finish(DATA_CMD_MF1_MAGIC_PROBE);
    assert(!active && !reading);
    control(1); capture();
    assert(active && prepared);
    previous_resets = resets; previous_off = field_off;
    web_owned = true; frame_source = DATA_SOURCE_BLE;
    on_data_frame_received(DATA_CMD_WEB_API, 0, 0, NULL);
    assert(ble_calls == 1 && frame.status == STATUS_SUCCESS);
    assert(active && prepared && resets == previous_resets && field_off == previous_off);
    frame_source = 0;
    on_data_frame_received(DATA_CMD_WEB_API, 0, 0, NULL);
    assert(ble_calls == 1 && frame.status == STATUS_NOT_IMPLEMENTED);
    assert(active && prepared && resets == previous_resets && field_off == previous_off);
    web_owned = false; control(0);
    return 0;
}
