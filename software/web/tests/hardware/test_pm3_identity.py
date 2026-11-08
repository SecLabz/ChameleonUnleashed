"""Read-only version probe fixtures; never runs a real PM3 client."""
import copy
from pathlib import Path
import subprocess
import tempfile
import unittest
from pm3_identity import identity_digest, parse_identity, read_identity


VERSION = b'''
[=] Session log /synthetic/log.txt
 [ Proxmark3 ]

 [ Client ]
  Iceman/fixture client

 [ Model ]
  Firmware.................. PM3 GENERIC

 [ ARM ]
  Bootrom.... Iceman/fixture bootrom
  OS......... Iceman/fixture OS
  Compiler... fixture compiler

 [ FPGA ]
 fpga_pm3_hf.ncd image fixture-hf
 fpga_pm3_lf.ncd image fixture-lf

 [ Hardware ]
  --= uC: synthetic processor
  --= Internal SRAM size: 64K bytes

[!] synthetic diagnostic warning
'''


class IdentityTests(unittest.TestCase):
    def test_parses_firmware_sections_without_client_paths_or_warning_noise(self):
        identity = parse_identity(VERSION)
        self.assertEqual(set(identity), {'Model', 'ARM', 'FPGA', 'Hardware'})
        self.assertEqual(identity['ARM'][1], 'OS......... Iceman/fixture OS')
        changed_noise = VERSION.replace(b'/synthetic/log.txt', b'/other/log.txt').replace(b'fixture client', b'other client')
        self.assertEqual(parse_identity(changed_noise), identity)
        self.assertEqual(parse_identity(VERSION.replace(b'ARM', b'\x1b[33mARM\x1b[0m')), identity)

    def test_missing_partial_duplicate_or_oversized_versions_are_refused(self):
        for output in (b'Client started; return code zero', VERSION.replace(b'  OS......... Iceman/fixture OS', b''),
                       VERSION.replace(b'  Bootrom.... Iceman/fixture bootrom', b''),
                       VERSION.replace(b' [ FPGA ]', b' [ Missing ]'),
                       VERSION + b'\n [ ARM ]\n duplicate', VERSION * 1000, VERSION + b'\xff'):
            with self.subTest(output=output[:50]), self.assertRaises((ValueError, UnicodeError)):
                parse_identity(output)

    def test_every_reported_component_changes_the_fingerprint(self):
        original = parse_identity(VERSION)
        for section, index in [('Model', 0), ('ARM', 0), ('ARM', 1), ('FPGA', 0), ('Hardware', 0)]:
            changed = copy.deepcopy(original)
            changed[section][index] += '-changed'
            with self.subTest(section=section, index=index):
                self.assertNotEqual(identity_digest(changed), identity_digest(original))

    def test_probe_executes_only_bounded_read_only_version_command(self):
        calls = []

        def execute(args, **options):
            calls.append((args, options))
            return subprocess.CompletedProcess(args, 0, stdout=VERSION, stderr=b'')

        self.assertEqual(read_identity('/synthetic/client', '/not/a/serial/device', execute=execute), parse_identity(VERSION))
        self.assertEqual(calls, [(['/synthetic/client', '/not/a/serial/device', '-c', 'hw version'],
                                 {'capture_output': True, 'timeout': 12, 'check': True})])

    def test_failed_or_timed_out_probe_is_not_retried(self):
        for error in (subprocess.TimeoutExpired('fixture', 12, output=b'partial', stderr=b'timeout'),
                      subprocess.CalledProcessError(1, 'fixture', output=b'partial', stderr=b'failed')):
            calls = []

            def execute(args, **options):
                calls.append(args)
                raise error

            with tempfile.TemporaryDirectory() as directory:
                log = Path(directory) / 'version.log'
                with self.assertRaises(type(error)):
                    read_identity('/synthetic/client', '/not/a/serial/device', execute=execute, logfile=log)
                self.assertEqual(log.read_bytes(), error.stdout + error.stderr)
            self.assertEqual(len(calls), 1)

    def test_exit_zero_without_actual_version_evidence_is_refused(self):
        def execute(args, **options):
            return subprocess.CompletedProcess(args, 0, stdout=b'No version reply', stderr=b'')

        with self.assertRaises(ValueError):
            read_identity('/synthetic/client', '/not/a/serial/device', execute=execute)


if __name__ == '__main__':
    unittest.main()
