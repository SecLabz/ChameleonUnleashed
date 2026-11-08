"""Exercise the real reader-to-emulator transition and its required settle delays."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r'''
#include <stdbool.h>
#include <stdint.h>
typedef enum {DEVICE_MODE_NONE, DEVICE_MODE_READER, DEVICE_MODE_TAG} device_mode_t;
typedef struct {int tag_hf, tag_lf;} tag_slot_specific_type_t;
#define LF_ANT_DRIVER 1
#define READER_POWER 2
#define HF_ANT_SEL 3
#define RGB_LIST_NUM 8
#define TAG_SENSE_LF 1
#define TAG_SENSE_HF 2
#define TAG_TYPE_UNDEFINED 0
#define TAG_FIELD_LED_OFF() ((void)0)
void tag_emulation_sense_end(void);
void tag_emulation_sense_run(void);
void nfca_session_close(void);
void pcd_14a_reader_antenna_off(void);
void lf_125khz_radio_init(void);
void pcd_14a_reader_init(void);
void pcd_14a_reader_reset(void);
void lf_125khz_radio_uninit(void);
void pcd_14a_reader_uninit(void);
void nrf_gpio_cfg_output(uint32_t pin);
void nrf_gpio_pin_set(uint32_t pin);
void nrf_gpio_pin_clear(uint32_t pin);
void bsp_delay_ms(uint32_t delay);
uint32_t *hw_get_led_array(void);
uint8_t tag_emulation_get_slot(void);
void rgb_marquee_slot_switch(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void tag_emulation_get_specific_types_by_slot(uint8_t slot, tag_slot_specific_type_t *types);
bool is_slot_enabled(uint8_t slot, uint8_t sense);
uint8_t get_color_by_slot(uint8_t slot);
'''

TEST = r'''
#include "rfid_main.h"
#include <assert.h>
extern device_mode_t rfid_state;
void tag_mode_enter(void);
bool m_is_field_on;
static unsigned delay_ms, sense_calls, antenna_stops;
static bool selected, powered_down, reader_closed, lf_closed;
void tag_emulation_sense_end(void) {}
void tag_emulation_sense_run(void) {
    assert(selected && powered_down && reader_closed && lf_closed);
    assert(!m_is_field_on);
    sense_calls++;
}
void nfca_session_close(void) {}
bool hf_radio_release(void) { return true; }
void pcd_14a_reader_antenna_off(void) {antenna_stops++;}
void lf_125khz_radio_init(void) {}
void pcd_14a_reader_init(void) {}
void pcd_14a_reader_reset(void) {}
void lf_125khz_radio_uninit(void) {lf_closed = true;}
void pcd_14a_reader_uninit(void) {reader_closed = true;}
void nrf_gpio_cfg_output(uint32_t pin) {(void)pin;}
void nrf_gpio_pin_set(uint32_t pin) {if (pin == HF_ANT_SEL) selected = true;}
void nrf_gpio_pin_clear(uint32_t pin) {if (pin == READER_POWER) powered_down = true;}
void bsp_delay_ms(uint32_t delay) {delay_ms += delay;}
uint32_t *hw_get_led_array(void) {static uint32_t pins[8]; return pins;}
uint8_t tag_emulation_get_slot(void) {return 0;}
void rgb_marquee_slot_switch(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {(void)a;(void)b;(void)c;(void)d;}
void tag_emulation_get_specific_types_by_slot(uint8_t slot, tag_slot_specific_type_t *types) {
    (void)slot; types->tag_hf = 1; types->tag_lf = 0;
}
bool is_slot_enabled(uint8_t slot, uint8_t sense) {(void)slot;(void)sense;return true;}
static void transition(device_mode_t previous, bool field, unsigned expected_delay) {
    rfid_state = previous; m_is_field_on = field;
    delay_ms = sense_calls = antenna_stops = 0;
    selected = powered_down = reader_closed = lf_closed = false;
    tag_mode_enter();
    assert(rfid_state == DEVICE_MODE_TAG && sense_calls == 1);
    assert(delay_ms == expected_delay && antenna_stops == (unsigned)field);
    tag_mode_enter();
    assert(sense_calls == 1 && delay_ms == expected_delay);
}
int main(void) {
    transition(DEVICE_MODE_NONE, false, 0);
    transition(DEVICE_MODE_READER, false, 60);
    transition(DEVICE_MODE_READER, true, 60);
    transition(DEVICE_MODE_NONE, true, 60);
    transition(DEVICE_MODE_TAG, true, 60);
    return 0;
}
'''


class FastWakeTests(unittest.TestCase):
    def test_settle_kept_for_reader_and_field_and_default_startup(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            shutil.copy2(ROOT / 'firmware/application/src/rfid_main.c', directory)
            (directory / 'rfid_main.h').write_text(STUBS)
            (directory / 'rgb_marquee.h').write_text('')
            (directory / 'nfca_session.h').write_text('')
            (directory / 'hf_radio.h').write_text('#include <stdbool.h>\nbool hf_radio_release(void);\n')
            (directory / 'test.c').write_text(TEST)
            binary = directory / 'transition'
            subprocess.run([
                'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-DPROJECT_CHAMELEON_ULTRA',
                '-I', str(directory), str(directory / 'rfid_main.c'),
                str(directory / 'test.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
