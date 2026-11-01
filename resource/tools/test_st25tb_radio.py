"""Exercise the complete ST25TB radio against deterministic peripheral events."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'


class RadioTests(unittest.TestCase):
    def test_dma_lifecycle_and_recovery(self):
        radio = (RFID / 'nfctag/hf/nfc_st25tb_radio.c').read_text()
        # Supply hardware declarations locally, retaining every production function.
        radio = re.sub(r'^#include .*\n', '', radio, flags=re.M)
        # The embedded ELF RAM section has no equivalent in a native Mach-O test.
        radio = radio.replace('section(".data.st25tb"), ', '')
        # Retain the production disable sequence while mapping its MMIO store.
        radio = radio.replace('*(volatile uint32_t *)0x4002f004u', 'spim3_erratum195')
        for name in ('reset', 'settle', 'time', 'pair', 'batch'):
            radio = radio.replace(f'st25tb_receiver_{name}(', f'observed_receiver_{name}(')
        radio = radio.replace('st25tb_field_lost(', 'observed_field_lost(')
        fixture = (ROOT / 'resource/tools/tests/st25tb_radio.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / 'radio.c'
            source.write_text(fixture.replace('/* RADIO_IMPLEMENTATION */', radio))
            binary = path / 'radio'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-Wno-pointer-to-int-cast', '-fsanitize=address,undefined',
                '-I', str(RFID), '-I', str(ROOT / 'resource/tools/tests'), str(source),
                str(RFID / 'st25tb/receiver.c'), str(RFID / 'st25tb/model.c'),
                str(RFID / 'iso15693/frame.c'), '-lm', '-o', str(binary),
            ], check=True)
            scenarios = (
                'tx_waveform', 'banks', 'bank_gaps', 'descriptor_race', 'decode_overrun', 'reply', 'unready_frame',
                'reply_deadline', 'window_deadline', 'idle_deadline', 'delayed_ready',
                'field_off_deadline', 'active_deadline', 'button',
                'spi_timeout', 'dma_timeout', 'start_timeout', 'stop_timeout',
                'partial_bank', 'next_start_timeout', 'reply_clock_timeout',
                'counter_init', 'clock_init', 'ppi_alloc', 'ppi_assign',
                'ppi_fork', 'ppi_enable', 'timeslot_denied', 'field_loss',
                'unloaded', 'ble_window', 'active_window', 'rf_only_window',
                'usb_idle_window', 'usb_active_window', 'usb_ble_window',
                'usb_enumerating_window', 'usb_disconnect_window',
                'field_dip_reply', 'eof_noise', 'eof_noise_stale',
                'field_dips', 'field_drop_busy',
                'field_return', 'field_return_busy', 'field_unreadable',
            )
            scenarios += (
                'delayed_reply', 'start_ppi_alloc', 'start_ppi_assign', 'start_ppi_fork',
                'start_ppi_enable', 'rx_stall', 'phase_fault',
            )
            # Include every low offset and both sides of the first8192-byte boundary.
            offsets = sorted(set(range(67)) | set(range(7400, 8201, 10)))
            scenarios += tuple(f'shift_{offset}' for offset in offsets)
            for scenario in scenarios:
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
