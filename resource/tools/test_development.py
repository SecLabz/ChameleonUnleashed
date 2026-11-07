"""Exercise branch snapshots, durable resources and isolated stable/dev deployments."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from development import prepare, previous_resources, latest_development, manifest
from mirror_firmware import SOURCES
from package_release import PLATFORMS
from prepare_pages import prepare as prepare_pages
from sync_release_assets import superseded
from test_prepare_pages import resources, zipped
from test_web_update import archive, fixtures


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
        self.installer = archive(*data['normal'], *data['system'])
        for name, contents in {
            'app': self.application, 'install': self.installer,
            'bootloader': zipped({'bootloader.bin': b'signed bootloader placeholder'}),
            'full': self.installer, 'binaries': zipped({'application.hex': b'hex'})
        }.items():
            (self.artifacts / f'chameleon-unleashed-{name}.zip').write_bytes(contents)
        self.releases = {repo: [] for _, _, repo in SOURCES}
        self.downloads = {}

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

    def test_replacement_retains_old_resources_without_old_installable_firmware(self):
        first_name, first, original_first = self.build('a' * 40)
        name, second, original_second = self.build('b' * 40, first)
        retained = previous_resources(second)
        for original in (original_first, original_second):
            for key, value in original.items():
                self.assertEqual(retained[key], value)
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
            self.assertFalse(any(key.startswith('history/firmware/') for key in package.namelist()))
            current = {key.removeprefix('cdn/'): package.read(key)
                       for key in package.namelist() if key.startswith('cdn/')}
            _, document = manifest(current)
            self.assertEqual(document['channel'], 'development')
            html = current[document['webApp']['path'] + 'index.html'].decode()
            self.assertIn('Development · bbbbbbb', html)
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
        self.assertIn('Development · bbbbbbb', (site / 'dev/app/index.html').read_text())
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
        for key, value in previous_resources(current).items():
            self.assertEqual((site / key).read_bytes(), value)
        # Every subsequent deployment, including a stable refresh, uses the
        # same durable dev package rather than an expiring CI artifact.
        again = self.root / 'again'
        prepare_pages(again, get=self.get)
        self.assertEqual((site / 'dev/app/index.html').read_bytes(), (again / 'dev/app/index.html').read_bytes())
        self.assertEqual(latest_development(get=self.get), current)

    def test_bad_commit_corrupt_history_and_mismatched_installer_do_not_leave_output(self):
        _, previous, current = self.build('a' * 40)
        output = self.root / 'invalid'
        with self.assertRaisesRegex(ValueError, 'source commit'):
            prepare(self.artifacts, output, 'b' * 40)
        with self.assertRaises(Exception):
            prepare(self.artifacts, output, 'a' * 40, b'bad previous archive')
        broken = dict(previous_resources(previous))
        key = next(key for key in broken if key.endswith('.wasm'))
        broken[key] = b'corrupt old module'
        with self.assertRaises(ValueError):
            prepare(self.artifacts, output, 'a' * 40,
                    zipped({'history/' + key: value for key, value in broken.items()}))
        data = fixtures()
        (self.artifacts / 'chameleon-unleashed-install.zip').write_bytes(archive(*data['old'], *data['system']))
        with self.assertRaisesRegex(ValueError, 'Installer differs'):
            prepare(self.artifacts, output, 'a' * 40)
        self.assertFalse(output.exists())

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


if __name__ == '__main__':
    unittest.main()
