"""Exercise the local server against a real HTTP upstream."""

from concurrent.futures import ThreadPoolExecutor
from functools import partial
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
from threading import Thread
import unittest

import serve_web


class Device(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.server.calls.append((self.path, body, dict(self.headers)))
        self.send_response(409)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class LocalServerTests(unittest.TestCase):
    def start(self, http):
        thread = Thread(target=http.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(http.server_close)
        self.addCleanup(http.shutdown)
        return http

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        site = root / 'site'
        self.root = root
        self.site = site
        self.resources = root / 'cdn'
        site.mkdir()
        (site / 'index.html').write_bytes(b'local interface')
        (root / 'secret.txt').write_bytes(b'outside the served roots')
        module = self.resources / 'core/module.wasm'
        module.parent.mkdir(parents=True)
        module.write_bytes(b'local wasm')
        self.device = self.start(ThreadingHTTPServer(('127.0.0.1', 0), Device))
        self.device.calls = []
        address = f'http://127.0.0.1:{self.device.server_port}'
        self.http = self.start(serve_web.server(site, self.resources, address, 0))

    def enable_bundles(self):
        self.web = self.root / 'web'
        (self.web / 'src/runtime').mkdir(parents=True)
        (self.web / 'public').mkdir()
        (self.web / 'config.json').write_text('{"baseUrl":"https://cdn.example/"}')
        (self.web / 'src/value.mjs').write_text('export const value = "first";')
        (self.web / 'src/app.mjs').write_text(
            'import {value} from "./value.mjs";\n'
            'import {CORE} from "./runtime/core-build.mjs";\n'
            'import {HARDNESTED} from "./runtime/hardnested-build.mjs";\n'
            'import {ESSENTIALS} from "./documents/classic-keys.mjs";\n'
            'import {HID_FORMATS} from "./documents/hid-formats.mjs";\n'
            'import config from "../config.json" with {type: "json"};\n'
            'console.log(JSON.stringify({value, CORE, HARDNESTED, ESSENTIALS, HID_FORMATS, config}));\n'
        )
        self.shared = self.root / 'shared'
        self.package = self.shared / 'packages/nfc-wasm'
        self.package.mkdir(parents=True)
        (self.package / 'package.json').write_text(json.dumps({
            'files': ['entry.mjs'], 'exports': {'./worker': './entry.mjs'},
        }))
        (self.package / 'entry.mjs').write_text('console.log("worker first");')
        dictionary = self.shared / 'crates/nfc-mifare/resources/dictionaries/classic.dic'
        dictionary.parent.mkdir(parents=True)
        dictionary.write_text('FFFFFFFFFFFF\n# End essentials\n')
        self.cache = self.resources.parent
        (self.cache / 'core.json').write_text('{"path":"module.wasm"}')
        table = {'sha256': 'table-hash', 'path': 'pack.knhp'}
        (self.web / 'src/runtime/tables.json').write_text(json.dumps(table))
        (self.cache / 'hardnested.json').write_text(json.dumps({
            'tablePack': table, 'modules': {
                'scalar': {'path': 'scalar.wasm'}, 'simd': {'path': 'simd.wasm'},
            },
        }))
        (self.cache / 'cdn-build.json').write_text('{}')
        for name in ['hardnested/scalar.wasm', 'hardnested/simd.wasm', 'tables/table-hash/pack.knhp']:
            path = self.resources / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'resource')

        build = partial(serve_web.bundle, web=self.web, cache=self.cache,
                        shared=self.shared, bundler=serve_web.ensure_esbuild())
        address = f'http://127.0.0.1:{self.device.server_port}'
        self.http = self.start(serve_web.server(self.site, self.resources, address, 0, bundle=build))

    def app_output(self):
        status, headers, body = self.request('GET', '/app.mjs?v=edited')
        self.assertEqual(status, 200, body.decode())
        self.assertEqual(headers['cache-control'], 'no-store')
        self.assertTrue(headers['content-type'].startswith('text/javascript'))
        result = subprocess.run(['node', '--input-type=module'], input=body,
                                capture_output=True, check=True)
        return json.loads(result.stdout)

    def request(self, method, path, body=None, headers=None):
        connection = HTTPConnection('127.0.0.1', self.http.server_port, timeout=2)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, {name.lower(): value for name, value in response.headers.items()}, response.read()
        finally:
            connection.close()

    def test_local_assets_and_resources_are_served_without_exposing_other_files(self):
        status, headers, body = self.request('GET', '/')
        self.assertEqual((status, body), (200, b'local interface'))
        self.assertIn("connect-src 'self'", headers['content-security-policy'])
        self.assertEqual(headers['cache-control'], 'no-store')
        status, headers, body = self.request('GET', '/core/module.wasm')
        self.assertEqual((status, body), (200, b'local wasm'))
        self.assertEqual(headers['content-type'], 'application/wasm')
        self.assertEqual(self.request('GET', '/%2e%2e/secret.txt')[0], 404)
        self.assertEqual(self.request('GET', '/core/%2e%2e/%2e%2e/secret.txt')[0], 404)
        self.assertEqual(self.request('GET', '/core/')[0], 404)

    def test_all_published_resource_folders_are_served_at_the_root(self):
        for name in ['hardnested/module.wasm', 'tables/pack.knhp', 'releases/build.json']:
            with self.subTest(name=name):
                resource = self.resources / name
                resource.parent.mkdir(parents=True, exist_ok=True)
                resource.write_bytes(name.encode())
                status, headers, body = self.request('GET', '/' + name)
                self.assertEqual((status, body), (200, name.encode()))

    def test_static_edits_are_visible_without_restarting(self):
        (self.site / 'index.html').write_bytes(b'edited page')
        (self.site / 'style.css').write_bytes(b'body { color: red; }')
        self.assertEqual(self.request('GET', '/')[2], b'edited page')
        self.assertEqual(self.request('GET', '/style.css')[2], b'body { color: red; }')

    def test_ble_mode_uses_live_html_and_local_resources_without_usb_relay(self):
        self.http = self.start(serve_web.server(self.site, self.resources, None, 0))
        for label in ('first', 'edited'):
            (self.site / 'index.html').write_text(f'<html><head></head><body>{label}</body></html>')
            for path in ('/', '/index.html?v=edited'):
                status, headers, body = self.request('GET', path)
                self.assertEqual(status, 200)
                self.assertTrue(headers['content-type'].startswith('text/html'))
                self.assertEqual(headers['cache-control'], 'no-store')
                self.assertIn(b'<meta name="chameleon-hosted" content="1">', body)
                self.assertIn(b'<meta name="chameleon-resources" content="/">', body)
                self.assertNotIn(b'chameleon-shell', body)
                self.assertIn(label.encode(), body)
        self.assertEqual(self.request('GET', '/core/module.wasm')[2], b'local wasm')
        self.assertEqual(self.request('GET', '/api/device')[0], 404)
        self.assertEqual(self.request('POST', '/api/session', b'')[0], 404)
        self.assertEqual(self.device.calls, [])

    def test_updater_serves_every_embedded_module_license_and_local_resource_url(self):
        from build_recovery import assets
        for name, expected in assets('/').items():
            with self.subTest(name=name):
                status, headers, body = self.request('GET', '/' + name + '?v=edited')
                self.assertEqual((status, body), (200, expected))
                if name.endswith('.mjs'):
                    self.assertTrue(headers['content-type'].startswith('text/javascript'))
        self.assertEqual(self.request('GET', '/update')[2], assets('/')['update.html'])
        self.assertIn(b'RESOURCE_BASE = "/"', self.request('GET', '/update-config.mjs')[2])
        catalogue = self.resources / 'updates/catalog.json'
        catalogue.parent.mkdir()
        catalogue.write_bytes(b'{"format":1,"sources":[]}')
        self.assertEqual(self.request('GET', '/updates/catalog.json')[2], catalogue.read_bytes())

    def test_bundles_use_current_sources_and_generated_definitions(self):
        self.enable_bundles()
        output = self.app_output()
        self.assertEqual(output['value'], 'first')
        self.assertEqual(output['config']['baseUrl'], '/')
        self.assertEqual(output['CORE'], {'path': 'module.wasm'})
        self.assertEqual(set(output['HARDNESTED']), {'scalar', 'simd'})
        self.assertEqual(output['ESSENTIALS'], [[255] * 6])
        self.assertTrue(output['HID_FORMATS'])
        (self.web / 'src/added.mjs').write_text('export const value = "second";')
        (self.web / 'src/value.mjs').write_text('export {value} from "./added.mjs";')
        self.assertEqual(self.app_output()['value'], 'second')
        (self.package / 'entry.mjs').write_text('console.log("worker edited");')
        status, headers, body = self.request('GET', '/worker.mjs')
        self.assertEqual(status, 200)
        result = subprocess.run(['node', '--input-type=module'], input=body,
                                capture_output=True, check=True)
        self.assertEqual(result.stdout.strip(), b'worker edited')
        status, headers, body = self.request('HEAD', '/app.mjs')
        self.assertEqual((status, body), (200, b''))
        self.assertGreater(int(headers['content-length']), 0)

    def test_build_errors_are_reported_and_recover_on_the_next_request(self):
        self.enable_bundles()
        (self.web / 'src/value.mjs').write_text('export const value = ;')
        status, headers, body = self.request('GET', '/app.mjs')
        self.assertEqual(status, 500)
        self.assertIn(b'value.mjs', body)
        (self.web / 'src/value.mjs').write_text('export const value = "fixed";')
        self.assertEqual(self.app_output()['value'], 'fixed')

    def test_parallel_bundle_requests_use_independent_staging(self):
        self.enable_bundles()
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda path: self.request('GET', path),
                                    ['/app.mjs', '/worker.mjs', '/app.mjs', '/worker.mjs']))

        self.assertEqual([status for status, _, _ in results], [200] * 4)
        self.assertEqual(results[0][2], results[2][2])
        self.assertEqual(results[1][2], results[3][2])

    def test_resource_readiness_requires_complete_matching_resources(self):
        self.enable_bundles()
        self.assertTrue(serve_web.resources_ready(self.cache, self.web))
        module = self.resources / 'hardnested/simd.wasm'
        module.unlink()
        self.assertFalse(serve_web.resources_ready(self.cache, self.web))
        module.write_bytes(b'restored')
        (self.web / 'src/runtime/tables.json').write_text('{"sha256":"changed","path":"pack.knhp"}')
        self.assertFalse(serve_web.resources_ready(self.cache, self.web))
        (self.cache / 'hardnested.json').write_text('interrupted write')
        self.assertFalse(serve_web.resources_ready(self.cache, self.web))

    def test_binary_api_errors_and_query_parameters_survive_the_relay(self):
        origin = f'http://127.0.0.1:{self.http.server_port}'
        data = bytes([0, 255, 1, 2])
        status, headers, body = self.request('POST', '/api/command/1?id=7', data, {
            'Origin': origin,
            'Sec-Fetch-Site': 'same-origin',
            'Content-Type': 'application/octet-stream',
        })
        self.assertEqual((status, body), (409, data))
        path, forwarded, upstream_headers = self.device.calls[0]
        self.assertEqual((path, forwarded), ('/api/command/1?id=7', data))
        self.assertEqual(upstream_headers['Host'], f'127.0.0.1:{self.device.server_port}')
        self.assertEqual(upstream_headers['Origin'], 'http://' + upstream_headers['Host'])
        self.assertEqual(upstream_headers['Sec-Fetch-Site'], 'same-origin')

    def test_foreign_origins_and_oversized_requests_never_reach_the_device(self):
        for headers in [
            {'Origin': 'https://other.example'},
            {'Host': 'other.example'},
            {'Sec-Fetch-Site': 'cross-site'},
        ]:
            with self.subTest(headers=headers):
                self.assertEqual(self.request('POST', '/api/session', b'', headers)[0], 403)

        self.assertEqual(self.request('POST', '/api/session', bytes(1025))[0], 400)
        self.assertEqual(self.device.calls, [])

    def test_unavailable_device_returns_an_http_error(self):
        self.device.shutdown()
        self.device.server_close()
        self.assertEqual(self.request('POST', '/api/session', b'')[0], 502)


if __name__ == '__main__':
    unittest.main()
