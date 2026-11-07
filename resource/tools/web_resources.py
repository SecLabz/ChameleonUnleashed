"""Validate and merge immutable browser resource archives."""
import hashlib
import io
import json
from pathlib import PurePosixPath
import stat
from zipfile import ZipFile

from hosted_web import archived_shell

RESOURCE_FOLDERS = {'core', 'hardnested', 'tables', 'releases', 'web'}
MAX_EXPANDED = 256 * 1024 * 1024
MAX_RESOURCE_ENTRIES = 16384


def resource_path(name):
    path = PurePosixPath(name)
    if (not path.name or path.is_absolute() or '..' in path.parts or '\\' in name
            or ':' in name or path.as_posix() != name or path.parts[0] not in RESOURCE_FOLDERS):
        raise ValueError('Unexpected resource path: ' + name)
    return path


def cdn_files(data, prefix='', history=False):
    files = {}
    expanded = 0
    with ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_RESOURCE_ENTRIES or len({entry.filename for entry in entries}) != len(entries):
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
    if not manifests or (not history and len(manifests) != 1):
        raise ValueError('Expected exactly one CDN release manifest')
    expected = set(manifests)
    for manifest_path in manifests:
        manifest_bytes = files[manifest_path]
        if manifest_path != 'releases/' + hashlib.sha256(manifest_bytes).hexdigest() + '.json':
            raise ValueError('CDN release manifest differs from its content address')
        manifest = json.loads(manifest_bytes)
        if manifest.get('version') != 1 or not manifest.get('projectVersion'):
            raise ValueError('Unsupported CDN release manifest')
        listed = set()
        for asset in manifest['resources']:
            name = asset['path']
            resource_path(name)
            if name in listed or name in manifests:
                raise ValueError('Duplicate CDN manifest resource')
            listed.add(name)
            contents = files[name]
            # Folder discovery metadata follows the newest build. Runtime
            # modules and immutable manifests must always retain their pins.
            if history and name.endswith('/manifest.json'):
                continue
            if len(contents) != asset['size'] or hashlib.sha256(contents).hexdigest() != asset['sha256']:
                raise ValueError('CDN resource differs from its manifest: ' + name)
        expected.update(listed)
        archived_shell(manifest, files)
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
