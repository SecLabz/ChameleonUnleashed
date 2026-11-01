"""Exercise the production passive capture queue with mocked hardware ownership."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/nfctag/hf'
RFID = ROOT / 'firmware/application/src/rfid'


class NfcaSniffTests(unittest.TestCase):
    def test_frames_overflow_wrap_lease_and_radio_cleanup(self):
        source = (RFID / 'reader/hf/hf_sniff.c').read_text()
        # Replace hardware headers only; compile all production capture logic.
        source = '\n'.join(line for line in source.splitlines()
                           if not line.startswith('#include "'))
        fixture = (Path(__file__).with_name('tests') / 'nfca_sniff.c').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            test = directory / 'sniff.c'
            test.write_text(fixture.replace('/* PRODUCTION_CAPTURE */', source))
            binary = directory / 'sniff'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(HF),
                            '-I', str(RFID), '-I', str(RFID / 'reader/hf'), str(test), str(RFID / 'parity.c'),
                            str(RFID / 'crc_utils.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
