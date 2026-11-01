"""Verify read-only CDC stress outcomes, latency and cleanup without connected hardware."""
from importlib.util import module_from_spec, spec_from_file_location
import io
import json
from pathlib import Path
import struct
from types import SimpleNamespace
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'resource/tools/st25tb/usb_stress.py'
spec = spec_from_file_location('st25tb_usb_stress', TOOL)
usb_stress = module_from_spec(spec)
spec.loader.exec_module(usb_stress)

CAPABILITIES = struct.pack('!34H', 1000, 1002, 1035, *range(2000, 2030), 4310)


def response_data(command):
    return {1000: b'\x02\x00', 1002: b'\x00', 1035: CAPABILITIES}[command]


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class UsbStressTests(unittest.TestCase):
    def run_stress(self, response=None, stop=False, open_error=False, interrupt_sleep=False, max_latency=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        output = directory / 'result'
        stop_file = directory / 'stop'
        clock = Clock()
        com = mock.Mock()
        def reply(command, callback, timeout):
            self.assertEqual(timeout, 3)
            if response is not None:
                value = response(command, clock, stop_file)
            else:
                clock.now += 0.005
                value = SimpleNamespace(status=0x68, data=response_data(command))
            callback(command, value.status, value.data)
        com.send_cmd_auto.side_effect = reply
        if open_error:
            com.open.side_effect = OSError('port unavailable')
        sleep = clock.sleep
        if interrupt_sleep:
            sleep = mock.Mock(side_effect=KeyboardInterrupt())
        arguments = ['--duration', '0.25', '--port', '/test/CDC', '--output', str(output), '--label', 'test-package']
        if max_latency is not None:
            arguments.extend(['--max-latency', str(max_latency)])
        if stop:
            arguments.extend(['--stop-file', str(stop_file)])
        with mock.patch.object(usb_stress, 'create_connection', return_value=com) as constructor:
            with mock.patch.object(usb_stress.time, 'monotonic', side_effect=clock.monotonic):
                with mock.patch.object(usb_stress.time, 'sleep', side_effect=sleep):
                    with mock.patch('sys.stdout', new=io.StringIO()):
                        status = usb_stress.main(arguments)
        constructor.assert_called_once_with()
        com.open.assert_called_once_with('/test/CDC')
        com.close.assert_called_once()
        rows = [json.loads(line) for line in (output / 'commands.jsonl').read_text().splitlines()]
        result = json.loads((output / 'result.json').read_text())
        return status, result, rows, com

    def test_queries_all_three_read_only_commands_on_one_connection(self):
        status, result, rows, com = self.run_stress()
        self.assertEqual(status, 0)
        self.assertEqual(result['queries'], 9)
        self.assertEqual(result['validated_queries'], 9)
        self.assertEqual(result['stop_reason'], 'duration')
        self.assertTrue(result['connection_closed'])
        self.assertEqual([call.args[0] for call in com.send_cmd_auto.call_args_list], [1000, 1002, 1035] * 3)
        com.send_cmd_sync.assert_not_called()
        commands = [row for row in rows if row['type'] == 'command']
        self.assertAlmostEqual(commands[0]['duration_seconds'], 0.005)
        self.assertAlmostEqual(commands[3]['started_monotonic_s'], 0.1)
        self.assertEqual(commands[2]['data_hex'], CAPABILITIES.hex())
        self.assertEqual(result['commands']['1035']['queries'], 3)
        self.assertEqual(rows[0]['type'], 'metadata')
        self.assertEqual(rows[-1]['type'], 'summary')
        self.assertEqual(result['label'], 'test-package')

    def test_version_shape_bad_status_and_nonemulator_mode_fail(self):
        cases = [lambda command, clock, stop: SimpleNamespace(status=0x60, data=b'\x02\0'),
                 lambda command, clock, stop: SimpleNamespace(status=0x68, data=b'\x02'),
                 lambda command, clock, stop: SimpleNamespace(status=0x68, data=b'\x02\0' if command == 1000 else b'\1')]
        for response in cases:
            with self.subTest(response=response):
                status, result, rows, _ = self.run_stress(response=response)
                self.assertEqual(status, 1)
                self.assertEqual(result['errors'], 1)
                self.assertEqual(result['stop_reason'], 'error')
                self.assertTrue(any(row.get('errors') for row in rows if row['type'] == 'command'))

    def test_capabilities_reject_truncation_shape_and_missing_commands(self):
        short = struct.pack('!32H', 1000, 1002, 1035, 4310, *range(2000, 2028))
        cases = {'truncated_word': CAPABILITIES[:-1],
                 'truncated_required_tail': CAPABILITIES[:-2],
                 'one_packet_body': short,
                 'empty': b'',
                 'missing_required': struct.pack('!34H', *range(2000, 2034)),
                 'duplicates': CAPABILITIES + struct.pack('!H', 1000)}
        for name, data in cases.items():
            with self.subTest(name=name):
                def response(command, clock, stop):
                    clock.now += 0.005
                    return SimpleNamespace(status=0x68, data=data if command == 1035 else response_data(command))
                status, result, rows, _ = self.run_stress(response=response)
                self.assertEqual(status, 1)
                self.assertEqual(result['errors'], 1)
                self.assertEqual(result['queries'], 3)
                capability = next(row for row in rows if row.get('command') == 1035)
                self.assertTrue(capability['errors'])
                self.assertEqual(capability['data_hex'], data.hex())

    def test_capabilities_allow_additions_and_reordered_big_endian_codes(self):
        data = struct.pack('!H', 65000) + b''.join(reversed([CAPABILITIES[i:i + 2]
                                                         for i in range(0, len(CAPABILITIES), 2)]))
        def response(command, clock, stop):
            clock.now += 0.005
            return SimpleNamespace(status=0x68, data=data if command == 1035 else response_data(command))
        status, result, rows, _ = self.run_stress(response=response)
        self.assertEqual(status, 0)
        self.assertEqual(result['commands']['1035']['queries'], 3)
        self.assertEqual(result['commands']['1035']['errors'], 0)

    def test_client_timeout_callback_is_recorded_as_timeout(self):
        def timeout(command, clock, stop):
            clock.now += 3
            return SimpleNamespace(status=None, data=None)
        status, result, rows, _ = self.run_stress(response=timeout)
        self.assertEqual(status, 1)
        self.assertEqual(result['timeouts'], 1)
        self.assertEqual(result['max_duration_seconds'], 3)
        command = next(row for row in rows if row['type'] == 'command')
        self.assertTrue(command['timeout'])

    def test_disconnect_before_callback_registration_cannot_hang(self):
        com = mock.Mock()
        # A disconnect can discard the queued task before the client registers
        # its callback. Exercise the real bounded wait without a client callback.
        com.send_cmd_auto.return_value = None
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'result'
            started = time.perf_counter()
            with mock.patch.object(usb_stress, 'create_connection', return_value=com):
                with mock.patch.object(usb_stress, 'TIMEOUT_SECONDS', 0.03):
                    with mock.patch('sys.stdout', new=io.StringIO()):
                        status = usb_stress.main(['--port', '/test/disconnected', '--duration', '1',
                                                  '--max-latency', '0.02', '--output', str(output)])
            elapsed = time.perf_counter() - started
            result = json.loads((output / 'result.json').read_text())
            self.assertEqual(status, 1)
            self.assertEqual(result['timeouts'], 1)
            self.assertLess(elapsed, 1)
            self.assertGreaterEqual(result['max_duration_seconds'], 0.025)
            com.open.assert_called_once()
            com.close.assert_called_once()
            com.send_cmd_sync.assert_not_called()

    def test_duplicate_callback_does_not_escape_into_client_thread(self):
        com = mock.Mock()
        def send(command, callback, timeout):
            callback(command, 0x68, b'\x02\0')
            callback(command, None, None)
        com.send_cmd_auto.side_effect = send
        self.assertEqual(usb_stress.read_response(com, usb_stress.Command.GET_APP_VERSION),
                         (0x68, b'\x02\0'))

    def test_callback_deadline_also_includes_time_spent_queuing(self):
        clock = Clock()
        com = mock.Mock()
        def send(command, callback, timeout):
            clock.now += 3.01
            callback(command, 0x68, b'\x02\0')
        com.send_cmd_auto.side_effect = send
        with mock.patch.object(usb_stress.time, 'monotonic', side_effect=clock.monotonic):
            with self.assertRaises(TimeoutError):
                usb_stress.read_response(com, usb_stress.Command.GET_APP_VERSION)

    def test_latency_limit_2501ms_is_enforced_before_timeout(self):
        def slow(command, clock, stop):
            clock.now += 2.501
            return SimpleNamespace(status=0x68, data=b'\x02\0')
        status, result, rows, _ = self.run_stress(response=slow)
        self.assertEqual(status, 1)
        self.assertEqual(result['timeouts'], 0)
        self.assertEqual(result['queries'], 1)
        self.assertAlmostEqual(result['max_duration_seconds'], 2.501)
        self.assertEqual(result['latency_limit_seconds'], 2.5)

    def test_valid_reply_with_20052ms_transport_latency_passes(self):
        def slow(command, clock, stop):
            clock.now += 2.0052
            return SimpleNamespace(status=0x68, data=b'\x02\0')
        status, result, rows, _ = self.run_stress(response=slow)
        self.assertEqual(status, 0)
        self.assertEqual(result['errors'], 0)
        self.assertEqual(result['validated_queries'], 1)
        self.assertAlmostEqual(result['max_duration_seconds'], 2.0052)
        status, result, rows, _ = self.run_stress(response=slow, max_latency=2.0)
        self.assertEqual(status, 1)
        self.assertEqual(result['latency_limit_seconds'], 2.0)

    def test_stop_file_saves_summary_and_does_not_delete_stop(self):
        def response(command, clock, stop):
            clock.now += 0.005
            if command == 1002:
                stop.touch()
            return SimpleNamespace(status=0x68, data=b'\x02\0' if command == 1000 else b'\0')
        status, result, rows, _ = self.run_stress(response=response, stop=True)
        self.assertEqual(status, 0)
        self.assertEqual(result['stop_reason'], 'stop_file')
        self.assertEqual(result['queries'], 2)

    def test_sigint_during_query_or_wait_preserves_summary_and_cleanup(self):
        def response(command, clock, stop):
            raise KeyboardInterrupt()
        status, result, rows, _ = self.run_stress(response=response)
        self.assertEqual(status, 0)
        self.assertEqual(result['stop_reason'], 'sigint')
        self.assertEqual(result['interrupted_queries'], 1)
        self.assertEqual(result['validated_queries'], 0)
        status, result, rows, _ = self.run_stress(interrupt_sleep=True)
        self.assertEqual(status, 0)
        self.assertEqual(result['stop_reason'], 'sigint')
        self.assertEqual(result['validated_queries'], 3)

    def test_connect_error_still_closes_and_records_failure(self):
        status, result, rows, com = self.run_stress(open_error=True)
        self.assertEqual(status, 1)
        self.assertEqual(result['queries'], 0)
        self.assertEqual(result['errors'], 1)
        self.assertTrue(any(row['type'] == 'error' for row in rows))


if __name__ == '__main__':
    unittest.main(verbosity=2)
