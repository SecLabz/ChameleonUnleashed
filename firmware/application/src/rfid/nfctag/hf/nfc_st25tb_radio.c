/* SPDX-License-Identifier: GPL-3.0-only */
#include "nfc_st25tb.h"
#include "st25tb/model.h"
#include "st25tb/receiver.h"
#include "rc522.h"
#include "rf_timeslot.h"
#include "nrfx_power.h"
#include "nrfx_ppi.h"
#include "nrfx_timer.h"
#include "../../reader/rf_workspace.h"
#include "rfid_main.h"
#include "bsp_delay.h"
#include "bsp_wdt.h"
#include "ble_main.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include <string.h>

#define STB_BATCH 128u
#define STB_MIN_BATCH_BYTES 32u
#define STB_DMA_BANK_COUNT 3u
#define STB_DMA_BANK_BYTES 8192u
#define STB_DMA_GUARD_BYTES 32u
#define STB_DMA_LAG_BYTES 16u
#define STB_DMA_START_MARGIN_TICKS 32u
#define STB_TX_SETUP_RESERVE_US 10u

/* DWT uses the 64 MHz CPU clock; TIMER4 timestamps the 1 us ADC bytes at 16 MHz.
 * The receiver consumes pairs on a 2 us clock. Keep conversions explicit. */
enum {
    STB_CPU_CYCLES_PER_US = 64,
    STB_TIMER_TICKS_PER_US = 16,
    STB_RECEIVER_PAIR_TICKS = 32,
    STB_SPI_TIMEOUT_CYCLES = 1000 * STB_CPU_CYCLES_PER_US,
    STB_DMA_EVENT_TIMEOUT_CYCLES = 100 * STB_CPU_CYCLES_PER_US,
    STB_NFCT_READY_TIMEOUT_CYCLES = 100 * STB_CPU_CYCLES_PER_US,
    STB_WINDOW_CLEANUP_CYCLES = 2000 * STB_CPU_CYCLES_PER_US,
    STB_WINDOW_WORK_GUARD_CYCLES = 2000 * STB_CPU_CYCLES_PER_US,
    /* ST25TB needs at least 128/fs before its 16-ETU phase training.
     * The 2 us GPIO launch margin brings this quiet interval to about 160 us. */
    STB_RESPONSE_WAIT_US = 158,
    STB_RESPONSE_WAIT_TIMEOUT_CYCLES = 1000 * STB_CPU_CYCLES_PER_US,
    STB_FIELD_PROBE_CYCLES = 100 * STB_CPU_CYCLES_PER_US,
    STB_FIELD_LOSS_CYCLES = 3000 * STB_CPU_CYCLES_PER_US,
    STB_FIELD_DROP_CYCLES = 2000 * STB_CPU_CYCLES_PER_US,
    STB_FIELD_PRESENT_PROBE_CYCLES = 200000 * STB_CPU_CYCLES_PER_US,
    STB_FIELD_ABSENT_PROBE_CYCLES = 15000 * STB_CPU_CYCLES_PER_US,
    STB_ACTIVE_WINDOW_US = 2000000,
    STB_IDLE_WINDOW_US = 500000,
    STB_CONNECTED_WINDOW_US = 25000,
    STB_TX_BYTE_BUDGET_US = 100,
    STB_TX_GUARD_US = 2500,
    STB_POWER_SETTLE_MS = 1,
    STB_RESET_SETTLE_US = 500,
    STB_RESET_SETTLE_MS = 10,
    STB_RECEIVE_SETTLE_US = 100,
};

/* Qualified RC522 settings for field-off ADC reception and GPIO modulation. */
enum {
    RC522_SPI_ADDRESS_MASK = 0x7e,
    RC522_SPI_READ = 0x80,
    RC522_FIELD_OFF = 0x80,
    RC522_FIELD_ENABLE_MASK = 3,
    STB_TX_TYPE_B = 3,
    STB_RX_MODE = 0x14,
    STB_TX_AUTO = 0,
    STB_TX_CONSTANT_CARRIER = 0x30,
    STB_RX_SELECT = 0x84,
    STB_RX_THRESHOLD = 0x84,
    STB_DEMOD = 0x4d,
    STB_INITIAL_RF_GAIN = 0x48,
    STB_RF_GAIN = 0x78,
    STB_N_CONDUCTANCE = 0x88,
    STB_CW_CONDUCTANCE = 32,
    STB_MOD_CONDUCTANCE = 16,
    STB_ADC_READ_COMMAND = ((TestADCReg << 1) & RC522_SPI_ADDRESS_MASK) | RC522_SPI_READ,
    NFC_A_TX_AUTO = 0x40,
    NFC_A_MODE = 0x3d,
};

enum {
    STB_TX_BITS_CAPACITY = 160,
    STB_TX_LEADING_ONES = 16,
    STB_TX_SOF_ZEROS = 11,
    STB_TX_EOF_ZEROS = 11,
    STB_TX_HALVES_PER_BIT = 16,
    STB_TX_START_MARGIN_CYCLES = 128,
    STB_TX_HALF_PERIOD_BASE_CYCLES = 37,
    STB_TX_HALF_PERIOD_FRACTION = 257,
    STB_TX_HALF_PERIOD_DENOMINATOR = 339,
};

_Static_assert(STB_TX_LEADING_ONES + STB_TX_SOF_ZEROS + 2 + 10 * ST25TB_RESPONSE_MAX + STB_TX_EOF_ZEROS + 2 <=
                   STB_TX_BITS_CAPACITY,
               "ST25TB response exceeds transmit scratch");
static uint8_t *dma_rx;

static const uint32_t stb_t0_ticks = STB_RESPONSE_WAIT_US * STB_TIMER_TICKS_PER_US;
/* Sticky health flag for SPI, DMA/PPI and response scheduling within one open. */
static bool radio_ok;
static bool reader_open;
static bool stb_responded;
extern bool g_is_reader_antenna_on;

static void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;
    while (DWT->CYCCNT - start < us * STB_CPU_CYCLES_PER_US) {
    }
}

static uint8_t spi_transfer(uint8_t value) {
    if (!radio_ok) {
        return 0;
    }
    NRF_SPI0->EVENTS_READY = 0;
    NRF_SPI0->TXD = value;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPI0->EVENTS_READY) {
        if (DWT->CYCCNT - start > STB_SPI_TIMEOUT_CYCLES) {
            radio_ok = false;
            return 0;
        }
    }
    return NRF_SPI0->RXD;
}

static void write_register(uint8_t reg, uint8_t value) {
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    spi_transfer((reg << 1) & RC522_SPI_ADDRESS_MASK);
    spi_transfer(value);
    nrf_gpio_pin_set(HF_SPI_SELECT);
}

static uint8_t read_register(uint8_t reg) {
    uint8_t address = ((reg << 1) & RC522_SPI_ADDRESS_MASK) | RC522_SPI_READ;
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    spi_transfer(address);
    uint8_t value = spi_transfer(address);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    return value;
}

static void open_reader(void) {
    nrf_gpio_pin_set(READER_POWER);
    bsp_delay_ms(STB_POWER_SETTLE_MS);
    NRF_SPI0->ENABLE = 0;
    NRF_SPI0->PSELSCK = HF_SPI_SCK;
    NRF_SPI0->PSELMOSI = HF_SPI_MOSI;
    NRF_SPI0->PSELMISO = HF_SPI_MISO;
    nrf_gpio_cfg_output(HF_SPI_SELECT);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    nrf_gpio_cfg_output(HF_SPI_SCK);
    nrf_gpio_pin_clear(HF_SPI_SCK);
    nrf_gpio_cfg_output(HF_SPI_MOSI);
    nrf_gpio_cfg_input(HF_SPI_MISO, NRF_GPIO_PIN_NOPULL);
    NRF_SPI0->FREQUENCY = SPI_FREQUENCY_FREQUENCY_M8;
    NRF_SPI0->CONFIG = 0;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
    radio_ok = true;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    write_register(CommandReg, PCD_RESET);
    delay_us(STB_RESET_SETTLE_US);
    write_register(TxModeReg, STB_TX_TYPE_B);
    write_register(RxModeReg, STB_RX_MODE);
    write_register(TxAutoReg, STB_TX_AUTO);
    write_register(TxSelReg, STB_TX_CONSTANT_CARRIER);
    write_register(RxSelReg, STB_RX_SELECT);
    write_register(RxThresholdReg, STB_RX_THRESHOLD);
    write_register(DemodReg, STB_DEMOD);
    write_register(RFCfgReg, STB_INITIAL_RF_GAIN);
    write_register(GsNReg, STB_N_CONDUCTANCE);
    write_register(CWGsCfgReg, STB_CW_CONDUCTANCE);
    write_register(ModGsCfgReg, STB_MOD_CONDUCTANCE);
    write_register(TxControlReg, RC522_FIELD_OFF);
    g_is_reader_antenna_on = false;
    delay_us(STB_RESET_SETTLE_US);
}

static void close_reader(void) {
    write_register(CommandReg, PCD_IDLE);
    write_register(TxControlReg, RC522_FIELD_OFF);
    /* Restore the NFC-A defaults with bounded SPI, including on a failed read. */
    write_register(CommandReg, PCD_RESET);
    bsp_delay_ms(STB_RESET_SETTLE_MS);
    write_register(TxControlReg, RC522_FIELD_OFF);
    write_register(TModeReg, 0);
    write_register(TxAutoReg, NFC_A_TX_AUTO);
    write_register(ModeReg, NFC_A_MODE);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = 0;
    g_is_reader_antenna_on = false;
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_pin_set(HF_ANT_SEL);
}

/* RF workspace is exclusive to this priority0 emulation window. */
static uint8_t *stb_dma_at(unsigned bank) {
    return dma_rx + bank * STB_DMA_BANK_BYTES;
}

__attribute__((section(".data.st25tb"), noinline, aligned(4))) static void
stb_transmit(const uint8_t *data, unsigned length, uint32_t reply_at) {
    /* Active NFCT suppresses this board's GPIO load modulation. */
    NRF_NFCT->TASKS_DISABLE = 1;
    __DSB();
    uint8_t bits[STB_TX_BITS_CAPACITY];
    unsigned count = 0;
    for (unsigned n = 0; n < STB_TX_LEADING_ONES; n++) {
        bits[count++] = 1;
    }
    for (unsigned n = 0; n < STB_TX_SOF_ZEROS; n++) {
        bits[count++] = 0;
    }
    bits[count++] = 1;
    bits[count++] = 1;
    for (unsigned n = 0; n < length; n++) {
        bits[count++] = 0;
        for (unsigned b = 0; b < 8; b++) {
            bits[count++] = (data[n] >> b) & 1;
        }
        bits[count++] = 1;
    }
    for (unsigned n = 0; n < STB_TX_EOF_ZEROS; n++) {
        bits[count++] = 0;
    }
    bits[count++] = 1;
    bits[count++] = 1;
    uint32_t mask = 1u << (HF_ANT_SEL & 31u);
    uint32_t deadline = rf_timeslot_deadline() - STB_WINDOW_CLEANUP_CYCLES;
    uint32_t wait_start = DWT->CYCCNT;
    for (;;) {
        NRF_TIMER4->TASKS_CAPTURE[1] = 1;
        __DSB();
        if ((int32_t)(NRF_TIMER4->CC[1] - reply_at) >= 0) {
            break;
        }
        if (DWT->CYCCNT - wait_start > STB_RESPONSE_WAIT_TIMEOUT_CYCLES ||
            (int32_t)(deadline - DWT->CYCCNT) < 0) {
            radio_ok = false;
            return;
        }
    }
    uint32_t next = DWT->CYCCNT + STB_TX_START_MARGIN_CYCLES;
    uint32_t fraction = 0;
    for (unsigned bit = 0; bit < count; bit++) {
        uint8_t bit_value = bits[bit];
        for (unsigned half = 0; half < STB_TX_HALVES_PER_BIT; half++) {
            volatile uint32_t *out = ((half & 1u) != bit_value) ? &NRF_P1->OUTSET : &NRF_P1->OUTCLR;
            while ((int32_t)(DWT->CYCCNT - next) < 0) {
            }
            *out = mask;
            /* 64 MHz / (2 * 847.5 kHz): 37 + 257/339 cycles. */
            next += STB_TX_HALF_PERIOD_BASE_CYCLES;
            fraction += STB_TX_HALF_PERIOD_FRACTION;
            if (fraction >= STB_TX_HALF_PERIOD_DENOMINATOR) {
                fraction -= STB_TX_HALF_PERIOD_DENOMINATOR;
                next++;
            }
        }
    }
    while ((int32_t)(DWT->CYCCNT - next) < 0) {
    }
    NRF_P1->OUTCLR = mask;
    stb_responded = true;
}

static uint32_t stb_absence_started;
static bool stb_field_known;
static bool stb_absence_pending;

/* Returns true when the field appears after being unknown or absent. */
static bool stb_probe_field(void) {
    /* FIELDPRESENT is valid only after ACTIVATE/READY, on the tag path. */
    if (!NRF_NFCT->EVENTS_READY) {
        return false;
    }
    nrf_gpio_pin_set(HF_ANT_SEL);
    /* Sample from the switch: one read after a fixed 100 us missed a present field. */
    uint32_t start = DWT->CYCCNT;
    bool present;
    do {
        present = (NRF_NFCT->FIELDPRESENT & 1u) != 0;
    } while (!present && DWT->CYCCNT - start < STB_FIELD_PROBE_CYCLES);
    nrf_gpio_pin_clear(HF_ANT_SEL);
    if (present) {
        bool arrived = !stb_field_known || stb_absence_pending;
        stb_field_known = true;
        stb_absence_pending = false;
        return arrived;
    }
    st25tb_receiver_reset();
    uint32_t now = DWT->CYCCNT;
    if (stb_field_known) {
        if (!stb_absence_pending) {
            stb_absence_started = now;
            stb_absence_pending = true;
        } else if (now - stb_absence_started > STB_FIELD_LOSS_CYCLES) {
            st25tb_field_lost();
            stb_field_known = false;
            stb_absence_pending = false;
            stb_responded = false;
        }
    }
    return false;
}

_Static_assert(STB_DMA_BANK_COUNT * STB_DMA_BANK_BYTES + 2 * STB_DMA_GUARD_BYTES <= RF_HF_SNIFF_BYTES,
               "SPIM3 banks exceed RF workspace");
static const nrfx_timer_t stb_counter = NRFX_TIMER_INSTANCE(3);
static const nrfx_timer_t stb_clock = NRFX_TIMER_INSTANCE(4);
static nrf_ppi_channel_t stb_counter_ppi;
static nrf_ppi_channel_t stb_start_ppi;
static bool stb_start_owned;
static bool stb_streaming;
static unsigned stb_queued;
static uint32_t stb_bank_start[STB_DMA_BANK_COUNT];

static void stb_timer_handler(nrf_timer_event_t event, void *context) {
    (void)event;
    (void)context;
}

static void stb_dma_release(void);

static bool stb_dma_acquire(void) {
    nrfx_timer_config_t config = NRFX_TIMER_DEFAULT_CONFIG;
    config.mode = NRF_TIMER_MODE_COUNTER;
    config.bit_width = NRF_TIMER_BIT_WIDTH_32;
    if (nrfx_timer_init(&stb_counter, &config, stb_timer_handler) != NRFX_SUCCESS) {
        return false;
    }
    config.mode = NRF_TIMER_MODE_TIMER;
    config.frequency = NRF_TIMER_FREQ_16MHz;
    if (nrfx_timer_init(&stb_clock, &config, stb_timer_handler) != NRFX_SUCCESS) {
        nrfx_timer_uninit(&stb_counter);
        return false;
    }
    if (nrfx_ppi_channel_alloc(&stb_counter_ppi) != NRFX_SUCCESS) {
        nrfx_timer_uninit(&stb_clock);
        nrfx_timer_uninit(&stb_counter);
        return false;
    }
    if (nrfx_ppi_channel_assign(stb_counter_ppi, (uint32_t)&NRF_SPIM3->EVENTS_END,
                                (uint32_t)&NRF_TIMER3->TASKS_COUNT) != NRFX_SUCCESS ||
        nrfx_ppi_channel_fork_assign(stb_counter_ppi, (uint32_t)&NRF_TIMER4->TASKS_CAPTURE[2]) != NRFX_SUCCESS) {
        stb_dma_release();
        return false;
    }
    if (nrfx_ppi_channel_alloc(&stb_start_ppi) != NRFX_SUCCESS) {
        stb_dma_release();
        return false;
    }
    stb_start_owned = true;
    if (nrfx_ppi_channel_assign(stb_start_ppi, (uint32_t)&NRF_SPIM3->EVENTS_STARTED,
                                (uint32_t)&NRF_TIMER4->TASKS_CAPTURE[0]) != NRFX_SUCCESS ||
        nrfx_ppi_channel_fork_assign(stb_start_ppi, 0) != NRFX_SUCCESS) {
        stb_dma_release();
        return false;
    }
    nrfx_timer_enable(&stb_clock);
    nrfx_timer_enable(&stb_counter);
    return true;
}

static void stb_dma_release(void) {
    if (stb_start_owned) {
        nrfx_ppi_channel_disable(stb_start_ppi);
        nrfx_ppi_channel_fork_assign(stb_start_ppi, 0);
        nrfx_ppi_channel_free(stb_start_ppi);
        stb_start_owned = false;
    }
    nrfx_ppi_channel_disable(stb_counter_ppi);
    nrfx_ppi_channel_fork_assign(stb_counter_ppi, 0);
    nrfx_ppi_channel_free(stb_counter_ppi);
    nrfx_timer_uninit(&stb_clock);
    nrfx_timer_uninit(&stb_counter);
}

static uint32_t stb_completed(void) {
    NRF_TIMER3->TASKS_CAPTURE[0] = 1;
    __DSB();
    return NRF_TIMER3->CC[0];
}

static void stb_spi_disable(void) {
    NRF_SPIM3->ENABLE = 0;
    /* Match the SDK's SPIM3 erratum195 disable sequence. */
    *(volatile uint32_t *)0x4002f004u = 1;
}

static void stb_spi_stop(void) {
    NRF_SPIM3->SHORTS = 0;
    nrfx_ppi_channel_disable(stb_start_ppi);
    nrfx_ppi_channel_disable(stb_counter_ppi);
    if (!stb_streaming) {
        nrf_gpio_pin_set(HF_SPI_SELECT);
        return;
    }
    NRF_SPIM3->EVENTS_STOPPED = 0;
    NRF_SPIM3->TASKS_STOP = 1;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPIM3->EVENTS_STOPPED) {
        if (DWT->CYCCNT - start > STB_DMA_EVENT_TIMEOUT_CYCLES) {
            radio_ok = false;
            break;
        }
    }
    if (NRF_SPIM3->EVENTS_STOPPED) {
        stb_streaming = false;
    }
    nrf_gpio_pin_set(HF_SPI_SELECT);
}

static void stb_stream_start(void) {
    if (!radio_ok) {
        return;
    }
    NRF_TIMER3->TASKS_CLEAR = 1;
    NRF_SPIM3->RXD.PTR = (uint32_t)stb_dma_at(0);
    NRF_SPIM3->EVENTS_END = 0;
    NRF_SPIM3->EVENTS_STARTED = 0;
    NRF_SPIM3->STALLSTAT = 0;
    NRF_SPIM3->SHORTS = SPIM_SHORTS_END_START_Msk;
    if (nrfx_ppi_channel_enable(stb_counter_ppi) != NRFX_SUCCESS ||
        nrfx_ppi_channel_enable(stb_start_ppi) != NRFX_SUCCESS) {
        radio_ok = false;
        return;
    }
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    NRF_SPIM3->TASKS_START = 1;
    stb_streaming = true;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPIM3->EVENTS_STARTED) {
        if (DWT->CYCCNT - start > STB_DMA_EVENT_TIMEOUT_CYCLES) {
            radio_ok = false;
            return;
        }
    }
    stb_bank_start[0] = NRF_TIMER4->CC[0];
    stb_queued = 1;
    NRF_SPIM3->RXD.PTR = (uint32_t)stb_dma_at(1);
    NRF_SPIM3->EVENTS_STARTED = 0;
}

static void stb_stream_configure(void) {
    uint8_t *storage = rf_hf_sniff_workspace();
    dma_rx = storage + STB_DMA_GUARD_BYTES;
    memset(storage, 0xa5, STB_DMA_GUARD_BYTES);
    memset(dma_rx + STB_DMA_BANK_COUNT * STB_DMA_BANK_BYTES, 0xa5, STB_DMA_GUARD_BYTES);
    NRF_SPI0->ENABLE = 0;
    NRF_SPIM0->ENABLE = 0;
    NRF_SPIM3->PSEL.SCK = HF_SPI_SCK;
    NRF_SPIM3->PSEL.MOSI = HF_SPI_MOSI;
    NRF_SPIM3->PSEL.MISO = HF_SPI_MISO;
    NRF_SPIM3->CONFIG = 0;
    NRF_SPIM3->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
    NRF_SPIM3->ORC = STB_ADC_READ_COMMAND;
    /* ORC emits F6 without a TX RAM channel (erratum198). */
    NRF_SPIM3->TXD.PTR = 0;
    NRF_SPIM3->TXD.MAXCNT = 0;
    NRF_SPIM3->TXD.LIST = 0;
    NRF_SPIM3->RXD.MAXCNT = STB_DMA_BANK_BYTES;
    NRF_SPIM3->RXD.LIST = 0;
    NRF_SPIM3->ENABLE = SPIM_ENABLE_ENABLE_Enabled;
    stb_streaming = false;
    nrf_gpio_cfg(HF_ANT_SEL, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
    nrf_gpio_pin_clear(HF_ANT_SEL);
}

/* Publish the next descriptor once per 8192-byte transaction. Current RAM is
 * read with a conservative 16-byte lag behind the in-flight DMA frontier.
 * Stalls, ambiguous descriptors or old banks are refused before decoding. */
static unsigned stb_available(unsigned handled) {
    uint32_t completed = stb_completed();
    if (completed < handled || completed - handled >= STB_DMA_BANK_COUNT || completed > stb_queued) {
        radio_ok = false;
        return 0;
    }
    if (NRF_SPIM3->STALLSTAT) {
        radio_ok = false;
        return 0;
    }
    if (completed == stb_queued) {
        uint32_t start = DWT->CYCCNT;
        while (!NRF_SPIM3->EVENTS_STARTED) {
            if (DWT->CYCCNT - start > STB_DMA_EVENT_TIMEOUT_CYCLES) {
                radio_ok = false;
                return 0;
            }
        }
        uint32_t ended = NRF_TIMER4->CC[2];
        uint32_t phase = ended - stb_bank_start[(completed - 1u) % STB_DMA_BANK_COUNT] -
                         STB_DMA_BANK_BYTES * STB_TIMER_TICKS_PER_US;
        if (NRF_SPIM3->RXD.AMOUNT != STB_DMA_BANK_BYTES || phase > STB_DMA_START_MARGIN_TICKS) {
            radio_ok = false;
            return 0;
        }
        stb_bank_start[completed % STB_DMA_BANK_COUNT] = NRF_TIMER4->CC[0];
        NRF_SPIM3->EVENTS_STARTED = 0;
        NRF_SPIM3->RXD.PTR = (uint32_t)stb_dma_at((completed + 1u) % STB_DMA_BANK_COUNT);
        stb_queued = completed + 1u;
        if (stb_completed() != completed) {
            radio_ok = false;
            return 0;
        }
    }
    if (completed > handled) {
        return STB_DMA_BANK_BYTES;
    }
    NRF_TIMER4->TASKS_CAPTURE[1] = 1;
    __DSB();
    uint32_t elapsed = NRF_TIMER4->CC[1] - stb_bank_start[handled % STB_DMA_BANK_COUNT];
    unsigned bytes = elapsed / STB_TIMER_TICKS_PER_US;
    if (bytes > STB_DMA_BANK_BYTES + STB_DMA_EVENT_TIMEOUT_CYCLES / STB_CPU_CYCLES_PER_US) {
        radio_ok = false;
        return 0;
    }
    if (bytes > STB_DMA_BANK_BYTES) {
        bytes = STB_DMA_BANK_BYTES;
    }
    return bytes > STB_DMA_LAG_BYTES ? bytes - STB_DMA_LAG_BYTES : 0;
}

static void stb_window_restore(uint32_t interrupts) {
    NRF_NFCT->TASKS_DISABLE = 1;
    NRF_NFCT->EVENTS_READY = 0;
    NRF_NFCT->EVENTS_FIELDDETECTED = 0;
    NRF_NFCT->EVENTS_FIELDLOST = 0;
    NRF_NFCT->INTENSET = interrupts;
    stb_spi_disable();
    stb_streaming = false;
    NRF_SPIM3->SHORTS = 0;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
    nrf_gpio_cfg_output(HF_ANT_SEL);
    nrf_gpio_pin_set(HF_ANT_SEL);
}

static void stb_window(void) {
    uint32_t deadline = rf_timeslot_deadline() - STB_WINDOW_CLEANUP_CYCLES;
    uint32_t interrupts = NRF_NFCT->INTENSET;
    NRF_NFCT->INTENCLR = 0xffffffffu;
    NRF_NFCT->EVENTS_READY = 0;
    NRF_NFCT->EVENTS_FIELDDETECTED = 0;
    NRF_NFCT->EVENTS_FIELDLOST = 0;
    NRF_NFCT->TASKS_SENSE = 1;
    NRF_NFCT->TASKS_ACTIVATE = 1;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_NFCT->EVENTS_READY && DWT->CYCCNT - start < STB_NFCT_READY_TIMEOUT_CYCLES) {
    }
    stb_probe_field();
    write_register(CommandReg, PCD_RECEIVE);
    delay_us(STB_RECEIVE_SETTLE_US);
    stb_stream_configure();
    unsigned handled = 0;
    unsigned n = 1;
    bool have_carry = false;
    uint8_t carry = 0;
    uint8_t request[ST25TB_REQUEST_MAX];
    uint8_t reply[ST25TB_RESPONSE_MAX];
    uint32_t last_probe = DWT->CYCCNT;
    uint32_t field_seen = last_probe;
    bool field_readable = true;
    st25tb_receiver_reset();
    stb_stream_start();
    uint32_t sample_origin = stb_bank_start[0];
    while (radio_ok && (int32_t)(deadline - DWT->CYCCNT) > STB_WINDOW_WORK_GUARD_CYCLES) {
        if (nrf_gpio_pin_read(BUTTON_1) || nrf_gpio_pin_read(BUTTON_2)) {
            break;
        }
        unsigned available = stb_available(handled);
        if (!radio_ok) {
            break;
        }
        if (n == STB_DMA_BANK_BYTES) {
            handled++;
            n = 0;
            continue;
        }
        if (available <= n) {
            continue;
        }
        /* Nominal fill time after the first available byte is 31 us at 1 byte/us.
         * Finish partial banks immediately, including their carry byte.
         * Publish receiver time only when input is actually decoded.
         * Fault, button and timeslot guards still run on every iteration. */
        if (!have_carry && available - n < STB_MIN_BATCH_BYTES && available != STB_DMA_BANK_BYTES) {
            continue;
        }
        unsigned ready = handled % STB_DMA_BANK_COUNT;
        uint32_t bank_start = stb_bank_start[ready];
        unsigned length = 0;
        unsigned pair_end = n + (have_carry ? 1u : 2u);
        st25tb_receiver_time((uint32_t)(bank_start + pair_end * STB_TIMER_TICKS_PER_US - sample_origin) /
                            STB_RECEIVER_PAIR_TICKS);
        if (have_carry) {
            have_carry = false;
            st25tb_receiver_pair(carry, stb_dma_at(ready)[n++], request, &length);
        } else {
            unsigned bytes = (available - n) & ~1u;
            if (bytes > STB_BATCH) {
                bytes = STB_BATCH;
            }
            if (!bytes) {
                if (available == STB_DMA_BANK_BYTES) {
                    carry = stb_dma_at(ready)[n++];
                    have_carry = true;
                }
                continue;
            }
            n += st25tb_receiver_batch(stb_dma_at(ready) + n, bytes, request, &length);
        }
        bool restart = false;
        bool replied = false;
        if (length) {
            stb_field_known = true;
            stb_absence_pending = false;
            /* Keep the two-us start-phase margin when mapping EOF to the
             * hardware response clock. */
            uint32_t eof_tick = bank_start + n * STB_TIMER_TICKS_PER_US + STB_DMA_START_MARGIN_TICKS;
            NRF_TIMER4->TASKS_CAPTURE[1] = 1;
            __DSB();
            if ((int32_t)(eof_tick + stb_t0_ticks - NRF_TIMER4->CC[1]) <
                (int32_t)(STB_TX_SETUP_RESERVE_US * STB_TIMER_TICKS_PER_US)) {
                continue;
            }
            if (!stb_available(handled)) {
                continue;
            }
            unsigned bytes = st25tb_reply(request, length, reply);
            if (!bytes) {
                continue;
            }
            if ((int32_t)(deadline - DWT->CYCCNT) <
                (int32_t)((bytes * STB_TX_BYTE_BUDGET_US + STB_TX_GUARD_US) * STB_CPU_CYCLES_PER_US)) {
                continue;
            }
            NRF_TIMER4->TASKS_CAPTURE[1] = 1;
            __DSB();
            if ((int32_t)(eof_tick + stb_t0_ticks - NRF_TIMER4->CC[1]) <
                (int32_t)(STB_TX_SETUP_RESERVE_US * STB_TIMER_TICKS_PER_US) || !stb_available(handled)) {
                continue;
            }
            stb_spi_stop();
            if (!radio_ok) {
                break;
            }
            uint32_t irq = __get_PRIMASK();
            __disable_irq();
            stb_transmit(reply, bytes, eof_tick + stb_t0_ticks);
            NRF_NFCT->EVENTS_READY = 0;
            NRF_NFCT->TASKS_SENSE = 1;
            NRF_NFCT->TASKS_ACTIVATE = 1;
            __set_PRIMASK(irq);
            last_probe = DWT->CYCCNT;
            restart = true;
            replied = true;
        }
        bool present = stb_field_known && !stb_absence_pending;
        if (NRF_NFCT->FIELDPRESENT & 1u) {
            field_seen = DWT->CYCCNT;
        }
        bool dropped = field_readable && present && DWT->CYCCNT - field_seen > STB_FIELD_DROP_CYCLES;
        uint32_t interval = present ? STB_FIELD_PRESENT_PROBE_CYCLES : STB_FIELD_ABSENT_PROBE_CYCLES;
        if (!restart && NRF_NFCT->EVENTS_READY &&
            (dropped || (DWT->CYCCNT - last_probe > interval && (!present || st25tb_receiver_idle())))) {
            restart = true;
        }
        if (!restart) {
            continue;
        }
        stb_spi_stop();
        if (!radio_ok) {
            break;
        }
        if (!replied) {
            if (stb_probe_field()) {
                break;
            }
            if (dropped && !stb_absence_pending) {
                field_readable = false;
            }
            last_probe = DWT->CYCCNT;
        }
        handled = 0;
        n = 1;
        have_carry = false;
        st25tb_receiver_reset();
        if (replied) {
            st25tb_receiver_settle();
        }
        stb_stream_start();
        sample_origin = stb_bank_start[0];
        field_seen = DWT->CYCCNT;
    }
    stb_spi_stop();
    for (unsigned i = 0; i < STB_DMA_GUARD_BYTES; i++) {
        if (dma_rx[-(int)STB_DMA_GUARD_BYTES + (int)i] != 0xa5 ||
            dma_rx[STB_DMA_BANK_COUNT * STB_DMA_BANK_BYTES + i] != 0xa5) {
            radio_ok = false;
        }
    }
    stb_window_restore(interrupts);
}

bool nfc_st25tb_radio_poll(bool *field_present, bool active) {
    *field_present = false;
    if (!st25tb_loaded()) {
        return false;
    }
    if (!reader_open) {
        open_reader();
        write_register(RFCfgReg, STB_RF_GAIN);
        reader_open = true;
    }
    if (!stb_dma_acquire()) {
        nfc_st25tb_radio_stop();
        return false;
    }
    bool ready = radio_ok && (read_register(TxControlReg) & RC522_FIELD_ENABLE_MASK) == 0 && radio_ok;
    bool resume = false;
    bool paused = ready && advertising_pause(&resume);
    if (paused) {
        bsp_wdt_feed();
    }
    stb_responded = false;
    uint32_t window_us = STB_CONNECTED_WINDOW_US;
    /* Keep uninterrupted RF windows in standalone mode; USB needs regular service. */
    if (paused && nrfx_power_usbstatus_get() == NRFX_POWER_USB_STATE_DISCONNECTED) {
        window_us = active ? STB_ACTIVE_WINDOW_US : STB_IDLE_WINDOW_US;
    }
    bool granted = ready && rf_timeslot_run(window_us, true, paused, stb_window);
    if (resume) {
        advertising_start(false);
    }
    *field_present = stb_field_known || stb_responded;
    bool sampled = granted && radio_ok;
    stb_dma_release();
    if (!ready || !radio_ok || (granted && !*field_present)) {
        close_reader();
        reader_open = false;
    }
    return sampled;
}

void nfc_st25tb_radio_stop(void) {
    if (!reader_open) {
        return;
    }
    close_reader();
    reader_open = false;
    stb_field_known = false;
    stb_absence_pending = false;
}
