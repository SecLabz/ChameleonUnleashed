#pragma once
#include <stdint.h>

typedef struct {
    uint32_t ENABLE, EVENTS_READY, TXD, RXD;
    uint32_t PSELSCK, PSELMOSI, PSELMISO, FREQUENCY, CONFIG;
} test_spi_t;
typedef struct {
    uint32_t ENABLE, CONFIG, FREQUENCY, ORC, EVENTS_END, EVENTS_STOPPED, EVENTS_STARTED, SHORTS;
    uint32_t TASKS_START, TASKS_STOP;
    struct { uint32_t SCK, MOSI, MISO; } PSEL;
    struct { uint32_t PTR, MAXCNT; } TXD, RXD;
} test_spim_t;
typedef struct { uint32_t CYCCNT, CTRL; } test_dwt_t;
typedef struct { uint32_t DEMCR; } test_debug_t;
typedef struct { uint32_t HFCLKSTAT; } test_clock_t;

extern test_debug_t test_debug;
extern test_clock_t test_clock;
test_spi_t *test_spi_access(void);
test_spim_t *test_spim_access(void);
test_dwt_t *test_dwt_access(void);
void test_system_reset(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);

#define NRF_SPI0 test_spi_access()
#define NRF_SPIM0 test_spim_access()
#define DWT test_dwt_access()
#define CoreDebug (&test_debug)
#define NRF_CLOCK (&test_clock)
#define NVIC_SystemReset test_system_reset
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define CLOCK_HFCLKSTAT_STATE_Msk 0x10000u
#define CLOCK_HFCLKSTAT_SRC_Msk 1u
#define SPI_ENABLE_ENABLE_Enabled 1u
#define SPIM_ENABLE_ENABLE_Enabled 7u
#define SPI_FREQUENCY_FREQUENCY_M8 0x80000000u
#define SPIM_FREQUENCY_FREQUENCY_M8 0x80000000u

#define SPIM_SHORTS_END_START_Msk 1u
