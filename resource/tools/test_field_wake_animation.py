"""Compile the actual wake-dispatch function against observable service stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r'''
#define POWER_RESETREAS_SREQ_Msk (1u << 2)
#define FULL_BOOT_GPREGRET2 (1u << 1)
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define NRF_POWER_RESETREAS_OFF_MASK 1
#define NRF_POWER_RESETREAS_NFC_MASK 2
#define NRF_POWER_RESETREAS_LPCOMP_MASK 4
#define NRF_POWER_RESETREAS_VBUS_MASK 8
#define RESET_ON_LF_FIELD_EXISTS_Msk 1
#define GPREGRET_CLEAR_VALUE_DEFAULT 255
#define SLEEP_DELAY_MS_FIELD_WAKEUP 4000
#define SLEEP_DELAY_MS_FIRST_POWER 1000
#define NRFX_POWER_USB_STATE_DISCONNECTED 0
#define SettingsAnimationModeFull 0
#define SettingsAnimationModeMinimal 1
#define SettingsAnimationModeNone 2
#define SettingsAnimationModeSymmetric 3
#define NRF_LOG_INFO(...) ((void)0)
#define TAG_FIELD_LED_ON() (field_led++)
static uint32_t m_reset_source, m_gpregret_val, reason, regret;
static uint8_t slot, animation;
static unsigned sweeps, symmetric, field_led, lights, advertising, timeout, color;
static void sd_power_reset_reason_get(uint32_t *out) {*out = reason;}
static void sd_power_reset_reason_clr(uint32_t value) {(void)value;}
static void sd_power_gpregret_get(unsigned id, uint32_t *out) {(void)id;*out = regret;}
static void sd_power_gpregret_clr(unsigned id, unsigned value) {(void)id;(void)value;}
static uint32_t wake_boot_app_regret(uint32_t value) {return value;}
static uint8_t tag_emulation_get_slot(void) {return slot;}
static uint8_t get_color_by_slot(uint8_t value) {(void)value;return 0;}
static void advertising_start(bool value) {(void)value;advertising++;}
static uint8_t settings_get_animation_config(void) {return animation;}
static void rgb_marquee_sweep_to(uint8_t c, uint8_t dir, uint8_t end) {
    (void)dir; (void)end; color=c; sweeps++;
}
static void rgb_marquee_symmetric_out(uint8_t c, uint8_t s) {(void)s;color=c;symmetric++;}
static void set_slot_light_color(uint8_t c) {color=c;}
static void light_up_by_slot(void) {lights++;}
static uint32_t settings_get_sleep_timeout(void) {return 8123;}
static void sleep_timer_start(uint32_t value) {timeout=value;}
static void tag_emulation_factory_init(void) {assert(false);}
static void rgb_marquee_sweep_from_to(uint8_t c, uint8_t a, uint8_t b) {(void)c;(void)a;(void)b;}
static void rgb_marquee_symmetric_in(uint8_t c, uint8_t s) {(void)c;(void)s;}
static unsigned nrfx_power_usbstatus_get(void) {return 1;}
'''

TEST = r'''
static void reset(unsigned why, unsigned retained) {
    reason=why; regret=retained;
    sweeps=symmetric=field_led=lights=advertising=timeout=0; color=255;
    check_wakeup_src();
}
int main(void) {
    for (slot=0;slot<8;slot++) for (animation=0;animation<4;animation++) {
        for (unsigned rf=0;rf<3;rf++) {
            reset(rf==0 ? NRF_POWER_RESETREAS_NFC_MASK :
                  rf==1 ? NRF_POWER_RESETREAS_LPCOMP_MASK : 0, rf==2 ? 1 : 0);
            assert(field_led==1 && lights==1 && advertising==0 && timeout==4000);
            assert(color==(rf==0 ? 1u : 2u));
            assert(sweeps==0);
            assert(symmetric==0);
        }
        reset(NRF_POWER_RESETREAS_OFF_MASK,0);
        assert(advertising==1 && lights==1 && field_led==0 && timeout==8123);
        assert(sweeps==(animation==SettingsAnimationModeFull ? 3u : animation==SettingsAnimationModeMinimal ? 1u : 0u));
        assert(symmetric==(animation==SettingsAnimationModeSymmetric ? 1u : 0u));
        reset(NRF_POWER_RESETREAS_VBUS_MASK,0);
        assert(advertising==1 && lights==0 && field_led==0 && timeout==0);
        assert(sweeps==0 && symmetric==0);
        reset(POWER_RESETREAS_SREQ_Msk,FULL_BOOT_GPREGRET2);
        assert(advertising==1 && lights==1 && timeout==8123);
    }
    return 0;
}
'''


class FieldAnimationTests(unittest.TestCase):
    def test_hf_lf_retained_lf_button_and_vbus_for_all_slots_and_settings(self):
        source = (ROOT / 'firmware/application/src/app_main.c').read_text()
        signature = 'static void check_wakeup_src(void) {'
        # Keep the complete production function, including its unmodified branches.
        function = signature + source.split(signature, 1)[1].split('\n}\n', 1)[0] + '\n}\n'
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = directory / 'wake.c'
            path.write_text(STUBS + function + TEST)
            binary = directory / 'wake'
            subprocess.run([
                'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', str(path), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
