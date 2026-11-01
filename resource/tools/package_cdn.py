#!/usr/bin/env python3
"""Package matching browser modules, tables and notices for the CDN."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[2]


def copy_asset(source, destination, asset):
    name = asset['path']
    path = PurePosixPath(name)
    if not path.name or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError(f'Invalid resource path: {name}')

    data = (source / path).read_bytes()
    if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != asset['sha256']:
        raise ValueError(f'{name} differs from its build manifest')

    target = destination / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return target


def copy_shared_notices(shared_source, destination):
    """Preserve repository-relative notice links in each standalone distribution."""
    shutil.copyfile(shared_source / 'LICENSE', destination / 'LICENSE')
    shutil.copytree(shared_source / 'docs/licenses', destination / 'docs/licenses')


def write_manifest(destination, manifest):
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def prepare(shared_source, output):
    cache = ROOT / '.cache/web'
    core = json.loads((cache / 'core.json').read_text())
    hardnested = json.loads((cache / 'hardnested.json').read_text())
    build = json.loads((cache / 'cdn-build.json').read_text())
    table = json.loads((ROOT / 'software/web/src/runtime/tables.json').read_text())
    if hardnested['tablePack'] != table:
        raise ValueError('Table pin changed; rebuild the matching Hardnested bundle')
    if set(hardnested['modules']) != {'scalar', 'simd'}:
        raise ValueError('Build both Hardnested variants before packaging CDN resources')

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.cdn-') as temporary:
        staging = Path(temporary) / 'cdn'
        core_file = copy_asset(cache / 'cdn/core', staging / 'core', core)
        copy_shared_notices(shared_source, core_file.parent)
        write_manifest(core_file.parent, {'version': 1, 'module': core})

        tables = staging / 'tables' / table['sha256']
        resources = shared_source / 'crates/nfc-mifare/resources'
        copy_asset(resources, tables, table)
        notices = ['hardnested-assets-notice.txt', 'hardnested-assets-license.txt']
        for name in notices:
            shutil.copyfile(resources / name, tables / name)

        copy_shared_notices(shared_source, tables)
        write_manifest(tables, {'version': 1, 'tablePack': table})

        folders = set()
        for module in hardnested['modules'].values():
            target = copy_asset(cache / 'cdn/hardnested', staging / 'hardnested', module)
            folders.add(target.parent)

        for folder in sorted(folders):
            copy_shared_notices(shared_source, folder)
            write_manifest(folder, hardnested)

        resources = []
        for path in sorted(staging.rglob('*')):
            if path.is_file():
                data = path.read_bytes()
                resources.append({
                    'path': path.relative_to(staging).as_posix(),
                    'size': len(data),
                    'sha256': hashlib.sha256(data).hexdigest(),
                })

        release = {'version': 1, **build, 'resources': resources}
        data = (json.dumps(release, indent=2) + '\n').encode()
        identity = hashlib.sha256(data).hexdigest()
        # Content addressing also distinguishes edited builds of the same source revision.
        releases = staging / 'releases'
        releases.mkdir()
        (releases / f'{identity}.json').write_bytes(data)

        # Stage only the current manifests' resources; the build cache retains older releases.
        archive = Path(temporary) / 'cdn.zip'
        with ZipFile(archive, 'w', compression=ZIP_DEFLATED) as package:
            for path in sorted(staging.rglob('*')):
                if path.is_file():
                    package.write(path, path.relative_to(staging).as_posix())

        # Browser checks and local development serve this same verified resource tree.
        shutil.copytree(staging, cache / 'cdn', dirs_exist_ok=True)
        archive.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shared-source', type=Path, default=ROOT.parent / 'nfc-libs-rs')
    parser.add_argument('--output', type=Path, default=ROOT / '.cache/web/chameleon-unleashed-cdn.zip')
    args = parser.parse_args()
    try:
        prepare(args.shared_source, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')

    print(args.output)


if __name__ == '__main__':
    main()
