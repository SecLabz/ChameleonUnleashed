"""Exercise the actual software RF frontend with deterministic peripheral events."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/reader/hf'
STUBS = ROOT / 'resource/tools/tests/hf_frontend_stubs'


class FrontendTests(unittest.TestCase):
    def test_frontend_lifecycle_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'frontend'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-pointer-to-int-cast', '-fsanitize=address,undefined',
                '-include', str(STUBS / 'rc522.h'),
                '-I', str(STUBS), '-I', str(HF), '-I', str(HF.parent.parent),
                str(ROOT / 'resource/tools/tests/hf_frontend.c'),
                str(HF / 'rc522_raw.c'), '-o', str(binary),
            ], check=True)
            for scenario in ('passive_capture', 'passive_large', 'open_close', 'failed_open', 'spi_timeout',
                             'dma_success', 'live_complete', 'live_stop',
                             'live_timeout', 'live_stop_failure',
                             'software_b', 'software_b_masked',
                             'software_v', 'software_v_masked', 'software_v_deferred',
                             'dma_timeout', 'dma_stop_failure', 'tx_failure',
                             'recover', 'clock', 'read_write_read'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
