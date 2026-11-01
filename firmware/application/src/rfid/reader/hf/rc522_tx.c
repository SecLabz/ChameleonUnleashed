/* Finite hardware-timed ASK register writes; the reader owns SPI and RF. */
#include "nrf.h"
#include "nfcb_tx.h"
#include "rc522_tx.h"
#include "nrf_gpio.h"
#include "nrfx_timer.h"
#include "nrfx_ppi.h"
#include "nrfx_gpiote.h"
#include "rfid_main.h"
#include <stdbool.h>
#include <stddef.h>

/* 16 MHz timer ticks retain the qualified 106 kbit/s symbol and SPI spacing. */
#define SYMBOL_PERIOD_TICKS 151u
#define CHIP_SELECT_TICKS 1u
#define SPI_START_TICKS 4u
#define TRANSMIT_TIMEOUT_CYCLES 640000u
#define SPI_STOP_TIMEOUT_CYCLES 64000u

enum {
    SYMBOL_PERIOD_CC,
    CHIP_SELECT_CC,
    SPI_START_CC,
    SYMBOL_COMPARE_COUNT,
};
enum {
    CHIP_SELECT_PPI,
    SPI_START_PPI,
    SPI_END_PPI,
    FRAME_END_PPI,
    PPI_CHANNEL_COUNT,
};

static const nrfx_timer_t symbol_timer = NRFX_TIMER_INSTANCE(3);
static const nrfx_timer_t counter = NRFX_TIMER_INSTANCE(4);
static nrf_ppi_channel_t channels[PPI_CHANNEL_COUNT];
static unsigned allocated;
static bool symbol_ready, counter_ready, cs_ready;
static uint8_t sequence[RC522_TX_WRITE_BYTES * NFCB_TX_MAX_SYMBOLS];
static void timer_handler(nrf_timer_event_t event, void *context) {
    (void)event; (void)context;
}

void rc522_tx_close(void) {
    if (symbol_ready) nrfx_timer_disable(&symbol_timer);
    while (allocated) {
        nrf_ppi_channel_t ch = channels[--allocated];
        nrfx_ppi_channel_disable(ch);
        nrfx_ppi_channel_fork_assign(ch, 0);
        nrfx_ppi_channel_free(ch);
    }
    if (cs_ready) { nrfx_gpiote_out_uninit(HF_SPI_SELECT); cs_ready = false; }
    nrf_gpio_cfg_output(HF_SPI_SELECT);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    if (counter_ready) { nrfx_timer_uninit(&counter); counter_ready = false; }
    if (symbol_ready) { nrfx_timer_uninit(&symbol_timer); symbol_ready = false; }
}

bool rc522_tx_open(void) {
    nrfx_timer_config_t config = NRFX_TIMER_DEFAULT_CONFIG;
    config.frequency = NRF_TIMER_FREQ_16MHz;
    config.bit_width = NRF_TIMER_BIT_WIDTH_32;
    if (nrfx_timer_init(&symbol_timer, &config, timer_handler) != NRFX_SUCCESS) return false;
    symbol_ready = true;
    config.mode = NRF_TIMER_MODE_COUNTER;
    if (nrfx_timer_init(&counter, &config, timer_handler) != NRFX_SUCCESS) goto fail;
    counter_ready = true;
    for (; allocated < PPI_CHANNEL_COUNT; allocated++)
        if (nrfx_ppi_channel_alloc(&channels[allocated]) != NRFX_SUCCESS) goto fail;
    if (!nrfx_gpiote_is_init()) goto fail;
    nrfx_gpiote_out_config_t cs = NRFX_GPIOTE_CONFIG_OUT_TASK_TOGGLE(true);
    if (nrfx_gpiote_out_init(HF_SPI_SELECT, &cs) != NRFX_SUCCESS) goto fail;
    cs_ready = true;
    nrfx_gpiote_out_task_disable(HF_SPI_SELECT);
    const uint32_t events[PPI_CHANNEL_COUNT] = {
        [CHIP_SELECT_PPI] = (uint32_t)&NRF_TIMER3->EVENTS_COMPARE[CHIP_SELECT_CC],
        [SPI_START_PPI] = (uint32_t)&NRF_TIMER3->EVENTS_COMPARE[SPI_START_CC],
        [SPI_END_PPI] = (uint32_t)&NRF_SPIM0->EVENTS_END,
        [FRAME_END_PPI] = (uint32_t)&NRF_TIMER4->EVENTS_COMPARE[0],
    };
    const uint32_t tasks[PPI_CHANNEL_COUNT] = {
        [CHIP_SELECT_PPI] = nrfx_gpiote_clr_task_addr_get(HF_SPI_SELECT),
        [SPI_START_PPI] = (uint32_t)&NRF_SPIM0->TASKS_START,
        [SPI_END_PPI] = nrfx_gpiote_set_task_addr_get(HF_SPI_SELECT),
        [FRAME_END_PPI] = (uint32_t)&NRF_TIMER3->TASKS_STOP,
    };
    for (unsigned i = 0; i < PPI_CHANNEL_COUNT; i++)
        if (nrfx_ppi_channel_assign(channels[i], events[i], tasks[i]) != NRFX_SUCCESS) goto fail;
    if (nrfx_ppi_channel_fork_assign(channels[SPI_END_PPI], (uint32_t)&NRF_TIMER4->TASKS_COUNT) != NRFX_SUCCESS) goto fail;
    return true;
fail:
    rc522_tx_close();
    return false;
}

bool rc522_tx_send(const uint8_t *bytes, size_t length, uint8_t low) {
    if (!symbol_ready) return false;
    size_t symbols = rc522_tx_encode(bytes, length, low, sequence, sizeof(sequence));
    if (!symbols) return false;
    NRF_SPI0->ENABLE = 0;
    NRF_SPIM0->PSEL.SCK = HF_SPI_SCK;
    NRF_SPIM0->PSEL.MOSI = HF_SPI_MOSI;
    NRF_SPIM0->PSEL.MISO = HF_SPI_MISO;
    NRF_SPIM0->CONFIG = 0;
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
    NRF_SPIM0->SHORTS = 0;
    NRF_SPIM0->TXD.PTR = (uint32_t)sequence;
    NRF_SPIM0->TXD.MAXCNT = RC522_TX_WRITE_BYTES;
    NRF_SPIM0->TXD.LIST = SPIM_TXD_LIST_LIST_ArrayList;
    NRF_SPIM0->RXD.MAXCNT = 0;
    NRF_SPIM0->RXD.LIST = SPIM_RXD_LIST_LIST_Disabled;
    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->EVENTS_STOPPED = 0;
    NRF_SPIM0->ENABLE = SPIM_ENABLE_ENABLE_Enabled;
    NRF_TIMER3->TASKS_CLEAR = 1;
    NRF_TIMER3->CC[SYMBOL_PERIOD_CC] = SYMBOL_PERIOD_TICKS;
    NRF_TIMER3->CC[CHIP_SELECT_CC] = CHIP_SELECT_TICKS;
    NRF_TIMER3->CC[SPI_START_CC] = SPI_START_TICKS;
    NRF_TIMER3->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
    for (unsigned i = 0; i < SYMBOL_COMPARE_COUNT; i++) NRF_TIMER3->EVENTS_COMPARE[i] = 0;
    NRF_TIMER4->TASKS_CLEAR = 1;
    NRF_TIMER4->CC[0] = symbols;
    NRF_TIMER4->EVENTS_COMPARE[0] = 0;
    nrfx_timer_enable(&counter);
    nrfx_gpiote_out_task_enable(HF_SPI_SELECT);
    for (unsigned i = 0; i < PPI_CHANNEL_COUNT; i++) nrfx_ppi_channel_enable(channels[i]);
    uint32_t start = DWT->CYCCNT;
    nrfx_timer_enable(&symbol_timer);
    while (!NRF_TIMER4->EVENTS_COMPARE[0] && DWT->CYCCNT - start < TRANSMIT_TIMEOUT_CYCLES) {}
    bool complete = NRF_TIMER4->EVENTS_COMPARE[0] != 0;
    nrfx_timer_disable(&symbol_timer);
    nrfx_timer_disable(&counter);
    for (unsigned i = 0; i < PPI_CHANNEL_COUNT; i++) nrfx_ppi_channel_disable(channels[i]);
    NRF_SPIM0->TASKS_STOP = 1;
    start = DWT->CYCCNT;
    while (!NRF_SPIM0->EVENTS_STOPPED && DWT->CYCCNT - start < SPI_STOP_TIMEOUT_CYCLES) {}
    if (!NRF_SPIM0->EVENTS_STOPPED) NVIC_SystemReset();
    NRF_SPIM0->ENABLE = 0;
    NRF_SPIM0->TXD.LIST = SPIM_TXD_LIST_LIST_Disabled;
    nrfx_gpiote_out_task_disable(HF_SPI_SELECT);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
    return complete;
}
