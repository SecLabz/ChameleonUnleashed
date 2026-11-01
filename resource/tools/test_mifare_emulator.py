"""Differential emulator crypto, framing and Classic access-condition checks."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'
HF = RFID / 'nfctag/hf'
FIXTURES = Path(__file__).with_name('tests')


class MifareEmulatorTests(unittest.TestCase):
    def compile_and_run(self, sources, directory):
        binary = directory / 'test'
        subprocess.run([
            'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-I', str(directory),
            '-I', str(RFID), '-I', str(HF),
            *map(str, sources), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)

    def test_cipher_parity_authentication_speculation_and_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'cmsis_gcc.h').write_text(
                '#include <stdint.h>\n'
                'static inline uint32_t __REV(uint32_t x) { return __builtin_bswap32(x); }\n')
            crypto = directory / 'mf1_emulator_crypto.c'
            crypto.write_text((HF / 'mf1_emulator_crypto.c').read_text().replace(
                '__attribute__((section(".mf1_crypto_tables")))', ''))
            self.compile_and_run([
                FIXTURES / 'mifare_emulator_crypto.c',
                crypto, HF / 'crypto1_helper.c',
                RFID / 'mf1_crapto1.c', RFID / 'parity.c',
            ], directory)

    def test_all_access_groups_and_malformed_encodings(self):
        source = function((HF / 'nfc_mf1.c').read_text(), 'GetAccessCondition')
        fixture = (FIXTURES / 'mifare_access.c').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            test = directory / 'access.c'
            test.write_text(fixture.replace('/* ACCESS CONDITION FUNCTION */', source))
            self.compile_and_run([test], directory)

    def test_tables_after_cold_clear_and_warm_restart(self):
        source = (HF / 'mf1_emulator_crypto.c').read_text().replace(
            '__attribute__((section(".mf1_crypto_tables")))', '')
        fixture = (FIXTURES / 'mifare_emulator_tables.c').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            test = directory / 'tables.c'
            test.write_text(source + '\n' + fixture)
            self.compile_and_run([test, RFID / 'parity.c'], directory)


if __name__ == '__main__':
    unittest.main()
