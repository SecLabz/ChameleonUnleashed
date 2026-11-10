#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "app_status.h"
#include "bsp_time.h"
#include "mf1_capture_stats.h"
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;
#define ComIrqReg 4
#define PICC_REQALL 0x52
#define APP_TIMER_TICKS(ms) (((ms) * 32768 + 1000) / 2000)
#define U8ARR_BIT_LEN(bytes) (sizeof(bytes) * 8)

static uint32_t m_wakeup_timeout_ticks;
static bool m_scan_polling;
static autotimer timer;
static autotimer *g_timeout_auto_timer = &timer;
static uint16_t g_com_timeout_ms = 25;
static uint32_t rtc_ticks;
static int irq_reads, response_after, wakeup_calls;
static uint8_t irq_value;
static uint32_t waits[22];

uint8_t bsp_set_timer(autotimer *timer, uint32_t value) { timer->time = value; return 1; }
static uint32_t app_timer_cnt_get(void) { return rtc_ticks; }
static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return (now - before) & 0xffffff;
}
static uint8_t read_register_single(uint8_t address) {
    assert(address == ComIrqReg && ++irq_reads < 100);
    rtc_ticks = (rtc_ticks + 8) & 0xffffff;
    timer.time = (irq_reads * 8 / APP_TIMER_TICKS(10)) * 10;
    return irq_value;
}
static uint8_t pcd_14a_reader_bits_transfer(uint8_t *tx, uint16_t tx_bits,
    uint8_t *tx_parity, uint8_t *rx, uint8_t *rx_parity, uint16_t *bits, uint16_t capacity) {
    assert(tx[0] == PICC_REQALL && tx_bits == 7 && tx_parity == 0);
    assert(wakeup_calls < 22);
    waits[wakeup_calls] = m_wakeup_timeout_ticks;
    *bits = wakeup_calls++ == response_after ? 16 : 0;
    return *bits == 16 ? STATUS_HF_TAG_OK : STATUS_HF_TAG_NO;
}
typedef struct { uint8_t atqa[2]; } picc_14a_tag_t;
static uint8_t atqa_request(uint8_t *, uint16_t, bool);
static uint8_t pcd_14a_reader_scan_once(picc_14a_tag_t *tag) {
    return atqa_request(tag->atqa, 16, false);
}
/* CAPTURE FUNCTIONS */

int main(void) {
    assert(g_timeout_auto_timer == &timer);
    mf1_capture_stats_start(true);
    m_wakeup_timeout_ticks = APP_TIMER_TICKS(1);
    rtc_ticks = 0xfffff0; // Exercise the actual RTC counter boundary.
    uint8_t irq;
    assert(!wait_for_irq(0x30, &irq));
    assert(irq_reads == 2 && timer.time == 0 && mf1_capture_stats.timeouts == 1);
    m_wakeup_timeout_ticks = 0; irq_reads = 0;
    assert(!wait_for_irq(0x30, &irq));
    assert(irq_reads == 52 && timer.time == 20 && mf1_capture_stats.timeouts == 2);
    irq_value = 0x20; irq_reads = 0;
    assert(wait_for_irq(0x30, &irq) && irq == 0x20 && irq_reads == 1);

    uint8_t response[2];
    for (response_after = 0; response_after < 3; response_after++) {
        wakeup_calls = 0;
        assert(atqa_request(response, 16, false) == STATUS_HF_TAG_OK);
        assert(wakeup_calls == response_after + 1 && !m_wakeup_timeout_ticks);
        assert(waits[0] && (response_after < 1 || waits[1]));
        if (response_after == 2) assert(!waits[2]);
    }
    wakeup_calls = 0; response_after = -1;
    assert(atqa_request(response, 16, true) == STATUS_HF_TAG_NO);
    assert(wakeup_calls == 11 && !m_wakeup_timeout_ticks);
    assert(waits[0] == APP_TIMER_TICKS(1) / 2 && waits[1] == APP_TIMER_TICKS(1));
    for (int index = 2; index < 11; index++) assert(!waits[index]);

    picc_14a_tag_t tag;
    wakeup_calls = 0;
    assert(pcd_14a_reader_scan_poll(&tag) == STATUS_HF_TAG_NO);
    assert(wakeup_calls == 2 && waits[0] && waits[1]);
    assert(!m_scan_polling && !m_wakeup_timeout_ticks);
    wakeup_calls = 0;
    assert(pcd_14a_reader_scan_auto(&tag) == STATUS_HF_TAG_NO);
    assert(wakeup_calls == 22);
    wakeup_calls = 0; response_after = 0;
    assert(pcd_14a_reader_scan_poll(&tag) == STATUS_HF_TAG_OK);
    assert(wakeup_calls == 1 && !m_scan_polling && !m_wakeup_timeout_ticks);
    return 0;
}
