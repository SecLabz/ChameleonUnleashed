#!/usr/bin/env python3
"""Build the embedded web assets; never flashes hardware or publishes files."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from urllib.parse import urlsplit

from esbuild_tool import ensure_esbuild
from project_version import read_version

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / 'software/web'
LOCK = WEB / 'build-lock.json'
# Reserve the rest of the firmware's 512-byte HTTP header buffer for response headers.
RESOURCE_ORIGIN_MAX_BYTES = 80


def run(*args, env=None):
    subprocess.run([str(v) for v in args], check=True, env=env)


def resource_origin(base):
    address = urlsplit(base)
    if (
        not base.endswith('/') or any(c.isspace() or c in "'\";\\" for c in base)
        or address.query or address.fragment or address.username or address.password
    ):
        raise ValueError('The resource base must be a directory URL without credentials, query or fragment')

    if base.startswith('/') and not base.startswith('//') and not address.netloc:
        return ''

    if address.scheme not in {'http', 'https'} or not address.hostname or address.port == 0:
        raise ValueError('The resource base must be an HTTP(S) URL or an absolute local path')

    origin = address.scheme + '://' + address.netloc
    if len(origin.encode('utf-8')) > RESOURCE_ORIGIN_MAX_BYTES:
        raise ValueError(
            f'The resource origin must fit within {RESOURCE_ORIGIN_MAX_BYTES} UTF-8 bytes for firmware HTTP headers')

    return origin


def source_version(directory):
    revision = subprocess.run(['git', '-C', str(directory), 'rev-parse', '--verify', 'HEAD'],
                              capture_output=True, text=True)
    if revision.returncode:
        return {'revision': None, 'dirty': None, 'committedAt': None}

    status = subprocess.check_output([
        'git', '-C', str(directory), 'status', '--porcelain', '--untracked-files=normal'])
    timestamp = subprocess.check_output(['git', '-C', str(directory), 'show', '-s', '--format=%ct', 'HEAD'])
    return {'revision': revision.stdout.strip(), 'dirty': bool(status), 'committedAt': int(timestamp)}


def fingerprint(directory, shared=False):
    digest = hashlib.sha256()
    if shared:
        names = subprocess.check_output(['git', '-C', str(directory), 'ls-files', '-z',
            '--cached', '--others', '--exclude-standard']).decode().split('\0')
        paths = [directory / n for n in names if n and
                 (n in ('Cargo.toml', 'Cargo.lock', 'LICENSE', 'tools/build_release.py') or
                  n.startswith(('crates/', 'packages/nfc-wasm/', 'docs/licenses/')))]
    else:
        paths = [p for p in directory.rglob('*') if p.is_file() and '.git' not in p.relative_to(directory).parts]
    for path in sorted(paths):
        digest.update(path.relative_to(directory).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def wasm_package(shared, verify=True):
    directory = shared / 'packages/nfc-wasm'
    package = json.loads((directory / 'package.json').read_text())
    if verify:
        metadata = json.loads(subprocess.check_output([
            'cargo', 'metadata', '--manifest-path', str(shared / 'Cargo.toml'),
            '--no-deps', '--format-version=1', '--locked']))
        version = next(crate['version'] for crate in metadata['packages']
                       if crate['name'] == 'nfc-wasm')
        if package['version'] != version:
            raise SystemExit('JavaScript package and Rust WASM versions differ')

    modules = {name: (directory / name).read_bytes() for name in package['files']
               if name.endswith('.mjs')}
    worker = package['exports']['./worker'].removeprefix('./')
    if worker not in modules:
        raise ValueError('The JavaScript package must distribute its Worker entry point')
    return modules, worker


def update_config(base):
    return ('export const RESOURCE_BASE = ' + json.dumps(base) + ';\n').encode()


def frontend_sources(web, shared, core_asset, modules, package_modules, base):
    sources = {}
    for path in sorted((web / 'src').rglob('*')):
        if path.is_file() and path.suffix in ('.mjs', '.json'):
            sources['/' + path.relative_to(web / 'src').as_posix()] = path.read_bytes()
    for path in sorted((web / 'public').iterdir()):
        if path.suffix in ('.html', '.css', '.svg'):
            sources['/' + path.name] = path.read_bytes()
    for name, data in package_modules.items():
        sources['/runtime/nfc-wasm/' + name] = data
    # The update form reads its release base from the module the recovery also generates.
    sources['/ui/update-config.mjs'] = update_config(base)
    sources['/runtime/core-build.mjs'] = ('export const CORE = ' + json.dumps(core_asset) + ';\n').encode()
    sources['/runtime/hardnested-build.mjs'] = ('export const HARDNESTED = ' + json.dumps(modules) + ';\n').encode()
    # Use the shared essentials prefix without loading the recovery WASM during discovery.
    dictionary = (shared / 'crates/nfc-mifare/resources/dictionaries/classic.dic').read_text()
    prefix, boundary, _ = dictionary.partition('# End essentials')
    essentials = list(dict.fromkeys(re.findall(r'(?m)^([0-9A-Fa-f]{12})\s*$', prefix)))
    if not boundary or not 1 <= len(essentials) <= 64:
        raise ValueError('Invalid shared essentials dictionary')
    essentials = [list(bytes.fromhex(key)) for key in essentials]
    sources['/documents/classic-keys.mjs'] = ('export const ESSENTIALS = ' + json.dumps(essentials) + ';\n').encode()
    # The native CLI owns HID field limits; expose that same table to the editor.
    hid_source = (ROOT / 'software/rust/chameleon-client/src/lf_hid.rs').read_text()
    hid_formats = re.findall(r'\("(\w+)", (\d+), \[([\d, ]+)\]\)', hid_source)
    if not hid_formats:
        raise ValueError('HID format definitions could not be read')
    hid_formats = [[name, int(code), [int(v) for v in limits.split(',')]]
                   for name, code, limits in hid_formats]
    sources['/documents/hid-formats.mjs'] = ('export const HID_FORMATS = ' + json.dumps(hid_formats) + ';\n').encode()
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shared-source', type=Path, default=ROOT.parent / 'nfc-libs-rs')
    parser.add_argument('--prepare-deps', action='store_true', help='Fetch pinned TinyUSB/lwIP checkouts')
    parser.add_argument('--resources-only', action='store_true', help='Prepare local WASM and tables without embedded assets')
    parser.add_argument('--zopfli', action='store_true', help='Use installed Zopfli for smaller compatible gzip assets')
    parser.add_argument('--allow-local-changes', action='store_true', help='Build edited shared sources for local development')
    parser.add_argument('--output', type=Path, default=ROOT / '.cache/web/assets')
    parser.add_argument('--asset-base', help='Override the resource base URL in this web build')
    args = parser.parse_args()
    config = json.loads((WEB / 'config.json').read_text())
    config['projectVersion'] = read_version(ROOT)
    if args.asset_base is not None:
        config['baseUrl'] = args.asset_base

    try:
        origin = resource_origin(config['baseUrl'])
    except ValueError as error:
        parser.error(str(error))

    source = source_version(ROOT)
    if args.zopfli and not shutil.which('zopfli'):
        parser.error('--zopfli requires the zopfli executable on PATH')
    try:
        bundler = ensure_esbuild()
    except RuntimeError as error:
        parser.error(str(error))
    lock = json.loads(LOCK.read_text())
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    for name, dependency in lock['dependencies'].items():
        dest = ROOT / '.cache/web/deps' / name
        if not dest.exists() and args.prepare_deps:
            run('git', '-c', 'core.autocrlf=false', 'clone', '--no-checkout', dependency['url'], dest)
            run('git', '-C', dest, 'checkout', '--detach', dependency['revision'])
        if not dest.is_dir() or fingerprint(dest) != dependency['sha256']:
            raise SystemExit(f'{name} source differs from build-lock.json; prepare the pinned clean dependency')
    notices = [
        (WEB / 'THIRD_PARTY.txt').read_text(),
        (ROOT / 'LICENSE').read_text(),
        (ROOT / 'firmware/application/src/net/rndis.c').read_text().split('#include', 1)[0],
        (ROOT / '.cache/web/deps/tinyusb/LICENSE').read_text(),
        (ROOT / '.cache/web/deps/lwip/COPYING').read_text(),
    ]
    (out / 'THIRD_PARTY.txt').write_text('\n\n'.join(text.strip() for text in notices) + '\n')
    shared = args.shared_source.resolve()
    if not (shared / 'crates/nfc-wasm/Cargo.toml').is_file():
        raise SystemExit('Select the nfc-libs-rs checkout with --shared-source')
    shared_hash = fingerprint(shared, shared=True)
    if not args.allow_local_changes and shared_hash != lock['shared']['sha256']:
        raise SystemExit('Shared source differs from build-lock.json; use the matching workspace')
    shared_version = {**source_version(shared), 'sha256': shared_hash}
    package_modules, package_worker = wasm_package(shared)
    # An interrupted rebuild must not package new modules with older provenance.
    (ROOT / '.cache/web/cdn-build.json').unlink(missing_ok=True)

    def build(name, feature, simd=False):
        target = ROOT / '.cache/web' / name
        encoded = os.environ.get('CARGO_ENCODED_RUSTFLAGS')
        if encoded is None:
            flags = shlex.split(os.environ.get('RUSTFLAGS', ''))
        else:
            flags = encoded.split('\x1f') if encoded else []
        flags.extend(['-C', f'target-feature={"+" if simd else "-"}simd128'])
        run(sys.executable, shared / 'tools/build_release.py',
            '--config', 'profile.release.lto=true', '--config', 'profile.release.codegen-units=1',
            '--no-default-features', '--features', feature,
            '-p', 'nfc-wasm', '--target', 'wasm32-unknown-unknown', '--target-dir', target,
            env={**os.environ, 'CARGO_ENCODED_RUSTFLAGS': '\x1f'.join(flags)})
        return (target / 'wasm32-unknown-unknown/release/nfc_wasm.wasm').read_bytes()

    core = build('wasm', 'core')
    core_hash = hashlib.sha256(core).hexdigest()
    core_asset = {'path': f'{core_hash}/nfc_wasm.wasm', 'size': len(core), 'sha256': core_hash}
    core_dir = ROOT / '.cache/web/cdn/core' / Path(core_asset['path']).parent
    core_dir.mkdir(parents=True, exist_ok=True)
    (core_dir / 'nfc_wasm.wasm').write_bytes(core)
    (ROOT / '.cache/web/core.json').write_text(json.dumps(core_asset, indent=2) + '\n')
    variants = {'scalar': build('hardnested-scalar', 'hardnested'),
                'simd': build('hardnested-simd', 'hardnested', True)}
    table = json.loads((WEB / 'src/runtime/tables.json').read_text())
    release = hashlib.sha256(b''.join(variants.values()) + bytes.fromhex(table['sha256']) + b'ABI1').hexdigest()
    folder = Path(release)
    cdn = ROOT / '.cache/web/cdn/hardnested' / folder
    cdn.mkdir(parents=True, exist_ok=True)
    modules = {}
    for name, data in variants.items():
        filename = f'hardnested-{name}.wasm'
        (cdn / filename).write_bytes(data)
        modules[name] = {'path': (folder / filename).as_posix(), 'size': len(data),
                         'sha256': hashlib.sha256(data).hexdigest()}
    manifest = {'version': 2, 'abi': 1, 'tablePack': table, 'modules': modules}
    (cdn / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (ROOT / '.cache/web/hardnested.json').write_text(json.dumps(manifest, indent=2) + '\n')
    frontend = hashlib.sha256(''.join(fingerprint(WEB / name)
                             for name in ('src', 'public', 'hosted')).encode()
                             + (ROOT / 'software/rust/chameleon-client/src/lf_hid.rs').read_bytes()).hexdigest()
    build = {'projectVersion': config['projectVersion'], 'source': source,
             'shared': shared_version, 'resourceBase': config['baseUrl'], 'frontendSha256': frontend}
    if args.resources_only:
        from package_cdn import copy_asset

        copy_asset(shared / 'crates/nfc-mifare/resources',
                   ROOT / '.cache/web/cdn/tables' / table['sha256'], table)
        (ROOT / '.cache/web/cdn-build.json').write_text(json.dumps(build, indent=2) + '\n')
        return

    (out / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    sources = frontend_sources(WEB, shared, core_asset, modules, package_modules, config['baseUrl'])
    # Keep generated source modules for unit tests, separate from shipped assets.
    source_dir = out / 'modules'
    site = out / 'site'
    for directory in (source_dir, site):
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir()
    for name, data in sources.items():
        path = (source_dir if name.endswith(('.mjs', '.json')) else site) / name.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for entry, filename in [('app.mjs', 'app.mjs'), ('runtime/nfc-wasm/' + package_worker, 'worker.mjs'), ('ui/ble-firmware.mjs', 'ble-update.mjs')]:
        # The application owns the served Worker URL and passes its factory to the client.
        run(bundler, source_dir / entry, '--bundle', '--format=esm', '--target=es2022',
            '--minify', '--legal-comments=inline', f'--outfile={site / filename}',
            f'--metafile={out / (filename + ".json")}')
    from build_recovery import assets as recovery_assets, prepare as prepare_recovery
    for name, data in recovery_assets(config['baseUrl']).items():
        (site / name).write_bytes(data)
    prepare_recovery(config['baseUrl'], ROOT / '.cache/web/recovery')
    types = {'.html': 'text/html; charset=utf-8', '.css': 'text/css', '.mjs': 'text/javascript', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.txt': 'text/plain; charset=utf-8'}
    header = ['#pragma once', '#include <stdint.h>', '#include <string.h>',
              '#define WEB_RESOURCE_CONNECT ' + json.dumps("'self'" + (' ' + origin if origin else '')),
              'typedef struct { const char *path, *type; const uint8_t *bytes; size_t size; } web_asset_t;']
    rows, sizes = [], {}
    for i, raw_path in enumerate(sorted(site.iterdir())):
        name = '/' + raw_path.name
        data = raw_path.read_bytes()
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        if args.zopfli:
            optimized = subprocess.check_output(['zopfli', '-c', '--gzip', '--i15', str(raw_path)])
            if gzip.decompress(optimized) != data:
                raise SystemExit(f'Zopfli verification failed: {name}')
            if len(optimized) < len(packed):
                packed = optimized
        sizes[name] = {'raw': len(data), 'gzip': len(packed), 'sha256': hashlib.sha256(data).hexdigest()}
        if raw_path.name == 'ble-update.mjs':
            continue  # Loaded on demand by the hosted BLE app, never stored in firmware.
        (out / (str(i) + '.gz')).write_bytes(packed)
        header.append(f'static const uint8_t web_bytes_{i}[] = {{' + ','.join(map(str, packed)) + '};')
        rows.append('{' + json.dumps(name) + ',' + json.dumps(types[Path(name).suffix]) + f',web_bytes_{i},sizeof(web_bytes_{i})' + '}')
    header += ['static const web_asset_t web_assets[] = {' + ','.join(rows) + '};',
               'static const web_asset_t *web_asset_find(const char *path) {',
               ' if (!strcmp(path,"/")) path="/index.html";',
               ' for (size_t i=0;i<sizeof(web_assets)/sizeof(*web_assets);i++)',
               '  if (!strcmp(path,web_assets[i].path)) return &web_assets[i];',
               ' return NULL;', '}']
    (out / 'web_assets.h').write_text('\n'.join(header) + '\n')
    (out / 'sizes.json').write_text(json.dumps(sizes, indent=2) + '\n')
    # The resource-only command must never silently package an older frontend
    # with newer modules. Capture the frontend's exact pins and file hashes.
    site_build = {'build': build, 'core': core_asset, 'hardnested': modules,
                  'tables': table, 'files': sizes}
    (out / 'site-build.json').write_text(json.dumps(site_build, indent=2) + '\n')
    # Capture provenance with the built modules, not with a later packaging checkout.
    (ROOT / '.cache/web/cdn-build.json').write_text(json.dumps(build, indent=2) + '\n')
    print(f'Embedded assets: {sum(v["gzip"] for v in sizes.values())} gzip bytes')


if __name__ == '__main__':
    main()
