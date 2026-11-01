"""Active B/V codecs and bounded reader commands; RF qualification is separate."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'


class ReaderTest(unittest.TestCase):
    def test_st25tb_usb_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'cleanup'
            tests = ROOT / 'firmware/application/tests'
            subprocess.run([
                'cc', '-Os', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined',
                '-I', str(tests / 'st25tb_tearoff_stubs'),
                '-I', str(RFID.parent),
                str(tests / 'test_st25tb_read_cleanup.c'), '-o', str(target),
            ], check=True)
            subprocess.run([str(target)], check=True, timeout=10)

    def test_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / 'nrf.h').write_text(
                '#pragma once\n#include <stdint.h>\n'
                'typedef struct {uint32_t CYCCNT, CTRL;} test_dwt_t;\n'
                'extern test_dwt_t test_dwt;\n#define DWT (&test_dwt)\n'
                '#define DWT_CTRL_CYCCNTENA_Msk 1u\n'
                'typedef struct {uint32_t DEMCR;} test_core_t;\n'
                'extern test_core_t test_core;\n#define CoreDebug (&test_core)\n'
                '#define CoreDebug_DEMCR_TRCENA_Msk 1u\n')
            (directory / 'ble_main.h').write_text(
                '#include <stdbool.h>\nbool advertising_stop_checked(void);\n'
                'void advertising_start(bool erase_bonds);\n')
            (directory / 'bsp_wdt.h').write_text('void bsp_wdt_feed(void);\n')
            (directory / 'rfid_main.h').write_text(
                '#define DEVICE_MODE_READER 1\nint get_device_mode(void);\n')
            target = directory / 'commands'
            compiler = [
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(directory),
                '-I', str(RFID), '-I', str(RFID.parent), '-I', str(RFID / 'reader/hf'),
            ]
            common_objects = []
            for source in (ROOT / 'resource/tools/tests/hf_reader_commands.c',
                           RFID / 'reader/hf/nfcv_reader.c', RFID / 'reader/hf/nfcb_reader.c',
                           RFID / 'iso15693/frame.c'):
                obj = directory / f'{source.stem}.o'
                subprocess.run([*compiler, '-O3', '-c', str(source), '-o', str(obj)], check=True)
                common_objects.append(str(obj))
            objects = common_objects.copy()
            for name in ('hf_reader', 'st25tb_reader', 'st25tb_tearoff_command'):
                obj = directory / f'{name}.o'
                subprocess.run([
                    *compiler, '-Os', '-c', str(RFID / f'reader/hf/{name}.c'),
                    '-o', str(obj),
                ], check=True)
                objects.append(str(obj))
            subprocess.run([
                'cc', '-fsanitize=address,undefined', *objects, '-o', str(target),
            ], check=True)
            subprocess.run([str(target)], check=True, timeout=10)

    def test_tearoff_contracts(self):
        tests = ROOT / 'firmware/application/tests'
        for name, optimization in (('test_st25tb_tearoff', '-O3'),
                                   ('test_st25tb_tearoff_command', '-Os'),
                                   ('test_st25tb_tearoff_read', '-O3')):
            with self.subTest(test=name), tempfile.TemporaryDirectory() as directory:
                target = Path(directory) / name
                subprocess.run([
                    'cc', optimization, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I', str(tests / 'st25tb_tearoff_stubs'),
                    '-I', str(RFID.parent), '-I', str(RFID),
                    '-I', str(RFID / 'reader/hf'),
                    str(tests / f'{name}.c'), '-o', str(target),
                ], check=True)
                subprocess.run([str(target)], check=True, timeout=10)

    def test_tx(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'tx'
            subprocess.run([
                'cc', '-O3', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(RFID / 'reader/hf'),
                str(ROOT / 'resource/tools/tests/nfcb_tx.c'), '-o', str(target),
            ], check=True)
            subprocess.run([str(target)], check=True, timeout=10)

    def test_codecs(self):
        for test, sources in (
            ('nfcb_decode', ['hf/nfcb_decode', 'rf_workspace']),
            ('nfcv_reader', ['hf/nfcv_reader_codec', 'rf_workspace']),
        ):
            with self.subTest(test=test), tempfile.TemporaryDirectory() as directory:
                target = Path(directory) / test
                subprocess.run([
                    'cc', '-O3', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(RFID),
                    '-I', str(RFID / 'reader'),
                    '-I', str(RFID / 'reader/hf'),
                    str(ROOT / f'resource/tools/tests/{test}.c'),
                    *(str(RFID / f'reader/{name}.c') for name in sources),
                    str(RFID / 'iso15693/frame.c'), '-lm', '-o', str(target),
                ], check=True)
                subprocess.run([str(target)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
