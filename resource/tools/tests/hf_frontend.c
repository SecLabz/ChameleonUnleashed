#include "rc522_raw.h"
#include "rc522_tx.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "rfid_main.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static test_spi_t spi;
static test_spim_t spim;
static test_dwt_t cycles;
test_debug_t test_debug;
test_clock_t test_clock;
bool g_is_reader_antenna_on;

static uint8_t registers[64], address;
static bool selected, address_sent, spi_timeout, dma_timeout, stop_failure;
static bool tx_failure, tx_owned, tx_partial;
static unsigned carrier_on_writes;
static unsigned expected_dma_bytes = 9;
static unsigned spi_bytes, dma_starts, dma_stops, resets, power_off, power_on;
static unsigned reader_off, reader_uninit, reader_init, tx_opens, tx_closes;
static unsigned tx_partial_cleanups, transmissions;
static uint32_t delayed_ms, interrupt_mask;
static uint8_t level_trace[64], mask_trace[64];
static const uint8_t *expected_levels;
static size_t level_count, expected_level_count;
static uint32_t initial_mask;
static bool trace_levels, constant_carrier;
static jmp_buf reset_target;
static bool reset_expected;

/* Each DWT access advances 100 us at the independent 64 MHz test clock. */
test_dwt_t *test_dwt_access(void) {
    cycles.CYCCNT += 6400;
    assert(cycles.CYCCNT < 64000000); /* No test may spin for one virtual second. */
    if (selected && spi.ENABLE && !spi.EVENTS_READY && !spi_timeout) {
        spi_bytes++;
        if (!address_sent) {
            address = spi.TXD;
            address_sent = true;
        } else if (address & 0x80) {
            spi.RXD = registers[(address & 0x7e) >> 1];
        } else {
            uint8_t reg = (address & 0x7e) >> 1;
            registers[reg] = spi.TXD;
            if (reg == 0x14 && (spi.TXD & 3u)) {
                carrier_on_writes++;
            }
            if (trace_levels && reg == 0x16) {
                assert(spi.TXD == 0x30 && !level_count);
                constant_carrier = true;
            }
            if (trace_levels && reg == 0x28) {
                assert(constant_carrier && level_count < sizeof(level_trace));
                level_trace[level_count] = spi.TXD;
                mask_trace[level_count++] = interrupt_mask;
            }
        }
        spi.EVENTS_READY = 1;
    }
    return &cycles;
}

test_spi_t *test_spi_access(void) { return &spi; }

test_spim_t *test_spim_access(void) {
    if (spim.TASKS_START) {
        assert(!spi.ENABLE && spim.ENABLE == 7);
        assert(spim.PSEL.SCK == HF_SPI_SCK && spim.PSEL.MOSI == HF_SPI_MOSI);
        assert(spim.PSEL.MISO == HF_SPI_MISO && spim.CONFIG == 0);
        assert(spim.FREQUENCY == 0x80000000); /* 8 MHz SPIM. */
        assert(spim.TXD.MAXCNT == 1 && spim.RXD.MAXCNT == expected_dma_bytes);
        assert(spim.ORC == 0xf6); /* Read the RC522 ADC register. */
        if (trace_levels) {
            /* The complete TX trace and mask restoration must precede ADC DMA. */
            assert(level_count == expected_level_count);
            assert(!memcmp(level_trace, expected_levels, level_count));
            assert(mask_trace[0] == initial_mask && interrupt_mask == initial_mask);
            for (size_t i = 1; i < level_count; i++) assert(mask_trace[i] == 1);
            assert(!transmissions && !tx_opens);
        }
        spim.TASKS_START = 0;
        dma_starts++;
        if (!dma_timeout) spim.EVENTS_END = 1;
    }
    if (spim.TASKS_STOP) {
        spim.TASKS_STOP = 0;
        dma_stops++;
        if (!stop_failure) spim.EVENTS_STOPPED = 1;
    }
    return &spim;
}

void test_system_reset(void) {
    resets++;
    assert(reset_expected);
    longjmp(reset_target, 1);
}

uint32_t __get_PRIMASK(void) { return interrupt_mask; }
void __disable_irq(void) { interrupt_mask = 1; }
void __set_PRIMASK(uint32_t mask) { interrupt_mask = mask; }

void nrf_gpio_pin_clear(uint32_t pin) {
    if (pin == HF_SPI_SELECT) { selected = true; address_sent = false; }
    if (pin == READER_POWER) power_off++;
}

void nrf_gpio_pin_set(uint32_t pin) {
    if (pin == HF_SPI_SELECT) selected = false;
    if (pin == READER_POWER) power_on++;
}

void nrf_gpio_cfg_output(uint32_t pin) { (void)pin; }
void nrf_gpio_cfg_input(uint32_t pin, uint32_t pull) {
    assert(pin == HF_SPI_MISO && pull == 0);
}

void bsp_delay_ms(uint32_t milliseconds) { delayed_ms += milliseconds; }
void pcd_14a_reader_antenna_off(void) { reader_off++; g_is_reader_antenna_on = false; }
void pcd_14a_reader_uninit(void) { reader_uninit++; }
void pcd_14a_reader_init(void) {
    assert(!spi.ENABLE && !spim.ENABLE && !selected);
    assert(!g_is_reader_antenna_on);
    reader_init++;
}

/* The TX owner must unwind its own partial open before returning failure. */
bool rc522_tx_open(void) {
    assert(!tx_owned && !tx_partial);
    tx_opens++;
    tx_partial = true;
    if (tx_failure) {
        tx_partial = false;
        tx_partial_cleanups++;
        return false;
    }
    tx_partial = false;
    tx_owned = true;
    return true;
}

void rc522_tx_close(void) {
    assert(tx_owned && !tx_partial);
    tx_owned = false;
    tx_closes++;
}

bool rc522_tx_send(const uint8_t *data, size_t length, uint8_t low) {
    assert(tx_owned && data && length == 1 && low == 16);
    transmissions++;
    return true;
}

static void assert_restored(void) {
    assert(reader_init == 1 && !g_is_reader_antenna_on);
    assert(!spi.ENABLE && !spim.ENABLE && !selected);
    assert(registers[0x14] == 0x80); /* Field off. */
    assert(registers[0x2a] == 0 && registers[0x15] == 0x40 && registers[0x11] == 0x3d);
    assert(!tx_owned && !tx_partial && !resets && !interrupt_mask);
}

static void exchange(bool live) {
    uint8_t command[] = {0}, samples[9] = {0};
    uint32_t measured = 0;
    unsigned before = transmissions;
    rc522_raw_transfer_t transfer = {
        .data = command, .length = sizeof(command), .samples = samples,
        .sample_count = 8, .hardware_tx = true, .live = live, .dma_cycles = &measured,
    };
    rc522_raw_exchange(&transfer);
    if (live) {
        assert(!spi.ENABLE && spim.ENABLE == 7);
        rc522_raw_finish(true);
        assert(spi.ENABLE == 1 && !spim.ENABLE);
    }
    assert(measured > 0 && transmissions == before + 1);
}

static void open_close(void) {
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(reader_off == 1 && reader_uninit == 1 && !reader_init);
    assert(g_is_reader_antenna_on && rc522_raw_ready());
    assert(spi.ENABLE == 1 && spi.FREQUENCY == 0x80000000);
    assert(registers[0x12] == 3 && registers[0x13] == 0x14);
    assert(registers[0x14] == 0x83 && registers[0x16] == 0x30);
    assert(delayed_ms == 5 && !power_off && !power_on);
    assert(rc522_raw_close());
    assert(delayed_ms == 5);
    assert_restored();
}

static void failed_open(void) {
    spi_timeout = true;
    assert(!rc522_raw_open(RC522_RAW_NFCB));
    assert(!rc522_raw_ready() && !reader_init);
    spi_timeout = false;
    assert(!rc522_raw_close());
    assert(power_off == 1 && power_on == 1);
    assert_restored();
}

static void sticky_timeout(void) {
    assert(rc522_raw_open(RC522_RAW_NFCB));
    unsigned before = spi_bytes;
    uint32_t started = cycles.CYCCNT;
    spi_timeout = true;
    rc522_raw_reset();
    assert(!rc522_raw_ready() && spi_bytes == before);
    assert(cycles.CYCCNT - started > 64000); /* 1 ms SPI deadline. */
    spi_timeout = false;
    rc522_raw_reset();
    assert(!rc522_raw_ready() && spi_bytes == before);
    assert(!rc522_raw_close());
    assert_restored();
}

static void dma_failure(bool cannot_stop) {
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(rc522_raw_acquire_tx());
    rc522_raw_depth(false);
    dma_timeout = true;
    stop_failure = cannot_stop;
    if (cannot_stop) {
        reset_expected = true;
        if (!setjmp(reset_target)) { exchange(false); assert(false); }
        assert(resets == 1 && dma_starts == 1 && dma_stops == 1);
        assert(!spi.ENABLE && spim.ENABLE == 7 && !reader_init);
        return;
    }
    uint32_t started = cycles.CYCCNT;
    exchange(false);
    assert(cycles.CYCCNT - started > 640000); /* 10 ms ADC capture deadline. */
    assert(dma_starts == 1 && dma_stops == 1 && !spim.ENABLE && spi.ENABLE == 1);
    assert(!rc522_raw_ready());
    assert(!rc522_raw_close());
    assert(tx_closes == 1);
    assert_restored();
}

static void dma_success(void) {
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(rc522_raw_acquire_tx());
    rc522_raw_depth(false);
    exchange(false);
    assert(dma_starts == 1 && dma_stops == 1 && !spim.ENABLE && spi.ENABLE == 1);
    assert(rc522_raw_ready() && rc522_raw_close());
    assert(tx_closes == 1);
    assert_restored();
}

static void live_exchange(bool complete) {
    uint8_t command[] = {0}, samples[9];
    uint32_t measured = 0;
    memset(samples, 0xff, sizeof(samples));
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(rc522_raw_acquire_tx());
    rc522_raw_depth(false);
    dma_timeout = !complete;
    rc522_raw_transfer_t transfer = {
        .data = command, .length = sizeof(command), .samples = samples,
        .sample_count = 8, .hardware_tx = true, .live = true, .dma_cycles = &measured,
    };
    rc522_raw_exchange(&transfer);
    unsigned available = rc522_raw_available();
    assert(complete ? available == 8 : available <= 8);
    /* The DMA still runs: unwritten samples read as silence, never stale data. */
    assert(transmissions == 1 && dma_starts == 1 && !dma_stops && spim.ENABLE == 7);
    for (unsigned n = 0; n < sizeof(samples); n++) assert(!samples[n]);
    rc522_raw_finish(!complete);
    assert(dma_stops == 1 && !spim.ENABLE && spi.ENABLE == 1 && measured > 0);
    rc522_raw_finish(false); /* Idempotent once the DMA is released. */
    assert(dma_stops == 1 && rc522_raw_ready());
    assert(rc522_raw_close() && tx_closes == 1);
    assert_restored();
}

static void live_failure(bool cannot_stop) {
    uint8_t command[] = {0}, samples[9] = {0};
    uint32_t measured = 0;
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(rc522_raw_acquire_tx());
    rc522_raw_depth(false);
    dma_timeout = true;
    rc522_raw_transfer_t transfer = {
        .data = command, .length = sizeof(command), .samples = samples,
        .sample_count = 8, .hardware_tx = true, .dma_cycles = &measured, .live = true,
    };
    rc522_raw_exchange(&transfer);
    assert(!dma_stops && spim.ENABLE == 7 && !spi.ENABLE);
    stop_failure = cannot_stop;
    if (cannot_stop) {
        reset_expected = true;
        if (!setjmp(reset_target)) { rc522_raw_finish(true); assert(false); }
        assert(resets == 1 && dma_stops == 1);
        assert(!spi.ENABLE && spim.ENABLE == 7 && !reader_init);
        return;
    }
    uint32_t started = cycles.CYCCNT;
    rc522_raw_finish(false);
    assert(cycles.CYCCNT - started > 640000);
    assert(dma_stops == 1 && !spim.ENABLE && spi.ENABLE == 1 && measured > 0);
    assert(!rc522_raw_ready());
    assert(!rc522_raw_close());
    assert(tx_closes == 1);
    assert_restored();
}

static void software_exchange(bool vicinity, uint32_t mask) {
    /* Ordinary block-read bytes; the frontend only serializes prepared data. */
    const uint8_t read_block[] = {0x08,0x00};
    const uint8_t type_b_levels[] = {
        32,16,32,                         /* Carrier, SOF low, SOF high. */
        16,16,16,16,32,16,16,16,16,32,  /* Start, byte 08 LSB-first, stop. */
        16,16,16,16,16,16,16,16,16,32,  /* Start, byte 00, stop. */
        16,32,                           /* EOF low, carrier. */
    };
    const uint8_t prepared_v[] = {0,0,1,1,0,1,1,0};
    const uint8_t type_v_levels[] = {32,32,0,32,0,32,32};
    uint8_t samples[9] = {0};
    assert(rc522_raw_open(vicinity ? RC522_RAW_NFCV : RC522_RAW_NFCB));
    rc522_raw_depth(false);
    rc522_raw_transfer_t transfer = {
        .data = vicinity ? prepared_v : read_block,
        .length = vicinity ? sizeof(prepared_v) : sizeof(read_block),
        .samples = samples, .sample_count = 8, .hardware_tx = false,
    };
    expected_levels = vicinity ? type_v_levels : type_b_levels;
    expected_level_count = vicinity ? sizeof(type_v_levels) : sizeof(type_b_levels);
    interrupt_mask = initial_mask = mask;
    trace_levels = true;
    rc522_raw_exchange(&transfer);
    trace_levels = false;
    assert(dma_starts == 1 && dma_stops == 1 && level_count == expected_level_count);
    assert(interrupt_mask == mask && !transmissions && rc522_raw_ready());
    assert(rc522_raw_close() && interrupt_mask == mask);
    interrupt_mask = 0;
    assert_restored();
}

static void tx_failure_cleanup(void) {
    assert(rc522_raw_open(RC522_RAW_NFCB));
    tx_failure = true;
    assert(!rc522_raw_acquire_tx());
    assert(tx_opens == 1 && tx_partial_cleanups == 1 && !tx_owned);
    assert(rc522_raw_close());
    assert(!tx_closes);
    assert_restored();
}

static void recover(void) {
    assert(rc522_raw_open(RC522_RAW_NFCV));
    assert(registers[0x12] == 0 && delayed_ms == 20);
    assert(rc522_raw_acquire_tx());
    rc522_raw_invalidate();
    assert(!rc522_raw_ready());
    assert(rc522_raw_recover());
    assert(tx_closes == 1 && !tx_owned && power_off == 1 && power_on == 1);
    assert(registers[0x12] == 0 && delayed_ms == 60);
    assert(reader_off == 1 && reader_uninit == 1 && !reader_init);
    assert(rc522_raw_acquire_tx());
    assert(rc522_raw_close());
    assert(tx_opens == 2 && tx_closes == 2);
    assert_restored();
}

static void clock_ready(void) {
    test_clock.HFCLKSTAT = 0;
    assert(!rc522_raw_clock_ready());
    test_clock.HFCLKSTAT = 1;
    assert(!rc522_raw_clock_ready());
    test_clock.HFCLKSTAT = 0x10000;
    assert(!rc522_raw_clock_ready());
    test_clock.HFCLKSTAT = 0x10001;
    assert(rc522_raw_clock_ready());
}

static void read_write_read(void) {
    const uint8_t frame[8] = {9, 5, 0, 0, 0, 0, 0, 0};
    rc522_raw_transfer_t transfer = {.data = frame, .length = sizeof(frame)};
    assert(rc522_raw_open(RC522_RAW_NFCB));
    assert(rc522_raw_acquire_tx());
    rc522_raw_depth(false);
    dma_timeout = true;
    exchange(true);
    dma_timeout = false;
    unsigned before = spi_bytes;
    rc522_raw_write(&transfer);
    assert(transmissions == 1 && spi_bytes > before && !spim.ENABLE && spi.ENABLE == 1);
    rc522_raw_tearoff(&transfer, 100);
    assert(transmissions == 1 && rc522_raw_take_write_cycles() > 0);
    exchange(true);
    assert(transmissions == 2 && dma_starts == 2 && dma_stops == 2);
    assert(rc522_raw_close());
    assert_restored();
}

static void passive_capture(unsigned count) {
    assert(rc522_raw_sniff_open(7));
    assert(!g_is_reader_antenna_on && !carrier_on_writes && !tx_opens);
    assert(registers[0x14] == 0x80 && registers[0x26] == 0x78);
    uint8_t samples[44032] = {0};
    expected_dma_bytes = count;
    rc522_raw_capture(samples, count);
    assert(spim.SHORTS == SPIM_SHORTS_END_START_Msk);
    assert(spim.RXD.MAXCNT == count);
    rc522_raw_capture_stop();
    assert(!spim.SHORTS && !spim.ENABLE && spi.ENABLE);
    assert(rc522_raw_close());
    assert(!carrier_on_writes && !transmissions && dma_stops == 1);
    assert_restored();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "passive_capture")) passive_capture(64);
    else if (!strcmp(argv[1], "passive_large")) passive_capture(44032);
    else if (!strcmp(argv[1], "open_close")) open_close();
    else if (!strcmp(argv[1], "failed_open")) failed_open();
    else if (!strcmp(argv[1], "spi_timeout")) sticky_timeout();
    else if (!strcmp(argv[1], "dma_success")) dma_success();
    else if (!strcmp(argv[1], "live_complete")) live_exchange(true);
    else if (!strcmp(argv[1], "live_stop")) live_exchange(false);
    else if (!strcmp(argv[1], "live_timeout")) live_failure(false);
    else if (!strcmp(argv[1], "live_stop_failure")) live_failure(true);
    else if (!strcmp(argv[1], "software_b")) software_exchange(false, 0);
    else if (!strcmp(argv[1], "software_b_masked")) software_exchange(false, 1);
    else if (!strcmp(argv[1], "software_v")) software_exchange(true, 0);
    else if (!strcmp(argv[1], "software_v_masked")) software_exchange(true, 1);
    else if (!strcmp(argv[1], "dma_timeout")) dma_failure(false);
    else if (!strcmp(argv[1], "dma_stop_failure")) dma_failure(true);
    else if (!strcmp(argv[1], "tx_failure")) tx_failure_cleanup();
    else if (!strcmp(argv[1], "recover")) recover();
    else if (!strcmp(argv[1], "clock")) clock_ready();
    else if (!strcmp(argv[1], "read_write_read")) read_write_read();
    else assert(false);
    puts(argv[1]);
}
