"""Verify DES-EDE/CBC results, in-place operations and chaining contracts."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from native_test import function

ROOT = Path(__file__).resolve().parents[2]
CRYPTO = ROOT / 'firmware/application/src/rfid/crypto'


class UltralightCryptoTests(unittest.TestCase):
    def test_reader_matches_nxp_transcript_and_rejects_invalid_authentication(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = (ROOT / 'firmware/application/src/rfid/reader/hf/rc522.c').read_text()
            fixture = (ROOT / 'resource/tools/tests/ulc_reader.c').read_text()
            fixture = fixture.replace('/* PRODUCTION AUTHENTICATION */',
                function(source, 'ulc_reader_rng') + '\n' +
                function(source, 'pcd_14a_reader_mf0_ulc_auth'))
            path = directory / 'reader.c'
            path.write_text(fixture)
            binary = directory / 'reader'
            rfid = ROOT / 'firmware/application/src/rfid'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(CRYPTO), '-I', str(rfid),
                '-I', str(ROOT / 'firmware/application/src'),
                str(path), str(CRYPTO / 'ulc_3des.c'), str(rfid / 'crc_utils.c'),
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_cbc_vectors_and_buffer_contracts(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ulc_3des'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(CRYPTO),
                str(ROOT / 'resource/tools/tests/ulc_3des.c'),
                str(CRYPTO / 'ulc_3des.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
