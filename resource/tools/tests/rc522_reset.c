#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define CommandReg 0x01
#define ModeReg 0x11
#define TxAutoReg 0x15
#define TModeReg 0x2A
#define PCD_IDLE 0x00
#define PCD_RESET 0x0F
#define APP_TIMER_TICKS(ms) (((ms) * 32768u + 500u) / 1000u)

static bool m_reader_is_init;
static uint32_t ticks, started, idle_after, mode_after, field_off_at;
static unsigned writes;

static uint32_t app_timer_cnt_get(void) { return ticks & 0xFFFFFF; }
static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return (now - before) & 0xFFFFFF;
}

static uint8_t read_register_single(uint8_t address) {
    assert(writes == 2);
    if (address == CommandReg) return ticks - started < idle_after ? PCD_RESET : 0x20;
    assert(address == ModeReg);
    return ticks - started < mode_after ? 0x3D : 0x3F;
}

static void write_register_single(uint8_t address, uint8_t value) {
    static const uint8_t sequence[][2] = {
        {CommandReg, PCD_IDLE}, {CommandReg, PCD_RESET},
        {TModeReg, 0}, {TxAutoReg, 0x40}, {ModeReg, 0x3D},
    };
    assert(writes < sizeof(sequence) / sizeof(sequence[0]));
    assert(address == sequence[writes][0] && value == sequence[writes][1]);
    if (writes >= 2) assert(ticks == field_off_at);
    writes++;
}

static void pcd_14a_reader_antenna_off(void) {
    assert(writes == 2);
    field_off_at = ticks;
}

static void bsp_delay_us(uint32_t us) { assert(us > 0); ticks++; }
static void bsp_delay_ms(uint16_t ms) { ticks += APP_TIMER_TICKS(ms); }

/* READER RESET FUNCTION */

static void check_reset(uint32_t idle, uint32_t mode, uint32_t expected_wait) {
    // Start near the RTC wrap to exercise its elapsed-time calculation.
    ticks = started = 0xFFFFF8;
    idle_after = idle;
    mode_after = mode;
    field_off_at = 0;
    writes = 0;
    pcd_14a_reader_reset();
    assert(writes == 5);
    assert(field_off_at - started == expected_wait);
    assert(ticks - field_off_at == APP_TIMER_TICKS(10));
}

int main(void) {
    pcd_14a_reader_reset();
    assert(writes == 0);
    m_reader_is_init = true;
    check_reset(0, 0, 0);
    check_reset(5, 12, 12);
    check_reset(0, UINT32_MAX, APP_TIMER_TICKS(10));
    check_reset(UINT32_MAX, 0, APP_TIMER_TICKS(10));
    return 0;
}
