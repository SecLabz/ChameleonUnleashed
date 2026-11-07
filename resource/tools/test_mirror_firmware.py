"""Select five stable compatible releases, preserving signed upstream ZIP bytes."""
import hashlib
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit
from zipfile import ZipFile

from mirror_firmware import mirror, SOURCES, application_zip, softdevice
from test_web_update import fixtures, archive, signed_packet


class FirmwareMirrorTests(unittest.TestCase):
    def setUp(self):
        self.data = fixtures()
        self.app = archive(*self.data['normal'])
        system, packet = self.data['system']
        self.sd_size = 516
        self.system = softdevice()[:self.sd_size] + system[self.sd_size:]
        self.system_packet = signed_packet(self.system, version=1, kind=3,
                                           sd_size=self.sd_size, bl_size=len(system) - self.sd_size)
        self.full = archive(*self.data['legacy'], self.system, self.system_packet)
        self.downloads = {}
        self.releases = {repo: [] for _, _, repo in SOURCES}
        self.calls = []
        for source, _, _ in SOURCES:
            self.add_release(source, 'v1.2.3')

    def add_release(self, source, version, day=1, payload=None, **flags):
        repo = next(repo for name, _, repo in SOURCES if name == source)
        payload = payload or (self.app if source == 'unleashed' else self.full)
        if source == 'unleashed':
            bundle = io.BytesIO()
            with ZipFile(bundle, 'w') as zipped:
                zipped.writestr('firmware/application.zip', payload)
            payload = bundle.getvalue()
            name = 'ChameleonUnleashed-' + version + '-firmware-resources.zip'
        else:
            name = 'ultra-dfu-full.zip'
        url = f'https://github.com/{repo}/releases/download/{version}/{name}'
        self.downloads[url] = payload
        release = {'tag_name': version, 'assets': [{'name': name, 'browser_download_url': url,
                   'size': len(payload), 'digest': 'sha256:' + hashlib.sha256(payload).hexdigest()}],
                   'html_url': f'https://github.com/{repo}/releases/tag/{version}',
                   'published_at': f'2026-11-{day:02d}T00:00:00Z', **flags}
        self.releases[repo].append(release)
        return release

    def get(self, url, maximum, api=False):
        self.calls.append(url)
        if api:
            repo = url.split('/repos/')[1].split('/releases')[0]
            query = parse_qs(urlsplit(url).query)
            count, page = int(query['per_page'][0]), int(query['page'][0])
            return json.dumps(self.releases[repo][(page - 1) * count:page * count]).encode()
        return self.downloads[url]

    def test_original_zip_bytes_and_signatures_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            document = mirror(directory, get=self.get)
            for source, expected, kind, layout in [('unleashed', self.app, 'application', 2),
                                                  ('official', self.full, 'full', 0)]:
                release = next(s for s in document['sources'] if s['id'] == source)['releases'][0]
                self.assertEqual(release['layout'], layout)
                self.assertEqual(set(release['packages']), {kind})
                asset = release['packages'][kind]
                self.assertEqual((directory / asset['path']).read_bytes(), expected)
                self.assertEqual(stat.S_IMODE((directory / asset['path']).stat().st_mode), 0o644)
                self.assertEqual(asset['sha256'], hashlib.sha256(expected).hexdigest())
            self.assertEqual(json.loads((directory / 'catalog.json').read_text())['format'], 1)

    def test_five_latest_stable_compatible_releases_sorted_by_publication(self):
        for source, _, _ in SOURCES:
            for index in range(7):
                self.add_release(source, f'v2.0.{index}', day=index + 2)
            self.add_release(source, 'dev', day=25, prerelease=True)
            self.add_release(source, 'v99.0.0', day=26, draft=True)
        with tempfile.TemporaryDirectory() as temporary:
            document = mirror(Path(temporary), get=self.get)
        for source in document['sources']:
            self.assertEqual([r['version'] for r in source['releases']],
                             ['v2.0.6', 'v2.0.5', 'v2.0.4', 'v2.0.3', 'v2.0.2'])
        self.assertFalse(any('/releases/download/dev/' in url or '/releases/download/v99.' in url
                             for url in self.calls))

    def test_pagination_finds_compatible_releases_past_the_first_page(self):
        for source, _, repo in SOURCES:
            valid = self.releases[repo][0]
            self.releases[repo] = [{'tag_name': f'empty-{i}', 'assets': [],
                                    'published_at': '2026-11-02T00:00:00Z'} for i in range(4)] + [valid]
        with tempfile.TemporaryDirectory() as temporary, patch('mirror_firmware.PAGE_SIZE', 2):
            document = mirror(Path(temporary), get=self.get)
        self.assertTrue(all(len(source['releases']) == 1 for source in document['sources']))
        self.assertEqual(sum('page=3' in url for url in self.calls), 2)

    def test_legacy_unleashed_and_application_only_official_are_excluded(self):
        self.releases = {repo: [] for _, _, repo in SOURCES}
        self.add_release('unleashed', 'v1.0.0', payload=archive(*self.data['legacy']))
        release = self.add_release('official', 'v1.0')
        release['assets'][0]['name'] = 'chameleon_ultra_app_update.zip'
        with tempfile.TemporaryDirectory() as temporary:
            document = mirror(Path(temporary), get=self.get)
        self.assertTrue(all(not source['releases'] for source in document['sources']))

    def test_incompatible_new_releases_do_not_consume_the_five_slots(self):
        for source, _, repo in SOURCES:
            for index in range(5):
                self.add_release(source, f'v2.0.{index}', day=index + 2)
            release = self.add_release(source, 'v3.0.0', day=20)
            release['assets'] = []
        with tempfile.TemporaryDirectory() as temporary:
            document = mirror(Path(temporary), get=self.get)
        self.assertTrue(all(len(source['releases']) == 5 for source in document['sources']))

    def test_old_resources_archive_is_extracted_without_repackaging_firmware(self):
        output = io.BytesIO()
        with ZipFile(output, 'w') as z:
            z.writestr('firmware/application.zip', self.app)
            z.writestr('firmware/system.zip', self.full)
            z.writestr('cdn/unused.txt', b'not firmware')
        payload = output.getvalue()
        self.assertEqual(application_zip(payload), self.app)

    def test_network_failure_does_not_replace_a_previous_catalogue(self):
        for failing_part in ['/releases?', '/releases/download/']:
            with self.subTest(failing_part=failing_part), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                (directory / 'catalog.json').write_bytes(b'previous catalogue')
                def failing(url, maximum, api=False):
                    if 'RfidResearchGroup' in url and failing_part in url:
                        raise OSError('network unavailable')
                    return self.get(url, maximum, api)
                with self.assertRaises(OSError):
                    mirror(directory, get=failing)
                self.assertEqual((directory / 'catalog.json').read_bytes(), b'previous catalogue')

    def test_changed_release_asset_aborts_without_replacing_the_catalogue(self):
        self.releases[SOURCES[1][2]][0]['assets'][0]['digest'] = 'sha256:' + '0' * 64
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'catalog.json').write_bytes(b'previous catalogue')
            with self.assertRaisesRegex(ValueError, 'GitHub digest'):
                mirror(directory, get=self.get)
            self.assertEqual((directory / 'catalog.json').read_bytes(), b'previous catalogue')

    def test_signed_official_package_with_different_softdevice_is_excluded(self):
        system = bytes([self.system[0] ^ 1]) + self.system[1:]
        packet = signed_packet(system, kind=3, sd_size=self.sd_size, bl_size=len(system) - self.sd_size)
        bad = archive(*self.data['legacy'], system, packet)
        self.releases[SOURCES[1][2]] = []
        self.add_release('official', 'v2.0.0', payload=bad)
        with tempfile.TemporaryDirectory() as temporary:
            document = mirror(Path(temporary), get=self.get)
        self.assertEqual(document['sources'][1]['releases'], [])

    def test_signature_corruption_and_invalid_recovery_footer_are_excluded(self):
        bad_packet = self.data['normal'][1][:-1] + bytes([self.data['normal'][1][-1] ^ 1])
        bad_footer = bytearray(self.data['normal'][0])
        bad_footer[0x13fff] ^= 1
        for payload in [archive(self.data['normal'][0], bad_packet),
                        archive(bytes(bad_footer), signed_packet(bytes(bad_footer)))]:
            with self.subTest(payload_size=len(payload)), tempfile.TemporaryDirectory() as temporary:
                self.releases[SOURCES[0][2]] = []
                self.add_release('unleashed', 'v2.0.0', payload=payload)
                document = mirror(Path(temporary), get=self.get)
                self.assertEqual(document['sources'][0]['releases'], [])

    def test_cannot_request_more_than_five_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            for limit in (0, 6):
                with self.assertRaises(ValueError):
                    mirror(Path(temporary), limit=limit, get=self.get)
        self.assertEqual(self.calls, [])


if __name__ == '__main__': unittest.main()
