/* SPDX-License-Identifier: GPL-3.0-only */
#include "rc522_raw.h"
#include "rc522_tx.h"
#include "nfcb_tx.h"
#include "nfcv_reader_codec.h"
#include "rc522.h"
#include "rfid_main.h"
#include "bsp_delay.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include <string.h>

#define NFCB_IQ_SAMPLES 8192u
#define DMA_MAX_SAMPLES (UINT16_MAX - 1u)
#define CPU_CYCLES_PER_US 64u
#define SPI_TIMEOUT_CYCLES 64000u
#define READER_RESET_TIMEOUT_CYCLES 640000u
#define NFCB_DMA_TIMEOUT_CYCLES 640000u
#define NFCV_DMA_TIMEOUT_CYCLES 960000u
/* Each ETU spans 128 carrier cycles at 13.56 MHz; the CPU runs at 64 MHz. */
#define ETU_CYCLES_NUMERATOR 819200u
#define ETU_CYCLES_DENOMINATOR 1356u
#define TX_START_MARGIN_CYCLES 640u
#define FRONTEND_SETTLE_US 1000u
#define READER_POWER_SETTLE_MS 10u
#define READER_RESET_SETTLE_MS 10u
#define READER_FIELD_SETTLE_MS 10u
#define NFCB_FIELD_SETTLE_MS 5u
#define ST25TB_WRITE_COMMIT_US 6000u
#define SEND_SETTLE_US 200u
/* Live decoding trails the 1 us/byte ADC stream by the address byte and a margin,
 * and assumes a 1.5% slower stream so late bytes read as zeroed silence. */
#define LIVE_LAG_SAMPLES 3u
#define LIVE_CYCLES_PER_SAMPLE (CPU_CYCLES_PER_US + 1u)
#define TEAROFF_IRQ_GUARD_CYCLES 128u

/* Qualified frontend register settings for software TX and ADC I/Q RX. */
enum {
    RC522_SPI_ADDRESS_MASK = 0x7e,
    RC522_SPI_READ = 0x80,
    RC522_POWER_DOWN = 0x10,
    RC522_CLEAR_IRQ_FLAGS = 0x7f,
    RC522_FLUSH_FIFO = 0x80,
    RC522_FIELD_OFF = 0x80,
    RC522_FIELD_ON = 0x83,
    RC522_TX_TYPE_B = 3,
    RC522_TX_CONSTANT_CARRIER = 0x30,
    SOFTWARE_RX_MODE = 0x14,
    SOFTWARE_RX_SELECT = 0x84,
    SOFTWARE_RX_THRESHOLD = 0x84,
    SOFTWARE_DEMOD = 0x4d,
    SOFTWARE_RF_GAIN = 0x48,
    SOFTWARE_N_CONDUCTANCE = 0x88,
    NFC_A_TX_AUTO = 0x40,
    NFC_A_MODE = 0x3d,
};

static uint8_t dma_address = ((TestADCReg << 1) & RC522_SPI_ADDRESS_MASK) | RC522_SPI_READ;
static uint8_t *dma_rx;
static uint32_t *dma_cycles, dma_measured, dma_started;
static unsigned dma_count;
static bool dma_active;
static bool spi_ok, tx_acquired, hardware_tx;
static rc522_raw_protocol_t frontend_protocol;
static uint8_t nfcb_tx_low;
static uint32_t tearoff_delay_us, tearoff_delta;
#define ST25TB_TEAROFF_FIELD_OFF_US 10u
volatile uint32_t hf_radio_tearoff_last_off_cycles;
extern bool g_is_reader_antenna_on;

static void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;
    while (DWT->CYCCNT - start < us * CPU_CYCLES_PER_US) {
    }
}

static uint8_t spi_transfer(uint8_t value) {
    if (!spi_ok) return 0;
    NRF_SPI0->EVENTS_READY = 0;
    NRF_SPI0->TXD = value;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPI0->EVENTS_READY) {
        if (DWT->CYCCNT - start > SPI_TIMEOUT_CYCLES) {
            spi_ok = false;
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
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    spi_transfer((reg << 1) | RC522_SPI_READ);
    uint8_t value = spi_transfer(0);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    return value;
}

static void wait_reset(void) {
    uint32_t start = DWT->CYCCNT;
    while (read_register(CommandReg) & RC522_POWER_DOWN) {
        if (!spi_ok || DWT->CYCCNT - start > READER_RESET_TIMEOUT_CYCLES) { spi_ok = false; return; }
    }
}

static void start_dma(unsigned count) {
    if (!spi_ok || !count || count > DMA_MAX_SAMPLES) {
        spi_ok = false;
        return;
    }
    dma_measured = DWT->CYCCNT;
    NRF_SPI0->ENABLE = 0;
    NRF_SPIM0->PSEL.SCK = HF_SPI_SCK;
    NRF_SPIM0->PSEL.MOSI = HF_SPI_MOSI;
    NRF_SPIM0->PSEL.MISO = HF_SPI_MISO;
    NRF_SPIM0->CONFIG = 0;
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
    NRF_SPIM0->TXD.PTR = (uint32_t)&dma_address;
    NRF_SPIM0->TXD.MAXCNT = 1;
    NRF_SPIM0->ORC = dma_address;
    NRF_SPIM0->RXD.PTR = (uint32_t)dma_rx;
    /* Discard the RX byte clocked while sending the register address. */
    NRF_SPIM0->RXD.MAXCNT = count + 1;
    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->ENABLE = SPIM_ENABLE_ENABLE_Enabled;
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    dma_count = count;
    dma_active = true;
    dma_started = DWT->CYCCNT;
    NRF_SPIM0->EVENTS_STOPPED = 0;
    NRF_SPIM0->TASKS_START = 1;
}

static uint32_t dma_timeout(void) {
    return dma_count > NFCB_IQ_SAMPLES ? NFCV_DMA_TIMEOUT_CYCLES : NFCB_DMA_TIMEOUT_CYCLES;
}

static void end_dma(bool stop) {
    if (!dma_active) return;
    dma_active = false;
    while (!stop && !NRF_SPIM0->EVENTS_END) {
        if (DWT->CYCCNT - dma_started > dma_timeout()) {
            spi_ok = false;
            break;
        }
    }
    NRF_SPIM0->TASKS_STOP = 1;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPIM0->EVENTS_STOPPED && DWT->CYCCNT - start < SPI_TIMEOUT_CYCLES) {}
    if (!NRF_SPIM0->EVENTS_STOPPED) {
        /* Do not hand the SPI alias or DMA banks to another driver. */
        NVIC_SystemReset();
    }
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
    if (dma_cycles) *dma_cycles += DWT->CYCCNT - dma_measured;
}

static void sample_dma(unsigned count) {
    start_dma(count);
    end_dma(false);
}

unsigned rc522_raw_available(void) {
    if (!dma_active || NRF_SPIM0->EVENTS_END) return dma_count;
    uint32_t elapsed = DWT->CYCCNT - dma_started;
    if (elapsed > dma_timeout()) return dma_count;
    unsigned written = elapsed / LIVE_CYCLES_PER_SAMPLE;
    if (written <= LIVE_LAG_SAMPLES) return 0;
    return written - LIVE_LAG_SAMPLES < dma_count ? written - LIVE_LAG_SAMPLES : dma_count;
}

void rc522_raw_finish(bool stop) {
    if (!dma_active) return;
    end_dma(stop);
    /* An early stop follows the reply EOF closely; keep the next request spaced. */
    if (stop) delay_us(SEND_SETTLE_US);
}

static void symbol(uint32_t start, unsigned etu, uint8_t reg, uint8_t value) {
    uint32_t target = start + (etu * ETU_CYCLES_NUMERATOR) / ETU_CYCLES_DENOMINATOR;
    while ((int32_t)(DWT->CYCCNT - target) < 0) {
    }
    write_register(reg, value);
}

static void nfcb_tx(const uint8_t *data, size_t n) {
    uint8_t reg = CWGsCfgReg, low = nfcb_tx_low, high = RC522_CARRIER_LEVEL;
    write_register(TxSelReg, RC522_TX_CONSTANT_CARRIER); /* Constant carrier; bypass the Miller encoder. */
    write_register(reg, high);
    if (hardware_tx) {
        if (!rc522_tx_send(data, n, low)) spi_ok = false;
        return;
    }
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t start = DWT->CYCCNT + TX_START_MARGIN_CYCLES;
    symbol(start, 0, reg, low);
    symbol(start, NFCB_TX_SOF_LOW_SYMBOLS, reg, high);
    unsigned etu = NFCB_TX_SOF_SYMBOLS;
    for (size_t i = 0; i < n; i++) {
        symbol(start, etu++, reg, low);
        for (unsigned b = 0; b < 8; b++)
            symbol(start, etu++, reg, data[i] & (1u << b) ? high : low);
        symbol(start, etu++, reg, high);
    }
    symbol(start, etu, reg, low);
    symbol(start, etu + NFCB_TX_EOF_LOW_SYMBOLS, reg, high);
    __set_PRIMASK(mask);
}

static void nfcv_tx(const uint8_t *reader_levels, size_t ticks) {
    uint8_t reg = CWGsCfgReg, high = RC522_CARRIER_LEVEL, low = 0;
    write_register(TxSelReg, RC522_TX_CONSTANT_CARRIER);
    write_register(reg, high);
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t start = DWT->CYCCNT + TX_START_MARGIN_CYCLES;
    uint8_t previous = 0;
    for (unsigned tick = 0; tick < ticks; tick++) {
        if (tick == 0 || reader_levels[tick] != previous) {
            symbol(start, tick, reg, reader_levels[tick] ? low : high);
            previous = reader_levels[tick];
        }
    }
    symbol(start, ticks, reg, high);
    __set_PRIMASK(mask);
}

uint32_t rc522_raw_take_write_cycles(void) {
    uint32_t cycles = tearoff_delta;
    tearoff_delta = 0;
    return cycles;
}

bool rc522_raw_ready(void) { return spi_ok; }

void rc522_raw_invalidate(void) { spi_ok = false; }

bool rc522_raw_clock_ready(void) {
    uint32_t mask = CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk;
    return (NRF_CLOCK->HFCLKSTAT & mask) == mask;
}

bool rc522_raw_acquire_tx(void) {
    tx_acquired = rc522_tx_open();
    return tx_acquired;
}

void rc522_raw_depth(bool deeper) {
    nfcb_tx_low = deeper ? RC522_ASK_DEEP : RC522_ASK_SHALLOW;
}

static void prepare_receive(void) {
    write_register(CommandReg, PCD_IDLE);
    write_register(ComIrqReg, RC522_CLEAR_IRQ_FLAGS);
    write_register(FIFOLevelReg, RC522_FLUSH_FIFO);
    write_register(CommandReg, PCD_RECEIVE);
}

void rc522_raw_exchange(const rc522_raw_transfer_t *transfer) {
    hardware_tx = transfer->hardware_tx;
    dma_rx = transfer->samples;
    dma_cycles = transfer->dma_cycles;
    prepare_receive();
    if (!hardware_tx) delay_us(FRONTEND_SETTLE_US);
    /* Unwritten live samples read as silence, so a valid CRC needs this capture. */
    if (transfer->live) memset(dma_rx, 0, transfer->sample_count + 1u);
    if (frontend_protocol == RC522_RAW_NFCV) nfcv_tx(transfer->data, transfer->length);
    else nfcb_tx(transfer->data, transfer->length);
    if (transfer->live) start_dma(transfer->sample_count);
    else sample_dma(transfer->sample_count);
}

void rc522_raw_tearoff(const rc522_raw_transfer_t *transfer, uint32_t off_delay_us) {
    hardware_tx = transfer->hardware_tx;
    tearoff_delay_us = off_delay_us;
    tearoff_delta = 0;
    prepare_receive();
    delay_us(FRONTEND_SETTLE_US);
    if (!spi_ok) return;
    nfcb_tx(transfer->data, transfer->length);
    uint32_t requested = DWT->CYCCNT;
    uint32_t deadline = requested + tearoff_delay_us * CPU_CYCLES_PER_US;
    while ((int32_t)(DWT->CYCCNT - (deadline - TEAROFF_IRQ_GUARD_CYCLES)) < 0) {}
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    while ((int32_t)(DWT->CYCCNT - deadline) < 0) {}
    tearoff_delta = DWT->CYCCNT - requested;
    write_register(TxControlReg, RC522_FIELD_OFF);
    __set_PRIMASK(mask);
    uint32_t off_done = DWT->CYCCNT;
    delay_us(ST25TB_TEAROFF_FIELD_OFF_US);
    hf_radio_tearoff_last_off_cycles = DWT->CYCCNT - off_done;
    write_register(TxControlReg, RC522_FIELD_ON);
}

void rc522_raw_write(const rc522_raw_transfer_t *transfer) {
    hardware_tx = transfer->hardware_tx;
    prepare_receive();
    delay_us(FRONTEND_SETTLE_US);
    if (!spi_ok) return;
    nfcb_tx(transfer->data, transfer->length);
    delay_us(ST25TB_WRITE_COMMIT_US);
}

void rc522_raw_reset(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    write_register(TxControlReg, RC522_FIELD_OFF);
    __set_PRIMASK(mask);
    uint32_t off_done = DWT->CYCCNT;
    delay_us(ST25TB_TEAROFF_FIELD_OFF_US);
    hf_radio_tearoff_last_off_cycles = DWT->CYCCNT - off_done;
    write_register(TxControlReg, RC522_FIELD_ON);
}

void rc522_raw_send(const rc522_raw_transfer_t *transfer) {
    hardware_tx = transfer->hardware_tx;
    nfcb_tx(transfer->data, transfer->length);
    delay_us(SEND_SETTLE_US); /* ST25TB requires at least 132 us before the next request. */
}

static bool start_reader(bool power_cycle, bool passive, uint8_t gain) {
    if (tx_acquired) {
        rc522_tx_close();
    }
    tx_acquired = false;
    hardware_tx = false;
    if (power_cycle) {
        nrf_gpio_pin_clear(READER_POWER);
        bsp_delay_ms(READER_POWER_SETTLE_MS);
        nrf_gpio_pin_set(READER_POWER);
        bsp_delay_ms(READER_POWER_SETTLE_MS);
    }
    NRF_SPIM0->ENABLE = 0;
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
    spi_ok = true;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    write_register(CommandReg, PCD_RESET);
    if (frontend_protocol == RC522_RAW_NFCB) {
        wait_reset();
    } else {
        bsp_delay_ms(READER_RESET_SETTLE_MS);
    }
    write_register(TxModeReg, frontend_protocol == RC522_RAW_NFCV ? 0 : RC522_TX_TYPE_B);
    write_register(RxModeReg, SOFTWARE_RX_MODE);
    write_register(TxAutoReg, 0);
    write_register(TxSelReg, RC522_TX_CONSTANT_CARRIER);
    write_register(RxSelReg, SOFTWARE_RX_SELECT);
    write_register(RxThresholdReg, SOFTWARE_RX_THRESHOLD);
    write_register(DemodReg, SOFTWARE_DEMOD);
    write_register(RFCfgReg, passive ? (SOFTWARE_RF_GAIN & ~0x70u) | (gain << 4) : SOFTWARE_RF_GAIN);
    write_register(GsNReg, SOFTWARE_N_CONDUCTANCE);
    write_register(CWGsCfgReg, RC522_CARRIER_LEVEL);
    write_register(ModGsCfgReg, RC522_ASK_SHALLOW);
    write_register(TxControlReg, passive ? RC522_FIELD_OFF : RC522_FIELD_ON);
    g_is_reader_antenna_on = !passive;
    bsp_delay_ms(frontend_protocol == RC522_RAW_NFCB ? NFCB_FIELD_SETTLE_MS : READER_FIELD_SETTLE_MS);
    return spi_ok;
}

bool rc522_raw_open(rc522_raw_protocol_t protocol) {
    frontend_protocol = protocol;
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_uninit();
    return start_reader(false, false, 4);
}

bool rc522_raw_recover(void) {
    return start_reader(true, false, 4);
}

bool rc522_raw_close(void) {
    if (tx_acquired) rc522_tx_close();
    tx_acquired = hardware_tx = false;
    bool recovered = !spi_ok;
    if (!spi_ok) {
        nrf_gpio_pin_clear(READER_POWER);
        bsp_delay_ms(READER_POWER_SETTLE_MS);
        nrf_gpio_pin_set(READER_POWER);
        bsp_delay_ms(READER_POWER_SETTLE_MS);
        spi_ok = true;
    }
    write_register(CommandReg, PCD_IDLE);
    write_register(TxControlReg, RC522_FIELD_OFF);
    /* Restore the NFC-A defaults with bounded SPI, including on a failed read. */
    write_register(CommandReg, PCD_RESET);
    if (frontend_protocol == RC522_RAW_NFCB) wait_reset();
    else bsp_delay_ms(READER_RESET_SETTLE_MS);
    write_register(TxControlReg, RC522_FIELD_OFF);
    write_register(TModeReg, 0);
    write_register(TxAutoReg, NFC_A_TX_AUTO);
    write_register(ModeReg, NFC_A_MODE);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = 0;
    if (!spi_ok) NVIC_SystemReset();
    g_is_reader_antenna_on = false;
    pcd_14a_reader_init();
    return !recovered && spi_ok;
}

bool rc522_raw_sniff_open(uint8_t gain) {
    frontend_protocol = RC522_RAW_NFCV;
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_uninit();
    return start_reader(false, true, gain);
}

void rc522_raw_capture(uint8_t *samples, unsigned count) {
    dma_rx = samples;
    dma_cycles = NULL;
    prepare_receive();
    NRF_SPIM0->EVENTS_STARTED = 0;
    NRF_SPIM0->SHORTS = SPIM_SHORTS_END_START_Msk;
    start_dma(count - 1u);
}

void rc522_raw_capture_stop(void) {
    NRF_SPIM0->SHORTS = 0;
    end_dma(true);
}
