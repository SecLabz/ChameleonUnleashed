#!/usr/bin/env python3
"""Rebuild Pages from release archives, without committing firmware to Git."""
import argparse
import json
from pathlib import Path
import tempfile

from mirror_firmware import fetch, mirror, save_atomic
from hosted_web import archived_shell
from development import DEVELOPMENT_TAG, manifest, stage as stage_development

from web_resources import RESOURCE_FOLDERS, resource_path, cdn_files, merge_resources


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
        selected = None
        development = None
        development_catalogue = {'format': 1, 'sources': []}

        def receive(data, release):
            nonlocal selected, development, development_catalogue
            if release.get('tag_name') == DEVELOPMENT_TAG:
                development, development_catalogue = stage_development(data, release, site)
                return
            files = cdn_files(data, prefix='cdn/')
            merge_resources(files, site)
            _, document = manifest(files)
            candidate = archived_shell(document, files)
            if not release.get('prerelease') and candidate and (selected is None or candidate[0] > selected[0]):
                selected = candidate

        # A current CI build may precede the latest stable release (dev builds).
        if resource_archive is not None:
            merge_resources(cdn_files(resource_archive.read_bytes()), site)
        catalogue = mirror(site / 'updates', get=get, on_resources=receive)
        if baseline is not None:
            merge_resources(baseline_files(baseline), site)
        for candidate, folder in ((selected, 'app'), (development, 'dev/app')):
            if candidate is None:
                continue
            app = site / folder
            app.mkdir()
            for name, contents in candidate[1].items():
                (app / name).write_bytes(contents)
        save_atomic(site / 'updates/dev/catalog.json', (json.dumps(development_catalogue, indent=2) + '\n').encode())
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
