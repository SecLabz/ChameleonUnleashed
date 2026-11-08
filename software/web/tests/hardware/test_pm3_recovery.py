"""Recovery workflow failure tests with CLI and USB API fixtures, never real hardware."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('pm3_recovery', Path(__file__).with_name('pm3-recovery.py'))
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class Fixture:
    def __init__(self, root):
        self.root = root
        self.original = bytes(range(256)) * 16
        self.current = bytes(4096)
        self.commands = []
        self.requests = []
        self.ping_confirmed = True
        self.tag_present = False
        self.timeout_restore = False
        self.change_ultra_after_end = False
        self.session = 0
        self.device = {'chip': 'expected-ultra', 'git': 'test-firmware', 'reader': False,
                       'busy': False, 'active_slot': 1, 'settings': {'sleep': 8}}
        for name in ('client', 'port'):
            (root / name).touch()
        (root / 'original.bin').write_bytes(self.original)
        self.manifest = root / 'manifest.json'
        self.manifest.write_text(json.dumps({
            'version': 1, 'pm3Client': str(root / 'client'), 'pm3Port': str(root / 'port'),
            'ramBackup': str(root / 'original.bin'), 'ramSha256': hashlib.sha256(self.original).hexdigest(),
            'ultraURL': 'http://fixture', 'ultraChip': 'expected-ultra'}))

    def http(self, url, body=None):
        self.requests.append((url, body))
        path = url.removeprefix('http://fixture')
        if path == '/api/device':
            return json.loads(json.dumps(self.device))
        if path.startswith('/api/slot/'):
            return bytes([int(path.rsplit('/', 1)[1])]) * 16
        if path == '/api/session':
            self.session = 42
            return {'session': 42}
        if path == '/api/command/2000?id=42':
            return bytes.fromhex('0000041122334404000800') if self.tag_present else b'\x00\x01'
        if path == '/api/end?id=42':
            self.session = 0
            if self.change_ultra_after_end:
                self.device['active_slot'] = 2
            return b''
        raise AssertionError('Unexpected USB request: ' + path)

    def execute(self, args, **options):
        self.commands.append(args[-1])
        assert options == {'capture_output': True, 'timeout': 12, 'check': True}
        command = shlex.split(args[-1])
        output = b''
        if command == ['hw', 'break']:
            pass
        elif command == ['hw', 'ping']:
            output = b'Ping response received | content ( ok )' if self.ping_confirmed else b'No actual reply'
        elif command[:5] == ['hf', 'mf', 'esave', '--4k', '-f']:
            Path(command[5]).with_suffix('.bin').write_bytes(self.current)
        elif command[:5] == ['hf', 'mf', 'eload', '--4k', '-f']:
            self.current = Path(command[5]).read_bytes()
            if self.timeout_restore:
                raise subprocess.TimeoutExpired(args, 12, output=b'load sent; outcome unknown')
        else:
            raise AssertionError('No simulation or flash command is allowed: ' + args[-1])
        return subprocess.CompletedProcess(args, 0, stdout=output, stderr=b'')

    def run(self):
        return recovery.recover(self.manifest, self.root / 'output with spaces', http=self.http, execute=self.execute)

    def report(self):
        return json.loads(next((self.root / 'output with spaces').glob('pm3-recovery-*/result.json')).read_text())


class RecoveryTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return Fixture(Path(temporary.name))

    def test_verified_recovery_keeps_current_backup_and_compares_all_4096_bytes(self):
        fixture = self.fixture()
        report = fixture.run()
        self.assertTrue(report['ready'])
        self.assertTrue(report['ramLoadIssued'])
        self.assertTrue(report['ultraPreserved'])
        self.assertEqual(fixture.current, fixture.original)
        self.assertEqual((Path(report['outputDirectory']) / 'current-before-restore.bin').read_bytes(), bytes(4096))
        self.assertEqual(fixture.session, 0)

    def test_already_restored_ram_is_verified_without_another_load(self):
        fixture = self.fixture()
        fixture.current = fixture.original
        report = fixture.run()
        self.assertTrue(report['ready'])
        self.assertFalse(report['ramLoadIssued'])
        self.assertFalse(any('eload' in command for command in fixture.commands))

    def test_wrong_backup_hash_refuses_all_hardware_requests(self):
        fixture = self.fixture()
        (fixture.root / 'original.bin').write_bytes(bytes(4096))
        with self.assertRaisesRegex(ValueError, 'size/hash'):
            fixture.run()
        self.assertEqual(fixture.commands, [])
        self.assertEqual(fixture.requests, [])

    def test_missing_port_refuses_all_hardware_requests(self):
        fixture = self.fixture()
        (fixture.root / 'port').unlink()
        with self.assertRaisesRegex(ValueError, 'physical reconnection'):
            fixture.run()
        self.assertEqual(fixture.commands, [])
        self.assertEqual(fixture.requests, [])

    def test_wrong_ultra_identity_cannot_send_pm3_commands(self):
        fixture = self.fixture()
        fixture.device['chip'] = 'another-ultra'
        with self.assertRaisesRegex(RuntimeError, 'identity/mode'):
            fixture.run()
        self.assertEqual(fixture.commands, [])
        self.assertFalse(fixture.report()['ready'])

    def test_cli_exit_zero_without_ping_content_never_loads_ram_or_reports_ready(self):
        fixture = self.fixture()
        fixture.ping_confirmed = False
        with self.assertRaisesRegex(RuntimeError, 'confirmed ping'):
            fixture.run()
        self.assertFalse(any('eload' in command for command in fixture.commands))
        self.assertFalse(fixture.report()['ready'])

    def test_remaining_rf_identity_rejects_readiness_and_ends_the_ultra_lease(self):
        fixture = self.fixture()
        fixture.tag_present = True
        with self.assertRaisesRegex(RuntimeError, 'RF absence'):
            fixture.run()
        self.assertFalse(fixture.report()['ready'])
        self.assertTrue(fixture.report()['ramRestored'])
        self.assertEqual(fixture.session, 0)

    def test_unknown_load_outcome_is_not_retried_or_marked_restored(self):
        fixture = self.fixture()
        fixture.timeout_restore = True
        with self.assertRaises(subprocess.TimeoutExpired):
            fixture.run()
        self.assertEqual(sum('eload' in command for command in fixture.commands), 1)
        report = fixture.report()
        self.assertFalse(report['ready'])
        self.assertFalse(report['ramRestored'])
        self.assertIn(b'outcome unknown', (Path(report['outputDirectory']) / 'restore-original.log').read_bytes())

    def test_changed_ultra_state_cannot_report_ready(self):
        fixture = self.fixture()
        fixture.change_ultra_after_end = True
        with self.assertRaisesRegex(RuntimeError, 'Ultra state changed'):
            fixture.run()
        self.assertFalse(fixture.report()['ready'])


if __name__ == '__main__':
    unittest.main()
