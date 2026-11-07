#!/usr/bin/env python3
"""Rebuild Pages from release archives, without committing firmware to Git."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import stat
import tempfile
from zipfile import ZipFile

from mirror_firmware import fetch, mirror

RESOURCE_FOLDERS = {'core', 'hardnested', 'tables', 'releases'}
MAX_EXPANDED = 256 * 1024 * 1024


def resource_path(name):
    path = PurePosixPath(name)
    if (not path.name or path.is_absolute() or '..' in path.parts or '\\' in name
            or ':' in name or path.as_posix() != name or path.parts[0] not in RESOURCE_FOLDERS):
        raise ValueError('Unexpected resource path: ' + name)
    return path


def cdn_files(data, prefix=''):
    files = {}
    expanded = 0
    with ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if len(entries) > 4096 or len({entry.filename for entry in entries}) != len(entries):
            raise ValueError('Duplicate or excessive archive entries')
        for entry in entries:
            if not entry.filename.startswith(prefix) or entry.is_dir():
                continue
            name = entry.filename[len(prefix):]
            # Releases already bundle their update ZIP under cdn/updates/.
            # The live catalogue publishes only its selected five per source.
            if name.startswith('updates/'):
                continue
            resource_path(name)
            expanded += entry.file_size
            if (stat.S_ISLNK(entry.external_attr >> 16) or entry.flag_bits & 1
                    or entry.file_size > 64 * 1024 * 1024 or expanded > MAX_EXPANDED):
                raise ValueError('Unsafe or oversized resource archive')
            files[name] = archive.read(entry)
    manifests = [name for name in files if name.startswith('releases/') and name.endswith('.json')]
    if len(manifests) != 1:
        raise ValueError('Expected exactly one CDN release manifest')
    manifest_path = manifests[0]
    manifest_bytes = files[manifest_path]
    if manifest_path != 'releases/' + hashlib.sha256(manifest_bytes).hexdigest() + '.json':
        raise ValueError('CDN release manifest differs from its content address')
    manifest = json.loads(manifest_bytes)
    if manifest.get('version') != 1 or not manifest.get('projectVersion'):
        raise ValueError('Unsupported CDN release manifest')
    expected = {manifest_path}
    for asset in manifest['resources']:
        name = asset['path']
        resource_path(name)
        if name in expected:
            raise ValueError('Duplicate CDN manifest resource')
        expected.add(name)
        contents = files[name]
        if len(contents) != asset['size'] or hashlib.sha256(contents).hexdigest() != asset['sha256']:
            raise ValueError('CDN resource differs from its manifest: ' + name)
    if set(files) != expected:
        raise ValueError('CDN archive contains unlisted resources')
    return files


def merge_resources(files, destination):
    """Newest archive is applied first; old resources remain available by hash."""
    for name, contents in files.items():
        path = destination / resource_path(name)
        if path.exists():
            # Folder manifests are discovery metadata, unlike immutable modules
            # and content-addressed release manifests. Keep the newest metadata.
            if path.name == 'manifest.json' or path.read_bytes() == contents:
                continue
            raise ValueError('Conflicting published resource: ' + name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)


def baseline_files(directory):
    files = {}
    for folder in sorted(RESOURCE_FOLDERS):
        root = directory / folder
        if root.is_symlink():
            raise ValueError('Baseline resource directory is a symbolic link')
        for path in root.rglob('*'):
            if path.is_symlink():
                raise ValueError('Baseline resources contain a symbolic link')
            if path.is_file():
                name = path.relative_to(directory).as_posix()
                resource_path(name)
                files[name] = path.read_bytes()
    return files


def prepare(output, baseline=None, resource_archive=None, get=fetch):
    if output.exists() and any(output.iterdir()):
        raise ValueError('Pages output directory must be empty')
    if baseline is not None and not baseline.is_dir():
        raise ValueError('Existing Pages baseline directory is missing')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.pages-') as temporary:
        site = Path(temporary) / 'site'
        site.mkdir()
        # A current CI build may precede the latest stable release (dev builds).
        if resource_archive is not None:
            merge_resources(cdn_files(resource_archive.read_bytes()), site)
        catalogue = mirror(site / 'updates', get=get,
                           on_resources=lambda data: merge_resources(cdn_files(data, prefix='cdn/'), site))
        if baseline is not None:
            merge_resources(baseline_files(baseline), site)
        (site / '.nojekyll').write_bytes(b'')
        if output.exists():
            output.rmdir()
        site.replace(output)
    return catalogue


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, help='Read-only existing gh-pages resources')
    parser.add_argument('--resource-archive', type=Path, help='Matching CDN ZIP from this CI build')
    options = parser.parse_args()
    document = prepare(options.output, options.baseline, options.resource_archive)
    for source in document['sources']:
        print(source['label'], len(source['releases']), 'compatible stable releases (maximum 5)')
