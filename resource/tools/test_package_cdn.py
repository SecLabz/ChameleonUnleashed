"""Verify release CDN archives and reject incomplete or corrupt builds."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from zipfile import ZipFile

import package_cdn


class CdnArchiveTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cache = self.root / '.cache/web'
        self.shared = self.root / 'shared'
        self.output = self.root / 'release/chameleon-unleashed-cdn.zip'
        patch = mock.patch.object(package_cdn, 'ROOT', self.root)
        patch.start()
        self.addCleanup(patch.stop)

        self.write(self.shared / 'LICENSE', b'shared license')
        self.write(self.shared / 'docs/licenses/NOTICE.md', b'third-party notices')
        resources = self.shared / 'crates/nfc-mifare/resources'
        self.write(resources / 'hardnested-assets-notice.txt', b'table notice')
        self.write(resources / 'hardnested-assets-license.txt', b'table license')

        self.core = self.asset(
            self.cache / 'cdn/core', 'current/nfc_wasm.wasm', b'core module'
        )
        self.table = self.asset(resources, 'hardnested.tables.knhp', b'table pack')
        self.hardnested = {
            'version': 2,
            'abi': 1,
            'tablePack': self.table,
            'modules': {
                name: self.asset(
                    self.cache / 'cdn/hardnested',
                    f'current/hardnested-{name}.wasm',
                    name.encode(),
                )
                for name in ['scalar', 'simd']
            },
        }
        self.write(self.cache / 'core.json', json.dumps(self.core).encode())
        self.write(self.cache / 'hardnested.json', json.dumps(self.hardnested).encode())
        self.build = {
            'source': {'revision': 'a' * 40, 'dirty': False},
            'shared': {'revision': 'b' * 40, 'dirty': False, 'sha256': 'c' * 64},
            'resourceBase': 'https://alice.github.io/ChameleonUnleashed/',
        }
        self.write(self.cache / 'cdn-build.json', json.dumps(self.build).encode())
        self.write(
            self.root / 'software/web/src/runtime/tables.json', json.dumps(self.table).encode()
        )
        self.write(self.cache / 'cdn/core/1/old/nfc_wasm.wasm', b'old build')
        self.write(self.cache / 'cdn/core/current/obsolete.wasm', b'obsolete file')

    def write(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def asset(self, directory, name, data):
        path = directory / name
        self.write(path, data)
        return {'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

    def test_archive_is_ready_to_extract_at_the_cdn_root(self):
        package_cdn.prepare(self.shared, self.output)
        tables = 'tables/' + self.table['sha256']
        with ZipFile(self.output) as archive:
            expected = set()
            for folder in [
                'core/current',
                'hardnested/current',
                tables,
            ]:
                expected.update(f'{folder}/{name}' for name in [
                    'manifest.json', 'LICENSE', 'docs/licenses/NOTICE.md'
                ])
                self.assertEqual(archive.read(f'{folder}/LICENSE'), b'shared license')
                self.assertEqual(
                    archive.read(f'{folder}/docs/licenses/NOTICE.md'), b'third-party notices'
                )

            expected.add('core/current/nfc_wasm.wasm')
            expected.update(
                f'hardnested/current/hardnested-{name}.wasm'
                for name in ['scalar', 'simd']
            )
            expected.update(f'{tables}/{name}' for name in [
                'hardnested.tables.knhp',
                'hardnested-assets-notice.txt',
                'hardnested-assets-license.txt',
            ])
            self.assertEqual(archive.read(f'{tables}/hardnested.tables.knhp'), b'table pack')
            self.assertEqual(archive.read(f'{tables}/hardnested-assets-notice.txt'), b'table notice')
            self.assertEqual(archive.read(f'{tables}/hardnested-assets-license.txt'), b'table license')

            manifests = [name for name in archive.namelist() if name.startswith('releases/')]
            self.assertEqual(len(manifests), 1)
            data = archive.read(manifests[0])
            self.assertEqual(manifests[0], 'releases/' + hashlib.sha256(data).hexdigest() + '.json')
            release = json.loads(data)
            self.assertEqual({key: release[key] for key in self.build}, self.build)
            self.assertEqual(release['version'], 1)
            self.assertEqual(
                {asset['path'] for asset in release['resources']}, expected
            )
            for asset in release['resources']:
                data = archive.read(asset['path'])
                self.assertEqual(len(data), asset['size'])
                self.assertEqual(hashlib.sha256(data).hexdigest(), asset['sha256'])

            self.assertEqual(set(archive.namelist()), expected | set(manifests))
            self.assertEqual(archive.read('core/current/nfc_wasm.wasm'), b'core module')
            self.assertEqual(
                json.loads(archive.read('core/current/manifest.json')),
                {'version': 1, 'module': self.core},
            )
            self.assertEqual(
                json.loads(archive.read('hardnested/current/manifest.json')),
                self.hardnested,
            )
            self.assertEqual(
                json.loads(archive.read(f'{tables}/manifest.json')),
                {'version': 1, 'tablePack': self.table},
            )
            for name in archive.namelist():
                self.assertEqual((self.cache / 'cdn' / name).read_bytes(), archive.read(name))

        self.assertTrue((self.cache / 'cdn/core/1/old/nfc_wasm.wasm').exists())

    def test_rebuilding_the_same_release_reuses_its_manifest_and_keeps_previous_builds(self):
        package_cdn.prepare(self.shared, self.output)
        releases = self.cache / 'cdn/releases'
        original = next(releases.iterdir())
        data = original.read_bytes()

        package_cdn.prepare(self.shared, self.output)
        self.assertEqual(list(releases.iterdir()), [original])
        self.assertEqual(original.read_bytes(), data)

        self.build['source']['dirty'] = True
        self.write(self.cache / 'cdn-build.json', json.dumps(self.build).encode())
        package_cdn.prepare(self.shared, self.output)
        self.assertEqual(len(list(releases.iterdir())), 2)
        self.assertEqual(original.read_bytes(), data)
        with ZipFile(self.output) as archive:
            manifests = [name for name in archive.namelist() if name.startswith('releases/')]
            self.assertEqual(len(manifests), 1)
            self.assertTrue(json.loads(archive.read(manifests[0]))['source']['dirty'])

    def test_missing_build_provenance_does_not_replace_an_existing_archive(self):
        self.write(self.output, b'previous release')
        (self.cache / 'cdn-build.json').unlink()
        with self.assertRaises(FileNotFoundError):
            package_cdn.prepare(self.shared, self.output)

        self.assertEqual(self.output.read_bytes(), b'previous release')

    def test_table_updates_preserve_previously_published_resources(self):
        package_cdn.prepare(self.shared, self.output)
        previous = self.cache / 'cdn/tables' / self.table['sha256'] / self.table['path']
        table = self.asset(
            self.shared / 'crates/nfc-mifare/resources', self.table['path'], b'updated tables'
        )
        self.hardnested['tablePack'] = table
        self.write(self.cache / 'hardnested.json', json.dumps(self.hardnested).encode())
        self.write(self.root / 'software/web/src/runtime/tables.json', json.dumps(table).encode())

        package_cdn.prepare(self.shared, self.output)

        self.assertEqual(previous.read_bytes(), b'table pack')
        with ZipFile(self.output) as archive:
            tables = [name for name in archive.namelist() if name.endswith('.knhp')]
            self.assertEqual(tables, [f'tables/{table["sha256"]}/{table["path"]}'])
            self.assertEqual(archive.read(tables[0]), b'updated tables')

    def test_corrupt_resources_leave_existing_archive_intact_and_clean_up(self):
        self.write(self.output, b'previous release')
        path = self.cache / 'cdn/core' / self.core['path']
        data = path.read_bytes()
        for corrupted in [data + b'extra', bytes(len(data))]:
            with self.subTest(size=len(corrupted)):
                path.write_bytes(corrupted)
                with self.assertRaisesRegex(ValueError, 'differs from its build manifest'):
                    package_cdn.prepare(self.shared, self.output)

                self.assertEqual(self.output.read_bytes(), b'previous release')
                self.assertEqual(list(self.output.parent.glob('.cdn-*')), [])

    def test_mismatched_tables_or_missing_variants_do_not_produce_an_archive(self):
        self.hardnested['tablePack'] = {**self.table, 'sha256': 'outdated'}
        self.write(self.cache / 'hardnested.json', json.dumps(self.hardnested).encode())
        with self.assertRaisesRegex(ValueError, 'Table pin changed'):
            package_cdn.prepare(self.shared, self.output)

        self.hardnested['tablePack'] = self.table
        del self.hardnested['modules']['simd']
        self.write(self.cache / 'hardnested.json', json.dumps(self.hardnested).encode())
        with self.assertRaisesRegex(ValueError, 'both Hardnested variants'):
            package_cdn.prepare(self.shared, self.output)

        self.assertFalse(self.output.exists())

    def test_resource_paths_cannot_escape_the_archive_root(self):
        for name in ['../outside.wasm', '/outside.wasm', 'C:/outside.wasm', 'dir\\outside.wasm']:
            with self.subTest(name=name):
                self.write(self.cache / 'core.json', json.dumps({**self.core, 'path': name}).encode())
                with self.assertRaisesRegex(ValueError, 'Invalid resource path'):
                    package_cdn.prepare(self.shared, self.output)

                self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
