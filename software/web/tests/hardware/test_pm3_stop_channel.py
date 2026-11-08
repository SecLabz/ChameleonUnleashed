"""Software fault tests for the experimental persistent PM3 stop descriptor.

These PTY peers are protocol fixtures, not physical RF qualification.
"""
import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from pm3_stop_channel import StopChannel, crc_a


def reply(command, payload, status=0, trailer=b'b3'):
    return struct.pack('<IHbbH', 0x62334d50, 0x8000 | len(payload), status, -1, command) + payload + trailer


class Peer:
    def __init__(self, mode='normal'):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.commands = []
        self.mode = mode
        self.finished = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        pending = bytearray()
        pings = 0
        while not self.finished.is_set():
            if not select.select([self.master], [], [], 0.02)[0]:
                continue
            try:
                pending.extend(os.read(self.master, 4096))
            except OSError:
                return
            while len(pending) >= 8:
                magic, length, command = struct.unpack_from('<IHH', pending)
                size = length & 0x7fff
                total = 10 + size
                if len(pending) < total:
                    break
                if magic != 0x61334d50 or pending[total-2:total] != b'a3':
                    return
                data = bytes(pending[8:8+size])
                del pending[:total]
                self.commands.append(command)
                if command != 0x0109:
                    continue
                pings += 1
                if pings == 1:
                    output = reply(command, data)
                elif self.mode == 'wrong nonce':
                    output = reply(command, bytes(value ^ 0xff for value in data))
                elif self.mode == 'failed frames':
                    output = reply(command, data, status=1) + reply(command, data, trailer=b'xx')
                elif self.mode == 'fragmented':
                    oversized = struct.pack('<IHbbH', 0x62334d50, 0x8201, 0, 0, command)
                    output = b'noisePM' + oversized + reply(0x0100, b'debug') + reply(command, data)
                else:
                    output = reply(command, data)
                try:
                    for index in range(0, len(output), 3):
                        os.write(self.master, output[index:index+3])
                        time.sleep(0.001)
                except OSError:
                    return

    def close(self):
        self.finished.set()
        self.thread.join(timeout=1)
        os.close(self.master)
        os.close(self.slave)


class StopChannelTests(unittest.TestCase):
    def channel(self, mode='normal'):
        peer = Peer(mode)
        self.addCleanup(peer.close)
        channel = StopChannel(peer.path)
        self.addCleanup(channel.close)
        return peer, channel

    def test_stop_keeps_its_descriptor_after_cli_close_and_never_reopens(self):
        peer, channel = self.channel()
        serial_owner = os.open(peer.path, os.O_RDWR | os.O_NOCTTY)
        os.close(serial_owner)
        with patch('pm3_stop_channel.os.open', side_effect=AssertionError('Unexpected serial reopen')):
            channel.stop(timeout=1)
        self.assertEqual(peer.commands, [0x0109, 0x0118, 0x0109])

    def test_wrong_ping_nonce_is_not_success(self):
        _, channel = self.channel('wrong nonce')
        with self.assertRaises(TimeoutError):
            channel.stop(timeout=0.15)

    def test_failed_status_and_invalid_trailer_are_not_success(self):
        _, channel = self.channel('failed frames')
        with self.assertRaises(TimeoutError):
            channel.stop(timeout=0.15)

    def test_fragmented_stream_resynchronizes_after_noise_and_oversized_frame(self):
        _, channel = self.channel('fragmented')
        channel.stop(timeout=1)

    def test_crc_a_check_vector_uses_pm3_postamble_byte_order(self):
        self.assertEqual(crc_a(b'123456789'), bytes.fromhex('bf05'))

    def test_real_pm3_success_ping_with_undefined_reason_is_accepted(self):
        # Captured from this physical PM3 before any simulation was launched.
        wire = bytes.fromhex('504d3362108000ff0901515d5247b64b9a45a9b366339f4ee1816233')
        channel = StopChannel.__new__(StopChannel)
        channel.pending = bytearray(wire)
        self.assertTrue(channel._matching_ping(wire[10:-2]))

    def test_closed_channel_refuses_stop(self):
        _, channel = self.channel()
        channel.close()
        with self.assertRaises(ConnectionError):
            channel.stop(timeout=0.15)


class LauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).with_name('pm3-simulation.py')
        spec = importlib.util.spec_from_file_location('pm3_simulation', path)
        cls.simulation = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.simulation)

    def test_unqualified_classic_is_rejected_before_any_process_starts(self):
        with patch.dict(os.environ, {'CHAMELEON_PM3_TEST_CLASSIC_STOP': ''}), patch.object(self.simulation.subprocess, 'Popen') as spawn:
            with self.assertRaises(ValueError):
                self.simulation.main('unused', 'unused', 'hf mf sim --1k -u 11223344 -i', 'unused')
            spawn.assert_not_called()

    def test_classic_requires_every_explicit_guard_and_accepts_the_exact_command_only(self):
        complete = {'CHAMELEON_PM3_TEST_CLASSIC_STOP': '1', 'CHAMELEON_PM3_TEST_PERSISTENT_STOP': '1',
                    'CHAMELEON_PM3_SIM_RESTORE_BIN': '/explicit/backup.bin'}
        for missing in complete:
            with self.subTest(missing=missing), patch.dict(os.environ, {**complete, missing: ''}):
                with self.assertRaises(ValueError):
                    self.simulation.validate_command('hf mf sim --1k -u 11223344 -i')
        with patch.dict(os.environ, complete):
            self.assertTrue(self.simulation.validate_command('hf mf sim --1k -u 11223344 -i'))

    def test_other_classic_sizes_and_attack_options_are_refused(self):
        complete = {'CHAMELEON_PM3_TEST_CLASSIC_STOP': '1', 'CHAMELEON_PM3_TEST_PERSISTENT_STOP': '1',
                    'CHAMELEON_PM3_SIM_RESTORE_BIN': '/explicit/backup.bin'}
        for command in ('hf mf sim --4k -u 11223344 -i', 'hf mf sim --1k -u 11223344 -i -x',
                        'hf mf sim --1k -u 11223344 -i -e', 'hf mf sim --1k -u 11223344'):
            with self.subTest(command=command), patch.dict(os.environ, complete):
                with self.assertRaises(ValueError):
                    self.simulation.validate_command(command)

    def test_classic_wrong_backup_size_fails_before_any_serial_client_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            backup = root / 'invalid.bin'
            backup.write_bytes(b'bad')
            env = {**os.environ, 'CHAMELEON_PM3_TEST_CLASSIC_STOP': '1',
                   'CHAMELEON_PM3_TEST_PERSISTENT_STOP': '1',
                   'CHAMELEON_PM3_SIM_RESTORE_BIN': str(backup)}
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('pm3-simulation.py')),
                                     '/unused/client', '/unused/port', 'hf mf sim --1k -u 11223344 -i',
                                     str(root / 'simulation.log')], env=env, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'backup must contain exactly 4096 bytes', result.stderr)

    def test_failed_cleanup_retains_timeout_diagnostics_and_cannot_report_success(self):
        failure = subprocess.TimeoutExpired(['fake', 'hw break'], 10, output=b'partial output', stderr=b'error')
        with tempfile.TemporaryDirectory() as directory:
            log = str(Path(directory) / 'simulation.log')
            with patch.object(self.simulation.subprocess, 'run', side_effect=failure):
                with self.assertRaises(subprocess.TimeoutExpired):
                    self.simulation.cleanup('fake', 'unused', log, None, 'test')
            self.assertEqual(Path(log + '.cleanup.log').read_bytes(), b'partial outputerror')
            self.assertFalse(json.loads(Path(log + '.cleanup.json').read_text())['stopped'])

    def test_legacy_fallback_can_stop_rf_without_qualifying_a_failed_persistent_channel(self):
        class FailedChannel:
            received = b'unmatched response'

            def stop(self, timeout):
                raise TimeoutError('matching ping missing')
        with tempfile.TemporaryDirectory() as directory:
            log = str(Path(directory) / 'simulation.log')
            reply = subprocess.CompletedProcess([], 0, stdout=b'Ping response received content ( ok )', stderr=b'')
            with patch.object(self.simulation.subprocess, 'run', return_value=reply) as client:
                result = self.simulation.cleanup('fake', 'unused', log, None, 'test', stop_channel=FailedChannel())
                self.assertEqual(client.call_count, 2)
            self.assertTrue(Path(log + '.cleanup.log').read_bytes().startswith(b'unmatched response'))
            self.assertTrue(result['stopped'])
            self.assertFalse(result['persistentStopPingConfirmed'])

    def test_detached_guard_uses_the_held_channel_after_owner_sigkill(self):
        peer = Peer()
        self.addCleanup(peer.close)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = root / 'fake-pm3'
            client.write_text(f'#!{sys.executable}\n' + '''import sys,time
command = sys.argv[-1]
if command.startswith('hf 14a sim '):
    print('Press a key to abort simulation', flush=True)
    while True:
        time.sleep(0.1)
elif command == 'hw ping':
    print('Ping response received | content ( ok )', flush=True)
''')
            client.chmod(0o700)
            log = root / 'simulation.log'
            env = {**os.environ, 'CHAMELEON_PM3_TEST_PERSISTENT_STOP': '1',
                   'CHAMELEON_PM3_SIM_RESTORE_BIN': '', 'CHAMELEON_PM3_SIM_MAX_SECONDS': '30'}
            owner = subprocess.Popen(
                [sys.executable, str(Path(__file__).with_name('pm3-simulation.py')),
                 str(client), peer.path, 'hf 14a sim -t 1 -u 11223344', str(log)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
            )
            try:
                self.assertTrue(select.select([owner.stdout], [], [], 8)[0])
                self.assertTrue(json.loads(owner.stdout.readline())['ready'])
                owner.kill()
                owner.communicate(timeout=5)
                report = Path(str(log) + '.cleanup.json')
                deadline = time.monotonic() + 8
                while not report.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                result = json.loads(report.read_text())
                self.assertTrue(result['stopped'])
                self.assertTrue(result['persistentStopPingConfirmed'])
                self.assertEqual(peer.commands, [0x0109, 0x0118, 0x0109])
            finally:
                if owner.poll() is None:
                    owner.stdin.close()
                    owner.stdin = None
                    owner.communicate(timeout=8)


if __name__ == '__main__':
    unittest.main()
