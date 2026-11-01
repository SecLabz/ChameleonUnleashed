"""Test PM3 qualification evidence and fail-closed parsing without attached hardware."""

from importlib.util import module_from_spec, spec_from_file_location
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'resource/tools/st25tb'


def load(name, filename):
    spec = spec_from_file_location(name, TOOLS / filename)
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rig = load('st25tb_pm3_rig', 'rig.py')
with mock.patch.dict(sys.modules, {'rig': rig}):
    bench = load('st25tb_pm3_bench', 'bench.py')

INFO = '[+] UID: D0 02 06 05 04 03 02 01\n[+] Chip: 0C, SRT512\n'
# Fixed synthetic UID/CRC-B vectors in PM3 wire format.
# GET_UID returns wire-order UID bytes.
CHAIN = '''
  10 | 20 | Rdr | 06 00 97 5B | ok | INITIATE
  21 | 30 | Tag | A1 FB 44 | ok |
  40 | 50 | Rdr | 0E A1 D4 21 | ok | SELECT
  51 | 60 | Tag | A1 FB 44 | ok |
  70 | 80 | Rdr | 0B AB 4E | ok | GET_UID
  81 | 90 | Tag | 01 02 03 04 05 06 02 D0 A9 49 | ok |
'''
# Fixed selection/CRC vectors exercise changing physical-tag selection bytes.
PHYSICAL_CHAIN = CHAIN.replace('A1 FB 44', '21 F3 C0').replace('0E A1 D4 21', '0E 21 DC A5')
NEXT_PHYSICAL_CHAIN = CHAIN.replace('A1 FB 44', 'D2 E7 05').replace('0E A1 D4 21', '0E D2 C8 60')


def transcript(output=INFO, trace=CHAIN * 2):
    return ('[usb] pm3 --> hf 14b reader\n' + INFO
            + '[usb] pm3 --> hf 14b info\n' + output
            + '[usb] pm3 --> trace list -t 14b\n' + trace)


class OutputTests(unittest.TestCase):
    def setUp(self):
        self.memory = rig.image()

    def test_info_checks_uid_and_model_with_ansi_colors(self):
        output = '\x1b[32m' + INFO + '\x1b[0m'
        self.assertFalse(rig.check_output(output, 'hf 14b info', self.memory)['errors'])
        for value in [INFO.replace('06', '07'), INFO.replace('SRT512', 'SRIX4K'), 'UID:']:
            with self.subTest(value=value):
                self.assertTrue(rig.check_output(value, 'hf 14b info', self.memory)['errors'])

    def dump(self):
        return '\n'.join(f'[+] {block:3}/0x{block:02X} | '
                         + rig.block_value(self.memory, block).hex(' ').upper() + ' | unlocked |'
                         for block in [*range(16), 255])

    def test_dump_requires_every_block_and_exact_contents(self):
        output = self.dump()
        self.assertFalse(rig.check_output(output, 'hf 14b dump --ns', self.memory)['errors'])
        incomplete = '\n'.join(output.splitlines()[1:])
        changed = output.replace('00 01 02 03', '01 01 02 03')
        for value in [incomplete, changed, output + '\n' + output.splitlines()[0], '255/0xFF']:
            with self.subTest(value=value):
                self.assertTrue(rig.check_output(value, 'hf 14b dump --ns', self.memory)['errors'])

    def test_read_checks_address_value_and_retries(self):
        value = rig.block_value(self.memory, 15).hex(' ')
        output = f'[+] block 15... {value} | ascii\n[-] crc fail, retrying one more time'
        result = rig.check_output(output, 'hf 14b rdbl -b 15', self.memory)
        self.assertFalse(result['errors'])
        self.assertEqual(result['retries'], 1)
        self.assertTrue(rig.check_output(output, 'hf 14b rdbl -b 14', self.memory)['errors'])

    def test_split_fails_if_client_omits_a_command_output(self):
        self.assertEqual(len(rig.split_commands(transcript(), 3)), 3)
        with self.assertRaises(ValueError):
            rig.split_commands(transcript(), 4)


class TraceTests(unittest.TestCase):
    def result(self, text, *, physical_tag=False):
        return rig.trace_result(text, rig.image(), physical_tag=physical_tag)[0]

    def test_crc_and_complete_fixture_replies(self):
        result = self.result(CHAIN)
        self.assertEqual(result['ok'], 3)
        self.assertFalse(result['errors'])

    def test_physical_tag_selection_can_change_without_relaxing_emulator_checks(self):
        text = PHYSICAL_CHAIN + NEXT_PHYSICAL_CHAIN
        result = self.result(text, physical_tag=True)
        self.assertEqual(result['ok'], 6)
        self.assertFalse(result['errors'])
        self.assertTrue(self.result(text)['errors'])

    def test_physical_tag_select_requires_the_announced_address_and_echo(self):
        cases = [PHYSICAL_CHAIN.replace('0E 21 DC A5', '0E D2 C8 60'),
                 PHYSICAL_CHAIN.replace('51 | 60 | Tag | 21 F3 C0',
                                        '51 | 60 | Tag | D2 E7 05')]
        for text in cases:
            with self.subTest(text=text):
                result = self.result(text, physical_tag=True)
                self.assertEqual(result['commands']['SELECT']['bad'], 1)
                self.assertTrue(result['errors'])

    def test_physical_tag_does_not_learn_invalid_or_missing_initiate_replies(self):
        cases = [PHYSICAL_CHAIN.replace('21 F3 C0', '21 F3 C1', 1),
                 PHYSICAL_CHAIN.replace('21 F3 C0', 'F3 C0', 1),
                 PHYSICAL_CHAIN.replace('06 00 97 5B', '06 00 97 5A'),
                 PHYSICAL_CHAIN.replace('  21 | 30 | Tag | 21 F3 C0 | ok |\n', '')]
        for text in cases:
            with self.subTest(text=text):
                result = self.result(text, physical_tag=True)
                self.assertEqual(result['commands']['SELECT']['bad'], 1)
                self.assertGreaterEqual(result['request_bad'], 1)
                self.assertTrue(result['errors'])

    def test_physical_tag_failed_initiate_cannot_reuse_a_previous_address(self):
        text = PHYSICAL_CHAIN + PHYSICAL_CHAIN.replace('21 F3 C0', '21 F3 C1', 1)
        result = self.result(text, physical_tag=True)
        self.assertEqual(result['commands']['SELECT']['ok'], 1)
        self.assertEqual(result['commands']['SELECT']['bad'], 1)
        self.assertTrue(result['errors'])

    def test_physical_tag_still_requires_exact_uid_blocks_and_complete_trace(self):
        # Different synthetic UID and block values both have valid CRCs.
        text = PHYSICAL_CHAIN.replace('01 02 03 04 05 06 02 D0 A9 49',
                                      '08 07 06 05 04 03 02 D0 CC 2F')
        result = self.result(text, physical_tag=True)
        self.assertEqual(result['commands']['GET_UID']['bad'], 1)
        self.assertTrue(result['errors'])
        text = PHYSICAL_CHAIN + ('100 | 200 | Rdr | 08 02 95 E2 | ok |\n'
                                 '201 | 300 | Tag | DE AD BE EF CB E5 | ok |\n')
        result = self.result(text, physical_tag=True)
        self.assertEqual(result['commands']['READ_BLK']['bad'], 1)
        self.assertTrue(result['errors'])
        result = self.result(PHYSICAL_CHAIN, physical_tag=True)
        rig.check_trace_coverage(result, 'hf 14b info', 1)
        self.assertTrue(result['errors'])

    def test_bad_column_crc_length_content_and_request_are_rejected(self):
        cases = [CHAIN.replace('A1 FB 44 | ok', 'A1 FB 44 | !!', 1),
                 CHAIN.replace('A1 FB 44', 'A1 FB 45', 1),
                 CHAIN.replace('A1 FB 44', 'A1 FB', 1),
                 CHAIN.replace('01 02 03 04 05 06 02 D0 A9 49', '01 02 03 04 05 06 02 D0 A9 48'),
                 CHAIN.replace('06 00 97 5B', '06 00 97 5A')]
        for text in cases:
            with self.subTest(text=text):
                self.assertTrue(self.result(text)['errors'])

    def test_missing_reply_and_empty_trace_fail(self):
        result = self.result('\n'.join(CHAIN.splitlines()[:-1]))
        self.assertEqual(result['missing'], 1)
        self.assertTrue(self.result('no trace available')['errors'])

    def test_orphan_tag_rows_fail_with_or_without_valid_crc(self):
        for tag in ['10 | 20 | Tag | A1 FB 44 | ok |\n',
                    '10 | 20 | Tag | DE AD BE EF | !! |\n']:
            for text in [tag + CHAIN, CHAIN + tag]:
                with self.subTest(text=text):
                    result = self.result(text)
                    self.assertEqual(result['ok'], 3)
                    self.assertEqual(result['invalid'], 1)
                    self.assertTrue(result['errors'])

    def test_reply_to_an_unknown_reader_command_is_not_an_orphan(self):
        standard = ('10 | 20 | Rdr | 05 00 00 71 FF | ok | REQB\n'
                    '21 | 30 | Tag | A1 FB 44 | ok |\n')
        result = self.result(standard + CHAIN)
        self.assertEqual(result['ok'], 3)
        self.assertEqual(result['invalid'], 0)
        self.assertFalse(result['errors'])

    def test_partial_or_parity_rows_cannot_hide_in_a_clean_trace(self):
        for data in ['A1 FB 44!', 'A1 FB 44\'', '', '[]', '...']:
            text = CHAIN.replace('A1 FB 44', data, 1)
            with self.subTest(data=data):
                self.assertGreater(self.result(text)['invalid'], 0)

    def test_malformed_reader_row_breaks_reply_adjacency(self):
        text = '10 | 20 | Rdr | 0B AB 4E | ok |\n' + '21 | 22 | Rdr | 06! | !! |\n'
        text += '23 | 24 | Tag | 01 02 03 04 05 06 02 D0 A9 49 | ok |\n'
        result = self.result(text)
        self.assertEqual(result['ok'], 0)
        self.assertEqual(result['missing'], 1)
        self.assertEqual(result['invalid'], 1)

    def test_clean_but_truncated_session_trace_fails_coverage(self):
        result = self.result(CHAIN)
        rig.check_trace_coverage(result, 'hf 14b info', 1)
        self.assertTrue(result['errors'])
        result = self.result(CHAIN * 2)
        rig.check_trace_coverage(result, 'hf 14b info', 1)
        self.assertFalse(result['errors'])

    def test_read_coverage_cannot_replace_a_block_with_repeated_reads(self):
        read_zero = ('100 | 200 | Rdr | 08 00 87 C1 | ok |\n'
                     '201 | 300 | Tag | 00 01 02 03 29 A7 | ok |\n')
        result = self.result(CHAIN * 3 + read_zero * 17)
        self.assertFalse(result['errors'])
        rig.check_trace_coverage(result, 'hf 14b dump --ns', 1)
        self.assertTrue(result['errors'])
        result = self.result(CHAIN * 2 + read_zero)
        rig.check_trace_coverage(result, 'hf 14b rdbl -b 15', 1)
        self.assertTrue(result['errors'])
        result = self.result(CHAIN * 2 + read_zero)
        rig.check_trace_coverage(result, 'hf 14b rdbl -b 0', 1)
        self.assertFalse(result['errors'])
        capture = ROOT / 'resource/tools/tests/st25tb/pm3-trace.txt'
        result = self.result(capture.read_text(errors='replace'))
        self.assertEqual(result['bad'], 0)
        self.assertEqual(result['missing'], 1)
        previous_errors = list(result['errors'])
        rig.check_trace_coverage(result, 'hf 14b dump --ns', 3)
        self.assertEqual(result['errors'], previous_errors)


class BenchTests(unittest.TestCase):
    def run_bench(self, response, *, physical_tag=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        destination = Path(temporary.name) / 'result'
        arguments = ['--sessions', '1', '--repeat', '1', '--command', 'hf 14b info',
                     '--output', str(destination), '--label', 'test-build']
        if physical_tag:
            arguments.append('--physical-tag')
        with mock.patch.object(rig, 'pm3_port', return_value='/test/pm3'):
            with mock.patch.object(rig, 'pm3', side_effect=['PM3 test version', response]):
                with mock.patch('sys.stdout', new=io.StringIO()):
                    status = bench.main(arguments)
        return status, json.loads((destination / 'result.json').read_text()), destination

    def test_physical_tag_mode_is_recorded_and_used_for_trace_scoring(self):
        response = transcript(trace=PHYSICAL_CHAIN + NEXT_PHYSICAL_CHAIN)
        status, report, destination = self.run_bench(response, physical_tag=True)
        self.assertEqual(status, 0)
        self.assertTrue(report['physical_tag'])
        self.assertEqual((destination / 'session-0001.txt').read_text(), response)
        status, report, _ = self.run_bench(response)
        self.assertEqual(status, 1)
        self.assertFalse(report['physical_tag'])

    def test_retries_content_mismatch_and_incomplete_trace_fail_the_run(self):
        cases = [transcript(INFO + 'retrying one more time'),
                 transcript(INFO.replace('06', '07')), transcript(trace=CHAIN)]
        for response in cases:
            with self.subTest(response=response):
                status, report, _ = self.run_bench(response)
                self.assertEqual(status, 1)
                self.assertFalse(report['passed'])

    def test_process_errors_and_interruptions_preserve_failed_summary(self):
        for failure in [RuntimeError('PM3 exited 1\nraw output'), KeyboardInterrupt()]:
            with self.subTest(failure=failure):
                status, report, destination = self.run_bench(failure)
                self.assertEqual(status, 1)
                self.assertTrue(report['errors'])
                self.assertTrue((destination / 'error.txt').exists())

    def test_transport_rejects_nonzero_exit_and_timeout(self):
        with mock.patch.object(rig, 'pm3_port', return_value='/test/pm3'):
            result = subprocess.CompletedProcess([], 2, stdout='output', stderr='error')
            with mock.patch.object(rig.subprocess, 'run', return_value=result):
                with self.assertRaisesRegex(RuntimeError, 'outputerror'):
                    rig.pm3('hw version')
            with mock.patch.object(rig.subprocess, 'run',
                                   side_effect=subprocess.TimeoutExpired([], 1, output=b'partial')):
                with self.assertRaisesRegex(RuntimeError, 'partial'):
                    rig.pm3('hw version', timeout=1)


class CaptureTests(unittest.TestCase):
    def test_replay_rejects_partial_bank_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'partial.bin'
            with ZipFile(ROOT / 'resource/tools/tests/st25tb/captures.zip') as archive:
                path.write_bytes(archive.read('lost/m1.bin')[:-1])
            result = subprocess.run([str(TOOLS / 'replay.sh'), str(path)], capture_output=True,
                                    text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('invalid or truncated capture', result.stderr)


if __name__ == '__main__':
    unittest.main()
