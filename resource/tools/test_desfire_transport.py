"""Verify actual DESFire reader and emulator transports with scripted RF frames."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/application/src'
RFID = SRC / 'rfid'
HF = RFID / 'nfctag/hf'


class DesfireTransportTests(unittest.TestCase):
    def run_transport(self, fixture_name, implementation, model=False):
        implementation = implementation.read_text()
        # Keep protocol constants shared by the production modules; substitute
        # hardware and persistence declarations without replacing their logic.
        implementation = re.sub(r'^#include "(?!desfire_protocol\.h")[^"]+"\n',
                                '', implementation, flags=re.M)
        fixture = (ROOT / f'resource/tools/tests/{fixture_name}.c').read_text()
        capture = json.loads((ROOT / 'resource/tools/tests/desfire-public.json').read_text())
        uid, metadata, memory = (bytes(capture[k]) for k in ('uid', 'metadata', 'memory'))
        image = (bytes([len(uid)]) + uid + len(metadata).to_bytes(2, 'big') +
                 len(memory).to_bytes(2, 'big') + metadata + memory)
        image += bytes(-len(image) % 4)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / 'transport.c'
            source.write_text(fixture.replace('/* IMPLEMENTATION */', implementation))
            (path / 'image').write_bytes(image)
            binary = path / 'transport'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(SRC), '-I', str(RFID), '-I', str(HF), str(source),
                *([str(HF / 'desfire_model.c')] if model else []),
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary), str(path / 'image')], check=True, timeout=10)

    def test_emulator_chaining_retries_apdus_and_images(self):
        self.run_transport('desfire_transport', HF / 'nfc_desfire.c', model=True)

    def test_reader_chaining_wtx_limits_and_session_cleanup(self):
        self.run_transport('desfire_reader', RFID / 'reader/hf/desfire_reader.c')

    def test_model_validation_rejects_invalid_observations_and_geometry(self):
        self.run_transport('desfire_validation', HF / 'desfire_model.c')


if __name__ == '__main__':
    unittest.main()
