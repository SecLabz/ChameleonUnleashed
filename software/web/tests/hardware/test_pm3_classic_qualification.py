"""Software-only gates: these tests never open a serial device or run PM3."""
import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from pm3_classic_qualification import CLASSIC_SCANS, FAULTS, require_stop_report
from pm3_identity import identity_digest


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.client = self.root / 'synthetic-client'
        self.client.write_bytes(b'software-only fixture')
        self.report_path = self.root / 'results.json'
        self.port = '/not/a/serial/device'
        self.identity = {
            'Model': ['Firmware.................. PM3 GENERIC'],
            'ARM': ['Bootrom.... fixture bootrom', 'OS......... fixture OS'],
            'FPGA': ['fpga_pm3_hf.ncd image fixture'],
            'Hardware': ['--= uC: synthetic processor'],
        }
        path = Path(__file__).with_name('pm3-simulation.py')
        spec = importlib.util.spec_from_file_location('simulation_gate', path)
        self.launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.launcher)

    def report(self, size):
        state = {'chip': 'synthetic', 'git': 'test', 'settings': {}, 'reader': False, 'active_slot': 1, 'busy': False}
        digest = 'a' * 64
        return {'command': f'hf mf sim --{size} -u 11223344 -i', 'port': self.port,
                'clientSha256': hashlib.sha256(self.client.read_bytes()).hexdigest(),
                'pm3Identity': self.identity, 'pm3IdentityAfter': copy.deepcopy(self.identity),
                'before': state, 'after': copy.deepcopy(state),
                'results': [{'mode': mode, 'positiveScan': CLASSIC_SCANS[size], 'negativeScan': '0001',
                             'maximumSeconds': 5 if mode == 'hard deadline' else 30,
                             'originalRamSha256': digest,
                             'pm3IdentitySha256': identity_digest(self.identity),
                             'ownerCode': 0 if mode == 'normal EOF' else -9 if 'owner SIGKILL' in mode else 1,
                             'cleanup': {'stopped': True, 'ramRestored': True,
                                         'persistentStopPingConfirmed': True, 'ramSha256': digest}}
                            for mode in FAULTS]}

    def validate(self, report, size='4k'):
        self.report_path.write_text(json.dumps(report))
        return require_stop_report(f'hf mf sim --{size} -u 11223344 -i', self.report_path, self.client, self.port)

    def guards(self):
        return {'CHAMELEON_PM3_TEST_CLASSIC_STOP': '1', 'CHAMELEON_PM3_TEST_PERSISTENT_STOP': '1',
                'CHAMELEON_PM3_SIM_RESTORE_BIN': '/explicit/full-ram.bin',
                'CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT': '', 'CHAMELEON_PM3_CLASSIC_STOP_REPORT': ''}

    def test_complete_reports_accept_only_the_exact_size_client_and_port(self):
        for size in ('mini', '2k', '4k'):
            with self.subTest(size=size):
                report = self.report(size)
                self.assertEqual(self.validate(report, size), report)
                for field, wrong in [('port', '/other'), ('clientSha256', 'b' * 64),
                                     ('command', 'hf mf sim --1k -u 11223344 -i')]:
                    changed = copy.deepcopy(report)
                    changed[field] = wrong
                    with self.assertRaises(ValueError):
                        self.validate(changed, size)

    def test_missing_or_duplicate_faults_and_state_changes_are_refused(self):
        original = self.report('4k')
        for edit in (lambda r: r['results'].pop(),
                     lambda r: r['results'].__setitem__(3, r['results'][0]),
                     lambda r: r['after'].__setitem__('settings', {'changed': True}),
                     lambda r: r['after'].__setitem__('busy', True),
                     lambda r: r.__setitem__('results', ['bad'] * 4),
                     lambda r: r.__setitem__('after', [])):
            changed = copy.deepcopy(original)
            edit(changed)
            with self.assertRaises(ValueError):
                self.validate(changed)

    def test_every_fault_requires_direct_stop_restored_ram_and_rf_disappearance(self):
        for index in range(4):
            for field, value in [('negativeScan', '0000'), ('positiveScan', CLASSIC_SCANS['1k']),
                                 ('maximumSeconds', 180), ('ownerCode', None), ('ownerCode', False),
                                 ('originalRamSha256', 'b' * 64)]:
                changed = self.report('4k')
                changed['results'][index][field] = value
                with self.subTest(index=index, field=field, value=value), self.assertRaises(ValueError):
                    self.validate(changed)
            for field in ('stopped', 'persistentStopPingConfirmed', 'ramRestored'):
                changed = self.report('4k')
                changed['results'][index]['cleanup'][field] = False
                with self.subTest(index=index, field=field), self.assertRaises(ValueError):
                    self.validate(changed)

    def test_unqualified_new_sizes_fail_before_any_process_launches(self):
        with patch.dict(os.environ, self.guards()), patch.object(self.launcher.subprocess, 'Popen') as spawn:
            for size in ('mini', '2k', '4k'):
                with self.assertRaises(ValueError):
                    self.launcher.main(str(self.client), self.port, f'hf mf sim --{size} -u 11223344 -i', 'unused')
            spawn.assert_not_called()

    def test_preflight_is_exact_size_and_bounded_to_thirty_seconds(self):
        for size in ('mini', '2k', '4k'):
            command = f'hf mf sim --{size} -u 11223344 -i'
            with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT': size}):
                for lifetime in (5, 30):
                    with patch.object(self.launcher, 'MAX_SECONDS', lifetime):
                        self.assertTrue(self.launcher.validate_command(command))
                with patch.object(self.launcher, 'MAX_SECONDS', 31), self.assertRaises(ValueError):
                    self.launcher.validate_command(command)
            with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT': '1k'}), \
                    patch.object(self.launcher, 'MAX_SECONDS', 30), self.assertRaises(ValueError):
                self.launcher.validate_command(command)
            for missing in ('CHAMELEON_PM3_TEST_CLASSIC_STOP', 'CHAMELEON_PM3_TEST_PERSISTENT_STOP',
                            'CHAMELEON_PM3_SIM_RESTORE_BIN'):
                with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT': size, missing: ''}), \
                        patch.object(self.launcher, 'MAX_SECONDS', 30), self.assertRaises(ValueError):
                    self.launcher.validate_command(command)

    def test_routine_launch_requires_the_matching_completed_report(self):
        report = self.report('4k')
        self.validate(report)
        with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_REPORT': str(self.report_path)}), \
                patch('pm3_classic_qualification.read_identity', return_value=self.identity) as probe:
            self.assertTrue(self.launcher.validate_command(report['command'], str(self.client), self.port))
            probe.assert_called_once_with(str(self.client), self.port, execute=None)
            with self.assertRaises(ValueError):
                self.launcher.validate_command('hf mf sim --mini -u 11223344 -i', str(self.client), self.port)
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('pm3_classic_qualification.py')),
                                 report['command'], str(self.report_path), str(self.client), self.port],
                                capture_output=True, timeout=5, check=True)
        self.assertEqual(json.loads(result.stdout), {'qualified': True, 'command': report['command'],
                         'liveIdentityChecked': False, 'pm3IdentitySha256': identity_digest(self.identity)})

    def test_changed_firmware_is_refused_by_the_routine_launcher(self):
        report = self.report('4k')
        self.validate(report)
        changed = copy.deepcopy(self.identity)
        changed['ARM'][1] = 'OS......... different firmware'
        with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_REPORT': str(self.report_path)}), \
                patch('pm3_classic_qualification.read_identity', return_value=changed), \
                self.assertRaisesRegex(ValueError, 'firmware identity changed'):
            self.launcher.validate_command(report['command'], str(self.client), self.port)

    def test_bad_evidence_fails_before_a_live_probe(self):
        report = self.report('4k')
        report['results'].pop()
        self.report_path.write_text(json.dumps(report))
        with patch('pm3_classic_qualification.read_identity') as probe, self.assertRaises(ValueError):
            require_stop_report(report['command'], self.report_path, self.client, self.port, live=True)
        probe.assert_not_called()

    def test_live_cli_checks_a_synthetic_client_and_detects_changed_reply(self):
        # This executable accepts only a fake port and hw version. It cannot
        # simulate, flash, or open serial; only its version reply file changes.
        self.client.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
assert sys.argv[1:] == ['/not/a/serial/device', '-c', 'hw version']
sys.stdout.buffer.write(Path(__file__).with_name('version-reply.txt').read_bytes())
''')
        self.client.chmod(0o755)
        reply = self.root / 'version-reply.txt'
        reply.write_text(''.join(f' [ {section} ]\n' + '\n'.join(lines) + '\n\n'
                                 for section, lines in self.identity.items()))
        report = self.report('4k')
        self.validate(report)
        args = [sys.executable, str(Path(__file__).with_name('pm3_classic_qualification.py')), '--live',
                report['command'], str(self.report_path), str(self.client), self.port]
        result = subprocess.run(args, capture_output=True, timeout=5, check=True)
        self.assertEqual(json.loads(result.stdout), {'qualified': True, 'command': report['command'],
                         'liveIdentityChecked': True, 'pm3IdentitySha256': identity_digest(self.identity)})
        reply.write_text(reply.read_text().replace('fixture OS', 'changed OS'))
        result = subprocess.run(args, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'firmware identity changed', result.stderr)

    def test_report_requires_identity_before_after_and_every_fault(self):
        for edit in (lambda r: r.pop('pm3Identity'),
                     lambda r: r['pm3IdentityAfter']['ARM'].__setitem__(1, 'OS......... changed'),
                     lambda r: r['results'][2].__setitem__('pm3IdentitySha256', 'b' * 64)):
            changed = self.report('4k')
            edit(changed)
            with self.assertRaises(ValueError):
                self.validate(changed)

    def test_preflight_does_not_enable_attack_flags_or_other_size(self):
        base = 'hf mf sim --4k -u 11223344 -i'
        with patch.dict(os.environ, {**self.guards(), 'CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT': '4k'}), \
                patch.object(self.launcher, 'MAX_SECONDS', 30):
            for command in (base + ' -x', base + ' -e', base + ' --sak 10', base.replace('--4k', '--2k')):
                with self.subTest(command=command), self.assertRaises(ValueError):
                    self.launcher.validate_command(command)

    def test_two_k_report_cannot_substitute_sl2_sak_or_a_one_k_command(self):
        report = self.report('2k')
        report['results'][0]['positiveScan'] = '0000041122334404001000'
        with self.assertRaises(ValueError):
            self.validate(report, '2k')
        report = self.report('2k')
        report['command'] = 'hf mf sim --1k -u 11223344 -i'
        with self.assertRaises(ValueError):
            self.validate(report, '2k')


if __name__ == '__main__':
    unittest.main()
