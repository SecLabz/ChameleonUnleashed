"""Build and validate the immutable shell carried by a browser resource archive."""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
FILES = ('index.html', 'app.mjs', 'worker.mjs', 'style.css', 'favicon.svg', 'chevron-down.svg')
BUNDLE_FILES = (*FILES, 'sw.mjs')


def markers(version, resource_base='../'):
    return ('<meta name="chameleon-hosted" content="1">\n'
            f'<meta name="chameleon-resources" content="{resource_base}">\n'
            f'<meta name="chameleon-shell" content="{version}">\n').encode()


def shell(site):
    files = {name: (site / name).read_bytes() for name in FILES}
    return bundle(files)


def bundle(source, resource_base='../'):
    files = dict(source)
    if b'chameleon-hosted' in files['index.html']:
        raise ValueError('Expected the original USB-served interface as input')
    template = (ROOT / 'software/web/hosted/sw.mjs').read_bytes()
    suffix = resource_base.encode() if resource_base != '../' else b''
    version = hashlib.sha256(b''.join(files[name] for name in FILES) + template + suffix).hexdigest()
    if files['index.html'].count(b'</head>') != 1:
        raise ValueError('The interface needs exactly one HTML head')
    files['index.html'] = files['index.html'].replace(b'</head>', markers(version, resource_base) + b'</head>')
    manifest = [{'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in files.items()]
    config = {'version': version, 'files': manifest}
    files['sw.mjs'] = ('globalThis.CHAMELEON_SHELL = ' + json.dumps(config) + ';\n').encode() + template
    return version, files


def archived_shell(manifest, files):
    """Read only a complete, manifest-bound shell; old module-only archives work."""
    descriptor = manifest.get('webApp')
    if descriptor is None:
        return None
    if not isinstance(descriptor, dict):
        raise ValueError('Invalid web app descriptor')
    version = descriptor.get('version', '')
    resource_base = descriptor.get('resourceBase', '../')
    if resource_base not in ('../', '../../'):
        raise ValueError('Unexpected web resource base')
    if not isinstance(version, str) or not re.fullmatch(r'[0-9a-f]{64}', version):
        raise ValueError('Invalid web app identity')
    prefix = f'web/{version}/'
    if descriptor.get('path') != prefix:
        raise ValueError('Unexpected web app bundle path')
    timestamp = manifest.get('source', {}).get('committedAt')
    if type(timestamp) is not int or timestamp <= 0:
        raise ValueError('Web app needs a source commit timestamp')
    expected = {prefix + name for name in BUNDLE_FILES}
    if {name for name in files if name.startswith(prefix)} != expected:
        raise ValueError('Incomplete or unlisted web app bundle')
    listed = {entry['path']: entry for entry in manifest['resources']}
    result = {}
    for name in BUNDLE_FILES:
        path = prefix + name
        data = files[path]
        entry = listed.get(path, {})
        if entry.get('size') != len(data) or entry.get('sha256') != hashlib.sha256(data).hexdigest():
            raise ValueError('Web app differs from its release manifest: ' + name)
        result[name] = data
    marker = markers(version, resource_base)
    if result['index.html'].count(marker) != 1:
        raise ValueError('Web app HTML differs from its shell identity')
    try:
        first_line, template = result['sw.mjs'].split(b'\n', 1)
        line = first_line.decode()
        start = 'globalThis.CHAMELEON_SHELL = '
        if not line.startswith(start) or not line.endswith(';'):
            raise ValueError('Invalid web app worker configuration')
        config = json.loads(line[len(start):-1])
        if config['version'] != version or {entry['path'] for entry in config['files']} != set(FILES):
            raise ValueError('Web app worker differs from its shell identity')
        if len(config['files']) != len(FILES):
            raise ValueError('Duplicate web app worker resources')
        for entry in config['files']:
            data = result[entry['path']]
            if len(data) != entry['size'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise ValueError('Web app worker contains a mismatched resource')
        source = [result[name] if name != 'index.html' else result[name].replace(marker, b'', 1) for name in FILES]
        suffix = resource_base.encode() if resource_base != '../' else b''
        if hashlib.sha256(b''.join(source) + template + suffix).hexdigest() != version:
            raise ValueError('Web app content differs from its immutable identity')
    except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError('Invalid web app worker configuration') from error
    return timestamp, result
