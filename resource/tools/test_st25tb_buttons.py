"""Exercise ST25TB RF scheduling, USB backpressure, field LEDs and button debounce."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/application/src'


class St25tbButtonsTests(unittest.TestCase):
    def test_rf_windows_usb_replies_field_leds_and_buttons(self):
        main = (SRC / 'app_main.c').read_text()
        service = (SRC / 'rfid/nfctag/hf/nfc_st25tb.c').read_text()
        radio = (SRC / 'rfid/nfctag/hf/nfc_st25tb_radio.c').read_text()
        fixture = (ROOT / 'resource/tools/tests/st25tb_buttons.c').read_text()
        # Retain the actual shared state, including its ISR/main qualifiers.
        state = main[main.index('static uint32_t m_last_btn_press'):main.index('// NFC field generator state')]
        callbacks = '\n'.join(function(main, name) for name in (
            'restart_full_boot', 'app_buttons_pending', 'button_pin_handler',
            'timer_button_event_handle', 'button_press_process'))
        fixture = fixture.replace('/* BUTTON_STATE */', state)
        fixture = fixture.replace('/* BUTTON_CALLBACKS */', callbacks)
        fixture = fixture.replace('/* FIELD_EVENT */', function(service, 'field_event'))
        fixture = fixture.replace('/* WATCH_FIELD */', function(service, 'watch_field'))
        fixture = fixture.replace('/* SERVICE */', function(service, 'nfc_st25tb_service'))
        fixture = fixture.replace('/* USB_LED_STATUS */', function(main, 'blink_usb_led_status'))
        constants = '\n'.join(re.findall(r'^#define STB_.*$', radio, re.M))
        constants += '\n' + '\n'.join(re.findall(r'^enum \{.*?^\};', radio, re.M | re.S))
        fixture = fixture.replace('/* RADIO_CONSTANTS */', constants)
        fixture = fixture.replace('/* RADIO_WINDOW */', '\n\n'.join(function(radio, name) for name in (
            'stb_dma_at', 'stb_stream_configure', 'stb_window_restore', 'stb_window')))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / 'buttons.c'
            source.write_text(fixture)
            binary = path / 'buttons'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-pointer-to-int-cast',
                '-DPROJECT_CHAMELEON_ULTRA', '-fsanitize=address,undefined',
                '-I', str(ROOT / 'resource/tools/tests'),
                str(source), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
