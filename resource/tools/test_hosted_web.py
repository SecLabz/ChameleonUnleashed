"""Validate standalone BLE packaging and its complete, content-checked shell."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from prepare_hosted_web import prepare, FILES
from hosted_web import bundle, archived_shell
from test_prepare_pages import resources, zipped


class HostedTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.site = self.root / 'input'
        self.site.mkdir()
        for name in FILES:
            (self.site / name).write_bytes(b'<!doctype html><head></head><body></body>' if name == 'index.html' else name.encode())
        self.cdn = self.root / 'resources.zip'
        self.cdn.write_bytes(zipped(resources('pinned modules')))
        self.output = self.root / 'output'

    def test_complete_shell_manifest_and_isolated_resources(self):
        version = prepare(self.site, self.cdn, self.output)
        html = (self.output / 'app/index.html').read_text()
        self.assertIn(f'name="chameleon-shell" content="{version}"', html)
        self.assertIn('name="chameleon-resources" content="../"', html)
        worker = (self.output / 'app/sw.mjs').read_text()
        config = json.loads(worker.split(' = ', 1)[1].split(';\n', 1)[0])
        self.assertEqual(config['version'], version)
        self.assertEqual({entry['path'] for entry in config['files']}, set(FILES))
        for entry in config['files']:
            data = (self.output / 'app' / entry['path']).read_bytes()
            self.assertEqual(entry['size'], len(data))
            self.assertEqual(entry['sha256'], hashlib.sha256(data).hexdigest())
        self.assertNotIn('skipWaiting(', worker)
        self.assertFalse((self.output / 'app/update.html').exists())
        self.assertTrue((self.output / '.nojekyll').exists())
        self.assertTrue(any((self.output / 'core').rglob('*.wasm')))

    def test_corrupt_resources_or_incomplete_shell_leave_no_output(self):
        for corrupt in (True, False):
            with self.subTest(corrupt=corrupt):
                if corrupt:
                    self.cdn.write_bytes(b'invalid ZIP')
                else:
                    self.cdn.write_bytes(zipped(resources('pinned modules')))
                    (self.site / 'worker.mjs').unlink()
                with self.assertRaises(Exception):
                    prepare(self.site, self.cdn, self.output)
                self.assertFalse(self.output.exists())

    def test_old_shells_remain_valid_and_new_updater_is_integrity_bound(self):
        for legacy in (True, False):
            raw = {name: (self.site / name).read_bytes() for name in FILES
                   if not legacy or name != 'ble-update.mjs'}
            version, files = bundle(raw)
            prefix = 'web/' + version + '/'
            resources = {prefix + name: data for name, data in files.items()}
            manifest = {'source': {'committedAt': 1}, 'webApp': {'version': version, 'path': prefix},
                'resources': [{'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                              for name, data in resources.items()]}
            self.assertEqual(archived_shell(manifest, resources)[1], files)
            if not legacy:
                resources[prefix + 'ble-update.mjs'] += b'corrupt'
                with self.assertRaises(ValueError):
                    archived_shell(manifest, resources)

    def test_existing_package_cannot_be_overwritten(self):
        self.output.mkdir()
        marker = self.output / 'keep.txt'
        marker.write_bytes(b'existing complete package')
        with self.assertRaisesRegex(ValueError, 'must be empty'):
            prepare(self.site, self.cdn, self.output)
        self.assertEqual(marker.read_bytes(), b'existing complete package')


if __name__ == '__main__':
    unittest.main()
