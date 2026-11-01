"""Exercise the production streaming ISO15693 decoder with physical I/Q samples."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'


class NfcvSniffTests(unittest.TestCase):
    def test_streaming_responses(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'sniff'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(RFID),
                            str(ROOT / 'resource/tools/tests/nfcv_sniff_decode.c'),
                            str(RFID / 'iso15693/sniff_decode.c'),
                            str(RFID / 'iso15693/frame.c'), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_backend_capture_boundaries_and_cleanup(self):
        source = (RFID / 'reader/hf/nfcv_sniff.c').read_text()
        source = '\n'.join(line for line in source.splitlines()
                           if not line.startswith('#include "'))
        fixture = (ROOT / 'resource/tools/tests/nfcv_sniff.c').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            test = Path(temporary) / 'backend.c'
            test.write_text(fixture.replace('/* BACKEND */', source))
            binary = Path(temporary) / 'backend'
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-pointer-to-int-cast', '-fsanitize=address,undefined',
                            '-I', str(RFID), '-I', str(RFID / 'reader/hf'), str(test),
                            str(RFID / 'iso15693/sniff_decode.c'),
                            str(RFID / 'iso15693/frame.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_lf_adc_banks_coexist_with_the_lf_capture_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'workspace'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(RFID / 'reader'),
                            str(ROOT / 'resource/tools/tests/rf_workspace.c'),
                            str(RFID / 'reader/rf_workspace.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
