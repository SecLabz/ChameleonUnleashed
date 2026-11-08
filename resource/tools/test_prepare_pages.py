"""Rebuild complete Pages deployments from releases, without publishing extra files."""
import hashlib
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest
from zipfile import ZipFile, ZipInfo

from mirror_firmware import SOURCES
from prepare_pages import prepare, cdn_files, merge_resources
from test_web_update import fixtures, archive
from hosted_web import FILES, BUNDLE_FILES, shell


def zipped(files):
    output = io.BytesIO()
    with ZipFile(output, 'w') as package:
        for name, data in files.items():
            package.writestr(name, data)
    return output.getvalue()


def resources(identity, app_timestamp=None, revision=None):
    path = 'core/' + hashlib.sha256(identity.encode()).hexdigest() + '/'
    files = {path + 'module.wasm': identity.encode(), path + 'LICENSE': b'license',
             path + 'manifest.json': b'{"version":1}'}
    descriptor = {}
    if app_timestamp is not None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in FILES:
                data = (b'<head><title>Chameleon</title></head><body><div class="shell"></div></body>'
                        if revision else b'<head></head>') if name == 'index.html' else (identity + ':' + name).encode()
                (directory / name).write_bytes(data)
            version, app = shell(directory)
        prefix = f'web/{version}/'
        files.update({prefix + name: data for name, data in app.items()})
        descriptor = {'source': {'committedAt': app_timestamp}, 'webApp': {'version': version, 'path': prefix}}
        if revision is not None:
            descriptor['source'].update(revision=revision, dirty=False)
    manifest = {'version': 1, 'projectVersion': '1.1.0', **descriptor, 'resources': [
        {'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        for name, data in files.items()]}
    data = json.dumps(manifest).encode()
    files['releases/' + hashlib.sha256(data).hexdigest() + '.json'] = data
    return files


class PagesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / 'site'
        self.releases = {repo: [] for _, _, repo in SOURCES}
        self.downloads = {}
        self.download_counts = {}
        self.app = archive(*fixtures()['normal'])

    def add_release(self, day, legacy=False, app_timestamp=None, **flags):
        repo = SOURCES[0][2]
        tag = f'v1.0.{day}'
        cdn = resources(tag, app_timestamp)
        bundle = {**{'cdn/' + name: data for name, data in cdn.items()},
                  'firmware/application.zip': archive(*fixtures()['legacy']) if legacy else self.app,
                  'cdn/updates/latest.json': b'old download index',
                  'cdn/updates/obsolete.zip': b'old firmware must not be published',
                  'images/private-debug.elf': b'not a web resource'}
        name = f'ChameleonUnleashed-{tag}-firmware-resources.zip'
        url = f'https://github.com/{repo}/releases/download/{tag}/{name}'
        self.downloads[url] = zipped(bundle)
        self.releases[repo].append({'tag_name': tag, 'html_url': f'https://github.com/{repo}/releases/tag/{tag}',
            'published_at': f'2026-11-{day:02d}T00:00:00Z', **flags,
            'assets': [{'name': name, 'browser_download_url': url, 'size': len(self.downloads[url])}]})
        return cdn

    def get(self, url, maximum, api=False):
        if api:
            repo = url.split('/repos/')[1].split('/releases')[0]
            return json.dumps(self.releases[repo]).encode()
        self.download_counts[url] = self.download_counts.get(url, 0) + 1
        return self.downloads[url]

    def test_catalogue_has_five_versions_but_old_web_resources_remain(self):
        expected = {}
        for day in range(1, 8):
            expected.update(self.add_release(day))
        document = prepare(self.output, get=self.get)
        self.assertEqual([r['version'] for r in document['sources'][0]['releases']],
                         ['v1.0.7', 'v1.0.6', 'v1.0.5', 'v1.0.4', 'v1.0.3'])
        for name, data in expected.items():
            self.assertEqual((self.output / name).read_bytes(), data)
        # Each resources archive is downloaded once for firmware and CDN together.
        self.assertEqual(set(self.download_counts.values()), {1})
        self.assertFalse((self.output / 'updates/latest.json').exists())
        self.assertFalse((self.output / 'updates/obsolete.zip').exists())
        self.assertFalse((self.output / 'images').exists())
        self.assertEqual(len(list((self.output / 'updates/files').glob('*.zip'))), 1)

    def test_legacy_and_preview_resources_are_kept_without_catalogue_entries(self):
        expected = self.add_release(1, legacy=True)
        expected.update(self.add_release(2, prerelease=True))
        expected.update(self.add_release(3))
        document = prepare(self.output, get=self.get)
        self.assertEqual([r['version'] for r in document['sources'][0]['releases']], ['v1.0.3'])
        self.assertTrue(all((self.output / name).read_bytes() == data for name, data in expected.items()))

    def test_current_build_and_existing_resources_are_preserved_without_git_or_test_files(self):
        baseline = self.root / 'baseline'
        for name, data in {'core/old/module.wasm': b'old', '.git/config': b'private git config',
                           'updates/TESTING.txt': b'temporary catalogue', 'local-dump.bin': b'private'}.items():
            path = baseline / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        current = resources('current CI build')
        cdn = self.root / 'current.zip'
        cdn.write_bytes(zipped({**current, 'updates/latest.json': b'old index'}))
        prepare(self.output, baseline, cdn, get=self.get)
        self.assertEqual((self.output / 'core/old/module.wasm').read_bytes(), b'old')
        for name, data in current.items():
            self.assertEqual((self.output / name).read_bytes(), data)
        actual = {p.relative_to(self.output).as_posix() for p in self.output.rglob('*') if p.is_file()}
        self.assertEqual(actual, {*current, 'core/old/module.wasm', 'updates/catalog.json',
                                  'updates/dev/catalog.json', '.nojekyll'})

    def test_corrupt_resources_and_network_errors_leave_no_partial_site(self):
        self.add_release(1)
        for cause in ('network', 'corruption'):
            with self.subTest(cause=cause):
                def failing(url, maximum, api=False):
                    if not api:
                        if cause == 'network':
                            raise OSError('network unavailable')
                        return b'corrupt ZIP'
                    return self.get(url, maximum, api)
                with self.assertRaises(Exception):
                    prepare(self.output, get=failing)
                self.assertFalse(self.output.exists())
                self.assertEqual(list(self.root.glob('.pages-*')), [])

    def test_existing_site_cannot_be_overwritten_by_partial_preparation(self):
        self.output.mkdir()
        marker = self.output / 'current.txt'
        marker.write_bytes(b'previous complete deployment')
        with self.assertRaisesRegex(ValueError, 'must be empty'):
            prepare(self.output, get=self.get)
        self.assertEqual(marker.read_bytes(), b'previous complete deployment')

    def test_missing_baseline_or_symlink_cannot_silently_drop_existing_resources(self):
        baseline = self.root / 'baseline'
        with self.assertRaisesRegex(ValueError, 'baseline directory is missing'):
            prepare(self.output, baseline=baseline, get=self.get)
        baseline.mkdir()
        target = self.root / 'private'
        target.mkdir()
        (target / 'debug.out').write_bytes(b'private')
        (baseline / 'core').symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symbolic link'):
            prepare(self.output, baseline=baseline, get=self.get)
        self.assertFalse(self.output.exists())

    def test_resources_must_match_their_manifest_and_reject_extra_files(self):
        for invalid in ('modified', 'unlisted', 'manifest'):
            files = resources('test')
            if invalid == 'modified':
                name = next(name for name in files if name.endswith('.wasm'))
                files[name] = b'modified wasm'
            elif invalid == 'unlisted':
                files['core/debug.txt'] = b'unlisted machine data'
            else:
                name = next(name for name in files if name.startswith('releases/'))
                files[name] += b' '
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                cdn_files(zipped(files))

    def test_unsafe_paths_symlinks_and_duplicate_entries_are_rejected(self):
        for name in ('../outside', '/outside', 'core/../../outside', 'core\\outside', '.git/config'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                cdn_files(zipped({name: b'bad'}))
        output = io.BytesIO()
        with ZipFile(output, 'w') as archive_file:
            info = ZipInfo('core/link')
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive_file.writestr(info, '/private/example')
        with self.assertRaises(ValueError):
            cdn_files(output.getvalue())
        output = io.BytesIO()
        with ZipFile(output, 'w') as archive_file:
            archive_file.writestr('core/same', b'one')
            with self.assertWarns(UserWarning):
                archive_file.writestr('core/same', b'two')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            cdn_files(output.getvalue())

    def test_conflicting_immutable_resources_abort_but_newest_folder_manifest_wins(self):
        destination = self.root / 'merge'
        merge_resources({'core/build/module.wasm': b'original', 'core/build/manifest.json': b'newest'}, destination)
        merge_resources({'core/build/manifest.json': b'older'}, destination)
        self.assertEqual((destination / 'core/build/manifest.json').read_bytes(), b'newest')
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            merge_resources({'core/build/module.wasm': b'different bytes'}, destination)

    def test_app_uses_newest_source_commit_and_preserves_all_immutable_bundles(self):
        old = self.add_release(1, app_timestamp=100)
        newest = self.add_release(2, app_timestamp=300)
        current = resources('intermediate CI build', app_timestamp=200)
        cdn = self.root / 'current.zip'
        cdn.write_bytes(zipped(current))
        prepare(self.output, resource_archive=cdn, get=self.get)
        expected = {name.rsplit('/', 1)[-1]: data for name, data in newest.items() if name.startswith('web/')}
        self.assertEqual(set(expected), set(BUNDLE_FILES))
        for name, data in expected.items():
            self.assertEqual((self.output / 'app' / name).read_bytes(), data)
        for files in (old, current, newest):
            for name, data in files.items():
                self.assertEqual((self.output / name).read_bytes(), data)

    def test_baseline_and_prereleases_cannot_replace_the_stable_app(self):
        stable = self.add_release(1, app_timestamp=100)
        self.add_release(2, app_timestamp=500, prerelease=True)
        current = resources('newer development build', app_timestamp=300)
        baseline = self.root / 'baseline'
        for name, data in current.items():
            path = baseline / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        prepare(self.output, baseline=baseline, get=self.get)
        app = next(data for name, data in stable.items() if name.startswith('web/') and name.endswith('/app.mjs'))
        self.assertEqual((self.output / 'app/app.mjs').read_bytes(), app)

    def test_invalid_web_descriptors_and_worker_pins_abort_before_publishing(self):
        for failure in ('path', 'timestamp', 'missing_worker', 'html_version', 'worker_pin'):
            with self.subTest(failure=failure):
                files = resources('app fixture', app_timestamp=300)
                name = next(name for name in files if name.startswith('releases/'))
                manifest = json.loads(files.pop(name))
                prefix = manifest['webApp']['path']
                if failure == 'path':
                    manifest['webApp']['path'] = '../app/'
                elif failure == 'timestamp':
                    manifest['source']['committedAt'] = 0
                elif failure == 'missing_worker':
                    del files[prefix + 'worker.mjs']
                elif failure == 'html_version':
                    files[prefix + 'index.html'] = b'<head></head>'
                else:
                    line, template = files[prefix + 'sw.mjs'].split(b'\n', 1)
                    config = json.loads(line.decode().removeprefix('globalThis.CHAMELEON_SHELL = ')[:-1])
                    config['files'][0]['sha256'] = '0' * 64
                    files[prefix + 'sw.mjs'] = ('globalThis.CHAMELEON_SHELL = ' + json.dumps(config) + ';\n').encode() + template
                manifest['resources'] = [{'path': path, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                                         for path, data in files.items()]
                data = json.dumps(manifest).encode()
                files['releases/' + hashlib.sha256(data).hexdigest() + '.json'] = data
                with self.assertRaises(ValueError):
                    cdn_files(zipped(files))


if __name__ == '__main__': unittest.main()
