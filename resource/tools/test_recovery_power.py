"""Recovery power transitions, exercised without USB, SWD or a physical device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecoveryPowerTests(unittest.TestCase):
    def test_power_transitions(self):
        fixtures = ROOT / 'resource/tools/tests'
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'test-recovery-power'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                '-fsanitize=address,undefined', '-DPROJECT_CHAMELEON_ULTRA',
                '-I', str(fixtures / 'recovery_power'),
                '-I', str(ROOT / 'firmware/recovery/src'),
                '-I', str(ROOT / 'firmware/common'),
                str(ROOT / 'firmware/recovery/src/power.c'),
                str(fixtures / 'recovery_power.c'), '-o', str(binary),
            ], check=True)
            for scenario in ('powered', 'timeout', 'reconnect',
                             'wrap', 'flash-busy', 'usb-busy', 'rebooting',
                             'button1', 'button2', 'reconnect-race', 'button-race',
                             'exit-valid', 'exit-invalid', 'exit-active', 'exit-rebooting',
                             'exit-flash-busy', 'exit-entry-held', 'exit-transfer-press',
                             'exit-bounce'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=10)
