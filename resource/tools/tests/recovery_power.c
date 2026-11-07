/* Run the actual recovery power controller against simulated peripheral state. */
#include "recovery.h"
#include "web_update.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "hw_connect.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

test_power_t test_power = {.GPREGRET2 = 0xff};
test_usbd_t test_usbd;
test_nvmc_t test_nvmc = {.READY = NVMC_READY_READY_Ready};
test_rtc_t test_rtc;
static uint32_t now, primask, reads[TEST_PIN_COUNT], outputs[TEST_PIN_COUNT], levels[TEST_PIN_COUNT];
static bool wake[TEST_PIN_COUNT], leds_off, reconnect_at_lock, button_at_lock;
static bool application_valid = true, update_active;
static unsigned reboots;
static jmp_buf powered_off;

uint32_t recovery_millis(void) { return now; }
bool web_update_application_valid(void) { return application_valid; }
bool web_update_active(void) { return update_active; }
void recovery_reboot_later(void) { reboots++; }
uint32_t __get_PRIMASK(void) { return primask; }
void __set_PRIMASK(uint32_t value) { primask = value; }
void __disable_irq(void) {
    primask = 1;
    if (reconnect_at_lock) test_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
    if (button_at_lock) reads[BUTTON_1] = 1;
}
uint32_t nrf_gpio_pin_read(uint32_t pin) { assert(pin < TEST_PIN_COUNT); return reads[pin]; }
void nrf_gpio_cfg_output(uint32_t pin) { assert(pin < TEST_PIN_COUNT); outputs[pin] = 1; }
void nrf_gpio_pin_clear(uint32_t pin) { assert(pin < TEST_PIN_COUNT); levels[pin] = 0; }
void nrf_gpio_pin_set(uint32_t pin) { assert(pin < TEST_PIN_COUNT); levels[pin] = 1; }
void nrf_gpio_cfg_sense_input(uint32_t pin, uint32_t pull, uint32_t sense) {
    assert(pin == BUTTON_1 || pin == BUTTON_2);
    assert(pull == BUTTON_PULL && sense == NRF_GPIO_PIN_SENSE_HIGH);
    wake[pin] = true;
}
void init_leds(void) { leds_off = true; }
_Noreturn void nrf_power_system_off(void) {
    assert(primask && !test_power.USBREGSTATUS);
    assert(!test_usbd.ENABLE && test_nvmc.READY == NVMC_READY_READY_Ready);
    assert(test_power.GPREGRET == 0 && test_power.GPREGRET2 == 3);
    assert(leds_off && wake[BUTTON_1] && wake[BUTTON_2]);
    assert(test_rtc.TASKS_STOP == 1 && test_rtc.INTENCLR == UINT32_MAX && test_rtc.EVTENCLR == UINT32_MAX);
    assert(outputs[LF_MOD] && !levels[LF_MOD]);
    assert(outputs[READER_POWER] && !levels[READER_POWER]);
    assert(outputs[LF_ANT_DRIVER] && !levels[LF_ANT_DRIVER]);
    assert(outputs[HF_ANT_SEL] && levels[HF_ANT_SEL]);
    for (unsigned i = 0; i < 8; i++) assert(test_power.RAM[i].POWERCLR == 0x30000);
    assert(test_power.RAM[8].POWERCLR == 0x3f0000);
    longjmp(powered_off, 1);
}

static bool poll(uint32_t time, bool restarting) {
    now = time;
    if (setjmp(powered_off)) return true;
    recovery_power_poll(restarting);
    assert(!leds_off && !test_power.GPREGRET && !primask);
    return false;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    if (!strcmp(test, "powered")) {
        /* VBUS alone suffices: no enumeration, OUTPUTRDY or network activity. */
        test_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
        assert(!poll(0, false));
        assert(!poll(0x7fffffff, false));
        assert(!poll(UINT32_MAX, false));
        assert(!poll(5, false));
        test_power.USBREGSTATUS = 0;
        assert(!poll(6, false));
        assert(!poll(5005, false));
        assert(poll(5006, false));
    } else if (!strcmp(test, "timeout")) {
        /* No update-session dependency: partial transfers cannot keep it awake. */
        assert(!poll(0, false));
        assert(!poll(4999, false));
        assert(poll(5000, false));
    } else if (!strcmp(test, "reconnect")) {
        assert(!poll(10, false));
        test_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
        assert(!poll(5009, false));
        test_power.USBREGSTATUS = 0;
        assert(!poll(20000, false));
        assert(!poll(24999, false));
        assert(poll(25000, false));
    } else if (!strcmp(test, "wrap")) {
        assert(!poll(UINT32_MAX - 2000, false));
        assert(!poll(2998, false));
        assert(poll(2999, false));
    } else if (!strcmp(test, "flash-busy") || !strcmp(test, "usb-busy")) {
        assert(!poll(0, false));
        if (!strcmp(test, "flash-busy")) test_nvmc.READY = 0;
        else test_usbd.ENABLE = 1;
        assert(!poll(5000, false));
        assert(!poll(60000, false));
        test_nvmc.READY = NVMC_READY_READY_Ready;
        test_usbd.ENABLE = 0;
        assert(poll(60001, false));
    } else if (!strcmp(test, "rebooting")) {
        assert(!poll(0, true));
        assert(!poll(60000, true));
    } else if (!strcmp(test, "button1") || !strcmp(test, "button2")) {
        unsigned pin = !strcmp(test, "button1") ? BUTTON_1 : BUTTON_2;
        reads[pin] = 1;
        assert(!poll(0, false));
        assert(!poll(60000, false));
        reads[pin] = 0;
        assert(!poll(64999, false));
        assert(poll(65000, false));
    } else if (!strcmp(test, "reconnect-race")) {
        assert(!poll(0, false));
        reconnect_at_lock = true;
        assert(!poll(5000, false));
        assert(!poll(5001, false));
        reconnect_at_lock = false;
        test_power.USBREGSTATUS = 0;
        assert(!poll(5002, false));
        assert(!poll(10001, false));
        assert(poll(10002, false));
    } else if (!strcmp(test, "button-race")) {
        assert(!poll(0, false));
        button_at_lock = true;
        assert(!poll(5000, false));
        assert(!poll(5001, false));
        button_at_lock = false;
        reads[BUTTON_1] = 0;
        assert(!poll(10000, false));
        assert(poll(10001, false));
    } else if (!strncmp(test, "exit-", 5)) {
        test_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
        application_valid = strcmp(test, "exit-invalid") != 0;
        update_active = !strcmp(test, "exit-active") || !strcmp(test, "exit-transfer-press");
        bool restarting = !strcmp(test, "exit-rebooting");
        if (!strcmp(test, "exit-flash-busy")) test_nvmc.READY = 0;
        if (!strcmp(test, "exit-entry-held")) {
            reads[BUTTON_1] = 1;
            assert(!poll(0, false));
            assert(!poll(30, false));
            reads[BUTTON_1] = 0;
            assert(!poll(40, false));
            assert(!poll(70, false));
            assert(reboots == 0);
        }
        assert(!poll(80, restarting));
        reads[BUTTON_1] = 1;
        assert(!poll(100, restarting));
        if (!strcmp(test, "exit-bounce")) {
            reads[BUTTON_1] = 0;
            assert(!poll(110, false));
            assert(!poll(200, false));
            assert(reboots == 0);
        } else {
            assert(!poll(129, restarting));
            assert(!poll(130, restarting));
            assert(reboots == 0); /* Holding B does not reset into classic DFU. */
            if (!strcmp(test, "exit-transfer-press")) update_active = false;
            reads[BUTTON_1] = 0;
            assert(!poll(200, restarting));
            assert(!poll(229, restarting));
            assert(reboots == 0);
            assert(!poll(230, restarting));
            bool allowed = !strcmp(test, "exit-valid") || !strcmp(test, "exit-entry-held");
            assert(reboots == (allowed ? 1u : 0u));
        }
    } else assert(!"Unknown scenario");
    printf("Recovery power: %s passed\n", test);
    return 0;
}
