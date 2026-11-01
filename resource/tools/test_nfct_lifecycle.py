"""Compile the NFCT field lifecycle against a deterministic timer/field boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'firmware/nrf52_sdk/modules/nrfx/drivers/src/nrfx_nfct.c'


class NfctLifecycle(unittest.TestCase):
    def test_cancelled_activation_allows_the_next_emulator_to_detect_fields(self):
        source = DRIVER.read_text()
        callbacks = '\n'.join(function(source, name) for name in (
            'nrfx_nfct_field_event_handler', 'nrfx_nfct_activate_check',
            'nrfx_nfct_uninit', 'nrfx_nfct_state_force'))
        fixture = (ROOT / 'resource/tools/tests/nfct_lifecycle.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'lifecycle.c'
            path.write_text(fixture.replace('/* DRIVER_CALLBACKS */', callbacks))
            binary = Path(directory) / 'lifecycle'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-DNRF52840_XXAA', '-DUSE_TIMER_WORKAROUND', '-fsanitize=address,undefined',
                str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
