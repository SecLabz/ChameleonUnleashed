"""Compile the production magic detector against a deterministic RF peer."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'


class MagicTest(unittest.TestCase):
    def test_wire_command_ids_do_not_alias_other_operations(self):
        definitions = subprocess.check_output([
            'cc', '-dM', '-E', '-include',
            str(ROOT / 'firmware/application/src/data_cmd.h'), '-'
        ], input=b'').decode()
        identifiers = {}
        for name, value in re.findall(r'^#define (DATA_CMD_\w+) \((\d+)\)$', definitions, re.M):
            self.assertNotIn(value, identifiers, f'{name} aliases {identifiers.get(value)}')
            identifiers[value] = name

    def test_special_uid_methods(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'uid'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(RFID.parent),
                '-I', str(RFID / 'reader/hf'), '-I', str(RFID.parent / 'bsp'),
                str(ROOT / 'resource/tools/tests/mifare_uid.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_guarded_block0_and_trailer_writes(self):
        source = (RFID / 'reader/hf/nfca_commands.c').read_text()
        functions = '\n'.join(function(source, name) for name in (
            'classic_model_check', 'block0_set', 'classic_access_valid', 'trailer_set'))
        fixture = (ROOT / 'resource/tools/tests/mifare_write.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'write.c'
            binary = Path(directory) / 'write'
            path.write_text(fixture.replace('/* WRITE FUNCTIONS */', functions))
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(RFID.parent),
                '-I', str(RFID / 'reader/hf'), str(path), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_signatures_evidence_identity_and_field_cutoff(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'magic'
            fixture = (ROOT / 'resource/tools/tests/mifare_magic.c').read_text()
            selected_uid = function((RFID / 'reader/hf/rc522.c').read_text(), 'get_4byte_tag_uid')
            source = Path(directory) / 'magic.c'
            source.write_text(fixture.replace('/* SELECTED UID FUNCTION */', selected_uid))
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(RFID.parent),
                '-I', str(RFID), '-I', str(RFID / 'reader/hf'),
                '-I', str(RFID.parent / 'bsp'),
                str(source),
                str(RFID / 'crc_utils.c'), '-o', str(target),
            ], check=True)
            subprocess.run([str(target)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
