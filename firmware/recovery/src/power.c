/* Recovery waits indefinitely on USB, but must not drain an unplugged battery. */
#include "recovery.h"
#include "web_update.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_power.h"
#include "hw_connect.h"

#define USB_REMOVED_SLEEP_MS 5000u
#define BUTTON_DEBOUNCE_MS 30u
static bool on_battery;
static uint32_t unplugged_at;
static bool button_sample, button_state, button_seen_release, button_armed;
static uint32_t button_changed_at;

static bool button_exit(uint32_t now, bool restarting) {
    bool pressed = nrf_gpio_pin_read(BUTTON_1);
    if (pressed != button_sample) {
        button_sample = pressed;
        button_changed_at = now;
    }
    if (button_sample != button_state && (uint32_t)(now - button_changed_at) >= BUTTON_DEBOUNCE_MS) {
        button_state = button_sample;
        if (button_state) {
            /* Ignore an entry button still held at boot and presses during a transfer. */
            button_armed = button_seen_release && !restarting && !web_update_active();
        } else {
            bool requested = button_armed;
            button_armed = false;
            if (requested && !restarting && !web_update_active() &&
                NRF_NVMC->READY == NVMC_READY_READY_Ready && web_update_application_valid()) {
                recovery_reboot_later();
                return true;
            }
        }
    }
    if (!button_state && !button_sample) button_seen_release = true;
    return false;
}

void recovery_power_poll(bool restarting) {
    uint32_t now = recovery_millis();
    if (button_exit(now, restarting)) return;
    if (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) {
        on_battery = false;
        return;
    }
    if (!on_battery) {
        on_battery = true;
        unplugged_at = now;
    }
    /* A held wake button would otherwise cause a reset/sleep loop. */
    if (nrf_gpio_pin_read(BUTTON_1) || nrf_gpio_pin_read(BUTTON_2)) {
        unplugged_at = now;
        return;
    }
    if (restarting || (uint32_t)(now - unplugged_at) < USB_REMOVED_SLEEP_MS) return;
    /* The USB removal handler disables USBD and aborts its DMA transfers.
     * Never power down a running peripheral or an unfinished NVMC operation.
     * Do not gate on web_update_active(): an interrupted transfer remains active
     * forever. Its uncommitted image will return to recovery on the next boot. */
    if (NRF_USBD->ENABLE || NRF_NVMC->READY != NVMC_READY_READY_Ready) return;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    /* Recheck before touching peripherals: a cable can be reinserted between
     * the first VBUS read and this point. A later VBUS rise wakes System OFF. */
    if ((NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) ||
        nrf_gpio_pin_read(BUTTON_1) || nrf_gpio_pin_read(BUTTON_2)) {
        __set_PRIMASK(primask);
        return;
    }
    NRF_RTC1->TASKS_STOP = 1;
    NRF_RTC1->INTENCLR = UINT32_MAX;
    NRF_RTC1->EVTENCLR = UINT32_MAX;
    init_leds(); /* All slot LEDs off; all RGB/field outputs inactive. */
    nrf_gpio_cfg_output(LF_MOD);
    nrf_gpio_pin_clear(LF_MOD);
#if defined(PROJECT_CHAMELEON_ULTRA)
    nrf_gpio_cfg_output(READER_POWER);
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_cfg_output(LF_ANT_DRIVER);
    nrf_gpio_pin_clear(LF_ANT_DRIVER);
    nrf_gpio_cfg_output(HF_ANT_SEL);
    nrf_gpio_pin_set(HF_ANT_SEL);
#endif
    nrf_gpio_cfg_sense_input(BUTTON_1, BUTTON_PULL, NRF_GPIO_PIN_SENSE_HIGH);
    nrf_gpio_cfg_sense_input(BUTTON_2, BUTTON_PULL, NRF_GPIO_PIN_SENSE_HIGH);
    /* Only GPREGRET is needed across sleep. The installer and its buffers are
     * reconstructed from flash, so retain no RAM sections in System OFF. */
    uint32_t retention = POWER_RAM_POWER_S0RETENTION_Msk | POWER_RAM_POWER_S1RETENTION_Msk;
    size_t ram_blocks = sizeof(NRF_POWER->RAM) / sizeof(NRF_POWER->RAM[0]);
    for (size_t i = 0; i + 1 < ram_blocks; i++) NRF_POWER->RAM[i].POWERCLR = retention;
    /* The final RAM block has six retention sections instead of two. */
    NRF_POWER->RAM[ram_blocks - 1].POWERCLR = retention | POWER_RAM_POWER_S2RETENTION_Msk |
        POWER_RAM_POWER_S3RETENTION_Msk | POWER_RAM_POWER_S4RETENTION_Msk | POWER_RAM_POWER_S5RETENTION_Msk;
    /* Recovery entry is one-shot. A valid app must boot normally after wake;
     * an invalid or interrupted installation still selects recovery itself. */
    NRF_POWER->GPREGRET = 0;
    NRF_POWER->GPREGRET2 &= WEB_RECOVERY_APP_STATE_MASK;
    nrf_power_system_off(); /* VBUS or either button causes a reset on wake. */
}
