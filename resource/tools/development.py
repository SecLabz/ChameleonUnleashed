#!/usr/bin/env python3
"""Package the latest development firmware and retain its immutable web resources."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tempfile
from zipfile import ZipFile

from hosted_web import FILES, archived_shell, bundle, markers
from bootloader_version import check_transition, read_version as bootloader_version
from mirror_firmware import SOURCES, application_zip, catalogue_package, fetch, releases, save_atomic, verified_asset
from nordic_update import MAX_ZIP, inspect_zip
from package_release import prepare as package_release, read_zip, write_zip
from web_resources import cdn_files, merge_resources

DEVELOPMENT_TAG = 'dev'
RESOURCE_PACKAGE = r'ChameleonUnleashed-.+-firmware-resources\.zip'


def manifest(files):
    names = [name for name in files if name.startswith('releases/') and name.endswith('.json')]
    if len(names) != 1:
        raise ValueError('Expected one current resource manifest')
    return names[0], json.loads(files[names[0]])


def development_resources(files, revision):
    """Bind the development label and repository-relative URLs to the shell hash."""
    name, document = manifest(files)
    selected = archived_shell(document, files)
    if selected is None:
        raise ValueError('Development firmware requires a matching web app')
    descriptor = document['webApp']
    raw = {key: selected[1][key] for key in FILES}
    raw['index.html'] = raw['index.html'].replace(
        markers(descriptor['version'], descriptor.get('resourceBase', '../')), b'', 1)
    label = ('Development · ' + revision[:7]).encode()
    raw['index.html'] = raw['index.html'].replace(b'</title>', ' · Development</title>'.encode())
    raw['index.html'] = raw['index.html'].replace(b'</head>',
        b'<meta name="chameleon-development" content="' + revision.encode() + b'">\n</head>')
    raw['index.html'] = raw['index.html'].replace(b'<div class="shell">',
        b'<div class="shell"><div class="development-build" role="note">' + label + b'</div>', 1)
    raw['style.css'] += b'\n.development-build{padding:.5rem;text-align:center;font-size:.75rem;color:#e6bd70}\n'
    version, web = bundle(raw, resource_base='../../')
    current = {key: data for key, data in files.items()
               if key != name and not key.startswith(descriptor['path'])}
    prefix = f'web/{version}/'
    current.update({prefix + key: data for key, data in web.items()})
    document['channel'] = 'development'
    document['webApp'] = {'version': version, 'path': prefix, 'resourceBase': '../../'}
    document['resources'] = [{'path': key, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                             for key, data in sorted(current.items())]
    data = (json.dumps(document, indent=2) + '\n').encode()
    current['releases/' + hashlib.sha256(data).hexdigest() + '.json'] = data
    return current


def previous_resources(data):
    with ZipFile(io.BytesIO(data)) as archive:
        retained = any(name.startswith('history/') for name in archive.namelist())
    return cdn_files(data, prefix='history/' if retained else 'cdn/', history=retained)


def latest_development(repo=SOURCES[0][2], get=fetch):
    release = next((item for item in releases(repo, get)
                    if item.get('tag_name') == DEVELOPMENT_TAG), None)
    if release is None:
        return None
    assets = sorted(release.get('assets', []), key=lambda asset: asset.get('created_at', ''), reverse=True)
    asset = next((asset for asset in assets if re.fullmatch(RESOURCE_PACKAGE, asset['name'])), None)
    if asset is None:
        return None  # Legacy dev releases only carried desktop archives.
    return verified_asset(repo, asset, get)


def stage(data, release, site):
    """Publish one verified dev package alongside the independent stable channel."""
    files = cdn_files(data, prefix='cdn/')
    merge_resources(files, site)
    merge_resources(previous_resources(data), site)
    _, document = manifest(files)
    catalogue = {'format': 1, 'sources': []}
    if document.get('channel') != 'development':
        return None, catalogue  # Old pre-releases had no separate development site.
    candidate = archived_shell(document, files)
    revision = document.get('source', {}).get('revision', '')
    if (not candidate or not re.fullmatch(r'[0-9a-f]{40}', revision)
            or document['source'].get('dirty') is not False or not release.get('prerelease')):
        raise ValueError('Development site must match its source commit and pre-release')
    package = application_zip(data)
    selected = catalogue_package('unleashed', package)
    if selected is None:
        raise ValueError('Development firmware has no compatible recovery')
    identity = hashlib.sha256(package).hexdigest()
    save_atomic(site / 'updates/dev/files' / (identity + '.zip'), package)
    catalogue['sources'] = [{
        'id': 'unleashed', 'label': 'Unleashed', 'repository': SOURCES[0][2],
        'releases': [{'version': 'v' + document['projectVersion'] + '-dev.' + revision[:7],
            'prerelease': True, 'published_at': release.get('published_at'), 'url': release['html_url'],
            'layout': selected[0], 'packages': {'application': {
                'path': 'files/' + identity + '.zip', 'size': len(package), 'sha256': identity}}}]}]
    save_atomic(site / 'dev/index.html', (
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta http-equiv="refresh" content="0;url=app/">'
        '<title>Chameleon Unleashed</title></html>').encode())
    return candidate, catalogue


def prepare(artifacts, output, revision, previous=None):
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('Expected a complete source commit')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output directory must be empty')
    original = cdn_files((artifacts / 'chameleon-unleashed-cdn.zip').read_bytes())
    _, document = manifest(original)
    if document.get('source', {}).get('revision') != revision or document['source'].get('dirty') is not False:
        raise ValueError('Development package must match the clean CI source commit')
    current = development_resources(original, revision)
    application = (artifacts / 'chameleon-unleashed-app.zip').read_bytes()
    if catalogue_package('unleashed', application) is None:
        raise ValueError('Development firmware requires permanent web recovery')
    installed = inspect_zip((artifacts / 'chameleon-unleashed-install.zip').read_bytes())
    if installed['application']['payload'] != inspect_zip(application)['application']['payload']:
        raise ValueError('Installer differs from the development application')
    bootloader = inspect_zip((artifacts / 'chameleon-unleashed-bootloader.zip').read_bytes())['bootloader']
    if bootloader['version'] != bootloader_version():
        raise ValueError('Bootloader package differs from its declared revision')
    if (installed.get('bootloader', {}).get('payload') != bootloader['payload']
            or installed['bootloader']['version'] != bootloader['version']):
        raise ValueError('Installer differs from the development bootloader')
    previous_bootloader = None
    if previous is not None:
        with ZipFile(io.BytesIO(previous)) as archive:
            entry = archive.getinfo('firmware/bootloader.zip')
            if entry.file_size > MAX_ZIP:
                raise ValueError('Previous bootloader package is too large')
            previous_bootloader = inspect_zip(archive.read(entry))['bootloader']
    check_transition(bootloader, previous_bootloader)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.development-') as temporary:
        staging = Path(temporary)
        history = staging / 'history'
        merge_resources(current, history)
        merge_resources(original, history)
        if previous is not None:
            merge_resources(previous_resources(previous), history)
        packages = staging / 'packages'
        names = package_release(artifacts, packages, document['projectVersion'], development=True)
        name = next(name for name in names if re.fullmatch(RESOURCE_PACKAGE, name))
        contents = {key: data for key, data in read_zip(packages / name).items() if not key.startswith('cdn/')}
        contents.update({'cdn/' + key: data for key, data in current.items()})
        contents.update({'history/' + path.relative_to(history).as_posix(): path.read_bytes()
                         for path in history.rglob('*') if path.is_file()})
        write_zip(packages / name, contents)
        # Validate the complete retained set before replacing the public package.
        previous_resources((packages / name).read_bytes())
        if output.exists():
            output.rmdir()
        packages.replace(output)
    return name


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', SOURCES[0][2]))
    args = parser.parse_args()
    print(args.output / prepare(args.artifacts, args.output, args.commit, latest_development(args.repo)))
