/* Independent USB network supervisor. No RFID, BLE, slot storage or external assets. */
#include "recovery.h"
#include "web_update.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_sdm.h"
#include "nrfx_power.h"
#include "hw_connect.h"
#include "tusb.h"
#include "net/network.h"

#define LFCLK_HZ 32768u
#define RTC_TICK_HZ 1024u
#define RTC_TASK_SETTLE_US 64u
#define RTC_IRQ_PRIORITY 7u

extern uint8_t tud_network_mac_address[6];
extern void tusb_hal_nrf_power_event(uint32_t event);
static volatile uint32_t milliseconds;
static uint32_t restart_at;
static bool restarting;
static uint32_t millisecond_fraction;

void RTC1_IRQHandler(void) {
    if (NRF_RTC1->EVENTS_TICK) {
        NRF_RTC1->EVENTS_TICK = 0;
        millisecond_fraction += 1000;
        milliseconds += millisecond_fraction / RTC_TICK_HZ;
        millisecond_fraction %= RTC_TICK_HZ;
    }
}

static void timer_init(void) {
    /* nRF52 stops SysTick in WFE. RTC keeps USB power polling, TCP timeouts
     * and watchdog service alive even before the host enumerates USB. */
    SysTick->CTRL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    /* A watchdog survives software reset and can keep LFCLKSTAT running while
     * the clock is routed only to WDT. Request LFCLK for RTC explicitly. */
    if (!(NRF_CLOCK->LFCLKRUN & CLOCK_LFCLKRUN_STATUS_Msk)) {
        NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos;
        NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
        NRF_CLOCK->TASKS_LFCLKSTART = 1;
        while (!NRF_CLOCK->EVENTS_LFCLKSTARTED) {}
    }
    NRF_RTC1->TASKS_STOP = 1;
    /* RTC tasks cross the 32 kHz clock domain. STOP and START issued in the
     * same LFCLK period can leave the counter stopped. Let each task settle. */
    nrf_delay_us(RTC_TASK_SETTLE_US);
    NRF_RTC1->INTENCLR = UINT32_MAX;
    NRF_RTC1->EVTENCLR = UINT32_MAX;
    NRF_RTC1->PRESCALER = LFCLK_HZ / RTC_TICK_HZ - 1u;
    NRF_RTC1->TASKS_CLEAR = 1;
    nrf_delay_us(RTC_TASK_SETTLE_US);
    NRF_RTC1->EVENTS_TICK = 0;
    NRF_RTC1->EVTENSET = RTC_EVTENSET_TICK_Msk;
    NRF_RTC1->INTENSET = RTC_INTENSET_TICK_Msk;
    NVIC_SetPriority(RTC1_IRQn, RTC_IRQ_PRIORITY);
    NVIC_ClearPendingIRQ(RTC1_IRQn);
    NVIC_EnableIRQ(RTC1_IRQn);
    NRF_RTC1->TASKS_START = 1;
    while (!NRF_RTC1->COUNTER) {}
}
void USBD_IRQHandler(void) { tud_int_handler(0); }
uint32_t recovery_millis(void) { return milliseconds; }
void recovery_reboot_later(void) {
    NRF_POWER->GPREGRET = 0;
    restart_at = milliseconds + WEB_UPDATE_RESTART_DELAY_MS;
    restarting = true;
}

void recovery_start_application(void) {
    /* Once launched, the RFID app cannot modify its executable or commit page. */
    bool protected = false;
    for (unsigned i = 0; i < sizeof(NRF_ACL->ACL) / sizeof(NRF_ACL->ACL[0]); i++) {
        if (!NRF_ACL->ACL[i].SIZE) {
            NRF_ACL->ACL[i].ADDR = WEB_RECOVERY_START;
            NRF_ACL->ACL[i].SIZE = WEB_UPDATE_END - WEB_RECOVERY_START;
            NRF_ACL->ACL[i].PERM = ACL_ACL_PERM_WRITE_Disable << ACL_ACL_PERM_WRITE_Pos;
            protected = true;
            break;
        }
    }
    if (!protected) return;
    /* Keep the MBR/SoftDevice SVC dispatcher, forwarding app interrupts to its
     * relocated vector table. No recovery peripheral has been started yet. */
    SCB->VTOR = 0;
    if (sd_softdevice_vector_table_base_set(WEB_UPDATE_START) != 0) NVIC_SystemReset();
    NRF_POWER->GPREGRET2 &= WEB_RECOVERY_APP_STATE_MASK;
    uint32_t sp = *(const uint32_t *)WEB_UPDATE_START;
    uint32_t pc = *(const uint32_t *)(WEB_UPDATE_START + sizeof(uint32_t));
    __asm volatile("msr msp, %0\n bx %1" :: "r"(sp), "r"(pc) : "memory");
    __builtin_unreachable();
}

int main(void) {
    hw_connect_init();
    bool requested = NRF_POWER->GPREGRET == WEB_RECOVERY_REQUEST;
    if (requested) NRF_POWER->GPREGRET = 0;
    nrf_gpio_cfg_input(BUTTON_1, BUTTON_PULL);
    nrf_gpio_cfg_input(BUTTON_2, BUTTON_PULL);
    bool button_recovery = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) &&
                           nrf_gpio_pin_read(BUTTON_2);
    bool installed = web_update_bootloader_ready();
    bool valid = installed && web_update_application_valid();
    if (installed && !valid) valid = web_update_recover_installation();
    if (!requested && !button_recovery && valid) recovery_start_application();

    /* SoftDevice is disabled here; use the recovery vectors and direct clocks. */
    extern uint32_t recovery_vectors[RECOVERY_VECTOR_COUNT];
    SCB->VTOR = (uint32_t)recovery_vectors;
    __DSB(); __ISB();
    __set_BASEPRI(0);
    __set_FAULTMASK(0);
    __enable_irq();
    init_leds();
    set_slot_light_color(RGB_CYAN);
    /* A fixed cyan pair identifies web recovery; classic DFU alternates them. */
    nrf_gpio_pin_set(LED_4);
    nrf_gpio_pin_set(LED_5);
    timer_init();
    memcpy(tud_network_mac_address + 1, (const void *)NRF_FICR->DEVICEADDR, 5);
    if (!tud_init(0) || !network_init()) NVIC_SystemReset();
    NRF_POWER->GPREGRET2 &= WEB_RECOVERY_APP_STATE_MASK;
    uint32_t previous = 0;
    for (;;) {
        uint32_t power = NRF_POWER->USBREGSTATUS;
        if (power != previous) {
            if (!(power & POWER_USBREGSTATUS_VBUSDETECT_Msk)) tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_REMOVED);
            else {
                if (!(previous & POWER_USBREGSTATUS_VBUSDETECT_Msk)) tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_DETECTED);
                if (power & POWER_USBREGSTATUS_OUTPUTRDY_Msk) tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_READY);
            }
            previous = power;
        }
        tud_task();
        network_poll();
        if (NRF_WDT->RUNSTATUS)
            for (unsigned i = 0; i < sizeof(NRF_WDT->RR) / sizeof(NRF_WDT->RR[0]); i++)
                if (NRF_WDT->RREN & (1u << i)) NRF_WDT->RR[i] = WDT_RR_RR_Reload;
        if (restarting && (int32_t)(milliseconds - restart_at) >= 0) NVIC_SystemReset();
        /* Flash writes and network callbacks finish synchronously above. */
        recovery_power_poll(restarting);
        __WFE();
    }
}
