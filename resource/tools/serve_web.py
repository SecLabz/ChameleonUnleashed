"""Serve local web resources and relay the Ultra's HTTP API."""

from functools import partial
from http.client import HTTPConnection, HTTPException
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
from pathlib import Path
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit

from build_web import frontend_sources, wasm_package
from esbuild_tool import ensure_esbuild

TIMEOUT_SECONDS = 20
REQUEST_MAX_BYTES = 1024
CSP = (
    "default-src 'self'; connect-src 'self'; worker-src 'self'; "
    "script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; "
    "font-src data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"
)


def resources_ready(cache, web):
    try:
        core = json.loads((cache / 'core.json').read_text())
        hardnested = json.loads((cache / 'hardnested.json').read_text())
        table = json.loads((web / 'src/runtime/tables.json').read_text())
        if not (cache / 'cdn-build.json').is_file() or hardnested['tablePack'] != table:
            return False

        resources = cache / 'cdn'
        paths = [resources / 'core' / core['path'],
                 resources / 'tables' / table['sha256'] / table['path']]
        paths.extend(resources / 'hardnested' / hardnested['modules'][name]['path']
                     for name in ('scalar', 'simd'))
        return all(path.is_file() for path in paths)
    except (OSError, ValueError, KeyError):
        return False


def bundle(name, web, cache, shared, bundler):
    core = json.loads((cache / 'core.json').read_text())
    hardnested = json.loads((cache / 'hardnested.json').read_text())
    package_modules, worker = wasm_package(shared, verify=False)
    sources = frontend_sources(web, shared, core, hardnested['modules'], package_modules)
    config = json.loads((web / 'config.json').read_text())
    config['baseUrl'] = '/'
    # Each request owns its staging directory, including concurrent app/worker loads.
    with tempfile.TemporaryDirectory(prefix='chameleon-web-') as temporary:
        out = Path(temporary)
        (out / 'config.json').write_text(json.dumps(config))
        for path, data in sources.items():
            if path.endswith(('.mjs', '.json')):
                target = out / 'modules' / path.lstrip('/')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)

        entry = 'app.mjs'
        if name == '/worker.mjs':
            entry = 'runtime/nfc-wasm/' + worker

        result = subprocess.run([
            str(bundler), str(Path('modules') / entry), '--bundle', '--format=esm',
            '--target=es2022', '--legal-comments=inline',
        ], cwd=out, capture_output=True, check=True)
        return result.stdout


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, resources, device, bundle=None, **kwargs):
        self.resources = resources
        self.device = device
        self.bundle = bundle
        super().__init__(*args, **kwargs)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Security-Policy', CSP)
        self.send_header('X-Content-Type-Options', 'nosniff')
        super().end_headers()

    def translate_path(self, path):
        name = unquote(urlsplit(path).path).lstrip('/')
        directory = Path(self.directory)
        if name.startswith(('core/', 'hardnested/', 'tables/', 'releases/')):
            directory = self.resources

        target = (directory / name).resolve()
        if not target.is_relative_to(directory.resolve()):
            return str(directory / '.missing')

        return str(target)

    def list_directory(self, path):
        self.send_error(404)

    def do_GET(self):
        if self.path.startswith('/api/'):
            self.relay()
        else:
            super().do_GET()

    def send_head(self):
        name = urlsplit(self.path).path
        if self.bundle is None or name not in {'/app.mjs', '/worker.mjs'}:
            return super().send_head()

        try:
            body = self.bundle(name)
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
            detail = str(error)
            if isinstance(error, subprocess.CalledProcessError):
                detail = error.stderr.decode(errors='replace')

            self.log_error('Web build failed: %s', detail)
            self.send_error(500, 'Web build failed', detail)
            return None

        self.send_response(200)
        self.send_header('Content-Type', 'text/javascript; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        return BytesIO(body)

    def do_POST(self):
        if self.path.startswith('/api/'):
            self.relay()
        else:
            self.send_error(405)

    def relay(self):
        port = self.server.server_port
        host = self.headers.get('Host')
        origin = self.headers.get('Origin')
        site = self.headers.get('Sec-Fetch-Site')
        if (
            host not in {f'localhost:{port}', f'127.0.0.1:{port}'}
            or (origin is not None and origin != f'http://{host}')
            or (site is not None and site not in {'same-origin', 'none'})
        ):
            self.send_error(403)
            return

        lengths = self.headers.get_all('Content-Length', [])
        try:
            size = int(lengths[0]) if lengths else 0
        except ValueError:
            self.send_error(400)
            return

        if len(lengths) > 1 or size < 0 or size > REQUEST_MAX_BYTES or 'Transfer-Encoding' in self.headers:
            self.send_error(400)
            return

        try:
            self.connection.settimeout(TIMEOUT_SECONDS)
            data = self.rfile.read(size)
        except TimeoutError:
            self.send_error(408)
            return

        if len(data) != size:
            self.send_error(400)
            return

        connection = HTTPConnection(self.device.hostname, self.device.port, timeout=TIMEOUT_SECONDS)
        try:
            # The local origin was checked above; the device sees its own origin and host.
            headers = {
                'Host': self.device.netloc,
                'Origin': 'http://' + self.device.netloc,
                'Sec-Fetch-Site': 'same-origin',
                'Content-Type': self.headers.get('Content-Type', 'application/octet-stream'),
            }
            connection.request(self.command, self.path, body=data, headers=headers)
            response = connection.getresponse()
            body = response.read()
        except (OSError, HTTPException) as error:
            self.log_error('Device connection failed: %s', error)
            self.send_error(502, 'Device unavailable')
            return
        finally:
            connection.close()

        self.send_response(response.status)
        self.send_header('Content-Type', response.getheader('Content-Type', 'application/octet-stream'))
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            self.close_connection = True


def server(site, resources, device, port, bundle=None):
    address = urlsplit(device)
    if (
        address.scheme != 'http' or not address.hostname or address.username or address.password
        or address.path not in {'', '/'} or address.query or address.fragment
    ):
        raise ValueError('The device must be an HTTP origin, such as http://192.168.16.1')

    handler = partial(Handler, directory=str(site), resources=resources, device=address, bundle=bundle)
    return ThreadingHTTPServer(('127.0.0.1', port), handler)


def serve(web, cache, shared, device, port):
    build = partial(bundle, web=web, cache=cache, shared=shared, bundler=ensure_esbuild())
    with server(web / 'public', cache / 'cdn', device, port, bundle=build) as http:
        print(f'Local interface: http://127.0.0.1:{http.server_port} (device: {device})', flush=True)
        print('Refresh after web edits. After Rust changes, run ./dev cdn with the same --shared-source.', flush=True)
        http.serve_forever()
