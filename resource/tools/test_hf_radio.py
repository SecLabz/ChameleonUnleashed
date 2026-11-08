"""Verify radio orchestration with actual production code and bounded device stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/reader/hf'


class RadioTests(unittest.TestCase):
    def test_profiles_capture_windows_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            binary = directory / 'radio'
            compiler = [
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined',
                '-I', str(ROOT / 'resource/tools/tests/hf_radio_stubs'),
                '-I', str(ROOT / 'firmware/application/tests/st25tb_tearoff_stubs'),
                '-I', str(HF), '-I', str(HF.parent.parent),
                '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/utils'),
            ]
            common_objects = []
            for index, source in enumerate((ROOT / 'resource/tools/tests/hf_radio.c', HF / 'hf_radio.c')):
                obj = directory / f'{index}.o'
                subprocess.run([*compiler, '-O3', '-c', str(source), '-o', str(obj)], check=True)
                common_objects.append(str(obj))
            policy = directory / 'st25tb_reader.o'
            subprocess.run([
                *compiler, '-Os', '-c', str(HF / 'st25tb_reader.c'), '-o', str(policy),
            ], check=True)
            subprocess.run([
                'cc', '-fsanitize=address,undefined', *common_objects, str(policy), '-o', str(binary),
            ], check=True)
            for scenario in ('vicinity_ble', 'vicinity_clock_failures', 'profiles', 'direct', 'without_usb', 'vicinity', 'retained',
                             'clock_fallback', 'clock_invalidate', 'live_clock_invalidate', 'deadline',
                             'crc_send', 'skipped_slot', 'tearoff_reads'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
