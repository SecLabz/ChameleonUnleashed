"""Exercise current-only development snapshots and isolated stable/dev deployments."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from development import prepare, latest_development, manifest
from bootloader_version import read_version as bootloader_version
from mirror_firmware import SOURCES
from package_release import PLATFORMS
from prepare_pages import prepare as prepare_pages
from sync_release_assets import superseded
from test_prepare_pages import resources, zipped
from test_web_update import archive, fixtures, signed_packet
from web_resources import cdn_files


class DevelopmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.artifacts = self.root / 'artifacts'
        self.artifacts.mkdir()
        for platform in PLATFORMS:
            executable = 'chameleon.exe' if platform == 'windows-x64' else 'chameleon'
            (self.artifacts / f'chameleon-{platform}.zip').write_bytes(
                zipped({executable: ('cli-' + platform).encode()}))
        data = fixtures()
        self.application = archive(*data['normal'])
        self.app_payload, self.app_packet = data['normal']
        self.boot_payload = data['system'][0][516:]
        self.set_bootloader(self.boot_payload, bootloader_version())
        for name, contents in {
            'app': self.application, 'install': self.installer,
            'full': self.installer, 'binaries': zipped({'application.hex': b'hex'})
        }.items():
            (self.artifacts / f'chameleon-unleashed-{name}.zip').write_bytes(contents)
        self.releases = {repo: [] for _, _, repo in SOURCES}
        self.downloads = {}

    def set_bootloader(self, payload, version):
        packet = signed_packet(payload, version=version, kind=2, bl_size=len(payload))
        image = {'bin_file': 'bootloader.bin', 'dat_file': 'bootloader.dat'}
        files = {'bootloader.bin': payload, 'bootloader.dat': packet}
        (self.artifacts / 'chameleon-unleashed-bootloader.zip').write_bytes(zipped({
            **files, 'manifest.json': json.dumps({'manifest': {'bootloader': image}}).encode()}))
        self.installer = zipped({**files, 'application.bin': self.app_payload,
            'application.dat': self.app_packet, 'manifest.json': json.dumps({'manifest': {
                'bootloader': image, 'application': {'bin_file': 'application.bin', 'dat_file': 'application.dat'}}}).encode()})
        (self.artifacts / 'chameleon-unleashed-install.zip').write_bytes(self.installer)

    def build(self, revision, previous=None):
        current = resources(revision, app_timestamp=int(revision[0], 16) * 100, revision=revision)
        (self.artifacts / 'chameleon-unleashed-cdn.zip').write_bytes(zipped(current))
        output = self.root / revision[0]
        name = prepare(self.artifacts, output, revision, previous)
        return name, (output / name).read_bytes(), current

    def publish(self, tag, name, data, prerelease=False):
        repo = SOURCES[0][2]
        url = f'https://github.com/{repo}/releases/download/{tag}/{name}'
        self.downloads[url] = data
        self.releases[repo].append({'tag_name': tag, 'prerelease': prerelease,
            'published_at': '2026-11-07T00:00:00Z', 'html_url': f'https://github.com/{repo}/releases/tag/{tag}',
            'assets': [{'name': name, 'browser_download_url': url, 'size': len(data),
                        'digest': 'sha256:' + hashlib.sha256(data).hexdigest()}]})

    def get(self, url, maximum, api=False):
        if api:
            repo = url.split('/repos/')[1].split('/releases')[0]
            return json.dumps(self.releases[repo]).encode()
        return self.downloads[url]

    def test_replacement_contains_only_current_firmware_and_resources(self):
        first_name, first, original_first = self.build('a' * 40)
        name, second, original_second = self.build('b' * 40, first)
        current = cdn_files(second, prefix='cdn/')
        for key in original_first.keys() - original_second.keys():
            self.assertNotIn(key, current)
        for key, value in original_second.items():
            if key.startswith(('core/', 'hardnested/', 'tables/')):
                self.assertEqual(current[key], value)
        self.assertTrue(name.endswith('-dev.bbbbbbb-firmware-resources.zip'))
        self.assertEqual(len(list((self.root / 'b').glob('*.zip'))), len(PLATFORMS) + 1)
        for platform in PLATFORMS:
            executable = 'chameleon.exe' if platform == 'windows-x64' else 'chameleon'
            path = self.root / 'b' / f'ChameleonUnleashed-v1.1.0-dev.bbbbbbb-{platform}.zip'
            with ZipFile(path) as package:
                self.assertEqual(set(package.namelist()), {executable, 'firmware.zip', 'INSTALL.txt'})
                self.assertEqual(package.read(executable), ('cli-' + platform).encode())
                self.assertEqual(package.read('firmware.zip'), self.installer)
        with ZipFile(io.BytesIO(second)) as package:
            self.assertEqual(package.read('firmware/application.zip'), self.application)
            self.assertEqual(package.read('firmware/install.zip'), self.installer)
            self.assertNotIn(first_name, package.namelist())
            self.assertFalse(any(key.startswith('history/') for key in package.namelist()))
            self.assertEqual({key.split('/')[0] for key in package.namelist()},
                             {'README.txt', 'firmware', 'images', 'cdn'})
            current = {key.removeprefix('cdn/'): package.read(key)
                       for key in package.namelist() if key.startswith('cdn/')}
            _, document = manifest(current)
            self.assertEqual(document['channel'], 'development')
            html = current[document['webApp']['path'] + 'index.html'].decode()
            self.assertIn('<meta name="chameleon-development" content="' + 'b' * 40 + '">', html)
            self.assertIn(' · Development</title>', html)
            self.assertIn('name="chameleon-resources" content="../../"', html)

    def test_pages_publishes_one_dev_and_keeps_the_stable_site_and_catalogue(self):
        stable = resources('stable', app_timestamp=50, revision='1' * 40)
        stable_bundle = zipped({**{'cdn/' + key: value for key, value in stable.items()},
                               'firmware/application.zip': self.application})
        for number in range(6):
            self.publish(f'v1.0.{number}', f'ChameleonUnleashed-v1.0.{number}-firmware-resources.zip', stable_bundle)
        _, first, _ = self.build('a' * 40)
        name, current, _ = self.build('b' * 40, first)
        self.publish('dev', name, current, prerelease=True)
        site = self.root / 'site'
        catalogue = prepare_pages(site, get=self.get)
        self.assertEqual(len(catalogue['sources'][0]['releases']), 5)
        self.assertTrue(all(not entry['prerelease'] for entry in catalogue['sources'][0]['releases']))
        expected = next(value for key, value in stable.items() if key.startswith('web/') and key.endswith('/app.mjs'))
        self.assertEqual((site / 'app/app.mjs').read_bytes(), expected)
        self.assertIn('content="' + 'b' * 40 + '"', (site / 'dev/app/index.html').read_text())
        self.assertFalse((site / 'dev/firmware.zip').exists())
        self.assertIn('content="0;url=app/"', (site / 'dev/index.html').read_text())
        self.assertNotIn('<a ', (site / 'dev/index.html').read_text())
        self.assertEqual(len(list((site / 'updates/dev/files').glob('*.zip'))), 1)
        dev = json.loads((site / 'updates/dev/catalog.json').read_text())
        self.assertEqual(len(dev['sources']), 1)
        self.assertEqual(len(dev['sources'][0]['releases']), 1)
        entry = dev['sources'][0]['releases'][0]
        self.assertTrue(entry['prerelease'])
        self.assertIn('bbbbbbb', entry['version'])
        for key, value in cdn_files(current, prefix='cdn/').items():
            self.assertEqual((site / key).read_bytes(), value)
        # Every subsequent deployment, including a stable refresh, uses the
        # same durable dev package rather than an expiring CI artifact.
        again = self.root / 'again'
        prepare_pages(again, get=self.get)
        self.assertEqual((site / 'dev/app/index.html').read_bytes(), (again / 'dev/app/index.html').read_bytes())
        self.assertEqual(latest_development(get=self.get), current)

    def test_bad_commit_previous_bootloader_and_mismatched_installer_do_not_leave_output(self):
        self.build('a' * 40)
        output = self.root / 'invalid'
        with self.assertRaisesRegex(ValueError, 'source commit'):
            prepare(self.artifacts, output, 'b' * 40)
        with self.assertRaises(Exception):
            prepare(self.artifacts, output, 'a' * 40, b'bad previous archive')
        data = fixtures()
        (self.artifacts / 'chameleon-unleashed-install.zip').write_bytes(archive(*data['old'], *data['system']))
        with self.assertRaisesRegex(ValueError, 'Installer differs'):
            prepare(self.artifacts, output, 'a' * 40)
        self.assertFalse(output.exists())

    def test_legacy_history_is_neither_copied_nor_published(self):
        _, previous, _ = self.build('a' * 40)
        history = {'history/core/obsolete/module.wasm': b'obsolete',
                   'history/releases/obsolete.json': b'invalid obsolete manifest'}
        with ZipFile(io.BytesIO(previous)) as package:
            files = {name: package.read(name) for name in package.namelist()}
        name, current, _ = self.build('b' * 40, zipped({**files, **history}))
        with ZipFile(io.BytesIO(current)) as package:
            files = {name: package.read(name) for name in package.namelist()}
            self.assertFalse(any(key.startswith('history/') for key in files))
        # Already published archives may still carry history during migration.
        self.publish('dev', name, zipped({**files, **history}), prerelease=True)
        site = self.root / 'site'
        prepare_pages(site, get=self.get)
        for key, value in cdn_files(current, prefix='cdn/').items():
            self.assertEqual((site / key).read_bytes(), value)
        self.assertFalse((site / 'core/obsolete').exists())
        self.assertFalse((site / 'releases/obsolete.json').exists())

    def test_pages_drops_previous_dev_resources_from_baseline_and_keeps_stable(self):
        first_name, first, original_first = self.build('a' * 40)
        stable = resources('stable', app_timestamp=50, revision='1' * 40)
        # A stable release may still need a module also used by the old dev build.
        shared = next(key for key in original_first if key.endswith('.wasm'))
        manifest_name, document = manifest(stable)
        stable[shared] = original_first[shared]
        document['resources'].append({'path': shared, 'size': len(stable[shared]),
                                      'sha256': hashlib.sha256(stable[shared]).hexdigest()})
        del stable[manifest_name]
        contents = json.dumps(document).encode()
        stable['releases/' + hashlib.sha256(contents).hexdigest() + '.json'] = contents
        stable_bundle = zipped({**{'cdn/' + key: value for key, value in stable.items()},
                               'firmware/application.zip': self.application})
        self.publish('v1.0.0', 'ChameleonUnleashed-v1.0.0-firmware-resources.zip', stable_bundle)
        self.publish('dev', first_name, first, prerelease=True)
        baseline = self.root / 'baseline'
        prepare_pages(baseline, get=self.get)
        # Legacy deployments also contain the original, unlabelled web variant.
        for key, value in original_first.items():
            path = baseline / key
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        self.releases[SOURCES[0][2]] = [item for item in self.releases[SOURCES[0][2]]
                                       if item['tag_name'] != 'dev']
        name, current, _ = self.build('b' * 40, first)
        self.publish('dev', name, current, prerelease=True)
        site = self.root / 'site'
        prepare_pages(site, baseline=baseline, get=self.get)
        required = {**stable, **cdn_files(current, prefix='cdn/')}
        obsolete = {**original_first, **cdn_files(first, prefix='cdn/')}
        for key in obsolete.keys() - required.keys():
            self.assertFalse((site / key).exists(), key)
        for key, value in required.items():
            self.assertEqual((site / key).read_bytes(), value)
        self.assertIn('content="' + 'b' * 40 + '"', (site / 'dev/app/index.html').read_text())
        self.assertEqual(len(list((site / 'updates/dev/files').glob('*.zip'))), 1)

    def test_failed_upload_cannot_delete_the_previous_development_package(self):
        self.build('a' * 40)
        archives = {path.name: path.read_bytes() for path in (self.root / 'a').glob('*.zip')}
        old = {'id': 1, 'name': 'previous.zip'}
        assets = [{'id': index + 2, 'name': name, 'state': 'uploaded', 'size': len(data),
                   'digest': 'sha256:' + hashlib.sha256(data).hexdigest()}
                  for index, (name, data) in enumerate(archives.items())]
        self.assertEqual(superseded([old, *assets], archives), [old])
        for incomplete in ([old, *assets[:-1]], [old, {**assets[0], 'digest': 'sha256:bad'}, *assets[1:]]):
            with self.assertRaises(ValueError):
                superseded(incomplete, archives)

    def test_changed_bootloader_cannot_reuse_its_published_revision(self):
        _, previous, _ = self.build('a' * 40)
        changed = self.boot_payload[:32] + bytes([self.boot_payload[32] ^ 1]) + self.boot_payload[33:]
        self.set_bootloader(changed, bootloader_version())
        with self.assertRaisesRegex(ValueError, 'increment firmware/bootloader/version.json'):
            self.build('b' * 40, previous)
        self.assertFalse((self.root / 'b').exists())


if __name__ == '__main__':
    unittest.main()
