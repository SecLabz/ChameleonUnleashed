#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nrf.h"

test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

enum {
    HF_SPI_SELECT, HF_SPI_MISO, HF_SPI_MOSI, HF_SPI_SCK,
    NRF_DRV_SPI_MODE_0, NRF_DRV_SPI_FREQ_8M, TIMER_HANDLE
};
typedef int ret_code_t;
typedef struct { unsigned miso_pin, mosi_pin, sck_pin, mode, frequency; } nrf_drv_spi_config_t;
#define NRF_DRV_SPI_DEFAULT_CONFIG ((nrf_drv_spi_config_t){0})
#define APP_ERROR_CHECK(error) assert((error) == 0)

static struct { volatile uint32_t RXD, EVENTS_READY; } spi;
#define NRF_SPI0 (&spi)
static bool m_reader_is_init;
static unsigned s_spiHandle, g_timeout_auto_timer;
static unsigned gpio_calls, spi_calls, timer_calls;

static void nrf_gpio_cfg_output(unsigned pin) {
    assert(pin == HF_SPI_SELECT);
    gpio_calls++;
}

static ret_code_t nrf_drv_spi_init(const unsigned *handle,
                                  const nrf_drv_spi_config_t *config,
                                  void *callback, void *context) {
    assert(handle == &s_spiHandle && !callback && !context);
    assert(config->miso_pin == HF_SPI_MISO && config->mosi_pin == HF_SPI_MOSI);
    assert(config->sck_pin == HF_SPI_SCK && config->mode == NRF_DRV_SPI_MODE_0);
    assert(config->frequency == NRF_DRV_SPI_FREQ_8M);
    /* Driver initialization preserves the previous owner's completed byte. */
    spi.RXD = UINT8_MAX;
    spi.EVENTS_READY = 1;
    spi_calls++;
    return 0;
}

static unsigned bsp_obtain_timer(unsigned milliseconds) {
    assert(milliseconds == 0 && spi.EVENTS_READY == 0);
    timer_calls++;
    return TIMER_HANDLE;
}

/* READER INIT FUNCTION */

int main(void) {
    pcd_14a_reader_init();
    assert(m_reader_is_init && spi.EVENTS_READY == 0);
    assert(gpio_calls == 1 && spi_calls == 1 && timer_calls == 1);
    assert(g_timeout_auto_timer == TIMER_HANDLE);

    /* Repeated initialization must leave an active reader's SPI state alone. */
    spi.EVENTS_READY = 1;
    pcd_14a_reader_init();
    assert(spi.EVENTS_READY == 1);
    assert(gpio_calls == 1 && spi_calls == 1 && timer_calls == 1);

    /* A later ownership acquisition must clear a stale completion again. */
    m_reader_is_init = false;
    pcd_14a_reader_init();
    assert(spi.EVENTS_READY == 0);
    assert(gpio_calls == 2 && spi_calls == 2 && timer_calls == 2);
    return 0;
}
