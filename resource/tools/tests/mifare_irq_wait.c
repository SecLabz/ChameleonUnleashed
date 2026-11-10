#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "bsp_time.h"
#include "mf1_capture_stats.h"
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;
#define ComIrqReg 4
#define APP_TIMER_TICKS(ms) (((uint32_t)(ms) * 32768 + 1000) / 2000)
static uint32_t m_wakeup_timeout_ticks;
static autotimer timer;
static autotimer *g_timeout_auto_timer = &timer;
static uint16_t g_com_timeout_ms;
static uint32_t rtc_ticks, step, next_bsp_tick;
static unsigned polls, ready_at;
static uint8_t irq_value;

uint8_t bsp_set_timer(autotimer *value, uint32_t start) { value->time = start; return 1; }
static uint32_t app_timer_cnt_get(void) { return rtc_ticks; }
static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return (now - before) & 0xffffff;
}
static uint8_t read_register_single(uint8_t address) {
    assert(address == ComIrqReg && ++polls < 4096);
    rtc_ticks = (rtc_ticks + step) & 0xffffff;
    /* A global BSP callback can fire just after this wait resets its counter.
     * Its first +10 ms does not mean this transaction has waited 10 ms. */
    if (polls == next_bsp_tick) {
        timer.time += 10;
        next_bsp_tick += APP_TIMER_TICKS(10) / step + 1;
    }
    return polls >= ready_at ? irq_value : 0;
}

/* WAIT FUNCTION */

static void reset(void) {
    m_wakeup_timeout_ticks = 0;
    g_com_timeout_ms = 5;
    rtc_ticks = 0xfffff8;
    step = 8;
    next_bsp_tick = 1;
    timer.time = 0;
    polls = 0;
    ready_at = 2;
    irq_value = 0x20;
    mf1_capture_stats_start(true);
    assert(g_timeout_auto_timer == &timer);
}

int main(void) {
    uint8_t irq;
    reset();
    /* The 5 ms wait must accept a reply after about 1 ms even when the global
     * 10 ms timer fires on the first poll. Also crosses the RTC wrap point. */
    assert(wait_for_irq(0x30, &irq));
    assert(irq == 0x20 && polls == 2 && timer.time == 10);
    assert(mf1_capture_stats.timeouts == 0);

    reset(); ready_at = UINT32_MAX;
    assert(!wait_for_irq(0x30, &irq));
    assert(polls == (APP_TIMER_TICKS(5) + step - 1) / step);
    assert(mf1_capture_stats.timeouts == 1);

    reset(); m_wakeup_timeout_ticks = APP_TIMER_TICKS(1); ready_at = UINT32_MAX;
    assert(!wait_for_irq(0x30, &irq));
    assert(polls == (APP_TIMER_TICKS(1) + step - 1) / step);

    reset(); g_com_timeout_ms = 25; ready_at = UINT32_MAX;
    assert(!wait_for_irq(0x30, &irq));
    assert(polls == (APP_TIMER_TICKS(25) + step - 1) / step);

    reset(); step = APP_TIMER_TICKS(20); ready_at = 1;
    /* The processor resumes after an interrupt with completion already
     * latched. Consume that completed transfer instead of inventing a timeout. */
    assert(wait_for_irq(0x30, &irq));
    assert(polls == 1 && irq == 0x20 && mf1_capture_stats.timeouts == 0);

    reset(); g_com_timeout_ms = 0; ready_at = UINT32_MAX;
    assert(!wait_for_irq(0x30, &irq) && polls == 1);
    reset(); g_com_timeout_ms = 0; ready_at = 1;
    assert(wait_for_irq(0x30, &irq) && polls == 1);

    reset(); irq_value = 0x40; ready_at = 1;
    assert(!wait_for_irq(0x30, &irq)); // TX completion is not RX completion.
    reset(); irq_value = 0x12; ready_at = 1;
    assert(wait_for_irq(0x30, &irq) && irq == 0x12); // Caller processes ErrorReg.
    return 0;
}
