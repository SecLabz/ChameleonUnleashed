"""Published downloads preserve tested binaries and contain platform-specific instructions."""

import hashlib
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest
from zipfile import ZipFile

from package_release import PLATFORMS, prepare
from sync_release_assets import superseded


class ReleaseDownloadTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir()
        self.output = self.root / 'downloads'
        for platform in PLATFORMS:
            binary = 'chameleon.exe' if platform.startswith('windows') else 'chameleon'
            self.archive(f'chameleon-{platform}.zip', {binary: platform.encode()})
        self.archive('chameleon-unleashed-app.zip', {'application.bin': b'original signed application'})
        self.archive('chameleon-unleashed-full.zip', {'system.bin': b'original signed system'})
        self.archive('chameleon-unleashed-binaries.zip', {'application.hex': b'original hex', 'THIRD_PARTY.txt': b'notices'})
        module = b'original wasm'
        self.manifest = {
            'version': 1, 'projectVersion': '1.0.0',
            'source': {'revision': 'a' * 40, 'dirty': False},
            'resources': [{'path': 'core/test.wasm', 'size': len(module),
                           'sha256': hashlib.sha256(module).hexdigest()}],
        }
        self.archive('chameleon-unleashed-cdn.zip', {
            'core/test.wasm': module, 'core/LICENSE': b'license',
            'releases/test.json': json.dumps(self.manifest).encode()})

    def archive(self, name, files):
        with ZipFile(self.inputs / name, 'w') as archive:
            for path, data in files.items():
                archive.writestr(path, data)

    def test_downloads_preserve_original_payloads_and_unix_permissions(self):
        names = prepare(self.inputs, self.output, '1.0.0', 'v1.0.0')
        self.assertEqual(len(names), 5)
        app = (self.inputs / 'chameleon-unleashed-app.zip').read_bytes()
        for platform in PLATFORMS:
            with ZipFile(self.output / f'ChameleonUnleashed-v1.0.0-{platform}.zip') as archive:
                binary = 'chameleon.exe' if platform.startswith('windows') else 'chameleon'
                self.assertEqual(set(archive.namelist()), {binary, 'firmware.zip', 'INSTALL.txt'})
                self.assertEqual(archive.read(binary), platform.encode())
                self.assertEqual(archive.read('firmware.zip'), app)
                with ZipFile(io.BytesIO(archive.read('firmware.zip'))) as inner:
                    self.assertEqual(inner.read('application.bin'), b'original signed application')
                instructions = archive.read('INSTALL.txt').decode()
                self.assertIn('v1.0.0', instructions)
                self.assertNotIn('kit', instructions.lower())
                if platform.startswith('windows'):
                    self.assertIn('PowerShell', instructions)
                    self.assertIn('.\\chameleon.exe flash firmware.zip', instructions)
                    self.assertNotIn('chmod', instructions)
                else:
                    self.assertIn('./chameleon flash firmware.zip', instructions)
                    self.assertEqual(stat.S_IMODE(archive.getinfo(binary).external_attr >> 16), 0o755)
        with ZipFile(self.output / 'ChameleonUnleashed-v1.0.0-firmware-resources.zip') as archive:
            self.assertEqual(archive.read('firmware/application.zip'), app)
            self.assertEqual(archive.read('firmware/system.zip'),
                             (self.inputs / 'chameleon-unleashed-full.zip').read_bytes())
            self.assertEqual(archive.read('images/application.hex'), b'original hex')
            self.assertEqual(archive.read('images/THIRD_PARTY.txt'), b'notices')
            self.assertEqual(archive.read('cdn/core/test.wasm'), b'original wasm')
            self.assertEqual(archive.read('cdn/core/LICENSE'), b'license')

    def test_dev_names_include_source_revision_and_packaging_is_deterministic(self):
        first = prepare(self.inputs, self.output, '1.0.0', development=True)
        other = self.root / 'second'
        second = prepare(self.inputs, other, '1.0.0', development=True)
        self.assertEqual(first, second)
        for name in first:
            self.assertIn('-v1.0.0-dev.aaaaaaa-', name)
            self.assertEqual((self.output / name).read_bytes(), (other / name).read_bytes())

    def test_invalid_version_paths_or_missing_inputs_do_not_publish_partial_downloads(self):
        with self.assertRaises(ValueError):
            prepare(self.inputs, self.output, '2.0.0', 'v1.0.0')
        self.assertFalse(self.output.exists())
        with self.assertRaisesRegex(ValueError, 'CDN project version'):
            prepare(self.inputs, self.output, '2.0.0')
        self.assertFalse(self.output.exists())
        self.archive('chameleon-linux-x64.zip', {'../chameleon': b'unsafe'})
        with self.assertRaisesRegex(ValueError, 'Unsafe archive path'):
            prepare(self.inputs, self.output, '1.0.0')
        self.assertFalse(self.output.exists())

    def test_superseded_downloads_are_removed_only_after_every_new_archive_matches(self):
        archives = {f'archive-{i}.zip': str(i).encode() for i in range(5)}
        assets = [{'id': i, 'name': name, 'size': len(data), 'state': 'uploaded',
                   'digest': 'sha256:' + hashlib.sha256(data).hexdigest()}
                  for i, (name, data) in enumerate(archives.items())]
        old = {'id': 100, 'name': 'old.zip'}
        self.assertEqual(superseded(assets + [old], archives), [old])
        for invalid in [assets[:-1], [{**assets[0], 'digest': 'sha256:wrong'}, *assets[1:]]]:
            with self.assertRaises(ValueError):
                superseded(invalid + [old], archives)
        with self.assertRaises(ValueError):
            superseded(assets, {})


if __name__ == '__main__':
    unittest.main()
